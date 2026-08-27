#include "network/http/HttpClient.h"

#include "core/logging/Logger.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QUrl>

#include <chrono>

namespace fincept {

namespace {
/// URL with the query string removed, for logging.
///
/// Several callers put a live secret in the query because HttpClient's auth
/// headers are shared/global and cannot be set per request — Opsgenie
/// (`?apiKey=`), Gotify (`?token=`), ntfy (`?auth=`), and any saved data
/// mapping whose template carries `&apikey=`/`&token=`. Logging the raw URL
/// therefore writes those secrets to fincept.log in plaintext at the default
/// log level. The error path in handle_reply() already strips the query for
/// exactly this reason; the request path must do the same. Fixed here, once,
/// rather than at each call site.
QString log_url(const QString& url) {
    const QUrl parsed(url);
    if (!parsed.hasQuery())
        return url;
    return parsed.adjusted(QUrl::RemoveQuery).toString() + QStringLiteral("?<redacted>");
}

/// Error text for a failed response: `"HTTP_<status>: <server message>"`, or a
/// bare `"HTTP_<status>"` when the body carries nothing usable. Keeping the
/// status as a machine-readable prefix lets AuthApi/UserApi keep switching on
/// it (see HttpClient::status_from_error) while the server's own wording still
/// reaches the user.
///
/// Body shapes seen in practice: the Fincept backend sends `{"error": ...}` or
/// `{"message": ...}`; FastAPI sends `{"detail": "..."}` and, for request
/// validation, `{"detail": [{"loc": [...], "msg": "..."}]}`; third-party
/// webhooks send whatever they like.
std::string server_message(const QJsonDocument& doc, int status) {
    const QString prefix = QStringLiteral("HTTP_%1").arg(status);
    if (!doc.isObject())
        return prefix.toStdString();

    const QJsonObject obj = doc.object();
    QString msg;
    for (const auto* key : {"error", "message", "detail"}) {
        const QJsonValue v = obj.value(QLatin1String(key));
        if (!v.isString())
            continue;
        msg = v.toString().trimmed();
        if (!msg.isEmpty())
            break;
    }

    // Pydantic/FastAPI field-level validation errors (typically 422).
    if (msg.isEmpty() && obj.value(QLatin1String("detail")).isArray()) {
        QStringList parts;
        for (const auto& item : obj.value(QLatin1String("detail")).toArray()) {
            const QJsonObject entry = item.toObject();
            const QString m = entry.value(QLatin1String("msg")).toString().trimmed();
            if (m.isEmpty())
                continue;
            // loc is ["body", "field_name"] — take the last meaningful segment.
            QString field;
            const QJsonArray loc = entry.value(QLatin1String("loc")).toArray();
            for (qsizetype i = loc.size() - 1; i >= 0; --i) {
                QString seg = loc.at(i).toString();
                if (seg.isEmpty() || seg == QLatin1String("body"))
                    continue;
                seg[0] = seg[0].toUpper();
                seg.replace('_', ' ');
                field = seg;
                break;
            }
            parts << (field.isEmpty() ? m : field + QStringLiteral(": ") + m);
        }
        msg = parts.join(QStringLiteral("; "));
    }

    if (msg.isEmpty())
        return prefix.toStdString();
    return (prefix + QStringLiteral(": ") + msg).toStdString();
}
} // namespace

int HttpClient::status_from_error(const std::string& error) {
    const QString e = QString::fromStdString(error);
    if (!e.startsWith(QLatin1String("HTTP_")))
        return 0;
    const qsizetype colon = e.indexOf(QLatin1Char(':'), 5);
    const QString digits = (colon < 0) ? e.mid(5) : e.mid(5, colon - 5);
    bool ok = false;
    const int status = digits.toInt(&ok);
    return ok ? status : 0;
}

QString HttpClient::message_from_error(const std::string& error) {
    const QString e = QString::fromStdString(error);
    if (!e.startsWith(QLatin1String("HTTP_")))
        return e; // transport / JSON-parse failures carry their own text
    const qsizetype colon = e.indexOf(QLatin1Char(':'), 5);
    if (colon < 0)
        return {};
    return e.mid(colon + 1).trimmed();
}

HttpClient& HttpClient::instance() {
    static HttpClient s;
    return s;
}

HttpClient::HttpClient() {
    nam_ = new QNetworkAccessManager(this);
    // Qt disables the transfer timeout by default: a half-open TCP connection
    // never emits finished(), so the callback never runs, nothing is logged and
    // there is no cancel path — the only recovery is restarting the app. Every
    // other network site in this codebase sets one (BrokerHttp, NewsService, …).
    nam_->setTransferTimeout(std::chrono::seconds{20});
}

QNetworkRequest HttpClient::build_request(const QString& url, const Headers& extra_headers) const {
    const QString full_url = url.startsWith("http") ? url : (base_url_ + url);
    QNetworkRequest req{QUrl(full_url)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setHeader(QNetworkRequest::UserAgentHeader, "FinceptTerminal/4.0");

    // Applied last so a caller can override any default above for this one
    // request.
    for (auto it = extra_headers.constBegin(); it != extra_headers.constEnd(); ++it)
        req.setRawHeader(it.key(), it.value());
    return req;
}

void HttpClient::handle_reply(QNetworkReply* reply, JsonCallback callback, const QObject* context) {
    // Connect to caller-provided context so caller destruction auto-disconnects before the lambda fires.
    const QObject* receiver = context ? context : static_cast<const QObject*>(this);
    // Safety net: guarantee the reply is always deleted. If `context` is
    // destroyed before the reply finishes, Qt auto-disconnects the callback
    // lambda below and its reply->deleteLater() would never run → the reply
    // leaks until app exit. deleteLater() is idempotent, so a double-call when
    // the lambda also runs is harmless.
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    connect(reply, &QNetworkReply::finished, receiver, [reply, cb = std::move(callback)]() {
        reply->deleteLater();

        QByteArray data = reply->readAll();
        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        QJsonParseError parse_err;
        QJsonDocument doc = QJsonDocument::fromJson(data, &parse_err);

        if (reply->error() != QNetworkReply::NoError) {
            QUrl sanitized = reply->url();
            sanitized.setQuery(QString{}); // strip query to keep tokens out of logs
            LOG_WARN("HTTP",
                     QString("HTTP %1: %2 — %3").arg(status).arg(sanitized.toString()).arg(reply->errorString()));

            // 401/403 stay a bare "HTTP_<status>" so callers can match on
            // exactly that string rather than parsing a server message.
            if (status == 401 || status == 403) {
                cb(Result<QJsonDocument>::err(QString("HTTP_%1").arg(status).toStdString()));
                return;
            }

            // Every other HTTP error is an error, including one whose body
            // happens to parse. This used to return ok(doc) so AuthApi could
            // read the server's message off the document — but ~50 other call
            // sites share this client and were told a 400/404/429/500/502/503
            // had succeeded, then parsed the error body as data (DBnomics
            // reported "0 series found" for a 429; MaritimeService cached an
            // empty vessel for 60s). The message is preserved in the error
            // string instead — see server_message() / message_from_error().
            const bool parsed = !data.isEmpty() && parse_err.error == QJsonParseError::NoError;
            cb(Result<QJsonDocument>::err(server_message(parsed ? doc : QJsonDocument{}, status)));
            return;
        }

        if (parse_err.error != QJsonParseError::NoError) {
            cb(Result<QJsonDocument>::err(parse_err.errorString().toStdString()));
            return;
        }
        cb(Result<QJsonDocument>::ok(doc));
    });
}

void HttpClient::get(const QString& url, JsonCallback callback, const QObject* context, const Headers& extra_headers) {
    LOG_DEBUG("HTTP", "GET " + log_url(url));
    auto* reply = nam_->get(build_request(url, extra_headers));
    handle_reply(reply, std::move(callback), context);
}

void HttpClient::post(const QString& url, const QJsonObject& body, JsonCallback callback, const QObject* context,
                      const Headers& extra_headers) {
    LOG_DEBUG("HTTP", "POST " + log_url(url));
    QJsonDocument doc(body);
    auto* reply = nam_->post(build_request(url, extra_headers), doc.toJson());
    handle_reply(reply, std::move(callback), context);
}

void HttpClient::put(const QString& url, const QJsonObject& body, JsonCallback callback, const QObject* context,
                     const Headers& extra_headers) {
    LOG_DEBUG("HTTP", "PUT " + log_url(url));
    QJsonDocument doc(body);
    auto* reply = nam_->put(build_request(url, extra_headers), doc.toJson());
    handle_reply(reply, std::move(callback), context);
}

void HttpClient::del(const QString& url, JsonCallback callback, const QObject* context, const Headers& extra_headers) {
    LOG_DEBUG("HTTP", "DELETE " + log_url(url));
    auto* reply = nam_->deleteResource(build_request(url, extra_headers));
    handle_reply(reply, std::move(callback), context);
}

void HttpClient::del(const QString& url, const QJsonObject& body, JsonCallback callback, const QObject* context,
                     const Headers& extra_headers) {
    LOG_DEBUG("HTTP", "DELETE " + log_url(url));
    QJsonDocument doc(body);
    auto* reply =
        nam_->sendCustomRequest(build_request(url, extra_headers), "DELETE", doc.toJson(QJsonDocument::Compact));
    handle_reply(reply, std::move(callback), context);
}

void HttpClient::set_base_url(const QString& base) {
    base_url_ = base;
}

} // namespace fincept
