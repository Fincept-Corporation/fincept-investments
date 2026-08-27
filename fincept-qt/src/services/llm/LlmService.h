#pragma once
// Multi-provider LLM client (OpenAI-compatible, Anthropic, Gemini, Fincept). Streams via SSE.

#include "core/result/Result.h"
#include "mcp/McpTypes.h"
#include "storage/repositories/LlmProfileRepository.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>
#include <vector>

namespace fincept::ai_chat {

inline bool provider_supports_streaming(const QString& provider) {
    return provider == "openai" || provider == "anthropic" || provider == "gemini" || provider == "google" ||
           provider == "groq" || provider == "deepseek" || provider == "openrouter" || provider == "minimax" ||
           provider == "kimi" || provider == "ollama" || provider == "xai" || provider == "astraflow" ||
           provider == "astraflow_cn" || provider == "aihubmix";
}

inline bool provider_requires_api_key(const QString& provider) {
    return provider != "ollama";
}

/// In-band sentinel prefixed onto a streamed chunk to mark it as chain-of-thought
/// ("thinking") rather than answer text. Reasoning models (DeepSeek-R1, GLM, Kimi,
/// MiniMax, …) stream `reasoning_content` (and Anthropic streams `thinking_delta`)
/// before the real answer. The AI Chat tab strips this prefix and routes the
/// remainder into a separate, collapsible Thinking section so it never mixes into
/// the answer bubble; the floating Quick-Chat bubble simply drops these chunks.
/// Uses STX (\x02) so it can't collide with model text — mirrors the existing
/// \x01 __TOOL_CALL_CLEAR__ sentinel style.
inline QString think_stream_prefix() {
    return QStringLiteral("\x02__THINK__");
}

/// In-band sentinel marking a chunk as tool-activity progress ("Finding tools · …")
/// rather than answer text. Same mechanism as think_stream_prefix (\x02) and the
/// \x01 __TOOL_CALL_CLEAR__ sentinel; \x03 so the three can't collide.
///
/// Tool progress used to be appended straight into the answer bubble, which mixed
/// plumbing into the reply and — because the label carried no arguments — rendered
/// six different `tool_describe` calls as six identical "• tool_describe" lines.
/// The AI Chat tab now routes these into their own collapsible Tools card.
inline QString tool_stream_prefix() {
    return QStringLiteral("\x03__TOOL__");
}

struct ConversationMessage {
    QString role; // system | user | assistant
    QString content;
};

struct LlmResponse {
    QString content;
    QString error;
    int prompt_tokens = 0;
    int completion_tokens = 0;
    int total_tokens = 0;
    bool success = false;
    /// Failure looks transient (upstream/model error, not a malformed request),
    /// so the caller may resubmit the identical prompt once.
    bool retryable = false;
};

/// (chunk_text, is_done) — invoked on a background thread.
using StreamCallback = std::function<void(const QString&, bool)>;

class LlmService : public QObject {
    Q_OBJECT
  public:
    static LlmService& instance();

    /// Blocking — call from a background thread (QtConcurrent::run).
    LlmResponse chat(const QString& user_message, const std::vector<ConversationMessage>& history,
                     bool use_tools = true);

    /// Per-request tool scope. NoNavigation lets the floating bubble keep benign tools
    /// without yanking the user away. None drops tools entirely.
    enum class ToolPolicy { All, NoNavigation, None };

    /// Spawns a background thread. on_chunk runs on that thread; finished_streaming() lands on UI thread.
    /// `chat_session_id` lets per-chat MCP tools (e.g. report_session_context) scope state to
    /// the originating conversation — omit for non-chat callers.
    void chat_streaming(const QString& user_message, const std::vector<ConversationMessage>& history,
                        StreamCallback on_chunk, ToolPolicy policy = ToolPolicy::All,
                        const QString& chat_session_id = {});

    /// Back-compat: false→None, true→All. Prefer the enum overload.
    void chat_streaming(const QString& user_message, const std::vector<ConversationMessage>& history,
                        StreamCallback on_chunk, bool use_tools);

    /// Call after the user changes LLM settings.
    void reload_config();

    QString active_provider() const;
    QString active_model() const;
    QString active_api_key() const;
    QString active_base_url() const;
    double active_temperature() const;
    int active_max_tokens() const;
    /// Tool-call rounds ceiling used by do_tool_loop. Clamped to [1, 200].
    int active_max_tool_rounds() const;
    bool tools_enabled() const;
    bool is_configured() const;

    /// context_type ∈ {ai_chat, agent, agent_default, team, team_default, team_coordinator}.
    /// context_id is the agent/team id; empty for type-level queries.
    ResolvedLlmProfile resolve_profile(const QString& context_type, const QString& context_id = {}) const;

    /// JSON shape for embedding in AgentService payloads.
    static QJsonObject profile_to_json(const ResolvedLlmProfile& p);

    /// Async — emits models_fetched.
    void fetch_models(const QString& provider, const QString& api_key, const QString& base_url = {});

    LlmService(const LlmService&) = delete;
    LlmService& operator=(const LlmService&) = delete;

  signals:
    void finished_streaming(LlmResponse response);
    void config_changed();
    void models_fetched(const QString& provider, const QStringList& models, const QString& error);

  private:
    LlmService();

    mutable QMutex mutex_;

    // INTERIM serialization guard: held for the WHOLE duration of each LLM request
    // (chat() and the chat_streaming() worker) because do_request/do_streaming_request
    // read provider_/api_key_/base_url_/model_ lock-free across the network call — a
    // concurrent worker rewriting them mid-flight could send one provider's API key
    // to another provider's base_url. Only ever acquired on worker threads (never the
    // UI thread), and always BEFORE mutex_, never while holding it. Proper fix
    // (deferred): thread a per-request config context through do_*_request instead
    // of reading shared members.
    QMutex request_serialize_mutex_;

