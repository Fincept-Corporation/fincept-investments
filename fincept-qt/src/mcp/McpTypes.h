#pragma once
// McpTypes.h — Core type definitions for the Model Context Protocol system (Qt port)
// Translated from fincept-cpp mcp_types.h — std::string → QString, nlohmann::json → QJsonObject

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QPromise>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace fincept::mcp {

/// Default hard timeout for MCP tool handlers (ms). Single source of truth for
/// the 30s default referenced by ToolContext, ToolDef, and McpProvider.
constexpr int kMcpDefaultTimeoutMs = 30000;

/// How long an LLM-originated call to a `supports_async` tool waits for an
/// inline result before it converts to a background job and hands back a
/// receipt. Tuned so genuinely fast paths (warm cache, cheap query) still
/// answer in one round-trip while a real long-runner backgrounds promptly.
/// Overridable via the `mcp/job_grace_ms` setting.
constexpr int kMcpJobGraceMs = 2500;

// ============================================================================
// Tool Result — returned by tool handlers
// ============================================================================

struct ToolResult {
    bool success = false;
    QJsonValue data; // arbitrary result data
    QString message; // human-readable message
    QString error;   // error message if !success

    QJsonObject to_json() const {
        QJsonObject j;
        j["success"] = success;
        if (!data.isNull() && !data.isUndefined())
            j["data"] = data;
        if (!message.isEmpty())
            j["message"] = message;
        if (!error.isEmpty())
            j["error"] = error;
        return j;
    }

    static ToolResult ok(const QString& msg, const QJsonValue& data = QJsonValue()) {
        ToolResult r;
        r.success = true;
        r.message = msg;
        r.data = data;
        return r;
    }

    static ToolResult ok_data(const QJsonValue& data) {
        ToolResult r;
        r.success = true;
        r.data = data;
        return r;
    }

    static ToolResult fail(const QString& err) {
        ToolResult r;
        r.success = false;
        r.error = err;
        return r;
    }
};

// ============================================================================
// Tool Handler — function signatures for tool execution
// ============================================================================
//
// Two handler shapes are supported on the same ToolDef:
//
//   1. SYNC (`ToolHandler`) — returns ToolResult by value. Pre-Phase-4
//      shape; ~237 tools still use it. Suitable for handlers that finish
//      in microseconds (registry/cache lookups, EventBus publishes).
//      Long-running sync handlers block the calling thread and risk
//      starving the worker pool.
//
//   2. ASYNC (`AsyncToolHandler`) — Phase 4. Receives a ToolContext
//      (cancellation, progress, timeout) and a QPromise<ToolResult> the
//      handler resolves when work completes. Handler returns immediately;
//      the promise is fulfilled later from any thread. Use this for any
//      handler that calls Python, HTTP, or otherwise waits on I/O.
//
// McpProvider::call_tool dispatches to whichever shape is set on the
// ToolDef; if both are set, async wins. McpProvider::call_tool_async
// returns a QFuture<ToolResult> for either shape — sync handlers are
// wrapped in an immediately-resolved future.

using ToolHandler = std::function<ToolResult(const QJsonObject& args)>;

/// Context passed to async handlers. Each handler receives a fresh ToolContext
/// per invocation; capturing it by value is safe.
struct ToolContext {
    /// Optional progress callback. Handlers call this with progress in [0,1]
    /// and a short status message. Safe to call from any thread.
    /// May be null — check before calling.
    std::function<void(double progress, const QString& message)> on_progress;

    /// Returns true if the caller has signalled cancellation. Handlers
    /// should poll periodically and stop work / resolve the promise with
    /// `ToolResult::fail("cancelled")` when set. May be null — treat as
    /// "never cancelled" in that case.
    std::function<bool()> is_cancelled;

    /// Hard timeout in milliseconds. The provider arms a timer that
    /// resolves the promise with a timeout error if the handler hasn't
    /// finished by then. Default: 30s; per-tool override via
    /// ToolDef::default_timeout_ms; per-call override via _meta.timeout_ms.
    int timeout_ms = kMcpDefaultTimeoutMs;

    /// Convenience — true if cancellation hook is set AND signalled.
    bool cancelled() const { return is_cancelled && is_cancelled(); }
};

/// Async handler signature. The handler MUST eventually:
///   - call promise->addResult(result) followed by promise->finish(); or
///   - call promise->finish() with no result (caller treats as fail).
///
/// The promise is heap-allocated and shared so the handler can capture it
/// by value into lambdas / network callbacks.
using AsyncToolHandler =
    std::function<void(const QJsonObject& args, ToolContext ctx, std::shared_ptr<QPromise<ToolResult>> promise)>;

// ============================================================================
// Tool Input Schema — JSON Schema 2020-12 subset
// ============================================================================
//
// Phase 3 of the MCP refactor (see plans/mcp-refactor-phase-3-schema-validation.md).
//
// Two registration shapes are supported and may coexist on the same ToolDef:
//
//   1. LEGACY — set `properties` (QJsonObject) and `required` (QStringList)
//      directly. This is the pre-Phase-3 shape; ~237 tools still use it.
//      The validator (mcp::validate_args, McpProvider::call_tool) inspects
//      `properties` recursively for type / required and accepts JSON
//      Schema 2020-12 keywords inline.
//
//   2. STRUCTURED — populate `params` via ToolSchemaBuilder. Each param
//      declares its type, description, defaults, enum, and bounds in a
//      typed C++ struct. to_json() merges the two shapes when serialising
//      for the LLM, with `params` taking precedence on key collisions.
//
// All tools migrate to the STRUCTURED shape over Phase 3 batches A–E.
// Until they do, both shapes work side by side.

struct ToolParam {
    QString type = "string"; // "string" | "integer" | "number" | "boolean" | "array" | "object"
    QString description;
    bool required = false;
    QJsonValue default_value; // null/undefined if no default
    QStringList enum_values;  // string enums; empty if not enumerated
    std::optional<double> minimum;
    std::optional<double> maximum;
    std::optional<int> min_length; // for strings
    std::optional<int> max_length; // for strings
    QString pattern;               // regex string for strings
    QJsonObject items;             // schema for array elements (raw JSON)

    QJsonObject to_json() const {
        QJsonObject j;
        j["type"] = type;
        if (!description.isEmpty())
            j["description"] = description;
        if (!default_value.isNull() && !default_value.isUndefined())
            j["default"] = default_value;
        if (!enum_values.isEmpty()) {
            QJsonArray arr;
            for (const auto& v : enum_values)
                arr.append(v);
            j["enum"] = arr;
        }
        if (minimum.has_value())
            j["minimum"] = *minimum;
        if (maximum.has_value())
            j["maximum"] = *maximum;
        if (min_length.has_value())
            j["minLength"] = *min_length;
        if (max_length.has_value())
            j["maxLength"] = *max_length;
        if (!pattern.isEmpty())
            j["pattern"] = pattern;
        if (!items.isEmpty())
            j["items"] = items;
        return j;
    }
};

struct ToolSchema {
    QString type = "object";

    // Legacy shape — raw JSON Schema fragment per param.
    QJsonObject properties; // { "param": { "type": "string", "description": "..." } }

    // Structured shape (Phase 3) — typed param declarations.
    QHash<QString, ToolParam> params;

    QStringList required; // legacy callers may set this directly; migrated callers
                          // get it derived from params marked required=true.

    QJsonObject to_json() const {
        QJsonObject j;
        j["type"] = type;

        // Merge legacy `properties` with structured `params`. On key conflict,
        // structured wins (it's the migrated source of truth).
        QJsonObject merged_props = properties;
        for (auto it = params.constBegin(); it != params.constEnd(); ++it)
            merged_props[it.key()] = it.value().to_json();
        j["properties"] = merged_props;

        // Required = explicit list ∪ params marked required=true. Dedup.
        QStringList all_required = required;
        for (auto it = params.constBegin(); it != params.constEnd(); ++it) {
            if (it.value().required && !all_required.contains(it.key()))
                all_required.append(it.key());
        }
        if (!all_required.isEmpty()) {
            QJsonArray req;
            for (const auto& r : all_required)
                req.append(r);
            j["required"] = req;
        }
        return j;
    }
};

// ============================================================================
// Authorization — Phase 6 per-tool gates
// ============================================================================
//
// Every tool declares an AuthLevel; McpProvider::call_tool checks it before
// invoking the handler. Defaults to None.
//
// This is a DANGEROUS-ACTION gate, not a login gate — there are no user
// accounts. The Authenticated / Verified / Subscribed levels went with them.
//
// AuthLevel.None            — no gate
// AuthLevel.ExplicitConfirm — requires the user to OK each call (modal dialog
//                             wired in Phase 6.12). Pair with is_destructive
//                             to force the modal regardless of the session
//                             capability grant.

enum class AuthLevel {
    None = 0,
    ExplicitConfirm = 1,
};

inline const char* auth_level_str(AuthLevel a) {
    switch (a) {
        case AuthLevel::None:
            return "none";
        case AuthLevel::ExplicitConfirm:
            return "explicit_confirm";
    }
    return "unknown";
}

// ============================================================================
// Tool Definition — a registered MCP tool
// ============================================================================

struct ToolDef {
    QString name;
    QString description;
    QString category; // navigation, trading, portfolio, market-data, analytics, system, exchange
    ToolSchema input_schema;

    // Phase 4: dual-shape handler. Exactly one of these should be set per
    // tool. If both are set, async wins. If neither is set the tool is
    // unusable and McpProvider::call_tool returns "Tool has no handler".
    ToolHandler handler;            // legacy synchronous
    AsyncToolHandler async_handler; // new — for I/O-bound work

    bool enabled = true;

    /// Hard timeout for async handlers in milliseconds. Sync handlers ignore
    /// this — they block as long as they need. Override with smaller values
    /// for snappy tools (e.g. registry lookups, paper-trading orders) and
    /// larger for slow ones (e.g. analytics scripts that run a full backtest).
    /// Per-call overrides are merged from `_meta.timeout_ms` (Phase 6).
    int default_timeout_ms = kMcpDefaultTimeoutMs;

    /// Opt-in to the async job protocol. When true, an LLM-originated call that
    /// hasn't finished within the grace window (see `kMcpJobGraceMs`) returns a
    /// `{job_id, status:"running"}` receipt instead of blocking, and the work
    /// continues in the background under `JobRegistry`. The model then polls
    /// `job_status` / collects with `job_result`.
    ///
    /// Set this on tools whose p95 runtime is measured in tens of seconds —
    /// agent runs, backtests, bulk EDGAR pulls. Leave it false for everything
    /// else: a fast tool that returns a receipt costs the model an extra
    /// round-trip for no benefit. Tools that finish inside the grace window
    /// return their real result inline regardless, so setting it on a
    /// sometimes-slow tool is safe.
    ///
    /// Requires `async_handler` — a sync handler cannot be interrupted or
    /// backgrounded, so the flag is ignored for those.
    bool supports_async = false;

    // ── Phase 6: Authorization ──────────────────────────────────────────
    /// Required auth level — checked by McpProvider::call_tool before the
    /// handler runs. Defaults to None (no gate) so untagged tools behave
    /// as before; Phase 6.4 elevates dangerous tools.
    AuthLevel auth_required = AuthLevel::None;

    /// True for state-mutating tools that should always prompt the user
    /// (e.g. set_setting, set_active_llm, run_python_script, place_order,
    /// delete_*, file ops). The Phase 6.12 modal triggers regardless of
    /// auth_required when this is set.
    bool is_destructive = false;

    // ── Phase 6: Naming convention migration ────────────────────────────
    /// Old names this tool used to have. Lets us migrate names without
    /// breaking existing LLM workflows / saved chats — McpProvider's name
    /// resolver tries each alias if the canonical name doesn't match.
    /// New tools leave this empty.
    QStringList legacy_aliases;
};

// ============================================================================
// Tool Filter — Phase 6 catalogue subsetting
// ============================================================================
//
// Today's `format_tools_for_openai()` returns ALL ~237 enabled tools on
// every LLM request — ~100-150 KB of schema per turn. ToolFilter lets
// callers (LlmService dispatch path, agent tooling, future settings UI)
// scope the catalogue per-call so:
//
//   • prompt cost shrinks for non-cached providers
//   • the LLM has fewer distractors when picking a tool
//   • dangerous categories can be excluded by default and lazy-discovered
//     via the tool_list / tool_describe meta-tools (Phase 6.2).

struct ToolFilter {
    /// Inclusive — only tools whose category matches one of these are kept.
    /// Empty = no inclusion filter (all categories pass).
    QStringList categories;

    /// Exclusive — drop tools whose category matches one of these. Applied
    /// AFTER `categories` inclusion.
    QStringList exclude_categories;

    /// Regex include filter on tool name. Empty = no include filter.
    QStringList name_patterns;

    /// Regex exclude filter on tool name. Applied after include.
    QStringList exclude_name_patterns;

    /// Hard cap on returned tool count. 0 = no cap.
    /// Truncation happens after all other filters have run, in the order
    /// tools were registered (effectively: most-recently-registered last).
    int max_tools = 0;
};

// ============================================================================
// Server Status — for external MCP server management
// ============================================================================

enum class ServerStatus { Stopped, Starting, Running, Error };

inline const char* server_status_str(ServerStatus s) {
    switch (s) {
        case ServerStatus::Stopped:
            return "stopped";
        case ServerStatus::Starting:
            return "starting";
        case ServerStatus::Running:
            return "running";
        case ServerStatus::Error:
            return "error";
    }
    return "unknown";
}

// ============================================================================
// External Tool — tool discovered from an external MCP server
// ============================================================================

struct ExternalTool {
    QString server_id;
    QString server_name;
    QString name;
    QString description;
    QJsonObject input_schema;
};

// ============================================================================
// Unified Tool — merged view for consumers (Chat, Agents, Node Editor)
// ============================================================================

struct UnifiedTool {
    QString server_id; // "fincept-terminal" for internal tools
    QString server_name;
    QString name;
    QString description;
    QJsonObject input_schema;
    bool is_internal = false;
    QString category;            // Phase 6: enables ToolFilter category include/exclude
    bool is_destructive = false; // Tool RAG / tool_list surfacing — flag mutating tools
};

// ============================================================================
// Constants
// ============================================================================

inline constexpr const char* INTERNAL_SERVER_ID = "fincept-terminal";
inline constexpr const char* INTERNAL_SERVER_NAME = "Fincept Terminal";

} // namespace fincept::mcp
