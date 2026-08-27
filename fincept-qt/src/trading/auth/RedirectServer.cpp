#include "trading/auth/RedirectServer.h"

#include "core/security/ConstantTime.h"
#include "core/security/LoopbackGuard.h"
#include "core/logging/Logger.h"

#include <QHostAddress>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

#include <memory>

namespace fincept::trading::auth {

namespace {
constexpr const char* kResponseHtml =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<title>Fincept - Broker login</title>"
    "<style>body{font-family:system-ui;background:#0f172a;color:#e2e8f0;"
    "display:flex;align-items:center;justify-content:center;height:100vh;margin:0}"
    "div{text-align:center}h1{color:#d97706;font-size:20px}p{color:#94a3b8}</style>"
    "</head><body><div><h1>Login captured</h1>"
    "<p>You can close this tab and return to Fincept Terminal.</p></div></body></html>";

// 128 bits of CSPRNG output, hex-encoded. QRandomGenerator::system() is the
// OS entropy source — the non-system generator is seeded deterministically and
// must never be used for a security nonce.
QString make_state_nonce() {
    quint32 words[4];
    QRandomGenerator::system()->fillRange(words);
    QByteArray raw(reinterpret_cast<const char*>(words), static_cast<qsizetype>(sizeof(words)));
    return QString::fromLatin1(raw.toHex());
}

void write_page(QTcpSocket* sock, int status, const char* status_text, const QByteArray& body) {
    QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + ' ' + status_text + "\r\n";
    response += "Content-Type: text/html; charset=utf-8\r\n";
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    response += "Cache-Control: no-store\r\n";
    response += "X-Content-Type-Options: nosniff\r\n";
    response += "Referrer-Policy: no-referrer\r\n";
    response += "Connection: close\r\n\r\n";
    response += body;
    sock->write(response);
    sock->flush();
}
} // namespace

RedirectServer::RedirectServer(QObject* parent)
    : QObject(parent), server_(new QTcpServer(this)), timeout_timer_(new QTimer(this)) {
    timeout_timer_->setSingleShot(true);
    connect(server_, &QTcpServer::newConnection, this, &RedirectServer::handle_new_connection);
    connect(timeout_timer_, &QTimer::timeout, this, &RedirectServer::handle_timeout);
}

RedirectServer::~RedirectServer() {
    stop();
}

bool RedirectServer::start(quint16 preferred_port, int timeout_seconds) {
    if (server_->isListening())
        server_->close();

    // Loopback only — never QHostAddress::Any. Do not change this.
    if (!server_->listen(QHostAddress::LocalHost, preferred_port)) {
        LOG_WARN("RedirectServer", QString("Port %1 busy - falling back to ephemeral").arg(preferred_port));
        if (!server_->listen(QHostAddress::LocalHost, 0)) {
            LOG_ERROR("RedirectServer", QString("Failed to bind loopback: %1").arg(server_->errorString()));
            return false;
        }
    }
    port_ = server_->serverPort();
    state_ = make_state_nonce(); // fresh per attempt — never reuse across logins
    timeout_timer_->start(timeout_seconds * 1000);
    LOG_INFO("RedirectServer", QString("Listening on 127.0.0.1:%1").arg(port_));
    return true;
}

void RedirectServer::stop() {
    timeout_timer_->stop();
    if (server_->isListening())
        server_->close();
}

void RedirectServer::handle_new_connection() {
    QTcpSocket* sock = server_->nextPendingConnection();
    if (!sock)
        return;
    connect(sock, &QTcpSocket::disconnected, sock, &QTcpSocket::deleteLater);

    // The request can arrive in several TCP segments; accumulate until the
    // header block is complete, because the guards below read headers, not just
    // the request line.
    auto buffer = std::make_shared<QByteArray>();
    connect(sock, &QTcpSocket::readyRead, this, [this, sock, buffer]() {
        buffer->append(sock->readAll());
        if (buffer->indexOf("\r\n\r\n") < 0 && buffer->indexOf("\n\n") < 0) {
            if (buffer->size() < 16384)
                return; // wait for the rest of the head
            write_page(sock, 431, "Request Header Fields Too Large", QByteArrayLiteral("too large"));
            sock->disconnectFromHost();
            return;
        }

        const QByteArray request = *buffer;
        const qsizetype line_end = request.indexOf('\n');
        QByteArray first_line = (line_end >= 0) ? request.left(line_end) : request;
        if (first_line.endsWith('\r'))
            first_line.chop(1);
        const QList<QByteArray> parts = first_line.split(' ');

        // ── Gate 1: only a browser redirect, i.e. a GET ──────────────────────
        if (parts.size() < 2 || parts.at(0) != "GET") {
            LOG_WARN("RedirectServer", "Rejected non-GET request on the OAuth callback port");
            write_page(sock, 405, "Method Not Allowed", QByteArrayLiteral("method not allowed"));
            sock->disconnectFromHost();
            return;
        }

        // ── Gate 2: Host + fetch-metadata (see core/security/LoopbackGuard.h) ─
        // Fully qualified: this file lives in fincept::trading::auth, so an
        // unqualified name would resolve into the wrong namespace.
        const auto check = ::fincept::security::check_loopback_request(request, port_);
        if (!check.allowed) {
            LOG_WARN("RedirectServer", "Rejected OAuth callback: " + check.reason);
            write_page(sock, 403, "Forbidden", QByteArrayLiteral("forbidden"));
            sock->disconnectFromHost();
            return;
        }

        const QUrl url(QStringLiteral("http://127.0.0.1") + QString::fromLatin1(parts.at(1)));
        const QUrlQuery q(url);

        QString token = q.queryItemValue("request_token");
        if (token.isEmpty())
            token = q.queryItemValue("auth_code");

        // ── Gate 3: the state nonce ──────────────────────────────────────────
        // The only gate that also stops a cross-site *top-level* navigation
        // (window.open to the fixed port), which looks identical to the real
        // redirect at the header level.
        if (!token.isEmpty()) {
            const QString echoed_state = q.queryItemValue("state");
            if (echoed_state.isEmpty()) {
                if (require_state_) {
                    LOG_WARN("RedirectServer", "Rejected OAuth callback: no state nonce echoed back");
                    write_page(sock, 403, "Forbidden", QByteArrayLiteral("forbidden"));
                    sock->disconnectFromHost();
                    emit error(QStringLiteral("Login rejected: missing CSRF state"));
                    return;
                }
                LOG_WARN("RedirectServer",
                         "OAuth callback carried no state nonce — the authorize URL is not sending state(). "
                         "Any page the user has open can forge this callback until it does.");
            } else if (!::fincept::security::constant_time_equals(echoed_state, state_)) {
                LOG_WARN("RedirectServer", "Rejected OAuth callback: state nonce mismatch");
                write_page(sock, 403, "Forbidden", QByteArrayLiteral("forbidden"));
                sock->disconnectFromHost();
                emit error(QStringLiteral("Login rejected: CSRF state mismatch"));
                return;
            }
        }

        QString error_msg;
        if (token.isEmpty()) {
            const QString err = q.queryItemValue("error");
            error_msg = err.isEmpty() ? QStringLiteral("Token missing in redirect") : err;
        }

        write_page(sock, 200, "OK", QByteArray(kResponseHtml));
        sock->disconnectFromHost();

        if (!token.isEmpty()) {
            LOG_INFO("RedirectServer", "Captured auth token");
            timeout_timer_->stop();
            server_->close();
            emit request_token_received(token);
        } else {
            // Keep listening: this was a stray request (favicon, a probe, a
            // provider error page) and the real redirect may still be coming.
            // Stopping the timeout here would leave the dialog waiting forever.
            LOG_WARN("RedirectServer", "No token in redirect: " + error_msg);
            emit error(error_msg);
        }
    });
}

void RedirectServer::handle_timeout() {
    LOG_WARN("RedirectServer", "Browser login timed out");
    server_->close();
    emit timeout();
}

} // namespace fincept::trading::auth
