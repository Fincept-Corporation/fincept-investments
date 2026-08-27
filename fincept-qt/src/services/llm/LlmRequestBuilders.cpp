// Per-provider chat-completion request shape builders.

#include "core/logging/Logger.h"
#include "mcp/McpProvider.h"
#include "mcp/McpService.h"
#include "services/llm/LlmRequestPolicy.h"
#include "services/llm/LlmService.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace fincept::ai_chat {

namespace {
constexpr const char* kLlmBuildersTag = "LlmService";
}

void LlmService::apply_openai_token_limit(QJsonObject& body) const {
    // OpenAI/xAI native endpoints take max_completion_tokens universally.
    // AIHubMix is a pass-through aggregator (no param translation), so an
    // OpenAI-family model routed through it ALSO needs max_completion_tokens —
    // o-series and gpt-5 hard-reject max_tokens with a 400 ("Unsupported
    // parameter: 'max_tokens' ... Use 'max_completion_tokens' instead").
    // Non-OpenAI models (claude/gemini/deepseek/qwen/…) keep max_tokens.
    //
    // Centralised because the tool-loop follow-ups did NOT do this and sent
    // max_tokens unconditionally: on a reasoning model the opening turn was
    // built correctly and every follow-up 400'd, so the model would call one
    // tool and the reply never arrived.
    const QString model_lower = model_.toLower();
    const bool wants_completion_tokens = provider_ == "openai" || provider_ == "xai" ||
                                         (provider_ == "aihubmix" && detail::is_openai_family_model(model_lower));
    body.remove(wants_completion_tokens ? QStringLiteral("max_tokens") : QStringLiteral("max_completion_tokens"));
    body[wants_completion_tokens ? QStringLiteral("max_completion_tokens") : QStringLiteral("max_tokens")] =
        resolved_max_tokens();
}

QJsonObject LlmService::build_openai_request(const QString& user_message,
                                             const std::vector<ConversationMessage>& history, bool stream,
                                             bool with_tools) {
    const QString model_lower = model_.toLower();

    // Models that reject `tools`: deepseek-reasoner, groq whisper-* (audio), groq llama-guard-* (classifier).
    const bool is_ds_reasoner = (provider_ == "deepseek" && model_lower.contains("reasoner"));
    const bool groq_no_tools =
        (provider_ == "groq" && (model_lower.startsWith("whisper-") || model_lower.contains("llama-guard")));

    QJsonArray messages;
    if (!system_prompt_.isEmpty())
        messages.append(QJsonObject{{"role", "system"}, {"content", system_prompt_}});
    for (const auto& m : history)
        messages.append(QJsonObject{{"role", m.role}, {"content", m.content}});
    messages.append(QJsonObject{{"role", "user"}, {"content", user_message}});

    QJsonObject req;
    req["model"] = model_;
    req["messages"] = messages;
    // Temperature omitted — each provider's default.
    apply_openai_token_limit(req);
    if (stream) {
        req["stream"] = true;
        // OpenAI/xAI (and OpenAI-family models via AIHubMix) omit usage on streamed
        // responses unless we opt in. Don't send stream_options to non-OpenAI routes.
        if (req.contains("max_completion_tokens"))
            req["stream_options"] = QJsonObject{{"include_usage", true}};
    }

    // Send tools on streaming requests too — the stream path detects tool_calls and falls back
    // to do_request to execute. Otherwise the model answers from training and tool calling silently breaks.
    const bool tools_effectively_on = detail::effective_tools_enabled(tools_enabled_);
    if (with_tools && tools_effectively_on && !is_ds_reasoner && !groq_no_tools) {
        QJsonArray tools =
            mcp::McpService::instance().format_tools_for_openai(detail::apply_request_policy(tool_filter_));
        if (!tools.isEmpty())
            req["tools"] = tools;
        LOG_INFO(kLlmBuildersTag, QString("OpenAI request: stream=%1 provider=%2 tools=%3 (count=%4)")
                                      .arg(stream ? "true" : "false", provider_, tools.isEmpty() ? "none" : "attached")
                                      .arg(tools.size()));
    } else {
        LOG_WARN(kLlmBuildersTag, QString("OpenAI request: stream=%1 provider=%2 NO TOOLS — "
                                          "with_tools=%3 tools_effectively_on=%4 ds_reasoner=%5 groq_no_tools=%6")
                                      .arg(stream ? "true" : "false", provider_)
                                      .arg(with_tools ? "true" : "false")
                                      .arg(tools_effectively_on ? "true" : "false")
                                      .arg(is_ds_reasoner ? "true" : "false")
                                      .arg(groq_no_tools ? "true" : "false"));
    }
    return req;
}

