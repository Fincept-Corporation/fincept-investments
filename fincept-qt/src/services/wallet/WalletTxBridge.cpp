#include "services/wallet/WalletTxBridge.h"

#include "core/security/ConstantTime.h"
#include "core/security/LoopbackGuard.h"
#include "core/logging/Logger.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimerEvent>
#include <QUrlQuery>
#include <QUuid>

namespace fincept::wallet {

namespace {

constexpr int kSweepIntervalMs = 5 * 1000; // purge expired requests every 5s
constexpr int kMaxRequestBytes = 32 * 1024;

QByteArray tx_random_token_hex(int n_bytes) {
    QByteArray buf;
    buf.resize(n_bytes);
    QRandomGenerator::system()->generate(reinterpret_cast<quint32*>(buf.data()),
                                         reinterpret_cast<quint32*>(buf.data() + n_bytes));
    return buf.toHex();
}

QString new_request_id() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool tx_path_starts_with(const QByteArray& path, const char* prefix) {
    return path.startsWith(QByteArray::fromRawData(prefix, qstrlen(prefix)));
}

QByteArray tx_query_param(const QByteArray& path, const char* key) {
    const auto idx = path.indexOf('?');
    if (idx < 0) {
        return {};
    }
    QUrlQuery q(QString::fromLatin1(path.mid(idx + 1)));
    return q.queryItemValue(QString::fromLatin1(key)).toLatin1();
}

QByteArray load_swap_html() {
    const QString path = QCoreApplication::applicationDirPath() + QStringLiteral("/resources/wallet/swap.html");
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        LOG_WARN("WalletTxBridge", "swap.html not found at " + path);
        return {};
    }
    return f.readAll();
}

} // namespace

WalletTxBridge::WalletTxBridge(QObject* parent) : QObject(parent) {}

WalletTxBridge::~WalletTxBridge() {
    // Resolve any outstanding requests as cancelled so callers don't dangle.
    for (auto it = requests_.constBegin(); it != requests_.constEnd(); ++it) {
        if (it.value().cb) {
            it.value().cb(Result<QString>::err("bridge_destroyed"));
        }
    }
    requests_.clear();
    stop_server();
}

Result<QString> WalletTxBridge::request_signature(const QString& tx_base64, SignatureCallback cb, int timeout_seconds) {
    if (tx_base64.isEmpty()) {
        return Result<QString>::err("empty_tx");
    }
    if (!ensure_server_started()) {
        return Result<QString>::err("bridge_listen_failed");
    }
    purge_expired_requests();

    PendingRequest pr;
    pr.tx_base64 = tx_base64;
    pr.cb = std::move(cb);
    pr.created_at_ms = QDateTime::currentMSecsSinceEpoch();
    pr.timeout_seconds = timeout_seconds > 0 ? timeout_seconds : 60;

    const QString req_id = new_request_id();
    requests_.insert(req_id, std::move(pr));

    const quint16 port = server_->serverPort();
    const QString url = QStringLiteral("http://127.0.0.1:%1/swap?req=%2&token=%3")
                            .arg(port)
                            .arg(req_id, QString::fromLatin1(session_token_));
    LOG_INFO("WalletTxBridge",
             QStringLiteral("registered sign request %1 (expires %2s)").arg(req_id).arg(pr.timeout_seconds));
    return Result<QString>::ok(url);
}

void WalletTxBridge::cancel(const QString& request_id) {
    if (!requests_.contains(request_id)) {
        return;
    }
    LOG_INFO("WalletTxBridge", "cancel request " + request_id);
    resolve_request(request_id, Result<QString>::err("user_cancelled"));
}

bool WalletTxBridge::is_listening() const noexcept {
    return server_ != nullptr && server_->isListening();
}

bool WalletTxBridge::ensure_server_started() {
    if (server_ && server_->isListening()) {
        return true;
    }
    session_token_ = tx_random_token_hex(16);
    server_ = new QTcpServer(this);
    connect(server_, &QTcpServer::newConnection, this, &WalletTxBridge::on_new_connection);

    if (!server_->listen(QHostAddress::LocalHost, 0)) {
        const QString err = server_->errorString();
        LOG_ERROR("WalletTxBridge", "listen() failed: " + err);
        emit bridge_error(err);
        stop_server();
        return false;
    }
    if (sweep_timer_id_ == 0) {
        sweep_timer_id_ = startTimer(kSweepIntervalMs);
    }
    LOG_INFO("WalletTxBridge", QStringLiteral("listening on 127.0.0.1:%1").arg(server_->serverPort()));
    return true;
}

