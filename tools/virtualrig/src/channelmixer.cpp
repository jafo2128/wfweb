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
    : QObject(parent), numRigs(numRigs), channelRouting(true)
{
    rigs.resize(numRigs);
    noiseRms = QVector<float>(numRigs, 0.0f);
    linkGain.resize(numRigs);
    for (auto& row : linkGain) row = QVector<float>(numRigs, 0.1f);
}

void channelMixer::setAttenuation(float gain)
{
    QMutexLocker lock(&mx);
    for (auto& row : linkGain)
        for (auto& g : row) g = gain;
}

void channelMixer::setLinkAttenuation(int src, int dst, float gain)
{
    QMutexLocker lock(&mx);
    if (src >= 0 && src < linkGain.size() &&
        dst >= 0 && dst < linkGain[src].size()) {
        linkGain[src][dst] = gain;
    }
}

float channelMixer::linkAttenuation(int src, int dst) const
{
    QMutexLocker lock(&mx);
    if (src >= 0 && src < linkGain.size() &&
        dst >= 0 && dst < linkGain[src].size()) {
        return linkGain[src][dst];
    }
    return 0.0f;
}

void channelMixer::setNoiseLevel(float rms)
{
    QMutexLocker lock(&mx);
    for (auto& v : noiseRms) v = rms;
}

void channelMixer::setNoiseLevel(int dstRig, float rms)
{
    QMutexLocker lock(&mx);
    if (dstRig >= 0 && dstRig < noiseRms.size()) noiseRms[dstRig] = rms;
}

float channelMixer::noiseLevel(int dstRig) const
{
    QMutexLocker lock(&mx);
    if (dstRig >= 0 && dstRig < noiseRms.size()) return noiseRms[dstRig];
    return 0.0f;
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
    int n;
    bool gate;
    QVector<virtualRig*> snap;
    QVector<float> gains;
    {
        QMutexLocker lock(&mx);
        n = numRigs;
        gate = channelRouting;
        snap = rigs;
        if (srcRig >= 0 && srcRig < linkGain.size())
            gains = linkGain[srcRig];
        else
            gains = QVector<float>(numRigs, 0.0f);
    }

    const int sampleCount = pkt.data.size() / (int)sizeof(qint16);
    const qint16* srcSamples = reinterpret_cast<const qint16*>(pkt.data.constData());

    static int tick = 0;
    if (++tick % 50 == 1) {
        qInfo() << "mixer: tx from rig" << srcRig << "samples=" << sampleCount
                << "n=" << n;
    }

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

        // Per-link scaling: build a fresh PCM copy so each destination can
        // see a different fade without interfering with the others.
        float g = (dst < gains.size()) ? gains[dst] : 0.0f;
        audioPacket out = pkt;
        out.data.resize(pkt.data.size());
        qint16* dstSamples = reinterpret_cast<qint16*>(out.data.data());
        if (g == 1.0f) {
            memcpy(out.data.data(), pkt.data.constData(), pkt.data.size());
        } else {
            for (int i = 0; i < sampleCount; ++i) {
                int v = static_cast<int>(srcSamples[i] * g);
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                dstSamples[i] = static_cast<qint16>(v);
            }
        }
        emit rxAudioForRig(dst, out);
    }
}
