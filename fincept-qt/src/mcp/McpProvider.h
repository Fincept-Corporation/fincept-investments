#pragma once
// McpProvider.h — Internal tool registry and executor (Qt port)
// Singleton managing all built-in terminal tools.
// Tools register handlers invoked by AI chat, agents, or node editor.

#include "mcp/McpTypes.h"

#include <QFuture>
#include <QHash>
#include <QMutex>
#include <QSet>

#include <optional>
#include <vector>

namespace fincept::mcp {

class McpProvider {
  public:
    static McpProvider& instance();

    // ── Tool Registration ──────────────────────────────────────────────────

    void register_tool(ToolDef tool);
    void register_tools(std::vector<ToolDef> tools);
    void unregister_tool(const QString& name);

    // ── Tool Enable/Disable ────────────────────────────────────────────────

    void set_tool_enabled(const QString& name, bool enabled);
    bool is_tool_enabled(const QString& name) const;

    // ── Tool Discovery ─────────────────────────────────────────────────────

    /// List all enabled tools as UnifiedTool (for LLM consumption)
    std::vector<UnifiedTool> list_tools() const;

    /// List ALL tools including disabled (for management UI)
    std::vector<UnifiedTool> list_all_tools() const;

    std::size_t tool_count() const;
    bool has_tool(const QString& name) const;

    /// O(1) lookup of a single tool's UnifiedTool snapshot by canonical name.
    /// Returns `std::nullopt` if the name is unknown or disabled. Used by
    /// tool_describe so the hot tool-pick path doesn't pay the O(N) cost of
    /// list_tools()+linear-scan (was ~3 ms p95 across the 583-tool catalog).
    std::optional<UnifiedTool> find_tool(const QString& name) const;

    /// Audit-friendly snapshot of one tool. Carries the bits the self-test /
    /// management UI need to verify wiring — crucially handler-presence, which
    /// the LLM-facing UnifiedTool snapshot omits. Does not expose the handler
    /// std::functions themselves (kept inside the registry).
    struct ToolAuditInfo {
        QString name;
        QString category;
        QString description;
        bool has_handler = false; // sync OR async handler is set
        bool is_async = false;    // async_handler is set
        bool enabled = true;
        bool is_destructive = false;
        AuthLevel auth_required = AuthLevel::None;
        QJsonObject input_schema; // serialised JSON Schema
        QStringList legacy_aliases;
    };

    /// One ToolAuditInfo per registered tool (enabled and disabled). Used by
    /// the headless tool self-test to verify every tool has a handler, a valid
    /// schema, and a usable description without invoking anything.
    std::vector<ToolAuditInfo> audit_all_tools() const;

    // ── Tool Execution ─────────────────────────────────────────────────────

    /// Synchronous execution. For tools registered with `handler` (legacy),
    /// invokes directly and returns. For tools registered with `async_handler`,
    /// dispatches asynchronously and waits for the future to complete (with
    /// the tool's default_timeout_ms applied). Validation runs first via
    /// mcp::validate_args; failures short-circuit before the handler.
    ToolResult call_tool(const QString& name, const QJsonObject& args);

    /// Phase 4: asynchronous execution. Returns a QFuture that resolves with
    /// the ToolResult. For sync handlers, returns an already-resolved future
    /// (the handler runs on the calling thread). For async handlers, builds
    /// a ToolContext (cancellation hook + timeout from the per-call ctx or
    /// the tool's default), invokes the handler, and arms a timeout timer.
    ///
    /// Pass an empty ToolContext to get the tool's defaults. Pass a populated
    /// one to override timeout / inject a cancellation hook (Phase 5 will
    /// thread cancellation tokens through here).
    QFuture<ToolResult> call_tool_async(const QString& name, const QJsonObject& args, ToolContext ctx = {});

