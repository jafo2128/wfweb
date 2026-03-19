#ifndef TLSPROXY_H
#define TLSPROXY_H

#include <QtGlobal>

#ifdef Q_OS_MACOS

#include <QObject>
#include <QThread>
#include <atomic>
#include <vector>

// Forward declarations — avoid exposing OpenSSL headers
typedef struct ssl_ctx_st SSL_CTX;

class TlsProxyWorker : public QObject {
    Q_OBJECT
public:
    TlsProxyWorker(quint16 listenPort, quint16 backendPort,
                   const QString &certPath, const QString &keyPath,
                   QObject *parent = nullptr);
    ~TlsProxyWorker();

public slots:
    void start();
    void stop();

signals:
    void listening();
    void error(QString message);

private:
    void acceptLoop();
    static void proxyConnection(int clientFd, SSL_CTX *ctx, quint16 backendPort);

    quint16 m_listenPort;
    quint16 m_backendPort;
    QString m_certPath;
    QString m_keyPath;
    SSL_CTX *m_ctx = nullptr;
    int m_listenFd = -1;
    std::atomic<bool> m_running{false};
};

#endif // Q_OS_MACOS
#endif // TLSPROXY_H
