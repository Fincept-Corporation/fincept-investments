#pragma once
#include "core/result/Result.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QString>

#include <functional>
#include <string>

namespace fincept {

/// Async HTTP client wrapping QNetworkAccessManager. Responses delivered on the Qt event loop.
class HttpClient : public QObject {
    Q_OBJECT
  public:
    static HttpClient& instance();

    using JsonCallback = std::function<void(Result<QJsonDocument>)>;

    /// Extra raw headers for one request, applied last so they override the
    /// defaults set by build_request(). This is the supported way to send a
    /// per-caller credential — this class is a process-wide singleton, so it
    /// holds no credential state of its own.
    using Headers = QMap<QByteArray, QByteArray>;

    /// `context` scopes callback lifetime — pass `this` from anything that can outlive its reply.
    /// nullptr is safe only for objects whose lifetime equals the app's (singletons).
    void get(const QString& url, JsonCallback callback, const QObject* context = nullptr,
             const Headers& extra_headers = {});
    void post(const QString& url, const QJsonObject& body, JsonCallback callback, const QObject* context = nullptr,
              const Headers& extra_headers = {});
    void put(const QString& url, const QJsonObject& body, JsonCallback callback, const QObject* context = nullptr,
             const Headers& extra_headers = {});
    void del(const QString& url, JsonCallback callback, const QObject* context = nullptr,
             const Headers& extra_headers = {});
    void del(const QString& url, const QJsonObject& body, JsonCallback callback, const QObject* context = nullptr,
             const Headers& extra_headers = {});

    /// Error strings produced by this client are `"HTTP_<status>"` or
    /// `"HTTP_<status>: <server message>"`. These two recover the halves —
    /// use them instead of re-implementing the parse at each call site.
    /// status_from_error() returns 0 for transport/parse failures (no status).
    static int status_from_error(const std::string& error);
    /// Server-supplied text, or empty when the response carried none.
    /// For a non-HTTP failure (transport/JSON parse) the raw text is returned.
    static QString message_from_error(const std::string& error);

    void set_base_url(const QString& base);

  private:
    HttpClient();
    QNetworkRequest build_request(const QString& url, const Headers& extra_headers = {}) const;
    void handle_reply(QNetworkReply* reply, JsonCallback callback, const QObject* context);

    QNetworkAccessManager* nam_ = nullptr;
    QString base_url_;
};

} // namespace fincept
