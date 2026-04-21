#ifndef CHANNELMIXER_H
#define CHANNELMIXER_H

#include <QObject>
#include <QMutex>
#include <QVector>

#include "audioconverter.h"

// Simple broadcast audio bus. TX from rig i is routed to every other rig
// as RX, scaled by a per-link attenuation. MVP assumes all rigs share the
// same codec / sample rate (negotiated by icomServer at connect time).
class channelMixer : public QObject
{
    Q_OBJECT

public:
    explicit channelMixer(int numRigs, QObject* parent = nullptr);

    // Linear gain applied to the 16-bit PCM samples when copying TX -> RX.
    // Default ~0.1 (≈ -20 dB).
    void setAttenuation(float gain);

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
    QMutex mx;
};

#endif
