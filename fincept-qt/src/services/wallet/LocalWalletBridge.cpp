#include "services/wallet/LocalWalletBridge.h"

#include "core/security/ConstantTime.h"
#include "core/security/LoopbackGuard.h"
#include "core/logging/Logger.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimerEvent>
#include <QUrl>
#include <QUrlQuery>

namespace fincept::wallet {

namespace {

QByteArray random_token_hex(int n_bytes) {
    QByteArray buf;
    buf.resize(n_bytes);
    QRandomGenerator::system()->generate(reinterpret_cast<quint32*>(buf.data()),
                                         reinterpret_cast<quint32*>(buf.data() + n_bytes));
    return buf.toHex();
}

QByteArray load_connect_html() {
    // Resources are copied beside the executable at build time:
    //   $<TARGET_FILE_DIR:FinceptTerminal>/resources/wallet/connect.html
    const QString path = QCoreApplication::applicationDirPath() + QStringLiteral("/resources/wallet/connect.html");
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        LOG_WARN("WalletBridge", "connect.html not found at " + path);
        return {};
    }
    return f.readAll();
}

bool path_starts_with(const QByteArray& path, const char* prefix) {
    return path.startsWith(QByteArray::fromRawData(prefix, qstrlen(prefix)));
}

QByteArray query_param(const QByteArray& path, const char* key) {
    const auto idx = path.indexOf('?');
    if (idx < 0) {
        return {};
    }
    QUrlQuery q(QString::fromLatin1(path.mid(idx + 1)));
    return q.queryItemValue(QString::fromLatin1(key)).toLatin1();
}

} // namespace

LocalWalletBridge::LocalWalletBridge(QObject* parent) : QObject(parent) {}

LocalWalletBridge::~LocalWalletBridge() {
    stop();
}

QString LocalWalletBridge::start(const QByteArray& nonce_hex, int timeout_seconds) {
    stop();
    nonce_hex_ = nonce_hex;
    connect_token_ = random_token_hex(16);

    server_ = new QTcpServer(this);
    connect(server_, &QTcpServer::newConnection, this, &LocalWalletBridge::on_new_connection);

    if (!server_->listen(QHostAddress::LocalHost, 0)) {
        const QString err = server_->errorString();
        LOG_ERROR("WalletBridge", "listen() failed: " + err);
        emit bridge_error(err);
        stop();
        return {};
    }
    const quint16 port = server_->serverPort();
    LOG_INFO("WalletBridge", QStringLiteral("listening on 127.0.0.1:%1 (timeout %2s)").arg(port).arg(timeout_seconds));

    if (timer_id_ != 0) {
        killTimer(timer_id_);
    }
    timer_id_ = startTimer(timeout_seconds * 1000);

    return QStringLiteral("http://127.0.0.1:%1/connect?token=%2").arg(port).arg(QString::fromLatin1(connect_token_));
}

void LocalWalletBridge::stop() {
    if (timer_id_ != 0) {
        killTimer(timer_id_);
        timer_id_ = 0;
    }
    if (server_) {
        server_->close();
        server_->deleteLater();
        server_ = nullptr;
    }
    connect_token_.clear();
    nonce_hex_.clear();
}

bool LocalWalletBridge::is_listening() const noexcept {
    return server_ != nullptr && server_->isListening();
}

quint16 LocalWalletBridge::own_port() const noexcept {
    return server_ ? server_->serverPort() : 0;
}

QByteArray LocalWalletBridge::own_origin() const {
    const quint16 p = own_port();
    if (p == 0)
        return {};
    return "http://127.0.0.1:" + QByteArray::number(p);
}

void LocalWalletBridge::timerEvent(QTimerEvent* e) {
    if (e->timerId() == timer_id_) {
        LOG_WARN("WalletBridge", "timed out before callback");
        emit timed_out();
        stop();
    }
}

