#include "virtualrig.h"
#include "channelmixer.h"
#include "civemulator.h"

#include "icomserver.h"
#include "rigcommander.h"
#include "wfwebtypes.h"

#include <QDebug>
#include <QCryptographicHash>
#include <QDateTime>

virtualRig::virtualRig(const Config& cfg, channelMixer* mixer, QObject* parent)
    : QObject(parent), cfg(cfg), mixer(mixer)
{
    // --- SERVERCONFIG: single-rig, LAN-mode. ---
    serverCfg.enabled = true;
    serverCfg.disableUI = true;
    serverCfg.lan = true;
    serverCfg.controlPort = cfg.controlPort;
    serverCfg.civPort = cfg.civPort;
    serverCfg.audioPort = cfg.audioPort;
    serverCfg.audioOutput = 0;
    serverCfg.audioInput = 0;
    serverCfg.resampleQuality = 0;
    serverCfg.baudRate = 115200;

    // Fixed credentials so the client's login is accepted. icomServer rejects
    // any user config with empty username/password (icomserver.cpp:307), so we
    // can't be fully permissive — the orchestrator just has to send the same.
    SERVERUSER u;
    u.username = "wfweb";
    u.password = "wfweb";
    u.userType = 0;
    serverCfg.users.append(u);

    // --- RIGCONFIG: stable per-rig identity derived from the name. ---
    rigCfg.serialPort = "";
    rigCfg.baudRate = 115200;
    rigCfg.civAddr = cfg.civAddr;
    rigCfg.civIsRadioModel = true;
    rigCfg.hasWiFi = false;
    rigCfg.hasEthernet = true;
    rigCfg.pttType = pttCIV;
    rigCfg.modelName = "Virtual IC-7300";
    rigCfg.rigName = cfg.name;
    rigCfg.rigAvailable = true;
    rigCfg.rig = nullptr;      // ← the gap that the icomserver null-guards handle
    rigCfg.rigThread = nullptr;
    rigCfg.rxaudio = nullptr;
    rigCfg.txaudio = nullptr;
    rigCfg.queueInterval = 1000;

    // Deterministic GUID from the rig name so clients remember us.
    QByteArray h = QCryptographicHash::hash(cfg.name.toUtf8(), QCryptographicHash::Md5);
    memcpy(rigCfg.guid, h.constData(), GUIDLEN);

    serverCfg.rigs.append(&rigCfg);

    // --- Synthetic rigCapabilities (minimum fields actually read) ---
    caps.model = cfg.civAddr;
    caps.modelID = cfg.civAddr;
    caps.manufacturer = manufIcom;
    caps.modelName = rigCfg.modelName;
    caps.hasLan = true;
    caps.hasEthernet = true;
    caps.hasTransmit = true;
    caps.hasPTTCommand = true;
    caps.baudRate = 115200;
    memcpy(caps.guid, rigCfg.guid, GUIDLEN);

    civ = new civEmulator(cfg.civAddr, this);
    civ->setName(cfg.name);
}

virtualRig::~virtualRig()
{
    stop();
}

void virtualRig::start()
{
    if (server != nullptr) return;

    serverThread = new QThread(this);
    serverThread->setObjectName(QString("icomServer-%1").arg(cfg.name));

    server = new icomServer(&serverCfg);
    server->moveToThread(serverThread);

    // Kick init() once the thread starts.
    QObject::connect(serverThread, &QThread::started, server, &rigServer::init);
    QObject::connect(serverThread, &QThread::finished, server, &QObject::deleteLater);

    // CI-V: client → emulator → back to client.
    QObject::connect(server, &rigServer::haveDataFromServer,
                     civ, &civEmulator::onCivFromClient,
                     Qt::QueuedConnection);
    QObject::connect(civ, &civEmulator::replyFrame,
                     server, &rigServer::dataForServer,
                     Qt::QueuedConnection);
    QObject::connect(civ, &civEmulator::pttChanged,
                     this, &virtualRig::onPttChanged);

    // Audio: TX from client → our gate → mixer; RX from mixer → server.
    QObject::connect(server, &rigServer::haveAudioData,
                     this, &virtualRig::onTxAudioFromClient,
                     Qt::QueuedConnection);
    QObject::connect(mixer, &channelMixer::rxAudioForRig,
                     this, &virtualRig::onRxAudioFromMixer,
                     Qt::QueuedConnection);

    serverThread->start();

    // Deliver synthetic caps to the server once it's up. Queued so it runs
    // after init() on the server thread.
    QMetaObject::invokeMethod(server, [this]() {
        server->receiveRigCaps(&caps);
    }, Qt::QueuedConnection);

    // Continuous silence on RX. The wfweb client's icomUdpAudio watchdog
    // kills the TX audio handler if it sees no RX for 30 s, so real silence
    // (not absence of packets) is what keeps TX alive when nobody is keyed.
    idleRxTimer = new QTimer(this);
    idleRxTimer->setInterval(20); // 20 ms cadence
    connect(idleRxTimer, &QTimer::timeout, this, &virtualRig::emitIdleRx);
    idleRxTimer->start();

    qInfo() << "virtualRig" << cfg.name << "up on ports"
            << cfg.controlPort << cfg.civPort << cfg.audioPort;
}

