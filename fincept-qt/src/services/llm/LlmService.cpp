// Multi-provider LLM API client. Sync paths block on QEventLoop and must run on a worker thread.

#include "services/llm/LlmService.h"

#include "core/config/AppConfig.h"
#include "core/logging/Logger.h"
#include "datahub/DataHub.h"
#include "datahub/TopicPolicy.h"
#include "mcp/McpProvider.h"
#include "mcp/McpService.h"
#include "services/llm/LlmContentExtractors.h"
#include "services/llm/LlmRequestPolicy.h"
#include "services/llm/ModelCatalog.h"
#include "services/llm/ProviderCatalog.h"
#include "storage/repositories/LlmConfigRepository.h"
#include "storage/repositories/SettingsRepository.h"

#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QVariant>
#include <QtConcurrent/QtConcurrent>

#include <algorithm>

namespace fincept::ai_chat {

static constexpr const char* kLlmSvcTag = "LlmService";

LlmService::LlmService() = default;

LlmService& LlmService::instance() {
    static LlmService s;
    return s;
}

void LlmService::reload_config() {
    {
        QMutexLocker lock(&mutex_);
        config_loaded_ = false;
        ensure_config();
    }
    emit config_changed();
}

void LlmService::ensure_config() const {
    // Called with mutex_ held. The cache is one-shot.
    if (config_loaded_)
        return;

    provider_ = model_ = api_key_ = base_url_ = system_prompt_ = {};
    temperature_ = 0.7;
    // 0 means "no user override → ModelCatalog::output_cap()". The v15 migration reset legacy 2000 defaults.
    max_tokens_ = 0;
    tools_enabled_ = true;

    auto providers = LlmConfigRepository::instance().list_providers();
    if (providers.is_ok()) {
        for (const auto& c : providers.value()) {
            if (c.is_active) {
                provider_ = c.provider.toLower();
                api_key_ = c.api_key;
                base_url_ = c.base_url;
                model_ = c.model;
                tools_enabled_ = c.tools_enabled;
                break;
            }
        }
        // No active provider — pick the first one.
        if (provider_.isEmpty() && !providers.value().isEmpty()) {
            const auto& c = providers.value().first();
            provider_ = c.provider.toLower();
            api_key_ = c.api_key;
            base_url_ = c.base_url;
            model_ = c.model;
            tools_enabled_ = c.tools_enabled;
        }
    }

    // Nothing configured — leave provider_ empty. is_configured() then returns
    // false and the caller surfaces the "configure Settings → LLM Config" path.
    if (provider_.isEmpty())
        LOG_INFO(kLlmSvcTag, "No LLM provider configured");

    auto gs = LlmConfigRepository::instance().get_global_settings();
    if (gs.is_ok()) {
        temperature_ = gs.value().temperature;
        max_tokens_ = gs.value().max_tokens;
        system_prompt_ = gs.value().system_prompt;
        // Clamp [1, 200] so a corrupt DB row can't lock the loop at 0 or run forever.
        max_tool_rounds_ = std::clamp(gs.value().max_tool_rounds, 1, 200);
    }

    // Default system prompt — primes the model to actually use tools instead of declining tool-feasible requests.
    if (system_prompt_.trimmed().isEmpty()) {
        system_prompt_ =
            "You are Fincept AI, the intelligent assistant embedded inside the Fincept Terminal — "
            "a professional desktop financial intelligence application. You have access to tools that "
            "let you interact with the terminal directly: navigate screens, fetch live market data, "
            "manage watchlists, query portfolios, paper-trade, run Python analytics, search SEC Edgar "
            "filings, fetch news, and BUILD REPORTS LIVE in the Report Builder.\n"
            "\n"
            "Behaviour rules:\n"
            "• ALWAYS use a tool when one can fulfil the request — never decline an action that a tool "
            "  exists for. Never tell the user you cannot navigate or open screens.\n"
            "• Building a report (e.g. 'create an equity research report on TSLA'): your job is to "
            "  WRITE THE REPORT INTO THE REPORT BUILDER USING TOOLS. Do not narrate the report into "
            "  the chat. STEP 0 (mandatory): call report_session_context to see whether this chat "
            "  is already authoring a report (linked_report set) or is starting fresh (linked_report "
            "  null with stale components on the canvas). If linked, CONTINUE editing it. If unlinked "
            "  and the canvas has components, call report_clear first UNLESS the user explicitly "
            "  said 'edit this report' / 'add to this report' / 'continue the report' — in which "
            "  case keep the canvas. The flow is: (1) DESIGN A UNIQUE STRUCTURE for the subject — every ticker, "
            "  sector, or topic deserves its own outline. A Tesla EV-and-FSD memo, a JPM credit note, "
            "  and a TLT duration play should NOT share the same skeleton. Decide which sections "
            "  actually matter for THIS subject (could be Catalysts, Bear Case, Capital Structure, "
            "  Unit Economics, Regulatory Overhang, Insider Activity, Optionality, etc.) — do NOT "
            "  default to the generic Company Overview / Financials / Valuation / Thesis / Risks / "
            "  Target template. Do NOT call report_apply_template unless the user explicitly asks "
            "  for a template OR the request is for a generic non-research scaffold (Meeting Notes, "
            "  Trade Journal, Pre-Market Checklist). (2) call report_set_metadata with a "
            "  subject-specific title (e.g. 'Tesla: Margin Compression vs FSD Optionality' — not "
            "  'Stock Research'), author, company, date. (3) build the structure by calling "
            "  report_add_component for each section you designed in step 1. (4) gather data with "
            "  tools like get_equity_info, get_equity_quote, edgar_get_financials, "
            "  edgar_10k_sections, edgar_calc_multiples, get_equity_news, search_news. "
            "  (5) populate each section by calling report_update_component with the gathered data. "
            "  Use stable component ids returned by report_add_component — never indices. "
            "  COVERAGE: before declaring the report done, verify every section you added has "
            "  real content (no placeholders like 'Describe the company…').\n"
            "• Report formatting (CRITICAL for a polished result):\n"
            "  - text/list/quote/callout content SUPPORTS MARKDOWN. Use **bold** to highlight key "
            "    figures (e.g. 'Revenue grew **22% YoY** to **$96.8B**'). Use *italic* sparingly. "
            "    Do not paste raw asterisks expecting them to render — they do, but only inside the "
            "    content of those component types.\n"
            "  - For tables, ALWAYS pass real data via config={'csv':'Header1,Header2|Cell1,Cell2|...'}. "
            "    Pipe `|` separates rows, comma `,` separates cells. First row is auto-bolded. Never "
            "    leave a table empty — it renders as 'Header 1, Header 2, ...' placeholder text.\n"
            "  - For charts, pass config={'chart_type':'bar'|'line'|'pie','title':...,'data':'1,2,3',"
            "    'labels':'Q1,Q2,Q3'}.\n"
            "  - Set proper metadata FIRST via report_set_metadata: title (e.g. 'Tesla Equity Research "
            "    Report'), author (e.g. 'Fincept Research'), company, and date. Don't leave 'Analyst' "
            "    or 'Untitled Report' defaults.\n"
            "  - Avoid one-line ramblings. Each text component should be a tight paragraph.\n"
            "• Python scripts: ONLY pass script names returned by list_python_scripts. Never invent or "
            "  guess script names. If list_python_scripts returns nothing useful, fall back to other "
            "  tools (get_quote, edgar_*, get_candles, etc.) — those are the canonical data sources.\n"
            "• When you have completed the user's request, reply with a concise summary in chat. "
            "  Do not paste the full report content into chat — the report lives in the Report "
            "  Builder canvas and the user is watching it fill in.\n"
            "Be concise, accurate, and finance-focused.";
    }

    // Tool RAG discovery hint: lists categories dynamically so the model can form good tool_list() queries.
    // The `[Tool discovery]` sentinel makes append idempotent across reloads. Cached static for prompt-cache stability.
    if (tools_enabled_ && !system_prompt_.contains("[Tool discovery]")) {
        // Tool registry is immutable after McpInit; runtime additions won't appear until restart (acceptable here).
        static const QString kHint = []() -> QString {
            const auto all = mcp::McpProvider::instance().list_all_tools();
            QSet<QString> cats;
            for (const auto& t : all) {
                if (!t.category.isEmpty())
                    cats.insert(t.category);
            }
            QStringList sorted_cats = cats.values();
            std::sort(sorted_cats.begin(), sorted_cats.end());

            QString hint = "\n\n[Tool discovery] You see only a small subset of tools each turn. "
                           "To find a tool for any action you don't already have, call "
                           "tool_list(query=\"<natural-language description>\"). "
                           "It returns the top 5 most relevant tools (BM25-ranked). "
                           "Then call tool_describe(name) for the full input schema, then invoke it. "
                           "For requests with multiple intents (\"get news AND add to watchlist\"), "
                           "call tool_list MULTIPLE TIMES — once per intent. "
                           "Never decline an action you can fulfil via a discoverable tool. "
                           "Discovery is not free: only reach for a tool when the request needs live data "
                           "or an action inside the terminal. Greetings, small talk, and questions you can "
                           "answer from your own knowledge get a direct plain-text reply and NO tool call.";
            if (!sorted_cats.isEmpty()) {
                hint += "\nAvailable tool categories: " + sorted_cats.join(", ") + ".";
            }
            return hint;
        }();
        system_prompt_ += kHint;
    }

    config_loaded_ = true;
    const int resolved = resolved_max_tokens();
    const int catalog_cap = ModelCatalog::output_cap(provider_, model_);
    LOG_INFO(kLlmSvcTag, QString("LLM config loaded: provider=%1 model=%2 tools_enabled=%3 "
                                 "max_tokens(user=%4 catalog=%5 resolved=%6)")
                             .arg(provider_, model_, tools_enabled_ ? "TRUE" : "FALSE")
                             .arg(max_tokens_)
                             .arg(catalog_cap)
                             .arg(resolved));
}

// ============================================================================
// Accessors
// ============================================================================

QString LlmService::active_provider() const {
    QMutexLocker lock(&mutex_);
    ensure_config();
    return provider_;
}

QString LlmService::active_model() const {
    QMutexLocker lock(&mutex_);
    ensure_config();
    return model_;
}

QString LlmService::active_api_key() const {
    QMutexLocker lock(&mutex_);
    ensure_config();
    return api_key_;
}

QString LlmService::active_base_url() const {
    QMutexLocker lock(&mutex_);
    ensure_config();
    return base_url_;
}

double LlmService::active_temperature() const {
    QMutexLocker lock(&mutex_);
    ensure_config();
    return temperature_;
}

int LlmService::active_max_tokens() const {
    QMutexLocker lock(&mutex_);
    ensure_config();
    return max_tokens_;
}

int LlmService::active_max_tool_rounds() const {
    QMutexLocker lock(&mutex_);
    ensure_config();
    return max_tool_rounds_;
}

bool LlmService::tools_enabled() const {
    QMutexLocker lock(&mutex_);
    ensure_config();
    return tools_enabled_;
}

bool LlmService::is_configured() const {
    QMutexLocker lock(&mutex_);
    ensure_config();
    if (provider_.isEmpty())
        return false;
    // Blocked provider (AtlasCloud, removed) is never considered configured.
    if (ProviderCatalog::is_blocked(provider_, base_url_))
        return false;
    if (provider_requires_api_key(provider_))
        return !api_key_.isEmpty();
    return true;
}

ResolvedLlmProfile LlmService::resolve_profile(const QString& context_type, const QString& context_id) const {
    return LlmProfileRepository::instance().resolve_for_context(context_type, context_id);
}

// static
QJsonObject LlmService::profile_to_json(const ResolvedLlmProfile& p) {
    // Only fields the Python model constructor accepts. profile_id / profile_name are internal metadata —
    // sending them lands in extra_model_kwargs and fails as unexpected kwargs.
    QJsonObject obj;
    obj["provider"] = p.provider;
    obj["model_id"] = p.model_id;
    obj["api_key"] = p.api_key;
    obj["base_url"] = p.base_url;
    obj["temperature"] = p.temperature;
    obj["max_tokens"] = p.max_tokens;
    if (!p.system_prompt.isEmpty())
        obj["system_prompt"] = p.system_prompt;
    return obj;
}

int LlmService::resolved_max_tokens() const {
    // Called with mutex_ held.
    constexpr int kFallback = 8192;
    const int catalog_cap = ModelCatalog::output_cap(provider_, model_);

    // <=0 = unset; clamp user values to the model's published cap to avoid a 400.
    if (max_tokens_ > 0) {
        if (catalog_cap > 0 && max_tokens_ > catalog_cap)
            return catalog_cap;
        return max_tokens_;
    }

    if (catalog_cap > 0)
        return catalog_cap;
    return kFallback;
}

QString LlmService::get_endpoint_url() const {
    // Called with mutex_ held.
    const QString& p = provider_;

    // Blocked provider (AtlasCloud, removed) — refuse to resolve any endpoint,
    // including a base_url that was hand-pointed at the host. do_request /
    // do_streaming_request already block earlier with a clear message; this is
    // the last-line backstop for any other caller.
    if (ProviderCatalog::is_blocked(p, base_url_))
        return {};

    // Custom base_url wins over hard-coded defaults.
    if (!base_url_.isEmpty()) {
        QString base = base_url_;
        while (base.endsWith('/'))
            base.chop(1);

        // Gemini is the one provider whose request body is NOT OpenAI-shaped,
        // so a custom base_url has to compose Gemini's own path. Appending
        // "/chat/completions" (what every other provider gets) pointed a
        // native `contents`/`tools[].functionDeclarations` body at an
        // OpenAI-compat route, which 400s — so any user with a Gemini proxy or
        // a regional endpoint lost tool calling along with everything else.
        if (p == "gemini" || p == "google") {
            if (base.contains(QLatin1String(":generateContent")))
                return base;
            static const QRegularExpression gem_ver(QStringLiteral("/v\\d+[a-zA-Z]*$"));
            if (!gem_ver.match(base).hasMatch())
                base += QStringLiteral("/v1beta");
            return base + QStringLiteral("/models/") + model_ + QStringLiteral(":generateContent");
        }

        const QString suffix = (p == "anthropic") ? QStringLiteral("/messages") : QStringLiteral("/chat/completions");

        // Already a full endpoint — use verbatim.
        if (base.endsWith(suffix))
            return base;

        // Base already includes a version segment (e.g. ".../v1", ".../v1beta") —
        // append only the suffix. Otherwise inject the default "/v1".
        static const QRegularExpression re(QStringLiteral("/v\\d+[a-zA-Z]*$"));
        if (re.match(base).hasMatch())
            return base + suffix;
        return base + QStringLiteral("/v1") + suffix;
    }

    if (p == "openai")
        return "https://api.openai.com/v1/chat/completions";
    if (p == "anthropic")
        return "https://api.anthropic.com/v1/messages";
    if (p == "gemini" || p == "google")
        return "https://generativelanguage.googleapis.com/v1beta/models/" + model_ + ":generateContent";
    if (p == "groq")
        return "https://api.groq.com/openai/v1/chat/completions";
    if (p == "deepseek")
        return "https://api.deepseek.com/v1/chat/completions";
    if (p == "openrouter")
        return "https://openrouter.ai/api/v1/chat/completions";
    if (p == "minimax")
        return "https://api.minimax.io/v1/chat/completions";
    if (p == "kimi")
        return "https://api.moonshot.ai/v1/chat/completions";
    if (p == "ollama")
        return "http://localhost:11434/v1/chat/completions";
    if (p == "xai")
        return "https://api.x.ai/v1/chat/completions";
    if (p == "aihubmix")
        // Fallback for when the prefilled base_url was cleared; the custom-base_url
        // branch above wins whenever it's set (incl. regional/proxy overrides).
        return "https://aihubmix.com/v1/chat/completions";
    return {};
}

QMap<QString, QString> LlmService::get_headers() const {
    // Called with mutex_ held.
    QMap<QString, QString> h;
    const QString& p = provider_;

    if (p == "anthropic") {
        if (!api_key_.isEmpty())
            h["x-api-key"] = api_key_;
        h["anthropic-version"] = "2023-06-01";
    } else if (p == "gemini" || p == "google") {
        if (!api_key_.isEmpty())
            h["x-goog-api-key"] = api_key_;
    } else {
        if (!api_key_.isEmpty())
            h["Authorization"] = "Bearer " + api_key_;
        if (p == "openrouter") {
            // Attribution for the openrouter.ai/rankings leaderboard.
            h["HTTP-Referer"] = "https://fincept.in";
            h["X-Title"] = "Fincept Terminal";
        }
    }
    return h;
}

LlmResponse LlmService::do_request(const QString& user_message, const std::vector<ConversationMessage>& history) {
    LlmResponse resp;

    if (ProviderCatalog::is_blocked(provider_, base_url_)) {
        resp.error = "AtlasCloud has been banned by Fincept and cannot be used.";
        return resp;
    }

    QString url = get_endpoint_url();
    if (url.isEmpty()) {
        resp.error = "No endpoint URL for provider: " + provider_;
        return resp;
    }

    auto hdr = get_headers();
    QJsonObject req_body;

    if (provider_ == "anthropic") {
        req_body = build_anthropic_request(user_message, history, false);
    } else if (provider_ == "gemini" || provider_ == "google") {
        req_body = build_gemini_request(user_message, history);
        // Auth via x-goog-api-key header — do NOT append ?key= (would leak into access logs).
    } else {
        req_body = build_openai_request(user_message, history, false, true);
    }

    LOG_DEBUG(kLlmSvcTag, QString("POST %1 provider=%2 model=%3").arg(url, provider_, model_));

    auto http = blocking_post(url, req_body, hdr);

    if (!http.success) {
        resp.error = http.error;
        return resp;
    }

    auto doc = QJsonDocument::fromJson(http.body);
    if (doc.isNull()) {
        resp.error = "Failed to parse response JSON";
        return resp;
    }
    QJsonObject rj = doc.object();

    if (provider_ == "anthropic") {
        QJsonArray content = rj["content"].toArray();
        QString stop_reason = rj["stop_reason"].toString();

        // Check for tool_use blocks
        if (stop_reason == "tool_use") {
            LOG_INFO(kLlmSvcTag, "Anthropic requested tool_use");

            // Build follow-up messages: original + assistant turn + tool results
            QJsonArray loop_msgs;
            if (!system_prompt_.isEmpty()) {
            } // Anthropic puts system at top level, not in messages.
            for (const auto& h : history)
                if (h.role != "system")
                    loop_msgs.append(QJsonObject{{"role", h.role}, {"content", h.content}});
            loop_msgs.append(QJsonObject{{"role", "user"}, {"content", user_message}});
            loop_msgs.append(QJsonObject{{"role", "assistant"}, {"content", content}});

            // Tools the model discovers this turn via tool_list / tool_describe.
            // Tool RAG only ships the Tier-0 prefix, so without feeding these
            // back into the next round's `tools` array the model stalls right
            // after tool_describe with "I don't have that tool loaded" — the
            // OpenAI path has always done this; Anthropic never did.
            detail::ActivationTracker activated;

            QJsonArray tool_results;
            for (const auto& block_val : content) {
                QJsonObject block = block_val.toObject();
                if (block["type"].toString() != "tool_use")
                    continue;
                QString tool_id = block["id"].toString();
                QString tool_name = block["name"].toString();
                QJsonObject input = block["input"].toObject();

                LOG_INFO(kLlmSvcTag, "Executing Anthropic tool: " + tool_name);
                const QString bare = mcp::McpProvider::parse_openai_function_name(tool_name).second;
                detail::emit_tool_progress(bare, input);
                auto tr = mcp::McpService::instance().execute_openai_function(tool_name, input, /*allow_defer=*/true);
                detail::note_tool_activations(bare, input, tr, activated);
                tool_results.append(
                    QJsonObject{{"type", "tool_result"},
                                {"tool_use_id", tool_id},
                                {"content", detail::encode_tool_result_for_llm(bare, tr)}});
            }

            loop_msgs.append(QJsonObject{{"role", "user"}, {"content", tool_results}});

            // ── Multi-round tool loop ──
            // Previously this was a SINGLE follow-up with NO tools attached, so
            // the model could execute exactly one tool and never chain a second
            // (multi-step flows like "create a portfolio then add assets" broke).
            // We now loop, re-advertising the tool set each round, until the
            // model returns a final text answer or we hit the round cap. The
            // set is rebuilt per round rather than hoisted: under Tool RAG it
            // grows as the model discovers tools, so a hoisted array would
            // freeze the catalogue at whatever round 1 could see.
            const int kMaxRounds = active_max_tool_rounds();
            for (int round = 0; round < kMaxRounds; ++round) {
                const QJsonArray ant_tools = build_anthropic_tools(activated.names());
                QJsonObject fu;
                fu["model"] = model_;
                fu["messages"] = loop_msgs;
                fu["max_tokens"] = resolved_max_tokens();
                if (!system_prompt_.isEmpty())
                    fu["system"] = system_prompt_;
                if (!ant_tools.isEmpty())
                    fu["tools"] = ant_tools;

                auto fu_http = blocking_post(url, fu, hdr);
                if (!fu_http.success) {
                    resp.error = "Anthropic tool follow-up failed: " + fu_http.error;
                    return resp;
                }
                auto fu_doc = QJsonDocument::fromJson(fu_http.body);
                if (fu_doc.isNull()) {
                    resp.error = "Anthropic tool follow-up parse error";
                    return resp;
                }
                const QJsonObject frj = fu_doc.object();
                const QJsonArray fcontent = frj["content"].toArray();

                if (frj["stop_reason"].toString() != "tool_use") {
                    resp.content = extract_anthropic_content_text(fcontent); // final answer
                    break;
                }

                // More tools requested — record the assistant turn, execute each
                // tool_use block, and feed the results back for the next round.
                loop_msgs.append(QJsonObject{{"role", "assistant"}, {"content", fcontent}});
                QJsonArray more_results;
                for (const auto& block_val : fcontent) {
                    const QJsonObject block = block_val.toObject();
                    if (block["type"].toString() != "tool_use")
                        continue;
                    const QString tid = block["id"].toString();
                    const QString tname = block["name"].toString();
                    const QJsonObject targs = block["input"].toObject();
                    LOG_INFO(kLlmSvcTag, QString("Anthropic tool loop r%1: %2").arg(round).arg(tname));
                    const QString bare = mcp::McpProvider::parse_openai_function_name(tname).second;
                    detail::emit_tool_progress(bare, targs);
                    auto tr = mcp::McpService::instance().execute_openai_function(tname, targs,
                                                                                  /*allow_defer=*/true);
                    detail::note_tool_activations(bare, targs, tr, activated);
                    more_results.append(QJsonObject{{"type", "tool_result"},
                                                    {"tool_use_id", tid},
                                                    {"content", detail::encode_tool_result_for_llm(bare, tr)}});
                }
                loop_msgs.append(QJsonObject{{"role", "user"}, {"content", more_results}});
            }
            if (resp.content.isEmpty() && resp.error.isEmpty())
                resp.error = "Anthropic tool loop exceeded maximum rounds";
        } else {
            resp.content = extract_anthropic_content_text(content);
        }

    } else if (provider_ == "gemini" || provider_ == "google") {
        QJsonArray cands = rj["candidates"].toArray();
        if (!cands.isEmpty()) {
            QJsonArray parts = cands[0].toObject()["content"].toObject()["parts"].toArray();

            bool has_function_calls = false;
            for (const auto& part_val : parts) {
                if (part_val.toObject().contains("functionCall")) {
                    has_function_calls = true;
                    break;
                }
            }

            if (has_function_calls) {
                LOG_INFO(kLlmSvcTag, "Gemini requested function call(s)");
                // ── Multi-round tool loop ──
                // Gemini's multi-turn contract: user → model(functionCall parts)
                // → user(functionResponse parts) → … Previously this ran a SINGLE
                // follow-up with NO tools attached, so the model could execute one
                // tool and never chain a second. We now loop, re-attaching the tool
                // set each round, until a text answer or the round cap.
                const int kMaxRounds = active_max_tool_rounds();
                // Rebuilt per round, not hoisted: under Tool RAG the declared
                // set grows as tool_list / tool_describe surface tools, and a
                // hoisted array would pin the model to what round 1 could see.
                detail::ActivationTracker activated;

                // Constant conversation prefix: prior history + the user turn.
                QJsonArray fu_contents;
                for (const auto& m : history) {
                    if (m.role == "system")
                        continue;
                    const QString role = (m.role == "assistant") ? "model" : "user";
                    fu_contents.append(
                        QJsonObject{{"role", role}, {"parts", QJsonArray{QJsonObject{{"text", m.content}}}}});
                }
                fu_contents.append(
                    QJsonObject{{"role", "user"}, {"parts", QJsonArray{QJsonObject{{"text", user_message}}}}});

                // model_parts = the model turn whose functionCalls we execute this
                // round; seeded with the initial response's parts.
                QJsonArray model_parts = parts;
                QJsonArray last_response_parts;
                for (int round = 0; round < kMaxRounds; ++round) {
                    fu_contents.append(QJsonObject{{"role", "model"}, {"parts", model_parts}});

                    QJsonArray fn_response_parts;
                    for (const auto& part_val : model_parts) {
                        const QJsonObject part = part_val.toObject();
                        if (!part.contains("functionCall"))
                            continue;
                        const QJsonObject fc = part["functionCall"].toObject();
                        const QString fn_name = fc["name"].toString();
                        const QJsonObject fn_args = fc["args"].toObject();
                        LOG_INFO(kLlmSvcTag, QString("Gemini tool loop r%1: %2").arg(round).arg(fn_name));
                        const QString bare = mcp::McpProvider::parse_openai_function_name(fn_name).second;
                        detail::emit_tool_progress(bare, fn_args);
                        auto tr = mcp::McpService::instance().execute_openai_function(fn_name, fn_args,
                                                                                      /*allow_defer=*/true);
                        detail::note_tool_activations(bare, fn_args, tr, activated);
                        // Gemini requires response to be an object — wrap strings under "result".
                        QJsonObject response_obj;
                        if (!tr.data.isNull() && !tr.data.isUndefined() && tr.data.isObject())
                            response_obj = tr.data.toObject();
                        else if (!tr.message.isEmpty())
                            response_obj["result"] = tr.message;
                        else
                            response_obj = tr.to_json();
                        // Same transcript budget as the other providers — this
                        // path re-posts `fu_contents` wholesale every round too.
                        detail::fit_llm_payload(bare, response_obj);
                        fn_response_parts.append(QJsonObject{
                            {"functionResponse", QJsonObject{{"name", fn_name}, {"response", response_obj}}}});
                    }
                    fu_contents.append(QJsonObject{{"role", "user"}, {"parts", fn_response_parts}});
                    last_response_parts = fn_response_parts;

                    QJsonObject fu_body;
                    fu_body["contents"] = fu_contents;
                    QJsonObject gen_cfg;
                    gen_cfg["maxOutputTokens"] = resolved_max_tokens();
                    fu_body["generationConfig"] = gen_cfg;
                    if (!system_prompt_.isEmpty())
                        fu_body["systemInstruction"] =
                            QJsonObject{{"parts", QJsonArray{QJsonObject{{"text", system_prompt_}}}}};
                    const QJsonArray gem_tools = build_gemini_tools(activated.names());
                    if (!gem_tools.isEmpty())
                        fu_body["tools"] = gem_tools;

                    auto fu = blocking_post(url, fu_body, hdr);
                    if (!fu.success) {
                        resp.error = "Gemini tool follow-up failed: " + fu.error;
                        return resp;
                    }
                    auto fu_doc = QJsonDocument::fromJson(fu.body);
                    if (fu_doc.isNull()) {
                        resp.error = "Gemini tool follow-up parse error";
                        return resp;
                    }
                    const QJsonArray fu_cands = fu_doc.object()["candidates"].toArray();
                    const QJsonArray resp_parts = fu_cands.isEmpty()
                                                      ? QJsonArray{}
                                                      : fu_cands[0].toObject()["content"].toObject()["parts"].toArray();

                    bool more_calls = false;
                    for (const auto& pv : resp_parts) {
                        if (pv.toObject().contains("functionCall")) {
                            more_calls = true;
                            break;
                        }
                    }
                    if (!more_calls) {
                        resp.content = extract_gemini_parts_text(resp_parts); // final answer
                        break;
                    }
                    model_parts = resp_parts; // next round executes these
                }

                if (resp.content.isEmpty()) {
                    // Loop exhausted without a text turn — render the last round's
                    // function responses as readable text so the chat isn't blank.
                    QString fallback;
                    for (const auto& pv : last_response_parts) {
                        const QJsonObject fr = pv.toObject()["functionResponse"].toObject();
                        const QString fn_name = fr["name"].toString();
                        const int sep = fn_name.indexOf("__");
                        const QString short_name = (sep >= 0) ? fn_name.mid(sep + 2) : fn_name;
                        fallback +=
                            "\n**Tool: " + short_name + "**\n" +
                            QString::fromUtf8(QJsonDocument(fr["response"].toObject()).toJson(QJsonDocument::Compact))
                                .left(4000) +
                            "\n";
                    }
                    resp.content = fallback;
                }
            } else {
                resp.content = extract_gemini_parts_text(parts);
            }
        }

    } else {
        // OpenAI-compatible.
        QJsonArray choices = rj["choices"].toArray();
        if (!choices.isEmpty()) {
            QJsonObject msg = choices[0].toObject()["message"].toObject();
            QJsonArray tcs = msg["tool_calls"].toArray();

            if (!tcs.isEmpty()) {
                LOG_INFO(kLlmSvcTag, QString("LLM requested %1 tool calls").arg(tcs.size()));

                QJsonArray loop_msgs;
                if (!system_prompt_.isEmpty())
                    loop_msgs.append(QJsonObject{{"role", "system"}, {"content", system_prompt_}});
                for (const auto& h : history)
                    loop_msgs.append(QJsonObject{{"role", h.role}, {"content", h.content}});
                loop_msgs.append(QJsonObject{{"role", "user"}, {"content", user_message}});
                loop_msgs.append(msg); // assistant turn with tool_calls

                // Tools the model has discovered this turn — seeded from this
                // first round so the loop can re-declare them on the next turn
                // (Tool RAG only ships Tier-0 otherwise; see note_tool_activations).
                detail::ActivationTracker activated;

                for (const auto& tc_val : tcs) {
                    QJsonObject tc = tc_val.toObject();
                    QString call_id = tc["id"].toString();
                    QString fn_name = tc["function"].toObject()["name"].toString();
                    QJsonObject fn_args =
                        QJsonDocument::fromJson(tc["function"].toObject()["arguments"].toString("{}").toUtf8())
                            .object();

                    LOG_INFO(
                        kLlmSvcTag,
                        QString("Executing tool: %1 args=%2")
                            .arg(fn_name,
                                 QString::fromUtf8(QJsonDocument(fn_args).toJson(QJsonDocument::Compact)).left(200)));
                    auto tr =
                        mcp::McpService::instance().execute_openai_function(fn_name, fn_args, /*allow_defer=*/true);
                    LOG_INFO(kLlmSvcTag,
                             QString("Tool %1 -> %2 (msg=%3 err=%4)")
                                 .arg(fn_name, tr.success ? "OK" : "FAIL", tr.message.left(120), tr.error.left(120)));
                    const QString bare = mcp::McpProvider::parse_openai_function_name(fn_name).second;
                    detail::note_tool_activations(bare, fn_args, tr, activated);
                    loop_msgs.append(QJsonObject{{"role", "tool"},
                                                 {"tool_call_id", call_id},
                                                 {"content", detail::encode_tool_result_for_llm(bare, tr)}});
                }

                resp = do_tool_loop(loop_msgs, url, hdr, activated.names());
                parse_usage(resp, rj, provider_);
                return resp;

            } else {
                // Helper handles reasoning models, content-as-parts arrays, and refusal messages.
                resp.content = strip_think_blocks(extract_openai_message_text(msg));
            }
        }
    }

    // ── Detect text-based tool calls ────────────────────────────────────
    // Some models (e.g. minimax, certain OpenRouter models) don't use the
    // structured tool_calls field. Instead they emit tool invocations as
    // XML or custom markup in the text response. Detect these patterns and
    // execute the tools, then ask the LLM to summarize the results.
    if (!resp.content.isEmpty()) {
        LOG_INFO(kLlmSvcTag,
                 "Checking response for text-based tool calls, content starts with: " + resp.content.left(120));
        auto text_tool_result = try_extract_and_execute_text_tool_calls(resp.content, user_message, url, hdr);
        if (text_tool_result.has_value()) {
            resp = text_tool_result.value();
            if (resp.success)
                return resp;
        }
    }

    parse_usage(resp, rj, provider_);
    resp.success = true;
    return resp;
}

LlmResponse LlmService::do_streaming_request(const QString& user_message,
                                             const std::vector<ConversationMessage>& history, StreamCallback on_chunk) {
    // Gemini uses :streamGenerateContent, which does not fit this OpenAI-compat
    // SSE path. Fall back to do_request and emit as one chunk.
    if (provider_ == "gemini" || provider_ == "google") {
        detail::ProgressEmitterGuard pg([on_chunk](const QString& s) { on_chunk(s, false); });
        auto resp = do_request(user_message, history);
        if (resp.success && !resp.content.isEmpty())
            on_chunk(resp.content, false);
        on_chunk("", true);
        return resp;
    }

    LlmResponse resp;
    if (ProviderCatalog::is_blocked(provider_, base_url_)) {
        resp.error = "AtlasCloud has been banned by Fincept and cannot be used.";
        on_chunk("", true);
        return resp;
    }

    QString url = get_endpoint_url();
    if (url.isEmpty()) {
        resp.error = "No endpoint URL for provider: " + provider_;
        on_chunk("", true);
        return resp;
    }

    auto hdr = get_headers();
    // Anthropic streaming handles tools out-of-band so it goes through a different request shape.
    QJsonObject req_body = (provider_ == "anthropic") ? build_anthropic_request(user_message, history, true)
                                                      : build_openai_request(user_message, history, true, true);

    // Heap-allocate the QNAM so we can deleteLater() it and drain
    // DeferredDelete before this function returns — a stack QNAM here used
    // to leak socket-notifier teardown work onto the main runloop and crash
    // hours later inside qt_mac_socket_callback. See eventloop_request().
    auto* nam = new QNetworkAccessManager;
    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Accept", "text/event-stream");
    req.setRawHeader("Cache-Control", "no-cache");
    // Per Qt 6 network docs: HTTP/2 is enabled by default and batches frames,
    // which causes SSE chunks to arrive in bursts instead of as the server
    // flushes them. Force HTTP/1.1 so readyRead fires per TCP packet.
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    for (auto it = hdr.constBegin(); it != hdr.constEnd(); ++it)
        req.setRawHeader(it.key().toUtf8(), it.value().toUtf8());

    QByteArray json_data = QJsonDocument(req_body).toJson(QJsonDocument::Compact);
    QNetworkReply* reply = nam->post(req, json_data);

    QByteArray partial_line;
    QString accumulated;
    QJsonObject final_usage_obj;
    bool done = false;
    bool tool_call_detected = false;

    // <think>…</think> reasoning filter. Reasoning models (MiniMax M2.7,
    // DeepSeek-R1 derivatives, …) stream their chain-of-thought inline in
    // `delta.content` wrapped in <think> tags. The user shouldn't see that —
    // only the answer. State persists across SSE chunks because tags can
    // straddle the chunk boundary ("<thi" + "nk>...").
    // Returns the visible (answer) text; any reasoning found inside <think>…</think>
    // is appended to *think_out so the caller can route it to the Thinking channel
    // rather than dropping it.
    bool in_think = false;
    QString think_pending;
    auto filter_think = [&](const QString& chunk, QString* think_out) -> QString {
        QString out;
        QString work = think_pending + chunk;
        think_pending.clear();
        qsizetype pos = 0;
        const qsizetype n = work.length();
        while (pos < n) {
            if (in_think) {
                qsizetype end = work.indexOf(QStringLiteral("</think>"), pos, Qt::CaseInsensitive);
                if (end < 0) {
                    // Hold last 8 chars in case </think> straddles.
                    qsizetype safe = std::max<qsizetype>(pos, n - 8);
                    if (think_out)
                        *think_out += work.mid(pos, safe - pos);
                    think_pending = work.mid(safe);
                    return out;
                }
                if (think_out)
                    *think_out += work.mid(pos, end - pos);
                in_think = false;
                pos = end + 8;
            } else {
                qsizetype start = work.indexOf(QStringLiteral("<think>"), pos, Qt::CaseInsensitive);
                if (start < 0) {
                    // Emit everything except the last 7 chars (length of "<think>")
                    // which might be a partial open tag.
                    qsizetype safe = std::max<qsizetype>(pos, n - 7);
                    out += work.mid(pos, safe - pos);
                    think_pending = work.mid(safe);
                    return out;
                }
                out += work.mid(pos, start - pos);
                in_think = true;
                pos = start + 7;
            }
        }
        return out;
    };

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.start(120000);

    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, [&]() {
        done = true;
        loop.quit();
    });
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&]() {
        partial_line += reply->readAll();
        while (true) {
            int nl = partial_line.indexOf('\n');
            if (nl < 0)
                break;
            QByteArray raw_line = partial_line.left(nl);
            partial_line.remove(0, nl + 1);

            if (!raw_line.isEmpty() && raw_line.back() == '\r')
                raw_line.chop(1);

            QString line = QString::fromUtf8(raw_line).trimmed();
            if (line.isEmpty() || !line.startsWith("data: "))
                continue;

            QString data = line.mid(6); // strip "data: "
            if (data == "[DONE]") {
                if (!tool_call_detected)
                    on_chunk("", true);
                done = true;
                loop.quit();
                return;
            }

            // OpenAI sends a trailing usage chunk (empty choices[]) when stream_options.include_usage=true.
            {
                auto usage_doc = QJsonDocument::fromJson(data.toUtf8());
                if (!usage_doc.isNull() && usage_doc.isObject()) {
                    QJsonObject uobj = usage_doc.object();
                    if (uobj.contains("usage") && uobj["usage"].isObject())
                        final_usage_obj = uobj["usage"].toObject();
                }
            }

            if (!tool_call_detected) {
                auto doc = QJsonDocument::fromJson(data.toUtf8());
                if (!doc.isNull() && doc.isObject()) {
                    QJsonObject obj = doc.object();

                    if (provider_ == "anthropic") {
                        const QString type = obj["type"].toString();
                        if (type == "content_block_start" &&
                            obj["content_block"].toObject()["type"].toString() == "tool_use") {
                            LOG_INFO(kLlmSvcTag, "STREAM: Anthropic tool_use content_block_start detected");
                            tool_call_detected = true;
                            loop.quit();
                            return;
                        }
                        if (type == "message_delta" &&
                            obj["delta"].toObject()["stop_reason"].toString() == "tool_use") {
                            LOG_INFO(kLlmSvcTag, "STREAM: Anthropic stop_reason=tool_use detected");
                            tool_call_detected = true;
                            loop.quit();
                            return;
                        }
                    }

                    QJsonArray choices = obj["choices"].toArray();
                    if (!choices.isEmpty()) {
                        const QString finish = choices[0].toObject()["finish_reason"].toString();
                        if (finish == "tool_calls") {
                            LOG_INFO(
                                kLlmSvcTag,
                                QString("STREAM: OpenAI-compat finish_reason=tool_calls detected (%1)").arg(provider_));
                            tool_call_detected = true;
                            loop.quit();
                            return;
                        }
                        QJsonObject delta = choices[0].toObject()["delta"].toObject();
                        if (!delta["tool_calls"].isUndefined() && !delta["tool_calls"].isNull()) {
                            LOG_INFO(kLlmSvcTag,
                                     QString("STREAM: OpenAI-compat delta.tool_calls detected (%1)").arg(provider_));
                            tool_call_detected = true;
                            loop.quit();
                            return;
                        }
                    }

                    // Some providers surface tool_calls at top level.
                    if (!obj["tool_calls"].isUndefined() && !obj["tool_calls"].isNull() &&
                        obj["tool_calls"].toArray().size() > 0) {
                        LOG_INFO(kLlmSvcTag, "STREAM: top-level tool_calls detected");
                        tool_call_detected = true;
                        loop.quit();
                        return;
                    }
                }
            }

            const SseDelta delta = parse_sse_chunk(data, provider_);
            if (delta.is_reasoning) {
                // Chain-of-thought (reasoning_content / Anthropic thinking_delta).
                // Route to the UI's separate Thinking channel via the sentinel
                // prefix; never accumulate it into the answer or stored content.
                if (!delta.text.isEmpty())
                    on_chunk(think_stream_prefix() + delta.text, false);
            } else if (!delta.text.isEmpty()) {
                accumulated += delta.text;

                // Some providers stream tool calls as XML/text markup — detect, suppress output, fall back to
                // do_request. Patterns covered: <tool_call>, </tool_call>, <minimax:tool_call>, <invoke name=,
                // ```tool_call code fences, and bare `minimax:tool_call ... /minimax:tool_call`.
                if (!tool_call_detected) {
                    static const QRegularExpression rx_text_tool(
                        QStringLiteral("<\\s*/?\\s*(?:\\w+\\s*:\\s*)?tool_call\\b"
                                       "|<\\s*invoke\\s+name="
                                       "|```\\s*tool_call\\b"
                                       "|\\b\\w+\\s*:\\s*tool_call\\b"),
                        QRegularExpression::CaseInsensitiveOption);
                    if (rx_text_tool.match(accumulated).hasMatch()) {
                        tool_call_detected = true;
                        LOG_INFO(kLlmSvcTag,
                                 "Tool call markup detected in streamed text — falling back to non-streaming");
                        loop.quit();
                        return;
                    }
                }

                // Inline <think>…</think> reasoning (MiniMax M2.7, DeepSeek-R1
                // derivatives) gets split out here: think_seg → Thinking channel,
                // visible → answer bubble.
                QString think_seg;
                const QString visible = filter_think(delta.text, &think_seg);
                if (!think_seg.isEmpty())
                    on_chunk(think_stream_prefix() + think_seg, false);
                if (!visible.isEmpty())
                    on_chunk(visible, false);
            }
        }
    });

    loop.exec();
    timeout.stop();

    if (!in_think && !think_pending.isEmpty())
        on_chunk(think_pending, false);
    think_pending.clear();

    auto drain_nam = [&]() {
        reply->deleteLater();
        nam->deleteLater();
        // Pump DeferredDelete on this thread so socket-notifier teardown
        // completes while a dispatcher still exists — prevents the
        // qt_mac_socket_callback use-after-free on the main thread later.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    };

    if (tool_call_detected) {
        LOG_INFO(kLlmSvcTag, "Tool call detected in stream — falling back to tool loop");
        reply->abort();
        drain_nam();

        // Sentinel tells the chat screen to reset the bubble before the real reply arrives.
        on_chunk("\x01__TOOL_CALL_CLEAR__", false);

        // Wire per-tool progress into the chat bubble so the user sees something
        // during the multi-second tool execution phase instead of a blank wait.
        detail::ProgressEmitterGuard pg([on_chunk](const QString& s) { on_chunk(s, false); });
        auto tool_resp = do_request(user_message, history);
        if (tool_resp.success && !tool_resp.content.isEmpty())
            on_chunk(QStringLiteral("\n") + tool_resp.content, false);
        on_chunk("", true);
        return tool_resp;
    }

    int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError) {
        // A non-2xx (e.g. 400 from AIHubMix routing to a model that rejects a
        // param) carries a JSON error body that explains why. The SSE reader
        // never parses it because it isn't a "data:" line, so pull it from the
        // unconsumed buffer + any remainder and surface the real reason instead
        // of Qt's opaque "server replied: Bad Request".
        const QByteArray err_body = partial_line + reply->readAll();
        const QString server_msg = parse_server_error_message(err_body);
        if (!server_msg.isEmpty())
            resp.error = status > 0 ? QString("HTTP %1: %2").arg(status).arg(server_msg) : server_msg;
        else
            resp.error = reply->errorString();
        LOG_ERROR(kLlmSvcTag, "Stream request failed: " + resp.error);
    } else if (status >= 200 && status < 300) {
        // `accumulated` holds only answer deltas (reasoning_content was routed to
        // the Thinking channel and never added here). Strip any inline <think>…
        // </think> blocks so the persisted/replayed content is the answer alone.
        resp.content = strip_think_blocks(accumulated);
        resp.success = true;
        if (!final_usage_obj.isEmpty()) {
            QJsonObject wrap{{"usage", final_usage_obj}};
            parse_usage(resp, wrap, provider_);
        }
    } else {
        resp.error = QString("HTTP %1").arg(status);
    }

    // On the timeout path we fall straight out of `loop.exec()` with the reply
    // still streaming, and drain_nam() makes the delete synchronous — tearing
    // the reply down from under Qt's HTTP machinery. Abort first, as the
    // sibling helper does (LlmHttpHelpers.cpp:120). No-op when already finished.
    if (!reply->isFinished())
        reply->abort();
    drain_nam();

    if (!done)
        on_chunk("", true);

    return resp;
}
// ============================================================================
// Public API
// ============================================================================

