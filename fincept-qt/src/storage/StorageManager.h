#pragma once
#include "core/result/Result.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVector>
#include <QFuture>
#include <atomic>

class QTimer;

namespace fincept {

/// Per-category storage statistics.
struct StorageCategoryInfo {
    QString id;    // e.g. "chat_history"
    QString label; // e.g. "Chat History"
    QString group; // e.g. "AI & LLM", "Trading", "Data"
    int count = 0; // number of rows / entries
    bool clearable = true;
};

/// Centralized manager for querying and clearing all persistent data stores.
/// Wraps individual repositories and direct SQL for tables without bulk-delete methods.
class StorageManager : public QObject {
    Q_OBJECT
  public:
    static StorageManager& instance();

    /// Returns stats for every data category.
    QVector<StorageCategoryInfo> all_stats() const;

    /// Returns entry count for a single category.
    int count_for(const QString& category_id) const;

    /// Clears all data in a single category. Returns error string on failure.
    Result<void> clear_category(const QString& category_id);

    /// Returns the on-disk size in bytes of fincept.db.
    qint64 main_db_size() const;

    /// Returns the on-disk size in bytes of cache.db.
    qint64 cache_db_size() const;

    /// Returns the on-disk size in bytes of log files.
    qint64 log_files_size() const;

    /// Clears log file contents (truncates).
    Result<void> clear_log_files();

    /// Returns total workspace file size.
    qint64 workspace_files_size() const;

    /// Deletes all workspace files.
    Result<void> clear_workspace_files();

    /// Clears all QSettings (window geometry, perspectives, UI state).
    Result<void> clear_qsettings();

    // ── Retention ────────────────────────────────────────────────────────────
    //
    // Several tables were append-only with no reader and no retention, so they
    // grew for the life of the install: `workflow_audit_log` (a row per node
    // execution), `telemetry_events` (a row per action.invoke), and
    // `unified_cache`, which was swept only once at startup — a terminal left
    // open for days never reclaimed anything.

    /// Apply every retention policy once. Safe to call repeatedly; each policy
    /// is independent and a failure in one is logged, not propagated (this runs
    /// unattended on a timer — one missing table must not stop the rest).
    ///
    /// Policies: workflow_audit_log > 90 days, telemetry_events > 30 days,
    /// expired unified_cache.
    Result<void> prune_all();

    /// Dispatch `prune_all()` onto a worker thread. Never call `prune_all()`
    /// directly from the UI thread: it takes a write lock on the main DB, so a
    /// screen doing a synchronous read in its showEvent stalls behind it.
    /// Skips the tick if a sweep is already in flight.
    void prune_off_thread();

    /// Run `prune_all()` now and every `interval_ms` thereafter (default 15 min).
    /// Idempotent — calling twice does not create a second timer.
    void start_retention_sweeper(int interval_ms = 15 * 60 * 1000);

  signals:
    void category_cleared(const QString& category_id);

  private:
    explicit StorageManager(QObject* parent = nullptr);

    int count_table(const QString& table, const QString& where = {}) const;
    Result<void> delete_from(const QString& table, const QString& where = {});

    /// True when `table` exists in the main DB. Used so a cascade / retention
    /// sweep can skip auxiliary tables this build never creates instead of
    /// treating them as a failure.
    bool table_exists(const QString& table) const;

    /// Delete from every table in `tables` (children first) inside ONE
    /// transaction, checking each statement. Any failure rolls the whole cascade
    /// back — a half-cleared "Clear chat history" is worse than a failed one.
    Result<void> delete_cascade(const QStringList& tables);

    /// DELETE with params on the main DB; returns rows removed, or -1 on error.
    /// Logs and swallows a missing table (returns 0) so the sweeper stays quiet.
    int prune_main_db(const QString& table, const QString& where, const QVariantList& params = {});

    QTimer* retention_timer_ = nullptr;
    /// Guards against overlapping sweeps if one pass outlives the timer interval.
    std::atomic<bool> prune_running_{false};
    /// Held so aboutToQuit can join the sweep before the DBs are closed.
    QFuture<void> prune_future_;
};

} // namespace fincept
