#pragma once
// TerminalMcpBridge.h — Local HTTP bridge that exposes the internal MCP tool
// catalog to the Python `finagent_core` agent subprocess.
//
// The Python side (`scripts/agents/finagent_core/tools/terminal_toolkit.py`)
// already speaks this contract:
//
//   POST <endpoint>/tool       body: {id, tool, args}   → ToolResult JSON
//   (optional) GET <endpoint>/tools                     → [{name, description, inputSchema}]
//
// Auth: every request must include `X-MCP-Token: <token>`, compared in constant
// time. Two kinds of token are accepted: the per-process UUID (`token()`), and
// a per-run UUID minted by `begin_run()` that additionally binds that agent's
// own tool filter to dispatch. AgentService injects one into the agent config
// payload. Agents share the parent's user session, so the token only guards
// against OTHER local processes stumbling onto the port.
//
// Every request is also checked against core/security/LoopbackGuard.h (Host must be our
// own loopback authority; no cross-site fetch metadata), which is what stops a
// web page the user has open — or a DNS-rebound hostname — from driving this
// port. This endpoint is machine-to-machine, so cross-site navigations are
// rejected too.
//
// Transport: QTcpServer + manual HTTP/1.1 framing (Qt6::HttpServer is not
// available in this Qt 6.8.3 build). Localhost-only binding on 127.0.0.1:0
// (OS-assigned port). Connection: close after each response — no keep-alive.

#include "mcp/McpTypes.h"

#include <QHash>
#include <QJsonArray>
#include <QMutex>
#include <QObject>
#include <QString>

#include <optional>

class QTcpServer;
class QTcpSocket;

namespace fincept::mcp {

class TerminalMcpBridge : public QObject {
    Q_OBJECT
  public:
    static TerminalMcpBridge& instance();

    /// Bind the server. Idempotent — repeat calls are no-ops once active.
    /// Returns true if the bridge is listening after the call.
    bool start();

    /// Close the server and drop all live connections.
    void stop();

    bool is_active() const { return active_; }

    /// "http://127.0.0.1:<port>" — no path, no query. Empty when not active.
    QString endpoint() const;

    /// Per-process UUID. Accepted as `X-MCP-Token` for the life of the process.
    /// Prefer `begin_run()` for anything that spawns an agent — a run-scoped
    /// token carries that agent's own tool filter to the dispatch path.
    QString token() const { return token_; }

    /// Open a run scope and mint the `X-MCP-Token` the agent should present.
    ///
    /// The returned token authenticates exactly like `token()` but additionally
    /// binds `filter` / `include_external` to every call made with it, so the
    /// per-agent tool filter is enforced at DISPATCH and not only when the
    /// catalog is built. Without this an agent that knows a tool's name could
    /// POST a tool its own configuration excluded — the catalog is a hint, the
    /// bridge is the boundary.
    ///
    /// Returns `token()` unchanged when the bridge is not listening (nothing to
    /// scope). Pair with `end_run()`; scopes are also swept by age and count so
    /// a caller that never retires one cannot grow the map without bound.
    QString begin_run(const ToolFilter& filter, bool include_external = true);

    /// Retire a run scope minted by `begin_run()`. No-op for an empty token,
    /// the process token, or a token that has already been retired.
    void end_run(const QString& run_token);

    /// Second per-process UUID. Only injected into agent configs whose
    /// `allow_destructive_tools=true`; the agent's toolkit echoes it back as
    /// `X-MCP-Allow-Destructive` on each request. Bridge compares the header
    /// to this value — match → destructive tools permitted for THAT call.
    /// Treated as a capability token; never log it.
    QString destructive_token() const { return destructive_token_; }

    /// True iff the current thread is inside a bridge-dispatched tool call.
    /// Read by the McpProvider auth checker (installed by AgentService) so it
    /// can apply agent-specific gating without affecting the chat path.
    /// Set on the calling thread for the synchronous portion of
    /// McpProvider::call_tool_async (auth check + dispatch); cleared after.
    static bool is_call_in_progress();