LlmResponse LlmService::chat(const QString& user_message, const std::vector<ConversationMessage>& history,
                             bool use_tools) {
    // INTERIM serialization guard (see request_serialize_mutex_): held until return
    // because do_request() reads provider_/api_key_/base_url_/model_ lock-free across
    // the network call — a concurrent chat()/chat_streaming() worker rewriting them
    // mid-flight could send one provider's API key to another provider's base_url.
    // We run on a background thread, so this serializes LLM workers, not the UI.
    // Proper fix (deferred): thread a per-request config context through do_request.
    QMutexLocker request_lock(&request_serialize_mutex_);
    {
        QMutexLocker lock(&mutex_);
        ensure_config();
        if (provider_.isEmpty())
            return LlmResponse{.content = {}, .error = "No LLM provider configured"};
    }
    // Helpers read members directly. ensure_config() already wrote them under
    // mutex_, and request_lock keeps other workers from rewriting them before we
    // return. Reassigning a local snapshot back to the members after unlocking
    // mutex_ would clobber any reload_config() that landed in the gap.

    // thread_local guard avoids racing with concurrent chat_streaming calls from the floating bubble.
    detail::ToolPolicyGuard guard(use_tools ? ToolPolicy::All : ToolPolicy::None);
    return do_request(user_message, history);
}

