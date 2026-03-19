# Plan: Add LAN/UDP Connection Support to servermain (wfweb)

## Context

`wfweb -s 7300.ini` with `EnableLAN=true` in the config still opens a serial port because `servermain::openRig()` has no LAN code path — it always calls the serial overload of `commSetup()`. The desktop GUI (`wfmain.cpp`) already has full LAN support. We need to port that logic to `servermain`.

Additionally, `servermain::loadSettings()` expects a Qt array format (`[Radios]` with `1\key=value`) but the user's config and README use flat sections (`[Radio]`, `[LAN]`). We need to support the flat format.

## Files to Modify

1. **`include/servermain.h`** — add `enableLAN` field to `preferences` struct
2. **`src/servermain.cpp`** — main changes in `setDefPrefs()`, `loadSettings()`, `openRig()`, `receiveRigCaps()`

## Reference Files (read-only)

- `src/wfmain.cpp:476-502` — LAN branch in openRig
- `src/wfmain.cpp:2040-2100` — LAN settings loading
- `include/icomcommander.h:27` — UDP commSetup overload signature
- `include/webserver.h:83-84` — `setupAudio()` vs `setupUsbAudio()` signatures

## Step-by-Step Changes

### Step 1: Add `enableLAN` to preferences struct in `servermain.h`

In the inner `struct preferences` (line 241), add:
```cpp
bool enableLAN = false;
```

The struct already has `rxAudio`/`txAudio` (audioSetup) and the class already has `rxSetup`/`txSetup` members (line 263-264) plus `udpPrefs`/`udpDefPrefs` (line 259-260) and `usingLAN` (line 239). No other header changes needed.

### Step 2: Set LAN defaults in `setDefPrefs()`

After line 543 (`defPrefs.txAudio.name = ...`), add:
```cpp
defPrefs.enableLAN = false;
```

The `udpDefPrefs` are already initialized (lines 545-551). Add the missing `scopeLANPort` default if absent.

Also set LAN audio defaults on the existing class members `rxSetup`/`txSetup`:
```cpp
rxSetup.codec = 4;       // LPCM 16-bit
txSetup.codec = 4;
rxSetup.sampleRate = 48000;
txSetup.sampleRate = 48000;
rxSetup.isinput = false;
txSetup.isinput = true;
```

### Step 3: Read `[LAN]` section in `loadSettings()`

After the Server group is read (around line 759, before `settings->sync()`), add reading of the `[LAN]` group:

```cpp
settings->beginGroup("LAN");
prefs.enableLAN = settings->value("EnableLAN", defPrefs.enableLAN).toBool();
if (prefs.enableLAN) {
    udpPrefs.ipAddress = settings->value("IPAddress", udpDefPrefs.ipAddress).toString();
    udpPrefs.controlLANPort = settings->value("ControlLANPort", udpDefPrefs.controlLANPort).toInt();
    udpPrefs.serialLANPort = settings->value("SerialLANPort", udpDefPrefs.serialLANPort).toInt();
    udpPrefs.audioLANPort = settings->value("AudioLANPort", udpDefPrefs.audioLANPort).toInt();
    udpPrefs.username = settings->value("Username", udpDefPrefs.username).toString();
    udpPrefs.password = settings->value("Password", udpDefPrefs.password).toString();
    rxSetup.sampleRate = settings->value("AudioRXSampleRate", 48000).toInt();
    txSetup.sampleRate = rxSetup.sampleRate;
    rxSetup.codec = settings->value("AudioRXCodec", 4).toInt();
    txSetup.codec = settings->value("AudioTXCodec", 4).toInt();
}
settings->endGroup();
```

### Step 4: Support flat `[Radio]` section in `loadSettings()`

Currently when `numRadios == 0` (no `[Radios]` array), servermain writes defaults and creates a radio entry. Enhance this: also try reading from the flat `[Radio]` section to get the CIV address and manufacturer, so the user's `7300.ini` format works.

After writing the default Radios array (around line 585), read flat [Radio] values and apply them:
```cpp
settings->beginGroup("Radio");
unsigned char flatCiv = (unsigned char)settings->value("RigCIVuInt", defPrefs.radioCIVAddr).toInt();
manufacturersType_t flatManuf = static_cast<manufacturersType_t>(settings->value("Manufacturer", -1).toInt());
settings->endGroup();
```

Then use `flatCiv` and `flatManuf` when creating the default radio entry instead of the hardcoded defaults.

### Step 5: Add LAN branch to `openRig()`

Replace lines 126-142 with an if/else branch:

```cpp
for (RIGCONFIG* radio : serverConfig.rigs) {
    if (radio->rigThread != Q_NULLPTR) {
        if (prefs.enableLAN) {
            usingLAN = true;
            udpPrefs.waterfallFormat = radio->waterfallFormat;
            audioSetup lanRxSetup = rxSetup;
            audioSetup lanTxSetup = txSetup;
            memcpy(lanRxSetup.guid, radio->guid, GUIDLEN);
            memcpy(lanTxSetup.guid, radio->guid, GUIDLEN);
            QMetaObject::invokeMethod(radio->rig, [=]() {
                radio->rig->commSetup(rigList, radio->civAddr, udpPrefs,
                    lanRxSetup, lanTxSetup, QString("none"), 0);
            }, Qt::QueuedConnection);
            qInfo(logSystem()) << "Connecting via LAN to" << udpPrefs.ipAddress;
        } else {
            QMetaObject::invokeMethod(radio->rig, [=]() {
                radio->rig->commSetup(rigList, radio->civAddr, radio->serialPort,
                    radio->baudRate, QString("none"), 0, radio->waterfallFormat);
            }, Qt::QueuedConnection);
        }
    }
}

// Setup web audio
if (web != Q_NULLPTR) {
    if (prefs.enableLAN) {
        QMetaObject::invokeMethod(web, "setupAudio", Qt::QueuedConnection,
            Q_ARG(quint8, rxSetup.codec), Q_ARG(quint32, rxSetup.sampleRate));
    } else {
        QMetaObject::invokeMethod(web, "setupUsbAudio", Qt::QueuedConnection,
            Q_ARG(quint32, 48000));
    }
}
```

### Step 6: Adjust queue interval for LAN in `receiveRigCaps()`

In the queue interval calculation block (lines 342-354), add a LAN-specific path:

```cpp
if (usingLAN) {
    queue->interval(70);
    qInfo(logSystem()) << "Queue interval set to 70ms (LAN mode)";
} else {
    // existing baud-rate-based calculation
    quint32 baud = prefs.serialPortBaud;
    ...
}
```

### Step 7: Set `serverConfig.lan` in `setServerToPrefs()`

Before or after creating the server object (around line 476), add:
```cpp
serverConfig.lan = prefs.enableLAN;
```

This tells the outward-facing wfserver whether the radio connection is LAN-based (matching what `wfmain.cpp:1042` does).

## Verification

1. **Build**: `qmake && make` — confirm no compile errors
2. **Serial mode preserved**: Run `wfweb` without `-s` (or with a serial config) — should behave exactly as before
3. **LAN mode**: Run `wfweb -s 7300.ini` with:
   ```ini
   [Radio]
   Manufacturer=0
   RigCIVuInt=130

   [LAN]
   EnableLAN=true
   IPAddress=192.168.67.103
   ControlLANPort=50001
   SerialLANPort=50002
   AudioLANPort=50003
   ```
   Verify in logs: "Connecting via LAN to 192.168.67.103" instead of serial port messages
4. **Web audio**: Connect browser to the web server and verify audio playback works in LAN mode