QJsonObject LlmService::build_anthropic_request(const QString& user_message,
                                                const std::vector<ConversationMessage>& history, bool stream) {
    QJsonArray messages;
    for (const auto& m : history) {
        if (m.role != "system")
            messages.append(QJsonObject{{"role", m.role}, {"content", m.content}});
    }
    messages.append(QJsonObject{{"role", "user"}, {"content", user_message}});

    QJsonObject req;
    req["model"] = model_;
    req["messages"] = messages;
    req["max_tokens"] = resolved_max_tokens();
    if (!system_prompt_.isEmpty())
        req["system"] = system_prompt_;
    if (stream)
        req["stream"] = true;

    // Anthropic tools: bare {name, description, input_schema} — no OpenAI-style "type":"function" wrapper.
    QJsonArray ant_tools = build_anthropic_tools();
    if (!ant_tools.isEmpty())
        req["tools"] = ant_tools;
    return req;
}

QJsonArray LlmService::build_anthropic_tools(const QSet<QString>& activated) {
    if (!detail::effective_tools_enabled(tools_enabled_))
        return {};
    // Same selection as the OpenAI path (Tier-0 + whatever the model has
    // discovered this turn). This used to call get_all_tools(), which bypasses
    // Tool RAG entirely and hands back an arbitrary 50-tool slice of a ~900
    // tool catalogue — so `tool_list` was frequently not even declared and the
    // model had no way to reach the rest.
    return mcp::McpService::instance().format_tools_for_anthropic(detail::apply_request_policy(tool_filter_),
                                                                  activated);
}

QJsonObject LlmService::build_gemini_request(const QString& user_message,
                                             const std::vector<ConversationMessage>& history) {
    QJsonArray contents;
    for (const auto& m : history) {
        if (m.role == "system")
            continue;
        QString role = (m.role == "assistant") ? "model" : "user";
        contents.append(QJsonObject{{"role", role}, {"parts", QJsonArray{QJsonObject{{"text", m.content}}}}});
    }
    contents.append(QJsonObject{{"role", "user"}, {"parts", QJsonArray{QJsonObject{{"text", user_message}}}}});

    QJsonObject gen_cfg;
    gen_cfg["maxOutputTokens"] = resolved_max_tokens();

    QJsonObject req;
    req["contents"] = contents;
    req["generationConfig"] = gen_cfg;
    if (!system_prompt_.isEmpty()) {
        req["systemInstruction"] = QJsonObject{{"parts", QJsonArray{QJsonObject{{"text", system_prompt_}}}}};
    }

    // Gemini tools: tools[{functionDeclarations:[{name, description, parameters}]}].
    QJsonArray gem_tools = build_gemini_tools();
    if (!gem_tools.isEmpty())
        req["tools"] = gem_tools;

    return req;
}

QJsonArray LlmService::build_gemini_tools(const QSet<QString>& activated) {
    if (!detail::effective_tools_enabled(tools_enabled_))
        return {};
    // Gemini needs more than a reshuffle of the OpenAI payload: its
    // `parameters` field is an OpenAPI subset, not JSON Schema, and one
    // unrecognised key — or one parameterless tool sent as
    // {"type":"object","properties":{}} — fails the ENTIRE request rather than
    // the offending declaration. format_tools_for_gemini does that
    // translation; see mcp/GeminiSchema.h.
    return mcp::McpService::instance().format_tools_for_gemini(detail::apply_request_policy(tool_filter_), activated);
}

} // namespace fincept::ai_chat
