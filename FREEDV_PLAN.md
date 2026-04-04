# FreeDV Integration into wfweb

## Context

wfweb is a web-based remote control interface for amateur radios. Users want to operate FreeDV digital voice modes (700D, 700E, and eventually RADE) through their browser. FreeDV encodes speech into modem tones that travel over SSB radio; the other station's FreeDV software decodes those tones back to speech. Without this integration, wfweb users cannot participate in FreeDV QSOs.

The processing must happen **server-side** because:
- The radio emits/receives modem tones, not speech -- all browser clients need decoded speech
- RADE (the future flagship mode) requires heavy ML inference unsuitable for browser WASM
- Real-time bidirectional voice needs low latency (no extra network round-trip)
- Single decode serves all connected browser clients

## Architecture Overview

A new `FreeDVProcessor` class sits in the audio pipeline between the radio and the existing `audioConverter`. When FreeDV is enabled, radio audio (modem tones) passes through FreeDV RX to produce speech before reaching the browser. On TX, browser speech passes through FreeDV TX to produce modem tones before reaching the radio.

```
RX (normal):   Radio → receiveRxAudio → rxConverter → WebSocket → Browser
RX (FreeDV):   Radio → receiveRxAudio → FreeDVProcessor::processRx → rxConverter → WebSocket → Browser

TX (normal):   Browser → onWsBinaryMessage → txConverter/USB → Radio
TX (FreeDV):   Browser → onWsBinaryMessage → FreeDVProcessor::processTx → txConverter/USB → Radio
```

## Dependencies

- **libcodec2** (v1.2.0, LGPL-2.1): Available via `brew install codec2` on macOS, `libcodec2-dev` on Debian/Ubuntu. Contains both the Codec2 vocoder and the full FreeDV API (`freedv_api.h`) in a single library.
- No additional dependencies for modes 700D/700E (pure C99).
- RADE support deferred until its C port stabilizes.

## Implementation Plan

### Phase 1: Build Integration

**File: `wfweb.pro`**

Add codec2 library linkage following the existing pattern for opus/eigen:

```
# macOS: brew install codec2 → /opt/homebrew or /usr/local
macx:LIBS += -lcodec2

# Linux: apt install libcodec2-dev
linux:LIBS += -lcodec2

# Windows: vcpkg install codec2
win32:!isEmpty(VCPKG_DIR): LIBS += -lcodec2
```

The header `<codec2/freedv_api.h>` is installed by the system package. No need to vendor source -- this follows the same pattern as opus.

### Phase 2: FreeDVProcessor Class

**New files:**
- `include/freedvprocessor.h`
- `src/freedvprocessor.cpp`

```cpp
class FreeDVProcessor : public QObject {
    Q_OBJECT
public:
    explicit FreeDVProcessor(QObject *parent = nullptr);
    ~FreeDVProcessor();

public slots:
    // Init with mode (FREEDV_MODE_700D, etc.), radio sample rate
    bool init(int freedvMode, quint32 radioSampleRate);
    void processRx(audioPacket audio);  // modem tones in → speech out
    void processTx(audioPacket audio);  // speech in → modem tones out
    void setEnabled(bool enabled);
    void setSquelch(bool enabled, float thresholdDb);

signals:
    void rxReady(audioPacket audio);        // decoded speech → rxConverter
    void txReady(audioPacket audio);        // modem tones → radio
    void syncChanged(bool inSync);          // UI status
    void statsUpdate(float snr, int totalBitErrors, int totalBits);

private:
    struct freedv *fdv = nullptr;
    bool enabled = false;
    int mode = 0;

    // Resamplers: radio rate (48kHz) ↔ FreeDV modem rate (8kHz)
    SpeexResamplerState *rxDownsampler = nullptr;   // 48k → 8k (RX input)
    SpeexResamplerState *rxUpsampler = nullptr;     // 8k → 48k (RX output speech)
    SpeexResamplerState *txDownsampler = nullptr;   // 48k → 8k (TX input speech)
    SpeexResamplerState *txUpsampler = nullptr;     // 8k → 48k (TX output modem)

    // Accumulation buffers (FreeDV needs exact frame sizes)
    QByteArray rxAccumulator;
    QByteArray txAccumulator;

    // Frame sizes from FreeDV API
    int nSpeechSamples = 0;     // freedv_get_n_speech_samples()
    int nNomModemSamples = 0;   // freedv_get_n_nom_modem_samples()
    int nMaxModemSamples = 0;   // freedv_get_n_max_modem_samples()
    int nMaxSpeechSamples = 0;  // freedv_get_n_max_speech_samples()
    int modemSampleRate = 0;    // freedv_get_modem_sample_rate()
    int speechSampleRate = 0;   // freedv_get_speech_sample_rate()
};
```