    /// True iff the in-flight bridge call presented the matching destructive
    /// token. Only meaningful while is_call_in_progress() is true.
    static bool is_destructive_allowed();

    /// RAII helper that re-establishes the agent-gating flags on ANOTHER thread.
    /// The flags above are thread_local, so when an external tool call hops onto
    /// a QtConcurrent pool thread (McpService::execute_openai_function_async) the
    /// agent context is lost and the auth checker would treat it as a chat call
    /// — bypassing the destructive-tool gate. Capture the flags on the bridge
    /// thread, then construct one of these inside the pool-thread lambda.
    class ScopedCallFlags {
      public:
        ScopedCallFlags(bool call_in_progress, bool destructive_allowed);
        ~ScopedCallFlags();
        ScopedCallFlags(const ScopedCallFlags&) = delete;
        ScopedCallFlags& operator=(const ScopedCallFlags&) = delete;

      private:
        bool prev_in_progress_;
        bool prev_destructive_;
    };

    /// Build the catalog payload that AgentService injects into
    /// `config["terminal_tools"]`. Output shape (per item):
    ///   { "name": str, "description": str, "inputSchema": {JSON Schema} }
    /// Note: camelCase `inputSchema` — matches MCP wire spec and what the
    /// Python `TerminalToolkit` constructor expects.
    ///
    /// `include_external` controls whether tools from user-configured
    /// external MCP servers (Notion, Slack, etc., visible in the MCP Servers
    /// tab) are included alongside internal Fincept tools. Defaults to true
    /// to match prior behaviour.
    QJsonArray tool_definitions(const ToolFilter& filter, bool include_external = true) const;

    TerminalMcpBridge(const TerminalMcpBridge&) = delete;
    TerminalMcpBridge& operator=(const TerminalMcpBridge&) = delete;

  signals:
    void tool_called(const QString& tool_name, bool success);
    void bridge_error(const QString& message);

  private slots:
    void on_new_connection();
    void on_ready_read();
    void on_disconnected();

  private:
    explicit TerminalMcpBridge(QObject* parent = nullptr);

    struct RequestState {
        QByteArray buffer;
        bool headers_parsed = false;
        QString method;
        QString path;
        int content_length = 0;
        QHash<QString, QString> headers;
        /// Run scope this request authenticated against, empty when it
        /// presented the process-wide token. Resolved once in try_dispatch()
        /// so the handlers don't re-walk the token map.
        QString run_token;
    };

    /// One live agent run: the tool policy its calls are held to.
    struct RunScope {
        ToolFilter filter;
        bool include_external = true;
        qint64 started_ms = 0;
    };

    bool parse_headers(RequestState& st);
    void try_dispatch(QTcpSocket* sock);

    /// Constant-time match of a supplied `X-MCP-Token` against the process
    /// token and every live run token. On a run-token hit, `run_token_out`
    /// receives it; on a process-token hit it is cleared.
    bool authenticate(const QString& supplied, QString* run_token_out) const;

    std::optional<RunScope> run_scope(const QString& run_token) const;

    /// Drop run scopes that are too old or too numerous. Caller holds
    /// `runs_mutex_`. A leaked scope is fail-safe (the policy stays enforced),
    /// but it must not accumulate for the life of the process.
    void sweep_runs_locked();

    void handle_post_tool(QTcpSocket* sock, const QJsonObject& body);
    void handle_get_tools(QTcpSocket* sock);

    void write_json_response(QTcpSocket* sock, int status, const QJsonObject& body);
    void write_error(QTcpSocket* sock, int status, const QString& message);

    QTcpServer* server_ = nullptr;
    bool active_ = false;
    QString token_;
    QString destructive_token_;
    QHash<QTcpSocket*, RequestState> states_;

    /// Live run scopes keyed by the run token handed to the agent. Socket
    /// handling is GUI-thread-only, but begin_run/end_run are called from the
    /// agent layer, so this one map is mutex-guarded.
    QHash<QString, RunScope> runs_;
    mutable QMutex runs_mutex_;
};

} // namespace fincept::mcp