    /// Long-running-task entry point, for LLM-originated calls only.
    ///
    /// Behaves exactly like `call_tool` for tools that haven't opted into the
    /// job protocol (`ToolDef::supports_async`), so the ~580 existing tools and
    /// every internal C++ / workflow / agent-bridge caller are unaffected.
    ///
    /// For a tool that HAS opted in: the call runs under a `JobRegistry` job. If
    /// it completes within `grace_ms` the real result is returned inline and the
    /// job is retired — the common "it was fast this time" case costs nothing.
    /// Otherwise the work keeps running in the background and this returns a
    /// receipt:
    ///
    ///     { job_id: "job_00001f", status: "running", tool: "run_agent" }
    ///
    /// The caller (the LLM) then polls `job_status(job_id, wait_ms)` and collects
    /// with `job_result(job_id)`. That turns a five-minute blocking tool call
    /// into a ~40-token receipt plus a long-poll, instead of holding the
    /// provider HTTP turn open until it times out.
    ///
    /// Cancellation and progress are wired to the job automatically: handlers
    /// see `ctx.is_cancelled()` flip when `job_cancel` is called, and whatever
    /// they report through `ctx.on_progress` shows up in `job_status`.
    ToolResult call_tool_or_defer(const QString& name, const QJsonObject& args, int grace_ms = kMcpJobGraceMs);

    /// True if `name` is registered, enabled, and eligible for the job protocol
    /// (opted in AND has an async handler — a sync handler can't be backgrounded).
    bool supports_async_jobs(const QString& name) const;

    // ── Phase 6.3: Authorization hook ──────────────────────────────────────
    /// Caller-supplied predicate that returns true iff the call should
    /// proceed. The hook receives the tool's required AuthLevel and
    /// is_destructive flag; it is responsible for prompting the user (modal
    /// dialog) and returning the verdict synchronously. Hook lives in the app
    /// layer to avoid pulling UI headers into McpTypes.h.
    ///
    /// When unset, tools with auth_required == ExplicitConfirm or
    /// is_destructive=true fail closed; everything else passes through.
    using AuthChecker = std::function<bool(AuthLevel required, bool is_destructive)>;
    void set_auth_checker(AuthChecker checker);

    // ── Destructive-tool capability (fail-closed) ─────────────────────────
    //
    // `is_destructive` used to gate nothing on the chat path. The only consumer
    // was the AuthChecker installed by AgentService, which denies a destructive
    // tool ONLY when TerminalMcpBridge::is_call_in_progress() is true. The
    // interactive LLM tool loop calls McpService::execute_openai_function with
    // no ScopedCallFlags, so that flag was false and every non-ExplicitConfirm
    // destructive tool executed with no prompt of any kind.
    //
    // The gate now fails closed: a tool that declares `is_destructive` is
    // refused unless destructive capability has been granted for the current
    // context. Two ways to grant it, and both are explicit:
    //   1. Per call — the agent bridge's destructive capability token
    //      (TerminalMcpBridge::destructive_token(), echoed back as
    //      X-MCP-Allow-Destructive and surfaced via is_destructive_allowed()).
    //      Reused as-is; this is not a second mechanism.
    //   2. Per session — set_destructive_allowed(true), persisted as
    //      `mcp/allow_destructive_tools`. Defaults to false.
    //
    // AuthLevel::ExplicitConfirm tools additionally trip the fail-closed
    // branch below and stay refused even with the grant, until the
    // confirmation modal lands.

    /// Grant/revoke destructive-tool capability for this session. Persists to
    /// `mcp/allow_destructive_tools` so the choice survives a restart.
    static void set_destructive_allowed(bool allowed);

    /// True when destructive tools may run in the CURRENT context — i.e. the
    /// per-call capability token is present, or the session grant is on.
    static bool destructive_allowed();

