#pragma once
#include "core/result/Result.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QVector>

#include <functional>
#include <string>

namespace fincept {

/// A single versioned migration.
struct Migration {
    int version;
    QString name;
    std::function<Result<void>(QSqlDatabase&)> apply;
};

/// Runs versioned migrations tracked in a schema_version table.
/// Migration files auto-register via static initialization.
///
/// Safety contract (do not weaken — a half-migrated DB is silent data loss):
///   - A database written by a NEWER build is refused outright. Running an older
///     binary against a future schema is unrecoverable: v050, for instance, does
///     `DROP TABLE IF EXISTS aa_competitions`, so an older build would query
///     tables that no longer exist. `run()` returns an error tagged with
///     `kErrSchemaNewer` in that case.
///   - Before the FIRST forward migration of a session, a consistent snapshot of
///     the database file is written to `<db>.pre-v<target>.bak`. Backup failure
///     aborts the migration — migrating with no recovery path is the bug this
///     class exists to prevent.
///   - Any failure returns an error tagged with `kErrMigrationFailed` and, when
///     one exists, the backup path embedded in the message.
///
/// Callers MUST treat both tagged errors as fatal: show a blocking dialog and
/// exit rather than booting the UI onto a half-migrated / future schema.
class MigrationRunner {
  public:
    /// Error tag prefixes. `run()` prefixes its error strings with one of these
    /// so a caller can classify the failure without parsing prose.
    static constexpr const char* kErrSchemaNewer = "[schema-newer]";
    static constexpr const char* kErrMigrationFailed = "[migration-failed]";

    /// True when `error_message` came from `run()` refusing a future schema.
    static bool is_schema_newer_error(const std::string& error_message);
    /// True when `error_message` came from a migration that failed to apply.
    static bool is_migration_failure(const std::string& error_message);
    /// True for either — i.e. "the database is not safe to use, do not boot".
    static bool is_fatal_error(const std::string& error_message);

    explicit MigrationRunner(QSqlDatabase& db);

    /// Apply all pending migrations in order.
    Result<void> run();

    /// Current applied schema version (0 if none).
    int current_version() const;

    /// Highest version this build knows how to apply (0 if none registered).
    static int highest_registered_version();

    /// Path of the pre-migration backup taken by the last `run()`, or empty.
    QString backup_path() const { return backup_path_; }

    /// Register a migration (called at static-init time by each v00N file).
    static void register_migration(Migration m);

    /// All registered migrations, sorted by version.
    static const QVector<Migration>& all_migrations();

  private:
    Result<void> ensure_schema_version_table();
    Result<void> apply_migration(const Migration& m);
    Result<void> record_version(int version, const QString& name);
    int read_current_version();

    /// Snapshot the database file to `<db>.pre-v<target_version>.bak`.
    /// Prefers `VACUUM INTO` (consistent, safe on an open connection, no
    /// close/reopen); falls back to `wal_checkpoint(TRUNCATE)` + file copy.
    Result<void> backup_database(int current_version, int target_version);

    QSqlDatabase& db_;
    QString backup_path_;
};

// Explicit registration functions — call these before Database::open()
// to ensure MSVC linker doesn't strip the migration translation units.
void register_migration_v001();
void register_migration_v002();
void register_migration_v003();
void register_migration_v004();
void register_migration_v005();
void register_migration_v006();
void register_migration_v007();
void register_migration_v008();
void register_migration_v009();
void register_migration_v010();
void register_migration_v011();
void register_migration_v012();
void register_migration_v013();
void register_migration_v014();
void register_migration_v015();
void register_migration_v016();
void register_migration_v017();
void register_migration_v018();
void register_migration_v019();
void register_migration_v020();
void register_migration_v021();
void register_migration_v022();
void register_migration_v023();
void register_migration_v024();
void register_migration_v025();
void register_migration_v026();
void register_migration_v027();
void register_migration_v028();
void register_migration_v029();
void register_migration_v030();
void register_migration_v031();
void register_migration_v032();
void register_migration_v033();
void register_migration_v034();
void register_migration_v035();
void register_migration_v036();
void register_migration_v037();
void register_migration_v038();
void register_migration_v039();
void register_migration_v040();
void register_migration_v041();
void register_migration_v042();
void register_migration_v043();
void register_migration_v044();
void register_migration_v045();
void register_migration_v046();
void register_migration_v047();
void register_migration_v048();
void register_migration_v049();
void register_migration_v050();
void register_migration_v051();
void register_migration_v052();

} // namespace fincept
