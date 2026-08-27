// Per-provider model-list discovery. Async GET on the GUI thread; emits models_fetched.

#include "core/config/AppConfig.h"
#include "core/logging/Logger.h"
#include "services/llm/LlmService.h"
#include "services/llm/ProviderCatalog.h"
#include "storage/repositories/SettingsRepository.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>

namespace fincept::ai_chat {

static constexpr const char* TAG = "LlmModelsApi";

QString LlmService::get_models_url(const QString& provider, const QString& api_key, const QString& base_url) {
    Q_UNUSED(api_key) // Auth in headers, not URL.
    const QString p = provider.toLower();

    // Blocked provider (AtlasCloud, removed) — no models URL, so the Fetch button
    // can never reach it even if base_url was hand-pointed at the host.
    if (ProviderCatalog::is_blocked(p, base_url))
        return {};

    // Ollama uses /api/tags, not /v1/models — must short-circuit before the custom-base_url branch
    // or a user with base_url=http://localhost:11434 hits the wrong path and parses a 404 page.
    if (p == "ollama") {
        QString base = base_url.isEmpty() ? "http://localhost:11434" : base_url;
        if (base.endsWith('/'))
            base.chop(1);
        return base + "/api/tags";
    }

    // Custom base_url — assume OpenAI-compatible /v1/models.
    if (!base_url.isEmpty()) {
        QString base = base_url;
        while (base.endsWith('/'))
            base.chop(1);

        const QString suffix = (p == "anthropic") ? QStringLiteral("/models?limit=1000") : QStringLiteral("/models");

        // Already a full endpoint — use verbatim.
        if (base.contains(QStringLiteral("/models")))
            return base;

        // Base already includes a version segment (e.g. ".../v1", ".../v1beta") —
        // append only the suffix. Otherwise inject the default "/v1".
        static const QRegularExpression re(QStringLiteral("/v\\d+[a-zA-Z]*$"));
        if (re.match(base).hasMatch())
            return base + suffix;
        return base + QStringLiteral("/v1") + suffix;
    }

    if (p == "openai")
        return "https://api.openai.com/v1/models";
    if (p == "anthropic")
        return "https://api.anthropic.com/v1/models?limit=1000";
    if (p == "gemini" || p == "google") {
        return "https://generativelanguage.googleapis.com/v1beta/models?pageSize=100";
    }
    if (p == "groq")
        return "https://api.groq.com/openai/v1/models";
    if (p == "deepseek")
        return "https://api.deepseek.com/models";
    if (p == "openrouter")
        return "https://openrouter.ai/api/v1/models";
    if (p == "xai")
        return "https://api.x.ai/v1/models";
    if (p == "kimi")
        return "https://api.moonshot.ai/v1/models";
    if (p == "aihubmix")
        return "https://aihubmix.com/v1/models"; // fallback if prefilled base_url was cleared
    // minimax has no public /v1/models — caller falls back to known models.
    return {};
}

QMap<QString, QString> LlmService::get_models_headers(const QString& provider, const QString& api_key) {
    QMap<QString, QString> h;
    const QString p = provider.toLower();

    if (p == "anthropic") {
        if (!api_key.isEmpty())
            h["x-api-key"] = api_key;
        h["anthropic-version"] = "2023-06-01";
    } else if (p == "gemini" || p == "google") {
        if (!api_key.isEmpty())
            h["x-goog-api-key"] = api_key;
    } else if (p == "ollama") {
        // No auth.
    } else {
        // OpenAI-compatible.
        if (!api_key.isEmpty())
            h["Authorization"] = "Bearer " + api_key;
    }
    return h;
}

QStringList LlmService::parse_models_response(const QString& provider, const QByteArray& body) {
    QStringList models;
    auto doc = QJsonDocument::fromJson(body);
    if (doc.isNull() || !doc.isObject())
        return models;
    QJsonObject root = doc.object();
    const QString p = provider.toLower();

    if (p == "gemini" || p == "google") {
        QJsonArray arr = root["models"].toArray();
        for (const auto& v : arr) {
            QJsonObject m = v.toObject();
            // Filter to models that support generateContent.
            QJsonArray methods = m["supportedGenerationMethods"].toArray();
            bool can_generate = false;
            for (const auto& method : methods) {
                if (method.toString() == "generateContent") {
                    can_generate = true;
                    break;
                }
            }
            if (!can_generate)
                continue;

            QString name = m["name"].toString();
            if (name.startsWith("models/"))
                name = name.mid(7);
            if (!name.isEmpty())
                models.append(name);
        }
    } else if (p == "ollama") {
        QJsonArray arr = root["models"].toArray();
        for (const auto& v : arr) {
            QString name = v.toObject()["name"].toString();
            if (!name.isEmpty())
                models.append(name);
        }
    } else {
        // OpenAI-compatible: {"data": [{"id": ...}]}.
        QJsonArray arr = root["data"].toArray();
        for (const auto& v : arr) {
            QString id = v.toObject()["id"].toString();
            if (!id.isEmpty())
                models.append(id);
        }
    }

    models.sort(Qt::CaseInsensitive);
    return models;
}

void LlmService::fetch_models(const QString& provider, const QString& api_key, const QString& base_url) {
    const QString url = get_models_url(provider, api_key, base_url);
    if (url.isEmpty()) {
        emit models_fetched(provider, {}, "Unknown provider: " + provider);
        return;
    }

    // NAM must live on the main thread — its DNS/TLS path needs a pumping event loop.
    if (!models_nam_)
        models_nam_ = new QNetworkAccessManager(this);

    QNetworkRequest req{QUrl(url)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    auto headers = get_models_headers(provider, api_key);
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
        req.setRawHeader(it.key().toUtf8(), it.value().toUtf8());

    LOG_INFO(TAG, QString("fetch_models start: provider=%1 url=%2").arg(provider, url));

    QNetworkReply* reply = models_nam_->get(req);

    reply->setProperty("_provider", provider);
    auto* timer = new QTimer(reply);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, reply, [reply]() {
        if (reply->isRunning())
            reply->abort();
    });
    timer->start(15000);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        const QString p = reply->property("_provider").toString();

        QString error;
        QStringList models;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
            if (reply->error() == QNetworkReply::OperationCanceledError)
                error = "Request timed out";
            else {
                error = reply->errorString();
                if (error.isEmpty())
                    error = QString("HTTP %1").arg(status);
            }
            LOG_WARN(TAG, QString("fetch_models %1 failed: %2 (status=%3 qt_error=%4)")
                              .arg(p, error)
                              .arg(status)
                              .arg(static_cast<int>(reply->error())));
        } else {
            models = parse_models_response(p, reply->readAll());
            if (models.isEmpty())
                error = "No models found";
            LOG_INFO(TAG, QString("fetch_models %1 → %2 models").arg(p).arg(models.size()));
        }

        emit models_fetched(p, models, error);
    });
}

} // namespace fincept::ai_chat
