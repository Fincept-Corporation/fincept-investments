#pragma once
#include "core/identity/Uuid.h"

#include <QObject>
#include <QString>

namespace fincept {

class WindowRegistry;
class ActionRegistry;
class PanelRegistry;
class ProfileManager;
class WorkspaceDb;
class WorkspaceSnapshotRing;
class CrashRecovery;

/// Process-singleton owning everything that lives "above any single window."
///
/// The plan calls for `WindowFrame` to shed concerns it shouldn't own —
/// services init, theme, profile — into a place that survives window creation
/// and destruction. That place is `TerminalShell`.
///
/// Phase 1 ships the **skeleton**:
///   - `initialise()` is the formal "shell came up" event. main.cpp calls it
///     immediately after constructing QApplication. The implementation today
///     wires nothing new — just confirms the invariants and emits started().
///   - The shell exposes accessors for the registries it owns (just refs to
///     the existing singletons). These accessors give Phase 2-9 code a single
///     ergonomic entry point: `TerminalShell::instance().window_registry()`
///     instead of `WindowRegistry::instance()` everywhere.
///
/// Threading: UI-thread only. There is exactly one TerminalShell per process.
/// Calling instance() from a worker thread is a bug.
class TerminalShell : public QObject {
    Q_OBJECT
  public:
    static TerminalShell& instance();

    /// Bring the shell up. Call once from main.cpp after QApplication
    /// construction, before the first WindowFrame is shown. Idempotent —
    /// repeat calls are no-ops with a warning log.
    ///
    /// Today this:
    ///   - Marks the shell as initialised (gates instance() consumers from
    ///     racing with the construction order during early startup).
    ///   - Bootstraps profile paths under the active profile (mkpath the
    ///     new directories the multi-window refactor introduces).
    ///   - Resolves the active ProfileId so every later phase can use it.
    ///
    /// Phase 4 adds builtin_actions registration here.
    void initialise();

    /// Tear down the shell. main.cpp calls this on aboutToQuit so persistence
    /// flushes can run before Qt destroys QApplication. Idempotent.
    void shutdown();

    bool is_initialised() const { return initialised_; }

    /// Active profile UUID. Stable across the session. Null if shutdown()
    /// has been called or initialise() hasn't yet.
    ProfileId active_profile_id() const;

    /// Convenience accessors. Each forwards to the underlying singleton.
    /// Centralising them makes mocking the shell in unit tests tractable
    /// without per-singleton patches.
    WindowRegistry& window_registry();
    ActionRegistry& action_registry();
    PanelRegistry& panel_registry();
    ProfileManager& profile_manager();

    /// Workspace persistence + recovery. Exposed as pointers so they stay
    /// nullable during early init / after shutdown — callers should null-
    /// check rather than expect a constant reference.
    WorkspaceDb* workspace_db() { return workspace_db_; }
    WorkspaceSnapshotRing* snapshot_ring() { return snapshot_ring_; }
    CrashRecovery* crash_recovery() { return crash_recovery_; }

    /// The session_id minted in initialise(). Stable for this process. Used
    /// as the row key in workspace_db's session_history table.
    QString session_id() const { return session_id_; }

    /// True iff `crash_recovery_->needs_recovery()` was true at the moment
    /// `initialise()` ran. Latched at boot — `needs_recovery()` would start
    /// returning false positives within ~60s of boot (latest auto snapshot
    /// catches up to the clean-shutdown marker), so consumers that want
    /// "did we boot after a crash" must read this latched flag instead.
    bool started_after_crash() const { return started_after_crash_; }

  signals:
    /// Emitted at the end of initialise() once all bootstrapping has run.
    /// Subscribers can hook startup-time work here without knowing about
    /// main.cpp's exact wiring order.
    void started();

    /// Emitted at the start of shutdown(), before any teardown actually
    /// runs. Last chance for subscribers to flush state.
    void shutting_down();

  private:
    TerminalShell() = default;

    bool initialised_ = false;
    ProfileId active_profile_id_;
    QString session_id_;

    /// Owned objects. Constructed in initialise(), destroyed in shutdown().
    /// Raw pointers because the std::unique_ptr wrapper would force
    /// inclusion of WorkspaceDb.h here, leaking storage/ into every
    /// translation unit that touches the shell.
    WorkspaceDb* workspace_db_ = nullptr;
    WorkspaceSnapshotRing* snapshot_ring_ = nullptr;
    CrashRecovery* crash_recovery_ = nullptr;

    /// Latched at boot in initialise(); read via started_after_crash().
    bool started_after_crash_ = false;
};

} // namespace fincept