void WalletTxBridge::stop_server() {
    if (sweep_timer_id_ != 0) {
        killTimer(sweep_timer_id_);
        sweep_timer_id_ = 0;
    }
    if (server_) {
        server_->close();
        server_->deleteLater();
        server_ = nullptr;
    }
    session_token_.clear();
}

void WalletTxBridge::purge_expired_requests() {
    const auto now = QDateTime::currentMSecsSinceEpoch();
    QStringList expired;
    for (auto it = requests_.constBegin(); it != requests_.constEnd(); ++it) {
        const auto& pr = it.value();
        const auto age_ms = now - pr.created_at_ms;
        if (age_ms > qint64(pr.timeout_seconds) * 1000) {
            expired.append(it.key());
        }
    }
    for (const auto& id : expired) {
        LOG_WARN("WalletTxBridge", "expired request " + id);
        resolve_request(id, Result<QString>::err("timeout"));
    }
}

void WalletTxBridge::resolve_request(const QString& request_id, Result<QString> result) {
    auto it = requests_.find(request_id);
    if (it == requests_.end()) {
        return;
    }
    auto cb = std::move(it.value().cb);
    requests_.erase(it);
    if (requests_.isEmpty()) {
        stop_server();
    }
    if (cb) {
        cb(std::move(result));
    }
}

void WalletTxBridge::timerEvent(QTimerEvent* e) {
    if (e->timerId() == sweep_timer_id_) {
        purge_expired_requests();
    }
}

void WalletTxBridge::on_new_connection() {
    if (!server_) {
        return;
    }
    while (auto* socket = server_->nextPendingConnection()) {
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            const QByteArray buf = socket->peek(kMaxRequestBytes);
            const int header_end = buf.indexOf("\r\n\r\n");
            if (header_end < 0) {
                if (buf.size() >= kMaxRequestBytes) {
                    write_response(socket, 413, "text/plain", "request too large");
                    socket->disconnectFromHost();
                }
                return;
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
                return;
            }
            socket->read(total_required);

            const auto first_line = header_block.split('\n').value(0).trimmed();
            const auto parts = first_line.split(' ');
            if (parts.size() < 2) {
                write_response(socket, 400, "text/plain", "bad request");
                socket->disconnectFromHost();
                return;
            }
            // Host + fetch-metadata guard — blocks DNS rebinding and any
            // cross-origin subresource request. Our own swap page is
            // same-origin and passes. See core/security/LoopbackGuard.h.
            const auto guard =
                fincept::security::check_loopback_request(header_block, server_ ? server_->serverPort() : quint16(0));
            if (!guard.allowed) {
                LOG_WARN("WalletTxBridge", "rejected request: " + guard.reason);
                write_response(socket, 403, "text/plain", "forbidden");
                socket->disconnectFromHost();
                return;
            }

            const QByteArray method = parts.value(0);
            const QByteArray path = parts.value(1);
            const QByteArray body = buf.mid(header_end + 4, content_length);
            handle_request(socket, body, path, method);
        });
    }
}

