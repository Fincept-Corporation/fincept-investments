// ModelCatalog.cpp — see header for design rationale.
//
// All numbers verified against provider docs on 2026-04-28. Source URLs are
// in the comment above each entry. When a model isn't listed here we return
// 0 and the caller falls back to a conservative default.

#include "services/llm/ModelCatalog.h"

#include <QRegularExpression>

namespace fincept::ai_chat {

namespace {

struct CatalogEntry {
    const char* provider;   // exact match, lowercase
    const char* model_glob; // case-insensitive glob (* and ? wildcards)
    int output_cap_tokens;
};

// Sentinel used in the catalog where the provider does not publish a
// separate output cap (e.g. xAI, Moonshot, Ollama). We pick a generous
// default that's still below typical context windows so we don't waste
// budget on chat replies. Callers can override via user-set max_tokens.
constexpr int kNoPublishedCap = 16384;

// First match wins. Order from most-specific to least-specific within each
// provider. Catch-all for the provider goes last.
const CatalogEntry kCatalog[] = {
    // ── OpenAI ──────────────────────────────────────────────────────────
    // https://developers.openai.com/api/docs/models/<model>
    {"openai", "gpt-5*", 128000},  // gpt-5 base / 5.2 / 5.4 / 5.5
    {"openai", "gpt-4.1*", 32768}, // gpt-4.1, gpt-4.1-mini
    {"openai", "gpt-4o*", 16384},  // gpt-4o, gpt-4o-mini
    {"openai", "gpt-4-turbo*", 4096},
    {"openai", "o1-mini*", 65536},
    {"openai", "o1-pro*", 100000},
    {"openai", "o1*", 100000}, // o1 base, o1-preview
    {"openai", "o3-mini*", 100000},
    {"openai", "o3*", 100000},
    {"openai", "*", 4096}, // catch-all (chat/legacy)

    // ── Anthropic ───────────────────────────────────────────────────────
    // https://platform.claude.com/docs/en/docs/about-claude/models/overview
    // Sync Messages API caps. Batch API beta supports up to 300k via
    // header `output-300k-2026-03-24` — not used by interactive chat.
    {"anthropic", "claude-opus-4-7*", 128000},
    {"anthropic", "claude-opus-4-6*", 128000},
    {"anthropic", "claude-opus-4*", 64000}, // 4-5 and earlier 4.x
    {"anthropic", "claude-sonnet-4*", 64000},
    {"anthropic", "claude-haiku-4*", 64000},
    {"anthropic", "claude-3-5-sonnet*", 8192}, // legacy
    {"anthropic", "claude-3-5-haiku*", 8192},
    {"anthropic", "claude-3-opus*", 4096},
    {"anthropic", "claude-3*", 4096},
    {"anthropic", "*", 8192},

    // ── Google Gemini ───────────────────────────────────────────────────
    // https://ai.google.dev/gemini-api/docs/models/<model>
    // Note: 2.5 series budget is shared between thinking and visible output.
    {"gemini", "gemini-2.5-pro*", 65536},
    {"gemini", "gemini-2.5-flash*", 65536},
    {"gemini", "gemini-2.5*", 65536},
    {"gemini", "gemini-2.0-flash*", 8192},
    {"gemini", "gemini-2.0*", 8192},
    {"gemini", "gemini-1.5*", 8192}, // retired but still works on some keys
    {"gemini", "*", 8192},
    // Alias used by some user configs
    {"google", "gemini-2.5*", 65536},
    {"google", "gemini-2.0*", 8192},
    {"google", "*", 8192},

    // ── xAI (Grok) ──────────────────────────────────────────────────────
    // https://docs.x.ai/developers/models
    // No published per-model output cap — bounded by context. Use a
    // generous default; user can override.
    {"xai", "grok-4*", kNoPublishedCap}, // 256k context
    {"xai", "grok-3*", kNoPublishedCap}, // 131k context
    {"xai", "grok-2*", kNoPublishedCap},
    {"xai", "*", kNoPublishedCap},

    // ── Groq ────────────────────────────────────────────────────────────
    // https://console.groq.com/docs/model/<id>
    {"groq", "llama-3.3-70b*", 32768},
    {"groq", "llama-3.1-8b-instant*", 131072}, // ctx==output
    {"groq", "meta-llama/llama-4-scout*", 8192},
    {"groq", "meta-llama/llama-4-maverick*", 8192},
    {"groq", "llama-4*", 8192},
    {"groq", "gemma2-9b-it*", 8192},
    {"groq", "deepseek-r1-distill*", 32768}, // shared with reasoning
    {"groq", "qwen*", 32768},
    {"groq", "mixtral*", 8192}, // legacy
    {"groq", "*", 8192},

    // ── DeepSeek ────────────────────────────────────────────────────────
    // https://api-docs.deepseek.com/quick_start/pricing
    // V4 generation publishes 384k unified; deepseek-chat / -reasoner now
    // alias to v4. We cap at 65536 for interactive use — the model can
    // emit far more but we don't want a chat tab to consume 384k of budget
    // by accident. User override applies.
    {"deepseek", "deepseek-v4*", 65536},
    {"deepseek", "deepseek-reasoner*", 65536},
    {"deepseek", "deepseek-chat*", 65536},
    {"deepseek", "*", 16384},

    // ── Moonshot / Kimi ─────────────────────────────────────────────────
    // https://platform.kimi.ai/docs/pricing/chat-k26.md
    // No published hard output cap — output bounded by 256k context window
    // minus prompt. Pick a generous interactive default; thinking variants
    // get more headroom.
    {"kimi", "kimi-k2-thinking*", 131072},
    {"kimi", "kimi-k2.6*", 65536},
    {"kimi", "kimi-k2.5*", 65536},
    {"kimi", "kimi-k2-0905*", 65536},
    {"kimi", "kimi-k2*", 65536},
    {"kimi", "moonshot-v1-128k*", 100000},
    {"kimi", "moonshot-v1-32k*", 30000},
    {"kimi", "moonshot-v1-8k*", 8000},
    {"kimi", "*", 32768},

    // ── MiniMax ─────────────────────────────────────────────────────────
    // https://platform.minimax.io/docs/api-reference/text-chat-openai
    // OpenAI-compat endpoint hard-caps at 2048 for M2.7/M2.5/M2.1; native
    // v2 endpoint allows higher. We use the conservative number for the
    // OpenAI-compat path (which is what LlmService talks to).
    {"minimax", "minimax-m2.7*", 2048},
    {"minimax", "minimax-m2.5*", 2048},
    {"minimax", "minimax-m2.1*", 2048},
    {"minimax", "minimax-m2*", 8192}, // M2 native is higher; OpenAI-compat may clamp
    {"minimax", "minimax-m1*", 8192},
    {"minimax", "minimax-text*", 2048},
    {"minimax", "*", 2048},

    // ── OpenRouter (pass-through) ───────────────────────────────────────
    // https://openrouter.ai/docs/api/reference/parameters
    // OpenRouter doesn't impose a global cap — the upstream model's cap
    // applies. We can't know which provider routes a given OpenRouter
    // model id without a /api/v1/models lookup, so use a sane default.
    {"openrouter", "*", kNoPublishedCap},

    // ── Ollama (local) ──────────────────────────────────────────────────
    // https://docs.ollama.com/api/generate
    // num_predict default is -1 (unbounded). Effective ceiling is num_ctx,
    // which defaults to 4096 unless overridden. We send max_tokens via the
    // OpenAI-compat shim; pick a value that won't overflow num_ctx for a
    // typical local setup.
    {"ollama", "*", 4096},

    // ── Astraflow (UCloud — OpenAI-compatible aggregator) ───────────────────
    // https://astraflow.ucloud-global.com  /  https://astraflow.ucloud.cn
    // Supports 200+ models; output cap varies by underlying model. Use a
    // generous default; user can override via max_tokens.
    {"astraflow", "*", kNoPublishedCap},
    {"astraflow_cn", "*", kNoPublishedCap},

    // ── AIHubMix (OpenAI-compatible aggregator) ─────────────────────────────
    // https://aihubmix.com/models — routes 500+ upstream models; per-model cap
    // varies and we can't know the route from the id, so use a generous default
    // (user max_tokens still applies).
    {"aihubmix", "*", kNoPublishedCap},

    // ── Fincept (proxies upstream) ──────────────────────────────────────
    // Fincept's /research/llm/async wraps various upstream models. We
    // don't know which one is selected server-side, so go with a generous
    // default that most upstreams accept.
};

// Glob → regex. Supports * (zero-or-more) and ? (single char). Anchored.
QRegularExpression compile_glob(const QString& glob) {
    QString rx = "^";
    for (QChar c : glob) {
        if (c == '*') {
            rx += ".*";
        } else if (c == '?') {
            rx += '.';
        } else if (QString(".\\+^$()[]{}|").contains(c)) {
            rx += '\\';
            rx += c;
        } else {
            rx += c;
        }
    }
    rx += '$';
    return QRegularExpression(rx, QRegularExpression::CaseInsensitiveOption);
}

} // namespace

int ModelCatalog::output_cap(const QString& provider, const QString& model) {
    const QString p = provider.toLower();
    for (const auto& e : kCatalog) {
        if (p != QString::fromLatin1(e.provider))
            continue;
        QRegularExpression rx = compile_glob(QString::fromLatin1(e.model_glob));
        if (rx.match(model).hasMatch())
            return e.output_cap_tokens;
    }
    return 0;
}

} // namespace fincept::ai_chat
