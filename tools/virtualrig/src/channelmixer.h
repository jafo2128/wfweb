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

    // Linear gain applied to the 16-bit PCM samples when copying TX -> RX.
    // Default ~0.1 (≈ -20 dB).
    void setAttenuation(float gain);

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
    float attenuation;
    bool channelRouting;
    QVector<virtualRig*> rigs;
    QMutex mx;
};

#endif