**Key implementation details:**

1. **RX path (`processRx`):**
   - Receive radio audio (Int16 PCM, 48kHz from rig)
   - Resample 48kHz → 8kHz (modem rate) using Speex resampler
   - Accumulate samples until `freedv_nin(fdv)` samples available
   - Call `freedv_rx(fdv, speech_out, demod_in)` -- returns `nout` decoded speech samples
   - **Critical:** call `freedv_nin()` after every `freedv_rx()` -- the required input size changes dynamically (timing recovery)
   - Resample decoded speech 8kHz → 48kHz
   - Emit `rxReady(audio)` with speech PCM
   - Poll `freedv_get_modem_stats()` for sync/SNR, emit `statsUpdate()`

2. **TX path (`processTx`):**
   - Receive browser speech (Int16 PCM, 48kHz)
   - Resample 48kHz → 8kHz
   - Accumulate until `nSpeechSamples` available
   - Call `freedv_tx(fdv, mod_out, speech_in)` -- produces `nNomModemSamples` modem samples
   - Resample modem output 8kHz → 48kHz
   - Emit `txReady(audio)` with modem tones

3. **Accumulation buffers** are essential because:
   - Radio audio arrives in arbitrary chunk sizes (typically 20ms = 960 samples at 48kHz)
   - FreeDV needs exact frame sizes (e.g. 700D: `n_speech_samples` = 320, `nin` ≈ 1120 modem samples at 8kHz)
   - After resampling 48k→8k, chunks shrink ~6x, so several radio packets accumulate before one FreeDV frame

### Phase 3: Webserver Integration

**File: `include/webserver.h`** -- add members:

```cpp
// FreeDV processing
FreeDVProcessor *freedvProcessor = nullptr;
QThread *freedvThread = nullptr;
bool freedvEnabled = false;
int freedvMode = 0;  // FREEDV_MODE_700D etc.
```

Add signals for setup:
```cpp
signals:
    void setupFreeDV(int mode, quint32 radioSampleRate);
    void sendToFreeDV(audioPacket audio);
    void sendToFreeDVTx(audioPacket audio);
```

**File: `src/webserver.cpp`** -- modifications:

