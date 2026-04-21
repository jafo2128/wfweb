#include "channelmixer.h"

#include <QDebug>
#include <QtEndian>
#include <cstring>

channelMixer::channelMixer(int numRigs, QObject* parent)
    : QObject(parent), numRigs(numRigs), attenuation(0.1f)
{
}

void channelMixer::setAttenuation(float gain)
{
    QMutexLocker lock(&mx);
    attenuation = gain;
}

void channelMixer::pushTxAudio(int srcRig, const audioPacket& pkt)
{
    float gain;
    int n;
    {
        QMutexLocker lock(&mx);
        gain = attenuation;
        n = numRigs;
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

    for (int dst = 0; dst < n; ++dst) {
        if (dst == srcRig) continue;
        emit rxAudioForRig(dst, scaled);
    }
}
