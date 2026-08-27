#include "app/TerminalShell.h"

#include "app/WindowFrame.h"
#include "core/actions/ActionRegistry.h"
#include "core/actions/builtin_actions.h"
#include "core/config/ProfileManager.h"
#include "core/layout/LayoutCatalog.h"
#include "core/layout/WorkspaceShell.h"
#include "core/logging/Logger.h"
#include "core/panel/PanelRegistry.h"
#include "core/profile/ProfilePaths.h"
#include "core/screen/MonitorWatcher.h"
#include "core/telemetry/CloudTelemetryProvider.h"
#include "core/telemetry/LocalTelemetrySink.h"
#include "core/telemetry/TelemetryProvider.h"
#include "core/window/WindowRegistry.h"
#include "screens/ai_chat/ChatBubbleController.h"
#include "storage/repositories/SettingsRepository.h"
#include "storage/workspace/CrashRecovery.h"
#include "storage/workspace/WorkspaceDb.h"
#include "storage/workspace/WorkspaceFwspImporter.h"
#include "storage/workspace/WorkspaceSnapshotRing.h"

#include <QDateTime>
#include <QGuiApplication>
#include <QScreen>
#include <QUuid>

namespace fincept {

namespace {
constexpr const char* kShellTag = "TerminalShell";
} // namespace

TerminalShell& TerminalShell::instance() {
    static TerminalShell s;
    return s;
}

void TerminalShell::initialise() {
    if (initialised_) {
        LOG_WARN(kShellTag, "initialise() called twice — ignoring");
        return;
    }

    LOG_INFO(kShellTag, "Shell initialising");

    // Resolve and cache the active profile UUID so every later phase has
    // a stable handle without re-reading the manifest. ProfileManager
    // mints + persists a UUID on first read if the manifest is legacy.
    active_profile_id_ = ProfileManager::instance().active_profile_id();
    LOG_INFO(kShellTag, QString("Active profile: %1 (id=%2)")
                            .arg(ProfileManager::instance().active())
                            .arg(active_profile_id_.to_string()));
    // Create the new per-profile directory tree (workspace.db, layouts/,
    // crashes/). Distinct from AppPaths::ensure_all() which creates the
    // legacy tree (data/, logs/, cache/, etc.) — both run, both are
    // idempotent.
    ProfilePaths::ensure_all();
    // Touch the registries so their static singletons construct *before*
    // any WindowFrame tries to register against them. Avoids a startup
    // race where the first window's constructor runs faster than the
    // registry's singleton init under aggressive optimisation.
    (void)WindowRegistry::instance();
    (void)ActionRegistry::instance();
    (void)PanelRegistry::instance();
    (void)ProfileManager::instance();
    // Phase 6 trim: MonitorWatcher boots here so its QGuiApplication signal
    // connections are in place before any frame restores. Phase 6's full
    // workspace-variant matcher will subscribe to topology_changed.
    (void)MonitorWatcher::instance();
    // Rescue off-screen windows when monitors come or go at runtime. The
    // raw `screenAdded` / `screenRemoved` signals fire mid-burst (some
    // OSes emit removed-then-added on dock unplug/replug); MonitorWatcher
    // debounces to a single topology_changed once the dust has settled.
    QObject::connect(&MonitorWatcher::instance(), &MonitorWatcher::topology_changed, &MonitorWatcher::instance(),
                     [](const layout::MonitorTopologyKey&) {
                         const auto screens = QGuiApplication::screens();
                         if (screens.isEmpty())
                             return;
                         for (WindowFrame* w : WindowRegistry::instance().frames()) {
                             if (!w)
                                 continue;
                             const QRect frame = w->frameGeometry();
                             // A window is "rescued" only if no currently-connected screen's
                             // available geometry intersects it at all — i.e. it would be
                             // completely invisible. Partial overlap is fine; the user can
                             // still grab the title bar.
                             bool on_some_screen = false;
                             for (QScreen* s : screens) {
                                 if (s && s->availableGeometry().intersects(frame)) {
                                     on_some_screen = true;
                                     break;
                                 }
                             }
                             if (on_some_screen)
                                 continue;
                             QScreen* target = QGuiApplication::primaryScreen();
                             if (!target)
                                 target = screens.first();
                             if (!target)
                                 continue;
                             const QRect ag = target->availableGeometry();
                             // Keep the window's size if it fits, otherwise clamp to 9/10
                             // of the target screen (same heuristic the constructor and the
                             // new-window path use).
                             QSize sz = w->size();
                             sz.setWidth(qMin(sz.width(), ag.width() * 9 / 10));
                             sz.setHeight(qMin(sz.height(), ag.height() * 9 / 10));
                             w->resize(sz);
                             w->move(ag.center() - QPoint(sz.width() / 2, sz.height() / 2));
                             LOG_INFO(kShellTag, QString("Rescued window %1 onto '%2' after topology change")
                                                     .arg(w->window_id())
                                                     .arg(target->name()));
                         }
                     });
    // Phase 3 final: chat-bubble shell coordinator. Per-frame bubble
    // widgets stay where they are; this just centralises the shell-side
    // observation surface (frame add/remove tracking, future telemetry,
    // future cross-frame chat-session linking).
    ai_chat::ChatBubbleController::instance().initialise();
    // Phase 6: open the per-profile LayoutCatalog so Launchpad's recent-
    // layouts list + the layout.* actions can read/write immediately.
    {
        auto r = LayoutCatalog::instance().open();
        if (r.is_err()) {
            LOG_WARN(kShellTag, QString("LayoutCatalog open failed: %1").arg(QString::fromStdString(r.error())));
        } else {
            // One-shot: import legacy v3 *.fwsp filenames as empty named
            // layouts so users see them in the Launchpad list. No-op after
            // first successful run (tracked via _meta.fwsp_import_done).
            WorkspaceFwspImporter::run_once_if_needed();
        }
    }

    // Phase 10: install telemetry providers per opt-in settings. Defaults
    // off — no telemetry without consent.
    //
    //   telemetry.local_enabled  → LocalTelemetrySink writes to workspace.db
    //   telemetry.cloud_enabled  → CloudTelemetryProvider also POSTs to
    //                              telemetry.cloud_endpoint with bearer
    //                              telemetry.cloud_api_key. Always wraps a
    //                              LocalTelemetrySink so disk persistence
    //                              survives network outages.
    {
        auto local_r = SettingsRepository::instance().get("telemetry.local_enabled");
        const bool local_enabled = local_r.is_ok() && local_r.value() == "true";
        auto cloud_r = SettingsRepository::instance().get("telemetry.cloud_enabled");
        const bool cloud_enabled = cloud_r.is_ok() && cloud_r.value() == "true";

        // Provider singletons live for the process lifetime; statics keep
        // them out of the heap-leak audit. Construction order matters:
        // local first so cloud can chain to it.
        static telemetry::LocalTelemetrySink local_sink;
        static telemetry::CloudTelemetryProvider cloud_sink(&local_sink);

        if (cloud_enabled) {
            cloud_sink.start();
            telemetry::TelemetrySink::instance().set_provider(&cloud_sink);
            LOG_INFO(kShellTag, "Cloud telemetry enabled (chains LocalTelemetrySink)");
        } else if (local_enabled) {
            telemetry::TelemetrySink::instance().set_provider(&local_sink);
            LOG_INFO(kShellTag, "Local telemetry enabled");
        } else {
            LOG_DEBUG(kShellTag, "Telemetry off");
        }
    }

    // Phase 4 Track B: populate the action registry with the built-ins that
    // replaced WindowFrame's hard-coded keyboard wiring. Must happen before
    // any WindowFrame is constructed so the per-frame hotkey-binding loop
    // sees the full action set.
    actions::register_builtins();
    // ── Phase 2: workspace persistence + crash recovery ────────────────
    workspace_db_ = &WorkspaceDb::instance();
    auto db_open = workspace_db_->open(ProfilePaths::workspace_db());
    if (db_open.is_err()) {
        LOG_ERROR(kShellTag, QString("Failed to open workspace.db: %1").arg(QString::fromStdString(db_open.error())));
        // Keep going — the shell can still run without persistence; later
        // saves just become no-ops. We don't want a corrupted db to brick
        // the terminal entirely.
        workspace_db_ = nullptr;
    } else {
        snapshot_ring_ = new WorkspaceSnapshotRing(workspace_db_);
        crash_recovery_ = new CrashRecovery(workspace_db_, snapshot_ring_);

        // Crash recovery check. Phase 2 logs only — Phase 6 wires the
        // dialog UI. The clean-shutdown marker write below would mask
        // the unclean state if it ran first, so check_for_recovery
        // happens *before* the new session row is opened.
        // Latch the answer at boot. needs_recovery() compares against
        // the latest auto snapshot, which the *current* session also
        // writes to — so within ~60s of startup the same call would start
        // returning false positives. Consumers that want a stable "we
        // started after a crash" signal must read started_after_crash_.
        started_after_crash_ = crash_recovery_->needs_recovery();
        if (started_after_crash_) {
            LOG_WARN(kShellTag, "Previous session ended uncleanly — recovery candidates available");
        }

        // Mint a fresh session_id and open a session_history row.
        // ended_at + exit_kind stay NULL until shutdown() updates them.
        session_id_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
        auto sess =
            workspace_db_->execute("INSERT INTO session_history(session_id, profile_id, started_at) VALUES(?, ?, ?)",
                                   {session_id_, active_profile_id_.to_string(), now_ms});
        if (sess.is_err()) {
            LOG_WARN(kShellTag, QString("session_history insert failed: %1").arg(QString::fromStdString(sess.error())));
        } else {
            LOG_INFO(kShellTag, QString("Session %1 started").arg(session_id_));
        }
    }

    initialised_ = true;
    LOG_INFO(kShellTag, "Shell initialised");
    emit started();
}

void TerminalShell::shutdown() {
    if (!initialised_)
        return;
    LOG_INFO(kShellTag, "Shell shutting down");
    emit shutting_down();

    // Update the session row + write the clean-shutdown marker. Order
    // matters: marker last, so that if the marker write fails the row's
    // ended_at is still recorded for diagnostics.
    if (workspace_db_ && workspace_db_->is_open()) {
        const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
        if (!session_id_.isEmpty()) {
            // frame_count + panel_count are best-effort: WindowRegistry
            // may have already started tearing down by aboutToQuit time
            // depending on which Qt finalizer ran first. Read it anyway —
            // a stale snapshot is fine for analytics.
            const int frame_count = WindowRegistry::instance().frame_count();
            auto upd =
                workspace_db_->execute("UPDATE session_history SET ended_at = ?, exit_kind = 'clean', frame_count = ? "
                                       "WHERE session_id = ?",
                                       {now_ms, frame_count, session_id_});
            if (upd.is_err()) {
                LOG_WARN(kShellTag,
                         QString("session_history update failed: %1").arg(QString::fromStdString(upd.error())));
            }
        }
        if (crash_recovery_)
            crash_recovery_->mark_clean_shutdown();
    }

    // Drop the in-memory "currently loaded layout" pointers so any code that
    // peeks at WorkspaceShell::current_id() during teardown (or across a
    // future profile-switch) doesn't see a stale value pointing at a layout
    // catalogue we're about to close.
    layout::WorkspaceShell::clear_current();

    // Tear down owned objects in reverse construction order.
    delete crash_recovery_;
    crash_recovery_ = nullptr;
    delete snapshot_ring_;
    snapshot_ring_ = nullptr;
    if (workspace_db_) {
        workspace_db_->close();
        workspace_db_ = nullptr;
    }

    initialised_ = false;
    // The underlying singletons (Logger, SessionManager, etc.) clean
    // themselves up via qAddPostRoutine and Qt's exit hooks. We don't
    // tear them down explicitly here — main.cpp's aboutToQuit handlers
    // own that.
}

ProfileId TerminalShell::active_profile_id() const {
    return active_profile_id_;
}

WindowRegistry& TerminalShell::window_registry() {
    return WindowRegistry::instance();
}

ActionRegistry& TerminalShell::action_registry() {
    return ActionRegistry::instance();
}

PanelRegistry& TerminalShell::panel_registry() {
    return PanelRegistry::instance();
}

ProfileManager& TerminalShell::profile_manager() {
    return ProfileManager::instance();
}

} // namespace fincept
