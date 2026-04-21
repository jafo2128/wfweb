#ifndef CHANNELMIXER_H
#define CHANNELMIXER_H

#include <QObject>
#include <QMutex>
#include <QVector>

#include "audioconverter.h"

class virtualRig;

// Audio bus with channel-aware routing. TX from rig i is forwarded to rig j
// only when they share a mode and are within receiver-passband frequency
// tolerance — a simple simulation of "can you hear me on this channel?".
// Falls back to pure broadcast when freq/mode gating is disabled. Assumes
// all rigs share the same codec / sample rate (negotiated by icomServer
// at connect time).
class channelMixer : public QObject
{
    Q_OBJECT

public:
    explicit channelMixer(int numRigs, QObject* parent = nullptr);

    // Set uniform linear gain on every src→dst link. Default ~0.1 (≈ -20 dB).
    void setAttenuation(float gain);

    // Override gain for a specific directed pair. Allows asymmetric links
    // (one-way hearing, partial fade) without affecting the rest of the bus.
    void setLinkAttenuation(int src, int dst, float gain);
    float linkAttenuation(int src, int dst) const;

    // Per-destination-rig noise floor, in Int16 RMS units (0..32767).
    // White Gaussian noise at this RMS is added to every chunk the rig emits
    // to its client — so the noise floor is always present, signal or not.
    void setNoiseLevel(float rms);                  // all rigs
    void setNoiseLevel(int dstRig, float rms);      // one rig
    float noiseLevel(int dstRig) const;

    // Disable channel gating (everyone hears everyone). Default: enabled.
    void setChannelRouting(bool on);

    // Register a rig so the mixer can read its current freq/mode on forward.
    // Caller retains ownership; must outlive the mixer.
    void registerRig(int idx, virtualRig* rig);

public slots:
    void pushTxAudio(int srcRig, const audioPacket& pkt);

signals:
    // Emitted once per destination rig, per incoming TX packet.
    // Connect with a queued connection so it lands in the destination
    // icomServer's thread.
    void rxAudioForRig(int dstRig, const audioPacket& pkt);

private:
    int numRigs;
    bool channelRouting;
    // linkGain[src][dst] — directed gain per pair. Diagonal unused.
    QVector<QVector<float>> linkGain;
    // noiseRms[rig] — receive-side noise added on emit.
    QVector<float> noiseRms;
    QVector<virtualRig*> rigs;
    mutable QMutex mx;
};

#endif
