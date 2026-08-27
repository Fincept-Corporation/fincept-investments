// v052_drop_auth_tables — Remove the residue of user login, the PIN lock, and
// Fincept Cloud sync, all of which were deleted from the application.
//
// What goes, and why it is safe:
//
//  1. settings rows `fincept_session` / `fincept_api_key` — the persisted
//     server session blob and the legacy plaintext API key. Nothing reads them
//     any more, and `fincept_api_key` is cleartext credential material that
//     should not linger on disk.
//
//  2. secure_credentials rows `api_key`, `session_token`, and the five `pin_*`
//     keys. Named EXACTLY, never by prefix: this table also holds broker,
//     exchange, wallet, and LLM-provider credentials, which must survive.
//
//  3. settings rows under `security.` (lock timeout, auto-lock, lock-on-
//     minimise) and `cloud_sync.` (per-domain sync toggles) — the settings
//     pages that wrote them no longer exist.
//
//  4. Tables `security_audit_log` (v020), `sync_outbox` and `sync_map`
//     (cloud sync). No remaining code reads or writes any of them.
//
//  5. The `fincept` LLM provider row seeded by v002. The provider was removed
//     from the catalog, so leaving the row active would leave LlmService
//     pointing at a provider it can no longer dispatch.
//
// Historical migrations are never edited — this is a new forward migration, so
// databases already at v051 converge with fresh installs.
//
// SAFE ON A POPULATED DATABASE for everything the app still uses: the deletes
// are pinned to exact keys / prefixes owned solely by the removed features.

#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QSqlError>
#include <QSqlQuery>

namespace fincept {
namespace {

// Uniquely named (not a bare `sql`) — unity builds concatenate ~20 migration
// TUs, and two anonymous-namespace helpers sharing a name in one batch is a
// redefinition.
Result<void> sql_v052(QSqlDatabase& db, const char* stmt) {
    QSqlQuery q(db);
    if (!q.exec(stmt))
        return Result<void>::err(q.lastError().text().toStdString());
    return Result<void>::ok();
}

Result<void> apply_v052(QSqlDatabase& db) {
    static const char* const kStatements[] = {
        // 1 + 3 — settings rows owned by the removed features.
        "DELETE FROM settings WHERE key IN ('fincept_session', 'fincept_api_key')",
        "DELETE FROM settings WHERE key LIKE 'security.%'",
        "DELETE FROM settings WHERE key LIKE 'cloud_sync.%'",

        // 2 — exact credential keys only. A LIKE here would take broker rows.
        "DELETE FROM secure_credentials WHERE key IN "
        "('api_key', 'session_token', 'pin_hash', 'pin_salt', 'pin_failed_attempts', "
        "'pin_lockout_until', 'pin_lockout_stamp')",

        // 4 — tables with no remaining reader or writer.
        "DROP TABLE IF EXISTS security_audit_log",
        "DROP TABLE IF EXISTS sync_outbox",
        "DROP TABLE IF EXISTS sync_map",

        // 5 — the seeded Fincept LLM provider.
        "DELETE FROM llm_configs WHERE provider = 'fincept'",
    };

    for (const char* stmt : kStatements) {
        auto r = sql_v052(db, stmt);
        if (r.is_err())
            return r;
    }
    return Result<void>::ok();
}

} // anonymous namespace

void register_migration_v052() {
    static bool done = false;
    if (done)
        return;
    done = true;
    MigrationRunner::register_migration({52, "drop_auth_tables", apply_v052});
}

} // namespace fincept