1. **In `setupAudio()` (line ~2264):** After creating rxConverter/txConverter, also create the FreeDV processor thread (but don't activate it until user enables FreeDV).

2. **In `receiveRxAudio()` (line ~2343):** Route based on `freedvEnabled`:
   ```cpp
   void webServer::receiveRxAudio(audioPacket audio) {
       if (audioClients.isEmpty() || !audioConfigured) return;
       if (freedvEnabled) {
           emit sendToFreeDV(audio);  // → FreeDVProcessor::processRx
       } else {
           emit sendToConverter(audio);  // existing path
       }
   }
   ```

3. **FreeDV RX output:** Connect `FreeDVProcessor::rxReady` → `sendToConverter` (feeds decoded speech into the existing rxConverter for format conversion and WebSocket delivery).

4. **In `onWsBinaryMessage()` (line ~1179):** Route TX audio based on `freedvEnabled`:
   ```cpp
   if (freedvEnabled) {
       // Speech from browser → FreeDV TX → modem tones
       emit sendToFreeDVTx(pkt);
   } else {
       // existing path
   }
   ```

5. **FreeDV TX output:** Connect `FreeDVProcessor::txReady` to either the existing txConverter (LAN path) or direct USB write.

6. **New WebSocket command handler** in `handleCommand()`:
   ```cpp
   else if (type == "setFreeDV") {
       bool enable = cmd["enabled"].toBool();
       QString modeName = cmd["mode"].toString();  // "700D", "700E"
       // map to FREEDV_MODE_* constant, init/enable processor
   }
   ```

7. **Status updates:** In `buildStatusJson()` (line ~1750), add FreeDV status:
   ```cpp
   if (freedvEnabled && freedvProcessor) {
       status["freedv"] = true;
       status["freedvMode"] = freedvModeName;
       status["freedvSync"] = currentSync;
       status["freedvSNR"] = currentSNR;
   }
   ```

### Phase 4: Browser UI

**File: `resources/web/index.html`**

1. **FreeDV control panel** (minimal, in the existing toolbar/settings area):
   - Toggle switch: Enable/Disable FreeDV
   - Mode dropdown: 700D, 700E
   - Status indicators: Sync LED (green/red), SNR readout (dB)

2. **WebSocket commands:**
   - Send: `{"cmd": "setFreeDV", "enabled": true, "mode": "700D"}`
   - Receive status in periodic updates: `{"freedv": true, "freedvSync": true, "freedvSNR": 5.2}`

3. **No audio processing changes in browser** -- it continues to send/receive normal PCM audio over WebSocket. The FreeDV encode/decode is transparent to the browser audio pipeline.

### Phase 5: Mode Awareness (Optional Enhancement)

FreeDV is typically used on USB/LSB. The server could suggest enabling FreeDV when the radio is on a known FreeDV calling frequency (e.g., 14.236 MHz USB), or auto-disable when switching to FM/AM.

## Files to Create/Modify

| File | Action | Description |
|------|--------|-------------|
| `include/freedvprocessor.h` | **Create** | FreeDVProcessor class declaration |
| `src/freedvprocessor.cpp` | **Create** | FreeDV RX/TX processing, resampling, accumulation |
| `wfweb.pro` | Modify | Add codec2 linkage, new source/header files |
| `include/webserver.h` | Modify | Add FreeDV members, signals |
| `src/webserver.cpp` | Modify | Route audio through FreeDV, handle commands, push status |
| `resources/web/index.html` | Modify | FreeDV controls and status display |

## Existing Code to Reuse

- **Speex resampler** (`src/audio/resampler/`) -- already in project, used by audioConverter. FreeDVProcessor uses the same `wf_resampler_*` API for 48k↔8k conversion.
- **audioPacket struct** (`include/audioconverter.h:44`) -- standard audio container, used throughout.
- **Thread + signal/slot pattern** -- follow exact pattern from `setupAudio()` at `src/webserver.cpp:2278-2289` (create QObject, moveToThread, connect signals).
- **WebSocket command handling** -- extend `handleCommand()` at `src/webserver.cpp:1264`.
- **Status push** -- extend `buildStatusJson()` at `src/webserver.cpp:1750`.

## Verification

1. **Build:** `brew install codec2 && qmake && make clean && make` -- must compile without errors on macOS
2. **Unit test FreeDV processing:** Pipe a known FreeDV 700D modulated audio file through `FreeDVProcessor::processRx()`, verify decoded speech output is non-silent and matches expected duration
3. **Integration test:**
   - Connect wfweb to a radio on USB mode
   - Enable FreeDV 700D in the web UI
   - Have another station (or FreeDV GUI on another computer) transmit on the same frequency
   - Verify: sync indicator turns green, SNR shows reasonable value, decoded speech plays in browser
4. **TX test:** Enable FreeDV TX in browser, speak into mic, verify the other station's FreeDV GUI decodes the transmission
5. **Passthrough test:** With FreeDV disabled, verify normal SSB audio works unchanged (no regression)

## Out of Scope (Future Work)

- **RADE mode:** Requires C port of FARGAN vocoder (not yet stable). Will slot into same FreeDVProcessor via `freedv_open(FREEDV_MODE_RADE)` when available.
- **FreeDV 1600, 2020 modes:** Legacy/deprecated. Not worth implementing.
- **VHF modes (2400A/B, 800XA):** Different use case (FM radios). Could be added later.
- **Text/protocol callbacks:** FreeDV supports embedded text transmission. Nice-to-have, not essential.
