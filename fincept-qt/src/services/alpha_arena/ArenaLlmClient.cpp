#include "services/alpha_arena/ArenaLlmClient.h"

#include "core/config/AppConfig.h"
#include "services/llm/ModelCatalog.h"
#include "services/llm/ProviderCatalog.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

namespace fincept::arena {
using fincept::ai_chat::ModelCatalog;
using fincept::ai_chat::ProviderCatalog;

ArenaLlmClient::ArenaLlmClient(QObject* parent) : IArenaLlmClient(parent) {
    nam_ = new QNetworkAccessManager(this);
}

QByteArray ArenaLlmClient::build_body(const ArenaLlmRequest& req) {
    const QString p = req.provider.toLower();
    int max_tokens = req.max_tokens;
    if (const int cap = ModelCatalog::output_cap(p, req.model_id); cap > 0)
        max_tokens = qMin(max_tokens, cap);

    QJsonObject body;
    if (p == "anthropic") {
        body["model"] = req.model_id;
        body["max_tokens"] = max_tokens;
        body["system"] = req.system_prompt;
        body["messages"] = QJsonArray{QJsonObject{{"role", "user"}, {"content", req.user_prompt}}};
    } else if (p == "gemini" || p == "google") {
        body["systemInstruction"] = QJsonObject{{"parts", QJsonArray{QJsonObject{{"text", req.system_prompt}}}}};
        body["contents"] =
            QJsonArray{QJsonObject{{"role", "user"}, {"parts", QJsonArray{QJsonObject{{"text", req.user_prompt}}}}}};
        body["generationConfig"] = QJsonObject{{"maxOutputTokens", max_tokens}};
    } else { // OpenAI-compatible family
        body["messages"] = QJsonArray{QJsonObject{{"role", "system"}, {"content", req.system_prompt}},
                                      QJsonObject{{"role", "user"}, {"content", req.user_prompt}}};
        body["model"] = req.model_id;
        if (p == "openai" || p == "xai")
            body["max_completion_tokens"] = max_tokens;
        else
            body["max_tokens"] = max_tokens;
    }
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

ArenaLlmResult ArenaLlmClient::parse_response(const QString& provider, const QByteArray& body) {
    ArenaLlmResult r;
    const QString p = provider.toLower();
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    if (o.isEmpty()) {
        r.error = "non-JSON response";
        return r;
    }
    if (o.contains("error")) {
        const auto e = o.value("error");
        r.error = e.isObject() ? e.toObject().value("message").toString() : e.toString();
        if (r.error.isEmpty())
            r.error = "provider error";
        return r;
    }
    if (p == "anthropic") {
        for (const auto& blk : o.value("content").toArray())
            if (blk.toObject().value("type").toString() == "text")
                r.content += blk.toObject().value("text").toString();
        const auto u = o.value("usage").toObject();
        r.prompt_tokens = u.value("input_tokens").toInt();
        r.completion_tokens = u.value("output_tokens").toInt();
    } else if (p == "gemini" || p == "google") {
        const auto cands = o.value("candidates").toArray();
        if (!cands.isEmpty())
            for (const auto& part : cands[0].toObject().value("content").toObject().value("parts").toArray())
                r.content += part.toObject().value("text").toString();
        const auto u = o.value("usageMetadata").toObject();
        r.prompt_tokens = u.value("promptTokenCount").toInt();
        r.completion_tokens = u.value("candidatesTokenCount").toInt();
    } else {
        const auto choices = o.value("choices").toArray();
        if (!choices.isEmpty())
            r.content = choices[0].toObject().value("message").toObject().value("content").toString();
        if (r.content.isEmpty()) // tolerant fallbacks for non-standard shapes
            r.content = o.value("content").toString();
        if (r.content.isEmpty())
            r.content = o.value("response").toString();
        const auto u = o.value("usage").toObject();
        r.prompt_tokens = u.value("prompt_tokens").toInt();
        r.completion_tokens = u.value("completion_tokens").toInt();
    }
    if (r.content.isEmpty()) {
        r.error = "empty completion";
        return r;
    }
    r.success = true;
    return r;
}

void ArenaLlmClient::complete(const ArenaLlmRequest& req, std::function<void(ArenaLlmResult)> cb) {
    const QString p = req.provider.toLower();
    const QString url = ProviderCatalog::chat_endpoint(p, req.base_url, req.model_id);
    if (url.isEmpty()) {
        ArenaLlmResult r;
        r.error = "no endpoint for provider " + p;
        cb(r);
        return;
    }

    QNetworkRequest nr{QUrl(url)};
    nr.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (p == "anthropic") {
        nr.setRawHeader("x-api-key", req.api_key.toUtf8());
        nr.setRawHeader("anthropic-version", "2023-06-01");
    } else if (p == "gemini" || p == "google") {
        nr.setRawHeader("x-goog-api-key", req.api_key.toUtf8());
    } else if (!req.api_key.isEmpty()) {
        nr.setRawHeader("Authorization", ("Bearer " + req.api_key).toUtf8());
    }

    const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
    QNetworkReply* reply = nam_->post(nr, build_body(req));
    auto* timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    QObject::connect(timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    timeout->start(req.timeout_ms);

    const QString provider = p;
    QObject::connect(reply, &QNetworkReply::finished, this, [reply, provider, t0, cb]() {
        reply->deleteLater();
        ArenaLlmResult r;
        r.latency_ms = QDateTime::currentMSecsSinceEpoch() - t0;
        const QByteArray body = reply->readAll();
        if (reply->error() == QNetworkReply::OperationCanceledError) {
            r.error = "timeout";
            cb(r);
            return;
        }
        if (reply->error() != QNetworkReply::NoError && body.isEmpty()) {
            r.error = reply->errorString();
            cb(r);
            return;
        }
        // Provider errors often arrive as HTTP 4xx with a JSON body — parse it.
        ArenaLlmResult parsed = parse_response(provider, body);
        parsed.latency_ms = r.latency_ms;
        cb(parsed);
    });
}

} // namespace fincept::arena