    // Lazily reloaded; mutable so const accessors can call ensure_config().
    mutable QString provider_;
    mutable QString api_key_;
    mutable QString base_url_;
    mutable QString model_;
    mutable double temperature_ = 0.7;
    mutable int max_tokens_ = 4096;
    mutable int max_tool_rounds_ = 40;
    mutable QString system_prompt_;
    mutable bool tools_enabled_ = true;
    mutable bool config_loaded_ = false;

    // Empty filter = full catalogue. Read in build_*_request under mutex_.
    mcp::ToolFilter tool_filter_;

    // Lives on main thread — fetch_models() runs the reply on the GUI event loop.
    // Don't move to a worker thread: QNAM's DNS/TLS resolvers expect a Qt-managed thread.
    QNetworkAccessManager* models_nam_ = nullptr;

    void ensure_config() const;

    QJsonObject build_openai_request(const QString& user_message, const std::vector<ConversationMessage>& history,
                                     bool stream, bool with_tools = true);
    QJsonObject build_anthropic_request(const QString& user_message, const std::vector<ConversationMessage>& history,
                                        bool stream);
    QJsonObject build_gemini_request(const QString& user_message, const std::vector<ConversationMessage>& history);

    // Provider-specific tool-array builders. Shared by the initial request
    // builders AND the multi-round tool loops so the tools advertised on
    // follow-up turns stay identical to the first turn (Anthropic/Gemini
    // previously dropped tools on the follow-up, so they could never chain a
    // second tool call). Honour the active ToolPolicy via apply_request_policy.
    //
    // `activated` carries the tools the model has discovered this turn via
    // tool_list / tool_describe, exactly as the OpenAI path does. Without it
    // these two providers saw a fixed slice of the catalogue and could never
    // call anything they discovered — Tool RAG is on by default, so that slice
    // is what "tools don't work on Gemini/Claude" actually looked like.
    QJsonArray build_anthropic_tools(const QSet<QString>& activated = {}); // [{name, description, input_schema}]
    QJsonArray build_gemini_tools(const QSet<QString>& activated = {});    // [{functionDeclarations:[...]}]

    /// Stamp the output-token cap onto an OpenAI-shaped request body using the
    /// field the target endpoint actually accepts.
    ///
    /// OpenAI/xAI native endpoints — and OpenAI-family models routed through a
    /// pass-through aggregator — require `max_completion_tokens`; the o-series
    /// and gpt-5 hard-reject `max_tokens` with a 400. EVERY OpenAI-shaped body
    /// must go through this, tool-loop follow-ups included: those used to send
    /// `max_tokens` unconditionally, so on a reasoning model the first turn
    /// succeeded and every follow-up 400'd — the model called a tool once and
    /// the answer never came back.
    void apply_openai_token_limit(QJsonObject& body) const;

    QString get_endpoint_url() const;
    QMap<QString, QString> get_headers() const;

    /// Resolution order: user max_tokens (clamped to model cap) → model cap → 8192. Called with mutex_ held.
    int resolved_max_tokens() const;

    // Sync HTTP helpers (QNAM + QEventLoop) — call from background thread only.
    LlmResponse do_request(const QString& user_message, const std::vector<ConversationMessage>& history);
    LlmResponse do_streaming_request(const QString& user_message, const std::vector<ConversationMessage>& history,
                                     StreamCallback on_chunk);

    // `activated_tools` is seeded from the first round of tool calls (executed
    // by the caller) and grows as the model discovers more via tool_list /
    // tool_describe. In Tool RAG mode these names are force-declared to the
    // model each round so it can actually call what it discovers.
    LlmResponse do_tool_loop(QJsonArray loop_messages, const QString& url, const QMap<QString, QString>& headers,
                             QSet<QString> activated_tools = {});

    /// Returns nullopt if the content had no text/XML tool calls.
    std::optional<LlmResponse> try_extract_and_execute_text_tool_calls(const QString& content,
                                                                       const QString& user_message, const QString& url,
                                                                       const QMap<QString, QString>& headers);

    static QString get_models_url(const QString& provider, const QString& api_key, const QString& base_url);
    static QMap<QString, QString> get_models_headers(const QString& provider, const QString& api_key);
    static QStringList parse_models_response(const QString& provider, const QByteArray& body);

    /// One parsed SSE delta. `is_reasoning` is true for chain-of-thought text
    /// (OpenAI-compat `reasoning_content`, Anthropic `thinking_delta`) so the
    /// streaming loop can route it to the Thinking channel instead of the answer.
    struct SseDelta {
        QString text;
        bool is_reasoning = false;
    };
    static SseDelta parse_sse_chunk(const QString& data, const QString& provider);

    static void parse_usage(LlmResponse& resp, const QJsonObject& rj, const QString& provider);

    /// Blocks the calling thread via QEventLoop.
    struct HttpResult {
        bool success = false;
        int status = 0;
        QByteArray body;
        QString error;
    };
    static HttpResult blocking_post(const QString& url, const QJsonObject& body, const QMap<QString, QString>& headers,
                                    int timeout_ms = 120000);
    static HttpResult blocking_get(const QString& url, const QMap<QString, QString>& headers, int timeout_ms = 30000);

    /// QEventLoop-based path used for Cloudflare-protected Fincept endpoints.
    static HttpResult eventloop_request(const QString& method, const QString& url, const QByteArray& body,
                                        const QMap<QString, QString>& headers, int timeout_ms = 30000);
};

} // namespace fincept::ai_chat