    /// Run the Phase 6.3 authorization gate for one call WITHOUT executing
    /// anything. Returns std::nullopt when the call may proceed, or a
    /// ready-to-return failure ToolResult when blocked. Shared by
    /// call_tool_async (internal tools) and McpService::execute_tool
    /// (external tools, gated destructive-by-default) so both paths apply
    /// identical auth/destructive-confirmation rules. `name` is used for
    /// logging and error messages only.
    ///
    /// `destructive_declared` — true when `is_destructive` comes from the
    /// tool's own ToolDef. Pass false when the caller is merely applying a
    /// conservative default to a tool that carries no destructiveness metadata
    /// (external MCP servers over the wire). Only DECLARED destructive tools
    /// hit the fail-closed capability gate; undeclared ones stay on the
    /// checker-only path so user-configured external servers keep working.
    std::optional<ToolResult> check_authorization(const QString& name, AuthLevel auth_required, bool is_destructive,
                                                  bool destructive_declared = true) const;

    // ── LLM Integration ────────────────────────────────────────────────────

    /// Format all enabled tools for OpenAI function calling
    QJsonArray format_tools_for_openai() const;

    /// Parse "serverId__toolName" → { server_id, tool_name }.
    /// Reverses any wire-encoding applied by `encode_tool_name_for_wire` (e.g.
    /// `tool-dot-list` → `tool.list`) so the returned tool_name matches the
    /// registry key.
    static QPair<QString, QString> parse_openai_function_name(const QString& fn_name);

    /// Encode an internal tool name into a wire-safe form acceptable to every
    /// supported provider. The tightest common subset (intersection of every
    /// provider's published validation) is:
    ///
    ///   ^[a-zA-Z][a-zA-Z0-9_-]{0,63}$      total length 1..64
    ///
    /// Provider-specific rules surveyed (Apr 2026):
    ///   - OpenAI / Anthropic / Groq / OpenRouter / DeepSeek / xAI / MiniMax:
    ///     ^[a-zA-Z0-9_-]{1,64}$  (no dots, no leading symbol-only required).
    ///   - Kimi (Moonshot):
    ///     ^[a-zA-Z_][a-zA-Z0-9_-]{2,63}$  (must start with a letter or '_';
    ///     total length 3..64).
    ///   - Gemini:
    ///     [a-zA-Z0-9_:.-]{1,128}  (permissive; dots/colons allowed).
    ///
    /// Encoding rules:
    ///   1. Each '.' in the internal name → "-dot-" (no internal tool name uses
    ///      hyphens, so the round-trip is unambiguous).
    ///   2. If the resulting name fails the common-subset regex (e.g. starts
    ///      with a digit or contains other illegal chars), the encoder prefixes
    ///      it with `t_` and replaces every illegal byte with `_`.
    ///   3. The combined `<server_id>__<encoded_tool>` must fit in 64 chars.
    ///      Server prefix `fincept-terminal__` is 18 chars; longer tool names
    ///      are tail-truncated and a 4-char hash suffix preserves uniqueness.
    ///
    /// `decode_tool_name_from_wire` reverses step 1 only — steps 2 and 3 are
    /// effectively irreversible, so any tool whose name needs them must be
    /// looked up by its hash via `find_tool_by_wire_name` (registered when
    /// `format_tools_for_openai` is called) rather than by string round-trip.
    static QString encode_tool_name_for_wire(const QString& tool_name);
    static QString decode_tool_name_from_wire(const QString& wire_name);

    // ── Lifecycle ──────────────────────────────────────────────────────────

    void clear();

    // ── Generation Counter ─────────────────────────────────────────────────

    /// Monotonically increasing — incremented on any mutation.
    /// McpService uses this to detect stale cache.
    quint64 generation() const;

    McpProvider(const McpProvider&) = delete;
    McpProvider& operator=(const McpProvider&) = delete;

  private:
    McpProvider() = default;

    mutable QMutex mutex_;
    QHash<QString, ToolDef> tools_;
    // Pre-serialised view of each tool, kept in lockstep with `tools_`. Built
    // once at registration so list_tools()/find_tool() never re-serialise the
    // input_schema. `disabled_tools_` is still consulted at read time.
    QHash<QString, UnifiedTool> snapshots_;
    QSet<QString> disabled_tools_;
    quint64 generation_ = 0;

    // Phase 6.3 — guarded by mutex_ so set_auth_checker / call_tool_async
    // see consistent state across threads.
    AuthChecker auth_checker_;
};

} // namespace fincept::mcp
