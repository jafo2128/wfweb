#include "tlsproxy.h"

#ifdef Q_OS_MACOS

#include <QDebug>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <thread>

TlsProxyWorker::TlsProxyWorker(quint16 listenPort, quint16 backendPort,
                               const QString &certPath, const QString &keyPath,
                               QObject *parent)
    : QObject(parent)
    , m_listenPort(listenPort)
    , m_backendPort(backendPort)
    , m_certPath(certPath)
    , m_keyPath(keyPath)
{
}

TlsProxyWorker::~TlsProxyWorker()
{
    stop();
}

void TlsProxyWorker::start()
{
    // Create SSL context
    m_ctx = SSL_CTX_new(TLS_server_method());
    if (!m_ctx) {
        emit error("Failed to create SSL_CTX");
        return;
    }

    // Do NOT request client certificates — this is the whole point
    SSL_CTX_set_verify(m_ctx, SSL_VERIFY_NONE, nullptr);

    if (SSL_CTX_use_certificate_file(m_ctx, m_certPath.toLocal8Bit().constData(), SSL_FILETYPE_PEM) != 1) {
        emit error("Failed to load certificate: " + m_certPath);
        SSL_CTX_free(m_ctx); m_ctx = nullptr;
        return;
    }
    if (SSL_CTX_use_PrivateKey_file(m_ctx, m_keyPath.toLocal8Bit().constData(), SSL_FILETYPE_PEM) != 1) {
        emit error("Failed to load private key: " + m_keyPath);
        SSL_CTX_free(m_ctx); m_ctx = nullptr;
        return;
    }

    // Create listening socket
    m_listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_listenFd < 0) {
        emit error("Failed to create socket");
        SSL_CTX_free(m_ctx); m_ctx = nullptr;
        return;
    }

    int optval = 1;
    setsockopt(m_listenFd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(m_listenPort);

    if (bind(m_listenFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        emit error(QString("Failed to bind to port %1").arg(m_listenPort));
        close(m_listenFd); m_listenFd = -1;
        SSL_CTX_free(m_ctx); m_ctx = nullptr;
        return;
    }

    if (listen(m_listenFd, 16) < 0) {
        emit error("Failed to listen");
        close(m_listenFd); m_listenFd = -1;
        SSL_CTX_free(m_ctx); m_ctx = nullptr;
        return;
    }

    m_running = true;
    qInfo() << "Web: OpenSSL TLS proxy listening on port" << m_listenPort
            << "-> localhost:" << m_backendPort;
    emit listening();

    acceptLoop();
}

void TlsProxyWorker::stop()
{
    m_running = false;
    if (m_listenFd >= 0) {
        ::shutdown(m_listenFd, SHUT_RDWR);
        close(m_listenFd);
        m_listenFd = -1;
    }
    if (m_ctx) {
        SSL_CTX_free(m_ctx);
        m_ctx = nullptr;
    }
}

void TlsProxyWorker::acceptLoop()
{
    while (m_running) {
        struct pollfd pfd{};
        pfd.fd = m_listenFd;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, 500);
        if (ret <= 0) continue;
        if (!m_running) break;

        struct sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd = accept(m_listenFd, reinterpret_cast<struct sockaddr*>(&clientAddr), &clientLen);
        if (clientFd < 0) continue;

        // Spawn a detached thread per connection
        SSL_CTX *ctx = m_ctx;
        quint16 backendPort = m_backendPort;
        std::thread([clientFd, ctx, backendPort]() {
            TlsProxyWorker::proxyConnection(clientFd, ctx, backendPort);
        }).detach();
    }
}

void TlsProxyWorker::proxyConnection(int clientFd, SSL_CTX *ctx, quint16 backendPort)
{
    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        close(clientFd);
        return;
    }
    SSL_set_fd(ssl, clientFd);

    if (SSL_accept(ssl) != 1) {
        SSL_free(ssl);
        close(clientFd);
        return;
    }

    // Connect to the internal plain HTTP server
    int backendFd = socket(AF_INET, SOCK_STREAM, 0);
    if (backendFd < 0) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(clientFd);
        return;
    }

    struct sockaddr_in backendAddr{};
    backendAddr.sin_family = AF_INET;
    backendAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    backendAddr.sin_port = htons(backendPort);

    if (::connect(backendFd, reinterpret_cast<struct sockaddr*>(&backendAddr), sizeof(backendAddr)) < 0) {
        close(backendFd);
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(clientFd);
        return;
    }

    // Bidirectional forwarding
    char buf[16384];
    struct pollfd fds[2];
    fds[0].fd = clientFd;   // TLS side
    fds[0].events = POLLIN;
    fds[1].fd = backendFd;  // plain TCP side
    fds[1].events = POLLIN;

    // SSL may have buffered data after handshake — check before first poll
    bool running = true;
    while (running) {
        // If OpenSSL has buffered decrypted data, don't wait in poll
        int timeout = SSL_pending(ssl) > 0 ? 0 : 30000;
        int ret = poll(fds, 2, timeout);
        if (ret < 0) break;

        // TLS client -> backend
        if ((fds[0].revents & POLLIN) || SSL_pending(ssl) > 0) {
            int n = SSL_read(ssl, buf, sizeof(buf));
            if (n <= 0) { running = false; break; }
            int sent = 0;
            while (sent < n) {
                int w = send(backendFd, buf + sent, n - sent, 0);
                if (w <= 0) { running = false; break; }
                sent += w;
            }
        }
        if (fds[0].revents & (POLLHUP | POLLERR)) { running = false; break; }

        // Backend -> TLS client
        if (fds[1].revents & POLLIN) {
            int n = recv(backendFd, buf, sizeof(buf), 0);
            if (n <= 0) { running = false; break; }
            int sent = 0;
            while (sent < n) {
                int w = SSL_write(ssl, buf + sent, n - sent);
                if (w <= 0) { running = false; break; }
                sent += w;
            }
        }
        if (fds[1].revents & (POLLHUP | POLLERR)) { running = false; break; }
    }

    // Cleanup
    close(backendFd);
    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(clientFd);
}

#endif // Q_OS_MACOS