void LocalWalletBridge::on_new_connection() {
    if (!server_) {
        return;
    }
    while (auto* socket = server_->nextPendingConnection()) {
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            // Read full request: headers terminated by \r\n\r\n, body length from
            // Content-Length. Solana wallet POSTs are small (< 4KB) so a 32KB
            // cap is plenty and protects against runaway uploads.
            static constexpr int kMaxRequestBytes = 32 * 1024;
            const QByteArray buf = socket->peek(kMaxRequestBytes);
            const int header_end = buf.indexOf("\r\n\r\n");
            if (header_end < 0) {
                if (buf.size() >= kMaxRequestBytes) {
                    write_response(socket, 413, "text/plain", "request too large");
                    socket->disconnectFromHost();
                }
                return; // wait for more bytes
            }
            const QByteArray header_block = buf.left(header_end);
            int content_length = 0;
            for (const auto& line : header_block.split('\n')) {
                const auto trimmed = line.trimmed();
                if (trimmed.toLower().startsWith("content-length:")) {
                    content_length = trimmed.mid(15).trimmed().toInt();
                    break;
                }
            }
            const int total_required = header_end + 4 + content_length;
            if (buf.size() < total_required) {
                return; // wait for body
            }
            socket->read(total_required); // drain peek window from buffer

            const auto first_line = header_block.split('\n').value(0).trimmed();
            const auto parts = first_line.split(' ');
            if (parts.size() < 2) {
                write_response(socket, 400, "text/plain", "bad request");
                socket->disconnectFromHost();
                return;
            }
            // Host + fetch-metadata guard. Blocks DNS rebinding and any
            // cross-origin subresource request from another page the user has
            // open; our own served page is same-origin and always passes.
            // See core/security/LoopbackGuard.h for the full threat model.
            const auto guard = fincept::security::check_loopback_request(header_block, own_port());
            if (!guard.allowed) {
                LOG_WARN("WalletBridge", "rejected request: " + guard.reason);
                write_response(socket, 403, "text/plain", "forbidden");
                socket->disconnectFromHost();
                return;
            }

            const QByteArray method = parts.value(0);
            const QByteArray path = parts.value(1);
            const QByteArray body = buf.mid(header_end + 4, content_length);
            handle_request(socket, first_line, body, path, method);
        });
    }
}

void LocalWalletBridge::handle_request(QTcpSocket* socket, const QByteArray& /*request_line*/, const QByteArray& body,
                                       const QByteArray& path, const QByteArray& method) {
    LOG_INFO("WalletBridge", QStringLiteral("request %1 %2 (body=%3 bytes)")
                                 .arg(QString::fromLatin1(method))
                                 .arg(QString::fromLatin1(path))
                                 .arg(body.size()));

    // CORS preflight: respond 204 without checking token (token is in the
    // query string of the actual POST, which the browser hasn't yet sent).
    if (method == "OPTIONS") {
        write_response(socket, 204, "text/plain", QByteArray());
        socket->disconnectFromHost();
        return;
    }

    // Browser auto-requests /favicon.ico without our token; respond cleanly
    // so it doesn't pollute logs with token-mismatch warnings.
    if (method == "GET" && path == "/favicon.ico") {
        write_response(socket, 204, "image/x-icon", QByteArray());
        socket->disconnectFromHost();
        return;
    }

    // Constant-time compare — `!=` on QByteArray short-circuits at the first
    // differing byte, and the timing difference leaks the token byte by byte to
    // anything that can hammer this port.
    const auto token_in_query = query_param(path, "token");
    if (!fincept::security::constant_time_equals(token_in_query, connect_token_)) {
        LOG_WARN("WalletBridge", QStringLiteral("token mismatch on %1 (got %2 chars)")
                                     .arg(QString::fromLatin1(path))
                                     .arg(token_in_query.size()));
        write_response(socket, 403, "text/plain", "forbidden");
        socket->disconnectFromHost();
        return;
    }

    if (method == "GET" && path_starts_with(path, "/connect")) {
        const auto html = render_connect_page();
        LOG_INFO("WalletBridge", QStringLiteral("served connect.html (%1 bytes)").arg(html.size()));
        write_response(socket, 200, "text/html; charset=utf-8", html);
        socket->disconnectFromHost();
        return;
    }

    if (method == "POST" && path_starts_with(path, "/log")) {
        // Browser-side diagnostics — JS can POST {"level":"info","msg":"..."} here.
        QJsonParseError pe;
        const auto doc = QJsonDocument::fromJson(body, &pe);
        if (pe.error == QJsonParseError::NoError && doc.isObject()) {
            const auto obj = doc.object();
            const auto level = obj.value(QStringLiteral("level")).toString();
            const auto msg = obj.value(QStringLiteral("msg")).toString();
            if (level == QStringLiteral("error")) {
                LOG_ERROR("WalletPage", msg);
            } else if (level == QStringLiteral("warn")) {
                LOG_WARN("WalletPage", msg);
            } else {
                LOG_INFO("WalletPage", msg);
            }
        }
        write_response(socket, 200, "application/json", QByteArrayLiteral("{\"ok\":true}"));
        socket->disconnectFromHost();
        return;
    }

    if (method == "POST" && path_starts_with(path, "/callback")) {
        QJsonParseError pe;
        const auto doc = QJsonDocument::fromJson(body, &pe);
        if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
            write_response(socket, 400, "text/plain", "invalid json");
            socket->disconnectFromHost();
            return;
        }
        const auto obj = doc.object();
        const auto pubkey = obj.value(QStringLiteral("pubkey")).toString();
        const auto sig = obj.value(QStringLiteral("signature")).toString();
        const auto label = obj.value(QStringLiteral("label")).toString();
        if (pubkey.isEmpty() || sig.isEmpty()) {
            write_response(socket, 400, "text/plain", "missing fields");
            socket->disconnectFromHost();
            return;
        }
        LOG_INFO("WalletBridge", QStringLiteral("callback received: pubkey=%1 sig_len=%2 label=%3")
                                     .arg(pubkey.left(8) + QStringLiteral("…"))
                                     .arg(sig.size())
                                     .arg(label));
        write_response(socket, 200, "application/json", QByteArrayLiteral("{\"ok\":true}"));
        socket->disconnectFromHost();

        emit connect_payload(pubkey, sig, label.isEmpty() ? QStringLiteral("wallet") : label);
        // The dialog will call stop() once it has consumed the payload.
        return;
    }

    write_response(socket, 404, "text/plain", "not found");
    socket->disconnectFromHost();
}

