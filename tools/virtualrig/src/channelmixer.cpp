#include "channelmixer.h"
#include "virtualrig.h"

#include <QDebug>
#include <QtEndian>
#include <cstring>

namespace {
// Receiver passband tolerance per Icom mode code. 0x00=LSB 0x01=USB 0x02=AM
// 0x03=CW 0x04=RTTY 0x05=FM 0x07=CW-R 0x08=RTTY-R 0x17=DV. Rough but
// good enough for a test bench — models "signal lands in my receiver's
// passband" rather than any real propagation physics.
qint64 passbandHz(quint8 mode)
{
    switch (mode) {
    case 0x00: case 0x01: return 3000;   // SSB: ~2.7 kHz + slack
    case 0x02:            return 5000;   // AM (double-sideband)
    case 0x03: case 0x07: return 500;    // CW: narrow
    case 0x04: case 0x08: return 500;    // RTTY: narrow
    case 0x05:            return 10000;  // FM
    case 0x17:            return 5000;   // DV
    default:              return 3000;
    }
}

bool channelCompatible(quint64 srcFreq, quint8 srcMode,
                       quint64 dstFreq, quint8 dstMode)
{
    // Strict mode match. USB↔LSB etc. would require sideband-inversion
    // simulation we don't do; keep it simple.
    if (srcMode != dstMode) return false;
    qint64 dHz = (qint64)srcFreq - (qint64)dstFreq;
    if (dHz < 0) dHz = -dHz;
    return dHz <= passbandHz(srcMode);
}
} // namespace

channelMixer::channelMixer(int numRigs, QObject* parent)
    : QObject(parent), numRigs(numRigs), attenuation(0.1f), channelRouting(true)
{
    rigs.resize(numRigs);
}

void channelMixer::setAttenuation(float gain)
{
    QMutexLocker lock(&mx);
    attenuation = gain;
}

void channelMixer::setChannelRouting(bool on)
{
    QMutexLocker lock(&mx);
    channelRouting = on;
}

void channelMixer::registerRig(int idx, virtualRig* rig)
{
    QMutexLocker lock(&mx);
    if (idx >= 0 && idx < rigs.size()) rigs[idx] = rig;
}

void channelMixer::pushTxAudio(int srcRig, const audioPacket& pkt)
{
    float gain;
    int n;
    bool gate;
    QVector<virtualRig*> snap;
    {
        QMutexLocker lock(&mx);
        gain = attenuation;
        n = numRigs;
        gate = channelRouting;
        snap = rigs;
    }

    // Scale a copy of the PCM-16LE payload. If the negotiated codec is
    // something else, we still forward the bytes unchanged — MVP pins caps
    // to LPCM so this path is the expected one.
    audioPacket scaled = pkt;
    const int sampleCount = scaled.data.size() / 2;
    if (sampleCount > 0 && gain != 1.0f) {
        qint16* samples = reinterpret_cast<qint16*>(scaled.data.data());
        for (int i = 0; i < sampleCount; ++i) {
            int v = static_cast<int>(samples[i] * gain);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            samples[i] = static_cast<qint16>(v);
        }
    }

    static int tick = 0;
    if (++tick % 50 == 1) {
        qInfo() << "mixer: tx from rig" << srcRig << "samples=" << sampleCount
                << "n=" << n;
    }

    // Cache the source's channel so we only read it once even if the user
    // is rapidly retuning during TX.
    quint64 srcFreq = 0;
    quint8 srcMode = 0;
    if (gate && srcRig >= 0 && srcRig < snap.size() && snap[srcRig]) {
        srcFreq = snap[srcRig]->freq();
        srcMode = snap[srcRig]->mode();
    }

    for (int dst = 0; dst < n; ++dst) {
        if (dst == srcRig) continue;
        if (gate && dst < snap.size() && snap[dst]) {
            if (!channelCompatible(srcFreq, srcMode,
                                   snap[dst]->freq(), snap[dst]->mode())) {
                continue;
            }
        }
        emit rxAudioForRig(dst, scaled);
    }
}
