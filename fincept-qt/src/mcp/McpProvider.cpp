// McpProvider.cpp — Internal tool registry and executor (Qt port)

#include "mcp/McpProvider.h"

#include "core/config/AppConfig.h"
#include "core/logging/Logger.h"
#include "mcp/JobRegistry.h"
#include "mcp/SchemaValidator.h"
#include "mcp/TerminalMcpBridge.h"

#include <QCoreApplication>
#include <QFutureWatcher>
#include <QPointer>
#include <QPromise>
#include <QRegularExpression>
#include <QSemaphore>
#include <QSet>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <memory>

namespace fincept::mcp {

static constexpr const char* TAG = "McpProvider";

McpProvider& McpProvider::instance() {
    static McpProvider s;
    return s;
}

// Build the LLM-facing snapshot for a tool. Serialises input_schema once —
// the QJsonObject copy is cheap (CoW), so list_tools() can hand out copies
// without re-walking the schema each call.
static UnifiedTool make_snapshot(const ToolDef& t) {
    return UnifiedTool{
        QString(INTERNAL_SERVER_ID),
        QString(INTERNAL_SERVER_NAME),
        t.name,
        t.description,
        t.input_schema.to_json(),
        /*is_internal=*/true,
        t.category,
        t.is_destructive,
    };
}

// ============================================================================
// Registration
// ============================================================================

void McpProvider::register_tool(ToolDef tool) {
    QMutexLocker lock(&mutex_);
    QString name = tool.name;
    snapshots_.insert(name, make_snapshot(tool)); // serialise schema once
    tools_.insert(name, std::move(tool));
    ++generation_;
}

void McpProvider::register_tools(std::vector<ToolDef> tools) {
    QMutexLocker lock(&mutex_);
    for (auto& t : tools) {
        QString name = t.name;
        snapshots_.insert(name, make_snapshot(t));
        tools_.insert(name, std::move(t));
    }
    ++generation_;
}

void McpProvider::unregister_tool(const QString& name) {
    QMutexLocker lock(&mutex_);
    tools_.remove(name);
    snapshots_.remove(name);
    ++generation_;
}

// ============================================================================
// Enable / Disable
// ============================================================================

void McpProvider::set_tool_enabled(const QString& name, bool enabled) {
    QMutexLocker lock(&mutex_);
    if (enabled)
        disabled_tools_.remove(name);
    else
        disabled_tools_.insert(name);
    ++generation_;
}

bool McpProvider::is_tool_enabled(const QString& name) const {
    QMutexLocker lock(&mutex_);
    return !disabled_tools_.contains(name);
}

// ============================================================================
// Discovery
// ============================================================================

std::vector<UnifiedTool> McpProvider::list_tools() const {
    QMutexLocker lock(&mutex_);
    std::vector<UnifiedTool> result;
    result.reserve(static_cast<std::size_t>(snapshots_.size()));
    for (auto it = snapshots_.cbegin(); it != snapshots_.cend(); ++it) {
        if (disabled_tools_.contains(it.key()))
            continue;
        result.push_back(it.value());
    }
    return result;
}

std::vector<UnifiedTool> McpProvider::list_all_tools() const {
    QMutexLocker lock(&mutex_);
    std::vector<UnifiedTool> result;
    result.reserve(static_cast<std::size_t>(snapshots_.size()));
    for (auto it = snapshots_.cbegin(); it != snapshots_.cend(); ++it)
        result.push_back(it.value());
    return result;
}

std::optional<UnifiedTool> McpProvider::find_tool(const QString& name) const {
    QMutexLocker lock(&mutex_);
    auto it = snapshots_.constFind(name);
    if (it == snapshots_.constEnd())
        return std::nullopt;
    if (disabled_tools_.contains(name))
        return std::nullopt;
    return it.value();
}

std::vector<McpProvider::ToolAuditInfo> McpProvider::audit_all_tools() const {
    QMutexLocker lock(&mutex_);
    std::vector<ToolAuditInfo> result;
    result.reserve(static_cast<std::size_t>(tools_.size()));
    for (auto it = tools_.cbegin(); it != tools_.cend(); ++it) {
        const ToolDef& def = it.value();
        ToolAuditInfo info;
        info.name = def.name;
        info.category = def.category;
        info.description = def.description;
        // A std::function is truthy when a target is bound. async wins if both.
        info.has_handler = static_cast<bool>(def.handler) || static_cast<bool>(def.async_handler);
        info.is_async = static_cast<bool>(def.async_handler);
        info.enabled = !disabled_tools_.contains(it.key());
        info.is_destructive = def.is_destructive;
        info.auth_required = def.auth_required;
        info.input_schema = def.input_schema.to_json();
        info.legacy_aliases = def.legacy_aliases;
        result.push_back(std::move(info));
    }
    return result;
}

std::size_t McpProvider::tool_count() const {
    QMutexLocker lock(&mutex_);
    std::size_t count = 0;
    for (auto it = tools_.cbegin(); it != tools_.cend(); ++it) {
        if (!disabled_tools_.contains(it.key()))
            ++count;
    }
    return count;
}

bool McpProvider::has_tool(const QString& name) const {
    QMutexLocker lock(&mutex_);
    return tools_.contains(name);
}

// ============================================================================
// Execution
// ============================================================================

ToolResult McpProvider::call_tool(const QString& name, const QJsonObject& args) {
    // Sync entry point. If the registered tool is async, we still want to
    // give legacy callers (LlmService, TerminalToolBridge, workflow nodes)
    // a blocking ToolResult — so we dispatch async and wait. The caller is
    // already on a background thread (every existing call site is) so
    // blocking here is safe; .result() drains the QFuture.
    auto future = call_tool_async(name, args);
    future.waitForFinished();
    if (future.resultCount() == 0)
        return ToolResult::fail("Tool '" + name + "' produced no result");
    return future.result();
}

bool McpProvider::supports_async_jobs(const QString& name) const {
    QMutexLocker lock(&mutex_);
    auto it = tools_.constFind(name);
    if (it == tools_.constEnd() || disabled_tools_.contains(name))
        return false;
    // The flag alone isn't enough — a sync handler runs to completion on the
    // calling thread and can be neither observed nor interrupted, so there is
    // nothing a job wrapper could add.
    return it->supports_async && static_cast<bool>(it->async_handler);
}

ToolResult McpProvider::call_tool_or_defer(const QString& name, const QJsonObject& args, int grace_ms) {
    if (!supports_async_jobs(name))
        return call_tool(name, args);

    auto& jobs = JobRegistry::instance();
    const QString job_id = jobs.create(name);

    // Wire the job into the handler's context: cancellation reads the job's own
    // atomic flag (no registry lock in the handler's polling loop), progress
    // writes straight back to the snapshot the model polls.
    ToolContext ctx;
    if (auto flag = jobs.cancel_flag(job_id))
        ctx.is_cancelled = [flag]() { return flag->load(); };
    ctx.on_progress = [job_id](double progress, const QString& message) {
        JobRegistry::instance().set_progress(job_id, progress, message);
    };

    auto future = call_tool_async(name, args, ctx);

    // One continuation per QFuture is the Qt limit, and this is it — the future
    // is never handed to anyone else, so there is no contention.
    auto gate = std::make_shared<QSemaphore>(0);
    future.then([job_id, gate](QFuture<ToolResult> f) {
        // A handler is allowed to finish without ever adding a result; treat
        // that as a failure rather than dereferencing an empty future.
        JobRegistry::instance().complete(job_id, f.resultCount() > 0
                                                     ? f.result()
                                                     : ToolResult::fail("Tool produced no result"));
        gate->release();
    });

    if (gate->tryAcquire(1, std::max(0, grace_ms))) {
        // Beat the grace window — hand back the real result and let the job
        // record expire on the short collected-TTL. The model never learns a
        // job existed, which is the point: fast calls stay one round-trip.
        if (auto r = jobs.take_result(job_id))
            return *r;
        return ToolResult::fail("Job " + job_id + " finished without a result");
    }

    LOG_INFO(TAG, QString("Tool '%1' exceeded the %2 ms grace window — backgrounded as %3")
                      .arg(name)
                      .arg(grace_ms)
                      .arg(job_id));

    return ToolResult::ok(QString("Started background job %1 for '%2'. Poll job_status(job_id='%1', wait_ms=20000) "
                                  "until status is 'succeeded' or 'failed', then call job_result(job_id='%1'). "
                                  "Do NOT call '%2' again — it is already running.")
                              .arg(job_id, name),
                          QJsonObject{
                              {"job_id", job_id},
                              {"status", "running"},
                              {"tool", name},
                          });
}

QFuture<ToolResult> McpProvider::call_tool_async(const QString& name, const QJsonObject& args, ToolContext ctx) {
    ToolHandler sync_handler;
    AsyncToolHandler async_handler;
    ToolSchema schema;
    int default_timeout_ms = kMcpDefaultTimeoutMs;
    AuthLevel auth_required = AuthLevel::None;
    bool is_destructive = false;

    {
        QMutexLocker lock(&mutex_);

        auto fail_now = [](const QString& msg) {
            QPromise<ToolResult> p;
            p.start();
            p.addResult(ToolResult::fail(msg));
            p.finish();
            return p.future();
        };

        // Phase 6.10: try canonical name first, then any registered alias.
        // Aliases let us migrate names without breaking saved chats / Finagent
        // workflows that reference the old name.
        QString resolved = name;
        if (!tools_.contains(resolved)) {
            for (auto it = tools_.constBegin(); it != tools_.constEnd(); ++it) {
                if (it.value().legacy_aliases.contains(name)) {
                    LOG_INFO(TAG, QString("Tool called by legacy name '%1' — canonical '%2'").arg(name, it.key()));
                    resolved = it.key();
                    break;
                }
            }
        }

        if (!tools_.contains(resolved))
            return fail_now("Tool not found: " + name);
        if (disabled_tools_.contains(resolved))
            return fail_now("Tool is disabled: " + resolved);

        const auto& def = tools_[resolved];
        sync_handler = def.handler;
        async_handler = def.async_handler;
        schema = def.input_schema;
        default_timeout_ms = def.default_timeout_ms;
        auth_required = def.auth_required;
        is_destructive = def.is_destructive;

        if (!async_handler && !sync_handler)
            return fail_now("Tool '" + resolved + "' has no handler");
    }

    // Phase 6.3: authorization gate — shared with McpService's external-tool
    // dispatch via check_authorization() so internal and external calls apply
    // identical auth/destructive rules. See that method for the no-checker
    // semantics.
    if (auto denied = check_authorization(name, auth_required, is_destructive)) {
        QPromise<ToolResult> p;
        p.start();
        p.addResult(*denied);
        p.finish();
        return p.future();
    }

    // Phase 3: validate + inject defaults before invoking the handler.
    QJsonObject normalized = args;
    auto vr = validate_args(schema, normalized);
    if (vr.is_err()) {
        LOG_WARN(TAG, QString("Tool '%1' rejected: %2").arg(name, QString::fromStdString(vr.error())));
        QPromise<ToolResult> p;
        p.start();
        p.addResult(ToolResult::fail(QString::fromStdString(vr.error())));
        p.finish();
        return p.future();
    }

    // Per-call timeout overrides ToolDef::default_timeout_ms when supplied
    // (Phase 4 framework; Phase 6 wires _meta.timeout_ms here).
    if (ctx.timeout_ms == kMcpDefaultTimeoutMs) // ToolContext default — not explicitly set
        ctx.timeout_ms = default_timeout_ms;

    // Async preferred; fall back to sync wrapped in an immediately-resolved
    // future so call_tool() works uniformly for legacy handlers.
    if (async_handler) {
        auto promise = std::make_shared<QPromise<ToolResult>>();
        promise->start();

        // Cancellation flag — set by the timeout timer or by a caller-supplied
        // hook. Wired into ctx.is_cancelled below.
        auto cancelled = std::make_shared<std::atomic<bool>>(false);
        if (!ctx.is_cancelled) {
            ctx.is_cancelled = [cancelled]() { return cancelled->load(); };
        } else {
            // Compose: original hook OR our internal flag.
            auto orig = ctx.is_cancelled;
            ctx.is_cancelled = [orig, cancelled]() { return cancelled->load() || orig(); };
        }

        // Single-winner guard for promise resolution. `promise->future().isFinished()`
        // is NOT a valid guard on its own: the watchdog runs on the GUI thread
        // while the handler resolves on a service/worker thread, so both could
        // read "not finished" and then both addResult() — the second one lands
        // after finish() and trips a Qt assertion. A compare-exchange makes
        // exactly one caller the winner.
        //
        // Resolving also tears down the watchdog. Previously the timer was left
        // to fire at its full interval even when the tool finished in
        // milliseconds, so a tool with a 300 s budget parked a live timer on the
        // GUI thread for 300 s after it was already done — once per call.
        // `watchdog_slot` is filled in below; resolve() only reads it, so the
        // fast path (handler resolves before we even arm the timer) is safe.
        auto resolved = std::make_shared<std::atomic<bool>>(false);
        auto watchdog_slot = std::make_shared<QPointer<QTimer>>();
        auto resolve = [promise, resolved, watchdog_slot](ToolResult r) {
            bool expected = false;
            if (!resolved->compare_exchange_strong(expected, true))
                return;
            promise->addResult(std::move(r));
            promise->finish();
            if (QTimer* wd = watchdog_slot->data()) {
                QMetaObject::invokeMethod(
                    wd,
                    [watchdog_slot]() {
                        if (QTimer* t = watchdog_slot->data()) {
                            t->stop();
                            t->deleteLater();
                        }
                    },
                    Qt::QueuedConnection);
            }
        };

        // Arm a one-shot timeout. We post the timer onto the QApplication
        // thread so it ticks even if the caller returns immediately.
        auto* watchdog = new QTimer;
        watchdog->setSingleShot(true);
        watchdog->moveToThread(qApp->thread());
        *watchdog_slot = watchdog;
        QObject::connect(watchdog, &QTimer::timeout, watchdog, [resolve, cancelled, name]() {
            cancelled->store(true);
            LOG_WARN(TAG, QString("Tool '%1' timed out").arg(name));
            resolve(ToolResult::fail("Tool '" + name + "' timed out"));
        });
        QMetaObject::invokeMethod(
            watchdog, [watchdog, ms = ctx.timeout_ms]() { watchdog->start(ms); }, Qt::QueuedConnection);

        try {
            LOG_DEBUG(TAG, "Calling async tool: " + name);
            async_handler(normalized, ctx, promise);
        } catch (const std::exception& e) {
            LOG_ERROR(TAG, QString("Async tool '%1' threw: %2").arg(name, e.what()));
            resolve(ToolResult::fail(QString("Tool execution error: ") + e.what()));
        } catch (...) {
            LOG_ERROR(TAG, QString("Async tool '%1' threw unknown exception").arg(name));
            resolve(ToolResult::fail("Unknown error during tool execution"));
        }
        return promise->future();
    }

    // Legacy sync path — invoke immediately, wrap in a resolved future.
    QPromise<ToolResult> p;
    p.start();
    try {
        LOG_DEBUG(TAG, "Calling sync tool: " + name);
        p.addResult(sync_handler(normalized));
    } catch (const std::exception& e) {
        LOG_ERROR(TAG, QString("Tool '%1' threw exception: %2").arg(name, e.what()));
        p.addResult(ToolResult::fail(QString("Tool execution error: ") + e.what()));
    } catch (...) {
        LOG_ERROR(TAG, QString("Tool '%1' threw unknown exception").arg(name));
        p.addResult(ToolResult::fail("Unknown error during tool execution"));
    }
    p.finish();
    return p.future();
}

// ============================================================================
// LLM Integration
// ============================================================================

QJsonArray McpProvider::format_tools_for_openai() const {
    auto tools = list_tools();
    QJsonArray result;

    for (const auto& tool : tools) {
        QJsonObject schema = tool.input_schema;
        if (schema.isEmpty()) {
            schema["type"] = "object";
            schema["properties"] = QJsonObject();
        }

        QJsonObject fn;
        fn["name"] = QString(INTERNAL_SERVER_ID) + "__" + encode_tool_name_for_wire(tool.name);
        fn["description"] = tool.description;
        fn["parameters"] = schema;

        QJsonObject entry;
        entry["type"] = "function";
        entry["function"] = fn;
        result.append(entry);
    }

    return result;
}

QPair<QString, QString> McpProvider::parse_openai_function_name(const QString& fn_name) {
    int pos = fn_name.indexOf("__");
    if (pos > 0 && pos < fn_name.length() - 2)
        return {fn_name.left(pos), decode_tool_name_from_wire(fn_name.mid(pos + 2))};

    // Fallback: some models (minimax, certain OpenRouter routes) drop the
    // "<server>__" prefix and call the tool by its bare advertised name.
    // Accept either the raw form or the wire-encoded form if it matches
    // a known internal tool — anything else is still rejected.
    if (!fn_name.isEmpty()) {
        const QString decoded = decode_tool_name_from_wire(fn_name);
        if (instance().has_tool(decoded))
            return {QString(INTERNAL_SERVER_ID), decoded};
        if (instance().has_tool(fn_name))
            return {QString(INTERNAL_SERVER_ID), fn_name};
    }
    return {"", ""};
}

// Tightest common-subset regex for tool function names across every supported
// provider (see header for provenance). Each candidate emitted by
// encode_tool_name_for_wire MUST match this regex; non-matching names are
// logged so they can be renamed in source.
static const QRegularExpression& common_subset_regex() {
    static const QRegularExpression re(QStringLiteral("^[a-zA-Z][a-zA-Z0-9_-]{0,63}$"));
    return re;
}

QString McpProvider::encode_tool_name_for_wire(const QString& tool_name) {
    // Step 1: replace '.' with "-dot-". Internal tool names never contain
    // hyphens (verified across all 239 tools), so the round-trip is unique.
    QString out = tool_name;
    out.replace(QLatin1Char('.'), QStringLiteral("-dot-"));

    // Step 2: validate against the common subset. We intentionally do not
    // attempt to repair non-conforming names — any tool whose name fails this
    // check needs to be renamed in source, not silently mangled at the wire
    // boundary (mangled names break the model's ability to call the tool by
    // the catalog-advertised string). Log once per unique offender.
    if (!common_subset_regex().match(out).hasMatch()) {
        static QSet<QString> warned;
        if (!warned.contains(out)) {
            warned.insert(out);
            LOG_WARN("McpProvider", QString("Tool name '%1' does not match the provider-safe regex "
                                            "^[a-zA-Z][a-zA-Z0-9_-]{0,63}$ — Kimi/Anthropic/OpenAI will "
                                            "reject the request. Rename the tool at source.")
                                        .arg(out));
        }
    }
    return out;
}

QString McpProvider::decode_tool_name_from_wire(const QString& wire_name) {
    QString out = wire_name;
    out.replace(QStringLiteral("-dot-"), QStringLiteral("."));
    return out;
}

// ============================================================================
// Lifecycle
// ============================================================================

void McpProvider::clear() {
    QMutexLocker lock(&mutex_);
    tools_.clear();
    snapshots_.clear();
    disabled_tools_.clear();
    ++generation_;
}

// ============================================================================
// Authorization (Phase 6.3)
// ============================================================================

void McpProvider::set_auth_checker(AuthChecker checker) {
    QMutexLocker lock(&mutex_);
    auth_checker_ = std::move(checker);
}

namespace {
// Session-level destructive grant. -1 = not yet read from settings, 0 = denied,
// 1 = granted. Atomic because tool calls dispatch from pool threads.
std::atomic<int> s_destructive_session_grant{-1};

constexpr const char* kDestructiveSettingKey = "mcp/allow_destructive_tools";
} // namespace

void McpProvider::set_destructive_allowed(bool allowed) {
    s_destructive_session_grant.store(allowed ? 1 : 0, std::memory_order_relaxed);
    AppConfig::instance().set(QString::fromLatin1(kDestructiveSettingKey), allowed);
    LOG_WARN(TAG, QString("Destructive MCP tools are now %1 for this session")
                      .arg(allowed ? "ENABLED (tools may mutate state and write files)" : "disabled"));
}

bool McpProvider::destructive_allowed() {
    // 1. Per-call capability token — the agent bridge sets a thread_local flag
    //    for the duration of a call that presented the destructive token.
    //    Reusing that mechanism rather than adding a parallel one.
    if (TerminalMcpBridge::is_destructive_allowed())
        return true;

    // 2. Session grant, seeded once from the persisted setting.
    int v = s_destructive_session_grant.load(std::memory_order_relaxed);
    if (v < 0) {
        v = AppConfig::instance().get(QString::fromLatin1(kDestructiveSettingKey), QVariant(false)).toBool() ? 1 : 0;
        s_destructive_session_grant.store(v, std::memory_order_relaxed);
    }
    return v == 1;
}

std::optional<ToolResult> McpProvider::check_authorization(const QString& name, AuthLevel auth_required,
                                                           bool is_destructive, bool destructive_declared) const {
    // The confirmation surface lives in the app layer — we expose a hook the
    // app installs at startup rather than pulling UI headers in here.
    //
    // Order of checks:
    //   1. Nothing declared → pass immediately.
    //   2. Declared destructive without capability → REFUSE (fail closed, all
    //      callers). This runs ahead of the checker because the checker can
    //      only distinguish agent calls, and the chat path is the one that was
    //      unguarded.
    //   3. Installed checker → its verdict wins for everything else.
    //   4. No checker + AuthLevel::ExplicitConfirm → fail closed (the user
    //      never got the chance to confirm).
    if (auth_required == AuthLevel::None && !is_destructive)
        return std::nullopt;

    // ── Fail-closed destructive gate ─────────────────────────────────────
    // Runs BEFORE the caller-installed checker and applies to every caller,
    // because the checker could only ever see agent-originated calls (it keys
    // off TerminalMcpBridge::is_call_in_progress()) and the interactive chat
    // tool loop never sets that flag. See the header for the two ways to grant
    // the capability. Note this is deliberately caller-agnostic: "which caller
    // is this" is exactly the distinction that made the old gate a no-op.
    if (destructive_declared && is_destructive && !destructive_allowed()) {
        LOG_WARN(TAG, QString("Tool '%1' refused: destructive tools are disabled (auth_required=%2). "
                              "Grant the capability to allow it.")
                          .arg(name, auth_level_str(auth_required)));
        return ToolResult::fail(
            QString("Tool '%1' changes state or writes to disk and is disabled by default. To allow it, "
                    "the USER must enable destructive tools in Settings (`%2`). Do not retry this tool "
                    "until they confirm they have; tell them what you were trying to do and why.")
                .arg(name, QString::fromLatin1(kDestructiveSettingKey)));
    }

    AuthChecker checker;
    {
        QMutexLocker lock(&mutex_);
        checker = auth_checker_;
    }
    if (checker) {
        if (!checker(auth_required, is_destructive)) {
            LOG_WARN(TAG, QString("Tool '%1' blocked: auth_required=%2 is_destructive=%3")
                              .arg(name, auth_level_str(auth_required))
                              .arg(is_destructive ? "true" : "false"));
            return ToolResult::fail(QString("Tool '%1' requires %2 auth").arg(name, auth_level_str(auth_required)));
        }
    } else if (auth_required == AuthLevel::ExplicitConfirm) {
        // Fail-closed: ExplicitConfirm cannot be safely evaluated without a
        // checker — there is nothing to raise the confirmation with.
        LOG_WARN(TAG, QString("Tool '%1' blocked: no AuthChecker registered (required=%2)")
                          .arg(name, auth_level_str(auth_required)));
        return ToolResult::fail("Tool requires user confirmation but no authorisation hook is installed");
    } else if (is_destructive) {
        // Reaching here means the capability gate above already passed — the
        // user granted it for the session, or the caller presented the agent
        // destructive token. Record it: a destructive tool running is worth an
        // audit line even when it was authorised.
        LOG_WARN(TAG, QString("Destructive tool '%1' authorised and running").arg(name));
    }
    return std::nullopt;
}

// ============================================================================
// Generation Counter
// ============================================================================

quint64 McpProvider::generation() const {
    QMutexLocker lock(&mutex_);
    return generation_;
}

} // namespace fincept::mcp