void LlmService::chat_streaming(const QString& user_message, const std::vector<ConversationMessage>& history,
                                StreamCallback on_chunk, bool use_tools) {
    chat_streaming(user_message, history, std::move(on_chunk), use_tools ? ToolPolicy::All : ToolPolicy::None);
}

void LlmService::chat_streaming(const QString& user_message, const std::vector<ConversationMessage>& history,
                                StreamCallback on_chunk, ToolPolicy policy, const QString& chat_session_id) {
    QString p, k, b, m, sp;
    double t;
    int mx;
    {
        QMutexLocker lock(&mutex_);
        ensure_config();
        p = provider_;
        k = api_key_;
        b = base_url_;
        m = model_;
        sp = system_prompt_;
        t = temperature_;
        mx = max_tokens_;
    }

    if (p.isEmpty()) {
        on_chunk("", true);
        emit finished_streaming(LlmResponse{.content = {}, .error = "No LLM provider configured"});
        return;
    }

    std::vector<ConversationMessage> history_copy = history;

    QPointer<LlmService> self = this;
    // Shadow-publish every chunk to DataHub under a per-stream session id so observer panels
    // can tail `llm:session:*:stream` without being wired in. Push-only, coalesced.
    const QString stream_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString stream_topic = QStringLiteral("llm:session:") + stream_id + QStringLiteral(":stream");
    [[maybe_unused]] static bool policy_once = []() {
        auto& hub = fincept::datahub::DataHub::instance();
        fincept::datahub::TopicPolicy policy;
        policy.push_only = true;
        policy.coalesce_within_ms = 50;
        policy.ttl_ms = 5 * 60 * 1000;
        hub.set_policy_pattern(QStringLiteral("llm:session:*"), policy);
        LOG_INFO("LlmService", "Registered DataHub policy for llm:session:*");
        return true;
    }();
    // Skip on_chunk if LlmService dies before the chunk arrives.
    StreamCallback guarded_chunk = [self, on_chunk, stream_id, stream_topic](const QString& chunk, bool done) {
        if (!self)
            return;
        on_chunk(chunk, done);
        QJsonObject payload{
            {"session_id", stream_id},
            {"chunk", chunk},
            {"done", done},
        };
        auto& hub = fincept::datahub::DataHub::instance();
        hub.publish(stream_topic, QVariant(payload));
        if (done) {
            // Subscribers stay attached but cached state is discarded.
            hub.retire_topic(stream_topic);
        }
    };
    (void)QtConcurrent::run(
        [self, p, k, b, m, sp, t, mx, user_message, history_copy, guarded_chunk, policy, chat_session_id]() {
            if (!self)
                return;

            // INTERIM serialization guard (see request_serialize_mutex_): held until
            // this worker finishes so a concurrent chat()/chat_streaming() worker
            // can't rewrite the shared provider/key/url/model members below while
            // do_streaming_request() reads them lock-free — that could send one
            // provider's API key to another provider's base_url. QtConcurrent thread
            // only; the UI is never blocked. Proper fix (deferred): thread a
            // per-request config context through do_streaming_request.
            QMutexLocker request_lock(&self->request_serialize_mutex_);

            // Snapshot under mutex so do_*_request see consistent state and don't race with reload_config().
            {
                QMutexLocker lock(&self->mutex_);
                self->provider_ = p;
                self->api_key_ = k;
                self->base_url_ = b;
                self->model_ = m;
                self->system_prompt_ = sp;
                self->temperature_ = t;
                self->max_tokens_ = mx;
            }

            detail::ToolPolicyGuard guard(policy);
            detail::ChatSessionGuard session_guard(chat_session_id);
            auto resp = self->do_streaming_request(user_message, history_copy, guarded_chunk);

            if (self) {
                QMetaObject::invokeMethod(
                    self,
                    [self, resp]() {
                        if (self)
                            emit self->finished_streaming(resp);
                    },
                    Qt::QueuedConnection);
            }
        });
}

} // namespace fincept::ai_chat