QByteArray LocalWalletBridge::render_connect_page() const {
    QByteArray html = load_connect_html();
    if (html.isEmpty()) {
        // Fall back to a minimal inline page if the resource isn't bundled.
        html = "<!doctype html><html><body><h2>connect.html missing in resources</h2>"
               "</body></html>";
    }
    html.replace("__NONCE_HEX__", nonce_hex_);
    html.replace("__CALLBACK_TOKEN__", connect_token_);
    return html;
}

void LocalWalletBridge::write_response(QTcpSocket* socket, int status, const QByteArray& content_type,
                                       const QByteArray& body) {
    QByteArray status_text;
    switch (status) {
        case 200:
            status_text = "OK";
            break;
        case 400:
            status_text = "Bad Request";
            break;
        case 403:
            status_text = "Forbidden";
            break;
        case 404:
            status_text = "Not Found";
            break;
        case 413:
            status_text = "Payload Too Large";
            break;
        default:
            status_text = "Error";
            break;
    }
    QByteArray response;
    response.append("HTTP/1.1 " + QByteArray::number(status) + " " + status_text + "\r\n");
    response.append("Content-Type: " + content_type + "\r\n");
    response.append("Content-Length: " + QByteArray::number(body.size()) + "\r\n");
    response.append("Connection: close\r\n");
    response.append("Cache-Control: no-store\r\n");
    response.append("X-Content-Type-Options: nosniff\r\n");
    response.append("Referrer-Policy: no-referrer\r\n");
    // Same-origin in practice (page + fetch both served by us), but some
    // browsers (and some Phantom builds) trip on missing CORS for any
    // localhost POST. Send the page's OWN origin — the comment here used to
    // claim that while the code actually sent `*`, which invites every origin
    // on the web to read this bridge's responses.
    const QByteArray origin = own_origin();
    if (!origin.isEmpty()) {
        response.append("Access-Control-Allow-Origin: " + origin + "\r\n");
        response.append("Vary: Origin\r\n");
        response.append("Access-Control-Allow-Headers: Content-Type\r\n");
        response.append("Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n");
    }
    response.append("\r\n");
    response.append(body);
    socket->write(response);
    socket->flush();
}

} // namespace fincept::wallet
