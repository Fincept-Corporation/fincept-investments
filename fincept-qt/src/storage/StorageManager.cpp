#include "storage/StorageManager.h"

#include "core/config/AppPaths.h"
#include "core/logging/Logger.h"
#include "storage/cache/CacheManager.h"
#include "storage/sqlite/CacheDatabase.h"
#include "storage/sqlite/Database.h"
#include "storage/workspace/WorkspaceDb.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QSqlQuery>
#include <QTimer>
#include <QtConcurrent>
#include <QPointer>

namespace fincept {

StorageManager& StorageManager::instance() {
    static StorageManager s;
    return s;
}

StorageManager::StorageManager(QObject* parent) : QObject(parent) {}

// ── Helpers ──────────────────────────────────────────────────────────────────

int StorageManager::count_table(const QString& table, const QString& where) const {
    auto& db = Database::instance();
    if (!db.is_open())
        return 0;

    QString sql = "SELECT COUNT(*) FROM " + table;
    if (!where.isEmpty())
        sql += " WHERE " + where;

    auto r = db.execute(sql, {});
    if (r.is_err())
        return 0;
    auto& q = r.value();
    return q.next() ? q.value(0).toInt() : 0;
}

Result<void> StorageManager::delete_from(const QString& table, const QString& where) {
    auto& db = Database::instance();
    if (!db.is_open())
        return Result<void>::err("Database not open");

    QString sql = "DELETE FROM " + table;
    if (!where.isEmpty())
        sql += " WHERE " + where;

    return db.exec(sql);
}

bool StorageManager::table_exists(const QString& table) const {
    auto& db = Database::instance();
    if (!db.is_open())
        return false;
    auto r = db.execute("SELECT 1 FROM sqlite_master WHERE type='table' AND name = ? LIMIT 1", {table});
    return r.is_ok() && r.value().next();
}

Result<void> StorageManager::delete_cascade(const QStringList& tables) {
    auto& db = Database::instance();
    if (!db.is_open())
        return Result<void>::err("Database not open");

    auto tx = db.begin_transaction();
    if (tx.is_err())
        return tx;

    for (const QString& table : tables) {
        // A table this build never creates is not a failure — the old code
        // discarded every intermediate Result, so a missing auxiliary table was
        // silently tolerated. Preserve that, but make REAL errors atomic.
        if (!table_exists(table))
            continue;

        auto r = delete_from(table);
        if (r.is_err()) {
            db.rollback();
            LOG_ERROR("StorageManager",
                      QString("Cascade delete aborted at %1: %2").arg(table, QString::fromStdString(r.error())));
            return r;
        }
    }

    auto c = db.commit();
    if (c.is_err()) {
        db.rollback();
        return c;
    }
    return Result<void>::ok();
}

// ── Category definitions ─────────────────────────────────────────────────────

QVector<StorageCategoryInfo> StorageManager::all_stats() const {
    QVector<StorageCategoryInfo> stats;

    // --- Cache (cache.db) ---
    stats.append({"cache", "Temporary Cache", "Cache", CacheManager::instance().entry_count(), true});

    { // tab sessions from cache.db
        int ts_count = 0;
        auto& cdb = CacheDatabase::instance();
        if (cdb.is_open()) {
            auto r = cdb.execute("SELECT COUNT(*) FROM tab_sessions", {});
            if (r.is_ok()) {
                auto& q = r.value();
                if (q.next())
                    ts_count = q.value(0).toInt();
            }
        }
        stats.append({"tab_sessions", "Tab Session State", "Cache", ts_count, true});
    }

    // --- AI & LLM ---
    stats.append({"chat_sessions", "Chat Sessions", "AI & LLM", count_table("chat_sessions"), true});
    stats.append({"chat_messages", "Chat Messages", "AI & LLM", count_table("chat_messages"), true});
    stats.append({"context_recordings", "Context Recordings", "AI & LLM", count_table("recorded_contexts"), true});
    stats.append({"agent_configs", "Agent Configurations", "AI & LLM", count_table("agent_configs"), true});
    stats.append({"llm_configs", "LLM Provider Configs", "AI & LLM", count_table("llm_configs"), true});
    stats.append({"llm_model_configs", "LLM Model Configs", "AI & LLM", count_table("llm_model_configs"), true});
    stats.append({"llm_profiles", "LLM Profiles", "AI & LLM", count_table("llm_profiles"), true});

    // --- News ---
    stats.append({"news_articles", "News Articles", "News", count_table("news_articles"), true});
    stats.append({"news_monitors", "News Monitors", "News", count_table("news_monitors"), true});
    stats.append({"rss_feeds", "RSS Feeds", "News", count_table("user_rss_feeds"), true});

    // --- Notes & Reports ---
    stats.append({"notes", "Financial Notes", "Notes & Reports", count_table("financial_notes"), true});
    stats.append({"reports", "Reports", "Notes & Reports", count_table("reports"), true});

    // --- Portfolio & Trading ---
    stats.append({"portfolios", "Portfolios", "Portfolio & Trading", count_table("portfolios"), true});
    stats.append({"transactions", "Transactions", "Portfolio & Trading", count_table("portfolio_transactions"), true});
    stats.append({"watchlists", "Watchlists", "Portfolio & Trading", count_table("watchlists"), true});
    stats.append({"custom_indices", "Custom Indices", "Portfolio & Trading", count_table("custom_indices"), true});

    // --- Paper Trading ---
    stats.append({"pt_portfolios", "Paper Portfolios", "Paper Trading", count_table("pt_portfolios"), true});
    stats.append({"pt_orders", "Paper Orders", "Paper Trading", count_table("pt_orders"), true});
    stats.append({"pt_trades", "Paper Trades", "Paper Trading", count_table("pt_trades"), true});

    // --- Workflows ---
    stats.append({"workflows", "Workflows", "Workflows", count_table("workflows"), true});

    // --- Data Sources & Integration ---
    stats.append({"data_sources", "Data Sources", "Data & Integration", count_table("data_sources"), true});
    stats.append({"data_mappings", "Data Mappings", "Data & Integration", count_table("data_mappings"), true});
    stats.append({"mcp_servers", "MCP Servers", "Data & Integration", count_table("mcp_servers"), true});

    // --- Dashboard ---
    stats.append({"dashboard_layouts", "Dashboard Layouts", "Dashboard", count_table("dashboard_layouts"), true});

    // --- Broker ---
    stats.append({"instruments", "Broker Instruments", "Broker", count_table("instruments"), true});

    // --- Settings & Credentials ---
    stats.append({"settings", "App Settings", "Settings & Credentials", count_table("settings"), true});
    stats.append({"credentials", "Stored Credentials", "Settings & Credentials", count_table("credentials"), true});
    stats.append({"kv_storage", "Key-Value Storage", "Settings & Credentials", count_table("key_value_storage"), true});

    return stats;
}

int StorageManager::count_for(const QString& category_id) const {
    // Cache categories go through CacheDatabase
    if (category_id == "cache")
        return CacheManager::instance().entry_count();

    if (category_id == "tab_sessions") {
        auto& cdb = CacheDatabase::instance();
        if (!cdb.is_open())
            return 0;
        auto r = cdb.execute("SELECT COUNT(*) FROM tab_sessions", {});
        if (r.is_err())
            return 0;
        auto& q = r.value();
        return q.next() ? q.value(0).toInt() : 0;
    }

    // Map category_id → table name
    static const QHash<QString, QString> table_map = {
        {"chat_sessions", "chat_sessions"},
        {"chat_messages", "chat_messages"},
        {"context_recordings", "recorded_contexts"},
        {"agent_configs", "agent_configs"},
        {"llm_configs", "llm_configs"},
        {"llm_model_configs", "llm_model_configs"},
        {"llm_profiles", "llm_profiles"},
        {"news_articles", "news_articles"},
        {"news_monitors", "news_monitors"},
        {"rss_feeds", "user_rss_feeds"},
        {"notes", "financial_notes"},
        {"reports", "reports"},
        {"portfolios", "portfolios"},
        {"transactions", "portfolio_transactions"},
        {"watchlists", "watchlists"},
        {"custom_indices", "custom_indices"},
        {"pt_portfolios", "pt_portfolios"},
        {"pt_orders", "pt_orders"},
        {"pt_trades", "pt_trades"},
        {"workflows", "workflows"},
        {"data_sources", "data_sources"},
        {"data_mappings", "data_mappings"},
        {"mcp_servers", "mcp_servers"},
        {"dashboard_layouts", "dashboard_layouts"},
        {"instruments", "instruments"},
        {"settings", "settings"},
        {"credentials", "credentials"},
        {"kv_storage", "key_value_storage"},
    };

    auto it = table_map.find(category_id);
    if (it == table_map.end())
        return 0;
    return count_table(*it);
}

Result<void> StorageManager::clear_category(const QString& category_id) {
    LOG_INFO("StorageManager", "Clearing category: " + category_id);

    // --- Cache (cache.db) ---
    if (category_id == "cache") {
        CacheManager::instance().clear();
        emit category_cleared(category_id);
        return Result<void>::ok();
    }

    if (category_id == "tab_sessions") {
        auto& cdb = CacheDatabase::instance();
        if (!cdb.is_open())
            return Result<void>::err("Cache database not open");
        auto r = cdb.exec("DELETE FROM tab_sessions");
        if (r.is_ok())
            emit category_cleared(category_id);
        return r;
    }

    // --- Chat: clear messages first (FK), then sessions ---
    if (category_id == "chat_sessions") {
        auto r = delete_cascade({"chat_context_links", "chat_messages", "chat_sessions"});
        if (r.is_err())
            return r;
        emit category_cleared(category_id);
        return Result<void>::ok();
    }
    if (category_id == "chat_messages") {
        auto r = delete_from("chat_messages");
        if (r.is_ok())
            emit category_cleared(category_id);
        return r;
    }

    // --- Context recordings ---
    if (category_id == "context_recordings") {
        auto r = delete_cascade({"recording_sessions", "recorded_contexts"});
        if (r.is_err())
            return r;
        emit category_cleared(category_id);
        return Result<void>::ok();
    }

    // --- Agent configs ---
    if (category_id == "agent_configs") {
        auto r = delete_from("agent_configs");
        if (r.is_ok())
            emit category_cleared(category_id);
        return r;
    }

    // --- LLM configs ---
    if (category_id == "llm_configs") {
        auto r = delete_cascade({"llm_global_settings", "llm_configs"});
        if (r.is_ok())
            emit category_cleared(category_id);
        return r;
    }
    if (category_id == "llm_model_configs") {
        auto r = delete_from("llm_model_configs");
        if (r.is_ok())
            emit category_cleared(category_id);
        return r;
    }

    // --- LLM profiles + assignments ---
    if (category_id == "llm_profiles") {
        auto r = delete_cascade({"llm_profile_assignments", "llm_profiles"});
        if (r.is_err())
            return r;
        emit category_cleared(category_id);
        return Result<void>::ok();
    }

    // --- News ---
    if (category_id == "news_articles") {
        // FTS table clears automatically with content table (delete triggers).
        auto r = delete_cascade({"news_entity_cache", "news_instability", "news_baselines", "news_articles"});
        if (r.is_ok())
            emit category_cleared(category_id);
        return r;
    }
    if (category_id == "news_monitors") {
        auto r = delete_from("news_monitors");
        if (r.is_ok())
            emit category_cleared(category_id);
        return r;
    }
    if (category_id == "rss_feeds") {
        auto r = delete_from("user_rss_feeds");
        if (r.is_ok())
            emit category_cleared(category_id);
        return r;
    }

    // --- Notes ---
    if (category_id == "notes") {
        auto r = delete_from("financial_notes");
        if (r.is_ok())
            emit category_cleared(category_id);
        return r;
    }

    // --- Reports ---
    if (category_id == "reports") {
        auto r = delete_cascade({"report_templates", "reports"});
        if (r.is_ok())
            emit category_cleared(category_id);
        return r;
    }

    // --- Portfolios (cascade: snapshots, assets, transactions) ---
    if (category_id == "portfolios") {
        auto r = delete_cascade({"portfolio_snapshots", "portfolio_transactions", "portfolio_assets",
                                 "portfolio_holdings", "portfolios"});
        if (r.is_ok())
            emit category_cleared(category_id);
        return r;
    }
    if (category_id == "transactions") {
        auto r = delete_from("portfolio_transactions");
        if (r.is_ok())
            emit category_cleared(category_id);
        return r;
    }

    // --- Watchlists ---
    if (category_id == "watchlists") {
        auto r = delete_cascade({"watchlist_stocks", "watchlists"});
        if (r.is_ok())
            emit category_cleared(category_id);
        return r;
    }

    // --- Custom indices ---
    if (category_id == "custom_indices") {
        auto r = delete_cascade({"custom_index_values", "custom_indices"});
        if (r.is_ok())
            emit category_cleared(category_id);
        return r;
    }

    // --- Paper Trading ---
    if (category_id == "pt_portfolios") {
        auto r = delete_cascade({"pt_margin_blocks", "pt_trades", "pt_positions", "pt_orders", "pt_portfolios"});
        if (r.is_ok())
            emit category_cleared(category_id);
        return r;
    }
    if (category_id == "pt_orders") {
        auto r = delete_from("pt_orders");
        if (r.is_ok())
            emit category_cleared(category_id);
        return r;
    }
    if (category_id == "pt_trades") {
        auto r = delete_from("pt_trades");
        if (r.is_ok())
            emit category_cleared(category_id);
        return r;
    }

    // --- Workflows (cascade: edges, nodes) ---
    if (category_id == "workflows") {
        auto r = delete_cascade({"workflow_edges", "workflow_nodes", "workflows"});
        if (r.is_ok())
            emit category_cleared(category_id);
        return r;
    }

    // --- Data sources ---
    if (category_id == "data_sources") {
        auto r = delete_cascade({"data_source_connections", "ws_provider_configs", "data_sources"});
        if (r.is_ok())
            emit category_cleared(category_id);
        return r;
    }
    if (category_id == "data_mappings") {
        auto r = delete_cascade({"normalized_data", "data_mappings"});
        if (r.is_ok())
            emit category_cleared(category_id);
        return r;
    }

    // --- MCP servers ---
    if (category_id == "mcp_servers") {
        auto r = delete_cascade({"mcp_tool_usage_logs", "mcp_tools", "internal_mcp_tool_settings", "mcp_servers"});
        if (r.is_ok())
            emit category_cleared(category_id);
        return r;
    }

    // --- Dashboard layouts ---
    if (category_id == "dashboard_layouts") {
        auto r = delete_cascade({"dashboard_widget_instances", "dashboard_layouts"});
        if (r.is_ok())
            emit category_cleared(category_id);
        return r;
    }

    // --- Broker instruments ---
    if (category_id == "instruments") {
        auto r = delete_from("instruments");
        if (r.is_ok())
            emit category_cleared(category_id);
        return r;
    }

    // --- Settings & Credentials ---
    if (category_id == "settings") {
        auto r = delete_from("settings");
        if (r.is_ok())
            emit category_cleared(category_id);
        return r;
    }
    if (category_id == "credentials") {
        auto r = delete_from("credentials");
        if (r.is_ok())
            emit category_cleared(category_id);
        return r;
    }
    if (category_id == "kv_storage") {
        auto r = delete_from("key_value_storage");
        if (r.is_ok())
            emit category_cleared(category_id);
        return r;
    }

    return Result<void>::err("Unknown category: " + category_id.toStdString());
}

// ── File sizes ───────────────────────────────────────────────────────────────

qint64 StorageManager::main_db_size() const {
    QFileInfo fi(AppPaths::data() + "/fincept.db");
    if (!fi.exists())
        return 0;
    // Include WAL + SHM files
    qint64 total = fi.size();
    QFileInfo wal(AppPaths::data() + "/fincept.db-wal");
    if (wal.exists())
        total += wal.size();
    QFileInfo shm(AppPaths::data() + "/fincept.db-shm");
    if (shm.exists())
        total += shm.size();
    return total;
}

qint64 StorageManager::cache_db_size() const {
    QFileInfo fi(AppPaths::data() + "/cache.db");
    if (!fi.exists())
        return 0;
    qint64 total = fi.size();
    QFileInfo wal(AppPaths::data() + "/cache.db-wal");
    if (wal.exists())
        total += wal.size();
    QFileInfo shm(AppPaths::data() + "/cache.db-shm");
    if (shm.exists())
        total += shm.size();
    return total;
}

qint64 StorageManager::log_files_size() const {
    QDir dir(AppPaths::logs());
    if (!dir.exists())
        return 0;
    qint64 total = 0;
    for (const auto& fi : dir.entryInfoList(QDir::Files))
        total += fi.size();
    return total;
}

Result<void> StorageManager::clear_log_files() {
    LOG_INFO("StorageManager", "Clearing log files");
    QDir dir(AppPaths::logs());
    if (!dir.exists())
        return Result<void>::ok();

    for (const auto& fi : dir.entryInfoList(QDir::Files)) {
        QFile f(fi.absoluteFilePath());
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.close();
    }
    emit category_cleared("log_files");
    return Result<void>::ok();
}

qint64 StorageManager::workspace_files_size() const {
    QDir dir(AppPaths::workspaces());
    if (!dir.exists())
        return 0;
    qint64 total = 0;
    for (const auto& fi : dir.entryInfoList({"*.fwsp"}, QDir::Files))
        total += fi.size();
    return total;
}

Result<void> StorageManager::clear_workspace_files() {
    LOG_INFO("StorageManager", "Clearing workspace files");
    QDir dir(AppPaths::workspaces());
    if (!dir.exists())
        return Result<void>::ok();

    for (const auto& fi : dir.entryInfoList({"*.fwsp"}, QDir::Files)) {
        QFile::remove(fi.absoluteFilePath());
    }
    emit category_cleared("workspaces");
    return Result<void>::ok();
}

// ── Retention ────────────────────────────────────────────────────────────────

namespace {
// Retention windows. Deliberately generous — these tables exist for forensics.
constexpr int kAuditLogRetentionDays = 90;
constexpr int kTelemetryRetentionDays = 30;
} // namespace

int StorageManager::prune_main_db(const QString& table, const QString& where, const QVariantList& params) {
    auto& db = Database::instance();
    if (!db.is_open())
        return 0;

    // A table that was never created (its owning subsystem never ran) is not an
    // error — the sweeper is unattended and must stay quiet.
    if (!table_exists(table))
        return 0;

    auto r = db.execute("DELETE FROM " + table + " WHERE " + where, params);
    if (r.is_err()) {
        LOG_WARN("StorageManager",
                 QString("Retention sweep of %1 failed: %2").arg(table, QString::fromStdString(r.error())));
        return -1;
    }
    return r.value().numRowsAffected();
}

void StorageManager::prune_off_thread() {
    // prune_all() issues four bulk DELETEs across three databases. Deferring it
    // with singleShot() moved it off the *startup* path but left it on the UI
    // thread (P1) — and worse, it takes a write lock on the main DB, so any
    // screen doing a synchronous read in its showEvent stalls behind it until
    // busy_timeout. That is what made the alpha_arena screen miss an 8s
    // construction deadline in the smoke walk: it was waiting on this prune.
    //
    // Safe to run on a worker now: Database/CacheDatabase/WorkspaceDb all hand
    // out per-thread connections (this release's fix), so the prune gets its own
    // handle instead of borrowing the main thread's.
    // Never start a sweep during teardown. The databases this touches are
    // singletons that TerminalShell::shutdown() closes, so a worker launched
    // now would outlive them.
    if (QCoreApplication::closingDown())
        return;

    if (prune_running_.exchange(true))
        return; // a sweep is still going — skip this tick rather than pile up

    QPointer<StorageManager> self = this;
    prune_future_ = QtConcurrent::run([self]() {
        if (self)
            (void)self->prune_all();
        if (self)
            self->prune_running_.store(false);
    });
}

Result<void> StorageManager::prune_all() {
    int total = 0;

    // workflow_audit_log — one row per node execution, written by AuditLogger and
    // read by nothing outside it. `timestamp` is stored via datetime('now'), so
    // the same 'YYYY-MM-DD HH:MM:SS' form compares lexicographically.
    const int audit = prune_main_db("workflow_audit_log", QString("timestamp < datetime('now', '-%1 days')")
                                                              .arg(kAuditLogRetentionDays));
    if (audit > 0) {
        total += audit;
        LOG_INFO("StorageManager", QString("Retention: removed %1 workflow_audit_log rows older than %2 days")
                                       .arg(audit)
                                       .arg(kAuditLogRetentionDays));
    }

    // unified_cache (cache.db) — was swept once at startup only, so a terminal
    // left open for days never reclaimed expired rows.
    {
        auto& cdb = CacheDatabase::instance();
        if (cdb.is_open()) {
            const int expired = cdb.sweep_expired();
            if (expired > 0) {
                total += expired;
                LOG_INFO("StorageManager", QString("Retention: removed %1 expired unified_cache rows").arg(expired));
            }
        }
    }

    // telemetry_events (workspace.db) — one row per action.invoke, no reader.
    {
        auto& wdb = WorkspaceDb::instance();
        if (wdb.is_open()) {
            const qint64 cutoff_ms =
                QDateTime::currentMSecsSinceEpoch() - static_cast<qint64>(kTelemetryRetentionDays) * 86400LL * 1000LL;
            auto r = wdb.execute("DELETE FROM telemetry_events WHERE ts_ms < ?", {cutoff_ms});
            if (r.is_ok()) {
                const int removed = r.value().numRowsAffected();
                if (removed > 0) {
                    total += removed;
                    LOG_INFO("StorageManager", QString("Retention: removed %1 telemetry_events rows older than %2 days")
                                                   .arg(removed)
                                                   .arg(kTelemetryRetentionDays));
                }
            }
            // A missing telemetry_events table just means telemetry never wrote
            // this session — nothing to report.
        }
    }

    if (total > 0)
        LOG_INFO("StorageManager", QString("Retention sweep removed %1 rows in total").arg(total));
    return Result<void>::ok();
}

void StorageManager::start_retention_sweeper(int interval_ms) {
    if (retention_timer_)
        return; // already running

    retention_timer_ = new QTimer(this);
    retention_timer_->setInterval(interval_ms);
    retention_timer_->setTimerType(Qt::VeryCoarseTimer);
    connect(retention_timer_, &QTimer::timeout, this, [this]() { prune_off_thread(); });
    retention_timer_->start();

    // First pass deferred off the startup critical path.
    QTimer::singleShot(0, this, [this]() { prune_off_thread(); });

    // Join the worker before the app tears the databases down. Without this the
    // sweep is an unowned QtConcurrent task that keeps using Database /
    // CacheDatabase / WorkspaceDb while TerminalShell::shutdown() closes them —
    // an access violation during static destruction, i.e. after the crash
    // handler is gone, so it produces no dump and no log line.
    if (auto* app = QCoreApplication::instance()) {
        connect(app, &QCoreApplication::aboutToQuit, this, [this]() {
            if (retention_timer_)
                retention_timer_->stop();
            if (prune_future_.isRunning()) {
                LOG_INFO("StorageManager", "Waiting for in-flight retention sweep before shutdown");
                prune_future_.waitForFinished();
            }
        });
    }

    LOG_INFO("StorageManager", QString("Retention sweeper started (every %1 min)").arg(interval_ms / 60000));
}

Result<void> StorageManager::clear_qsettings() {
    LOG_INFO("StorageManager", "Clearing QSettings (window geometry, perspectives, UI state)");
    QSettings settings("Fincept", "FinceptTerminal");
    settings.clear();
    settings.sync();
    emit category_cleared("qsettings");
    return Result<void>::ok();
}

} // namespace fincept