void virtualRig::stop()
{
    if (idleRxTimer != nullptr) {
        idleRxTimer->stop();
        idleRxTimer = nullptr; // parented to this, deleted by Qt
    }
    if (serverThread != nullptr) {
        serverThread->quit();
        serverThread->wait(2000);
        serverThread = nullptr;
        server = nullptr; // deleted via finished()
    }
}

void virtualRig::emitIdleRx()
{
    if (server == nullptr || ptt) return;
    // Skip if a real RX packet was dispatched in the last 100 ms; otherwise the
    // silence would step on the voice stream in the client's audio buffer.
    if (QDateTime::currentMSecsSinceEpoch() - lastRealRxMs < 100) return;
    if (civ) civ->setSMeterFromPeak(0);
    // 20 ms of LPCM silence at 48 kHz mono = 960 samples * 2 bytes = 1920 B.
    // Matches the codec negotiated by the client (logs: rx sample rate 48000,
    // codec 4 = LPCM 16-bit uncompressed).
    static const int bytes = 48 * 20 * 2;
    audioPacket pkt;
    pkt.data = QByteArray(bytes, '\0');
    pkt.seq = rxSeq++;
    pkt.time = QTime::currentTime();
    pkt.sent = 0;
    pkt.amplitudePeak = 0;
    pkt.amplitudeRMS = 0;
    pkt.volume = 0;
    QMetaObject::invokeMethod(server, [this, pkt]() {
        server->receiveAudioData(pkt);
    }, Qt::QueuedConnection);
}

void virtualRig::onPttChanged(bool on)
{
    qInfo() << "virtualRig" << cfg.name << "PTT" << (on ? "ON" : "OFF");
    ptt = on;
}

void virtualRig::onTxAudioFromClient(const audioPacket& pkt)
{
    // Always compute peak so the TX meter can reflect recent mic level whenever
    // PTT transitions on, even for the first packet.
    const qint16* s = reinterpret_cast<const qint16*>(pkt.data.constData());
    int n = pkt.data.size() / 2;
    qint16 peak = 0;
    for (int i = 0; i < n; ++i) {
        qint16 v = s[i] < 0 ? (qint16)-s[i] : s[i];
        if (v > peak) peak = v;
    }
    if (civ) civ->setTxMeterFromPeak((quint16)peak);
    if (!ptt) return;
    if (mixer) mixer->pushTxAudio(cfg.index, pkt);
}

void virtualRig::onRxAudioFromMixer(int dstRig, const audioPacket& pkt)
{
    if (dstRig != cfg.index) return;
    static int tick = 0;
    if (++tick % 50 == 1) {
        qInfo() << "virtualRig" << cfg.name << "RX from mixer: bytes=" << pkt.data.size();
    }
    lastRealRxMs = QDateTime::currentMSecsSinceEpoch();
    if (civ) {
        // Drive the synthesized S-meter from the mixer's PCM peak so the
        // receiving UI shows activity while the other side transmits.
        const qint16* s = reinterpret_cast<const qint16*>(pkt.data.constData());
        int n = pkt.data.size() / 2;
        qint16 peak = 0;
        for (int i = 0; i < n; ++i) {
            qint16 v = s[i] < 0 ? (qint16)-s[i] : s[i];
            if (v > peak) peak = v;
        }
        civ->setSMeterFromPeak((quint16)peak);
    }
    if (server) {
        audioPacket copy = pkt;
        QMetaObject::invokeMethod(server, [this, copy]() {
            server->receiveAudioData(copy);
        }, Qt::QueuedConnection);
    }
}
