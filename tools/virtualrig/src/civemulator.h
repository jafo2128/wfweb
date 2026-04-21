#ifndef CIVEMULATOR_H
#define CIVEMULATOR_H

#include <QObject>
#include <QByteArray>
#include <QString>

// Minimal CI-V responder. Parses inbound frames from a client (as received
// by icomServer and forwarded via haveDataFromServer), tracks per-rig state,
// emits reply frames via replyFrame(), and emits unsolicited transceive
// broadcasts when state changes so the client UI tracks our virtual rig.
class civEmulator : public QObject
{
    Q_OBJECT

public:
    // radioCiv  — CI-V address we answer as (e.g. 0x98 for IC-7610).
    explicit civEmulator(quint8 radioCiv, QObject* parent = nullptr);

    void setName(const QString& n) { name = n; }

    bool isTransmitting() const { return ptt; }
    quint64 frequency() const { return freq; }

    // Drive the synthesized S-meter from an RX audio peak (0..32767).
    void setSMeterFromPeak(quint16 peak);
    // Drive synthesized TX meters (Po/ALC) from a mic audio peak (0..32767).
    void setTxMeterFromPeak(quint16 peak);

public slots:
    // Full CI-V frame as received from the client, including 0xFE 0xFE
    // preamble and 0xFD trailer.
    void onCivFromClient(const QByteArray& frame);

signals:
    // Full CI-V frame to send back to the client, ready for
    // icomServer::dataForServer().
    void replyFrame(QByteArray frame);

    // Emitted on PTT edges so the virtualRig can gate mixer input.
    void pttChanged(bool on);

private:
    // State.
    QString name;
    quint8 radioCiv;
    quint8 ctlCiv = 0xE0; // last-seen controller CIV (updated on each inbound)
    quint64 freq = 14074000ULL; // 20 m FT8 default
    quint8 mode = 0x01;         // USB
    quint8 filter = 0x01;       // FIL1
    quint8 dataMode = 0x00;     // no DATA
    bool ptt = false;
    quint16 gains[16] = {0};    // indexed by 0x14 subcommand (full byte)
    quint16 sMeter = 0;         // 0 = silent (S0). Updated later from mixer audio peak.
    quint16 poMeter = 0;        // 0x15 0x11 Power Meter (0..255 BCD).
    quint16 swrMeter = 0;       // 0x15 0x12 SWR Meter.
    quint16 alcMeter = 0;       // 0x15 0x13 ALC Meter.

    // Helpers.
    QByteArray buildFrame(quint8 to, const QByteArray& payload) const;
    QByteArray ack(bool ok = true) const;
    QByteArray freqToBcd5(quint64 hz) const;
    quint64 bcd5ToFreq(const QByteArray& bcd) const;
    QByteArray u16ToBcd2(quint16 v) const;
    quint16 bcd2ToU16(const QByteArray& b) const;

    // Emit a broadcast (to=0x00) transceive update for a cmd/payload.
    void emitTransceive(const QByteArray& payload);
};

#endif