void WalletTxBridge::handle_request(QTcpSocket* socket, const QByteArray& body, const QByteArray& path,
                                    const QByteArray& method) {
    LOG_INFO("WalletTxBridge", QStringLiteral("request %1 %2 (body=%3)")
                                   .arg(QString::fromLatin1(method), QString::fromLatin1(path))
                                   .arg(body.size()));

    if (method == "OPTIONS") {
        write_response(socket, 204, "text/plain", QByteArray());
        socket->disconnectFromHost();
        return;
    }
    if (method == "GET" && path == "/favicon.ico") {
        write_response(socket, 204, "image/x-icon", QByteArray());
        socket->disconnectFromHost();
        return;
    }

    // Static asset: vendored @solana/web3.js. Served without a per-request
    // token because the <script> tag in swap.html cannot carry one (the
    // page URL has it, the relative script src does not). The asset itself
    // is read-only, embedded in the binary's resource directory, and only
    // reachable from 127.0.0.1 — no privilege boundary is crossed.
    if (method == "GET" && tx_path_starts_with(path, "/vendor/web3.js")) {
        const QString abs_path =
            QCoreApplication::applicationDirPath() + QStringLiteral("/resources/wallet/vendor/web3.js");
        QFile f(abs_path);
        if (!f.open(QIODevice::ReadOnly)) {
            LOG_WARN("WalletTxBridge", "vendored web3.js missing at " + abs_path);
            write_response(socket, 404, "text/plain", "vendor asset missing");
        } else {
            write_response(socket, 200, "application/javascript; charset=utf-8", f.readAll());
        }
        socket->disconnectFromHost();
        return;
    }

    // Constant-time compare — a short-circuiting `!=` leaks the token byte by
    // byte through response timing to anything that can hammer this port.
    const auto token_in_query = tx_query_param(path, "token");
    if (!fincept::security::constant_time_equals(token_in_query, session_token_)) {
        LOG_WARN("WalletTxBridge", "token mismatch — rejecting");
        write_response(socket, 403, "text/plain", "forbidden");
        socket->disconnectFromHost();
        return;
    }
    const auto req_id = QString::fromLatin1(tx_query_param(path, "req"));
    if (req_id.isEmpty()) {
        write_response(socket, 400, "text/plain", "missing req");
        socket->disconnectFromHost();
        return;
    }
    if (!requests_.contains(req_id)) {
        // Either expired or unknown. Either way, single-use and gone.
        LOG_WARN("WalletTxBridge", "unknown / expired request " + req_id);
        write_response(socket, 410, "text/plain", "gone");
        socket->disconnectFromHost();
        return;
    }

    if (method == "GET" && tx_path_starts_with(path, "/swap")) {
        const auto html = render_swap_page();
        write_response(socket, 200, "text/html; charset=utf-8", html);
        socket->disconnectFromHost();
        return;
    }

    if (method == "GET" && tx_path_starts_with(path, "/tx")) {
        const auto& pr = requests_.value(req_id);
        QJsonObject out;
        out[QStringLiteral("tx_base64")] = pr.tx_base64;
        write_response(socket, 200, "application/json", QJsonDocument(out).toJson(QJsonDocument::Compact));
        socket->disconnectFromHost();
        return;
    }

    if (method == "POST" && tx_path_starts_with(path, "/result")) {
        QJsonParseError pe;
        const auto doc = QJsonDocument::fromJson(body, &pe);
        if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
            write_response(socket, 400, "text/plain", "bad json");
            socket->disconnectFromHost();
            return;
        }
        const auto obj = doc.object();
        const auto err_str = obj.value(QStringLiteral("error")).toString();
        if (!err_str.isEmpty()) {
            resolve_request(req_id, Result<QString>::err(err_str.toStdString()));
        } else {
            const auto sig = obj.value(QStringLiteral("signature")).toString();
            if (sig.isEmpty()) {
                resolve_request(req_id, Result<QString>::err("empty signature"));
            } else {
                resolve_request(req_id, Result<QString>::ok(sig));
            }
        }
        write_response(socket, 200, "application/json", QByteArrayLiteral("{\"ok\":true}"));
        socket->disconnectFromHost();
        return;
    }

    write_response(socket, 404, "text/plain", "not found");
    socket->disconnectFromHost();
}

QByteArray WalletTxBridge::render_swap_page() const {
    auto html = load_swap_html();
    if (html.isEmpty()) {
        // Defensive: until Stage 2.2 ships swap.html, return a clear stub.
        // Won't be reached in normal builds — CMakeLists copies the page.
        return QByteArrayLiteral("<!doctype html><meta charset='utf-8'><title>Sign</title>"
                                 "<body style='background:#0a0a0a;color:#d97706;font-family:monospace;"
                                 "padding:20px'>swap.html missing — Stage 2.2 work-in-progress</body>");
    }
    return html;
}

void WalletTxBridge::write_response(QTcpSocket* socket, int status, const QByteArray& content_type,
                                    const QByteArray& body) {
    QByteArray reason;
    switch (status) {
        case 200:
            reason = "OK";
            break;
        case 204:
            reason = "No Content";
            break;
        case 400:
            reason = "Bad Request";
            break;
        case 403:
            reason = "Forbidden";
            break;
        case 404:
            reason = "Not Found";
            break;
        case 410:
            reason = "Gone";
            break;
        case 413:
            reason = "Payload Too Large";
            break;
        default:
            reason = "Error";
            break;
    }
    QByteArray header;
    header += "HTTP/1.1 " + QByteArray::number(status) + " " + reason + "\r\n";
    header += "Content-Type: " + content_type + "\r\n";
    header += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    header += "Cache-Control: no-store\r\n";
    header += "Connection: close\r\n";
    header += "\r\n";
    socket->write(header);
    socket->write(body);
    socket->flush();
}

} // namespace fincept::wallet
