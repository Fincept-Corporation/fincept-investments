#pragma once
#include "core/identity/Uuid.h"
#include "core/layout/LayoutTypes.h"
#include "screens/ai_chat/AiChatBubble.h"

#include <QMainWindow>
#include <QPointer>
#include <QStackedWidget>
#include <QTimer>

class QScreen;
class QToolBar;

namespace ads {
class CDockManager;
}
namespace fincept {
class DockScreenRouter;
}
namespace fincept::ui {
class DockToolBar;
class DockStatusBar;
class PushpinBar;
class TabBar;
class DebugOverlay;
class QuickCommandBar;
} // namespace fincept::ui
namespace fincept {

class WindowFrame : public QMainWindow {
    Q_OBJECT
  public:
    /// @param window_id     Unique id for this window instance (0 = primary, 1+ = secondary).
    ///                      Primary restores saved geometry; secondary uses smart placement.
    /// @param parent        Qt parent (typically nullptr for top-level frames).
    /// @param adopted_uuid  When non-null, this frame adopts the given WindowId
    ///                      instead of minting a fresh one. Used by WorkspaceShell::apply
    ///                      so spawned frames carry the persisted UUID and the saved
    ///                      WorkspaceVariant geometry/screen lookups round-trip cleanly.
    ///                      Must be set BEFORE any panel is created — that's why it
    ///                      goes through the ctor rather than a setter (the default-
    ///                      dashboard navigate at the end of setup_docking_mode() runs
    ///                      before the ctor returns and would otherwise bind panels
    ///                      to a freshly-minted UUID).
    explicit WindowFrame(int window_id = 0, QWidget* parent = nullptr, const WindowId& adopted_uuid = {});

    int window_id() const { return window_id_; }

    /// Process-stable UUID for this frame instance. Lazily minted on first
    /// access and cached for the WindowFrame's lifetime. Used as the
    /// `PanelHandle::frame_id` for every panel docked into this frame so
    /// `PanelRegistry::find_by_frame(WindowId)` works without depending
    /// on the legacy int window_id_.
    ///
    /// The UUID is process-local (not persisted) — Phase 6 introduces the
    /// persisted Workspace.frames[].window_id mapping. Until then, this
    /// is a fresh UUIDv4 per frame construction.
    WindowId frame_uuid() const;

    /// Returns the next unique window ID (thread-safe via Qt UI thread only).
    /// The allocator is seeded from saved state on first call so that IDs
    /// never collide with orphaned saved layouts from a previous session.
    static int next_window_id();

    /// Adopt a saved WindowId UUID instead of minting a fresh one. Must be
    /// called immediately after construction and BEFORE any panel is created
    /// (i.e. before `apply_layout` or any `dock_router->navigate` call) —
    /// PanelHandle::frame_id is captured at panel creation and we need that
    /// to match the persisted layout's frame_id so subsequent saves round-
    /// trip cleanly. No-op if `frame_uuid_` was already minted; logs a
    /// warning in that case so the regression is visible.
    void adopt_frame_uuid(const WindowId& id);

    /// Screen router used by this window. Exposed so WindowCycler can ask
    /// the currently-focused window to cycle its own panels.
    DockScreenRouter* dock_router() const { return dock_router_; }

    /// Relocate this window to the given QScreen, preserving maximised /
    /// fullscreen state and clamping size to the target's available area.
    /// Called by WindowCycler for the Ctrl+Shift+N series of shortcuts.
    void move_to_screen(QScreen* target);

    /// Apply / clear Qt::WindowStaysOnTopHint. Handles the Qt requirement
    /// that the window be re-shown after toggling the flag. Persists the
    /// state per-window in SessionManager. Safe to call with the current
    /// state (no-op). Also called from Phase 12's right-click context menu.
    void set_always_on_top(bool on);
    bool is_always_on_top() const { return always_on_top_; }

    /// Phase 6 final: capture this frame's full layout into a `FrameLayout`
    /// suitable for serialisation into a `Workspace`. Walks every panel
    /// the dock router knows about, calls `IStatefulScreen::flush_pending_state`
    /// + `save_state()` on each, packages the per-panel blobs alongside
    /// the frame's metadata + ADS layout blob.
    ///
    /// The returned FrameLayout is fully self-contained — no aliases into
    /// live Qt widgets. Safe to JSON-serialise immediately.
    layout::FrameLayout capture_layout() const;

    /// Phase 6 final: restore this frame from a `FrameLayout`. Inverse of
    /// `capture_layout()`. For every PanelState in the layout: ensures the
    /// panel id is registered with the dock router (eager or factory),
    /// transfers the PanelInstanceId via `DockScreenRouter::adopt_panel_instance`,
    /// navigates to materialise. Then restores the ADS dock state via
    /// `CDockManager::restoreState`. Sets focus_mode + always_on_top
    /// from the layout. Activates the layout's `active_panel`.
    ///
    /// Returns true if the dock state restored cleanly. Returns false if
    /// the ADS blob was rejected (corrupt / version mismatch) — caller
    /// (WorkspaceMatcher fallback path) may then try a different variant
    /// or default layout.
    bool apply_layout(const layout::FrameLayout& layout);

    /// Live dock manager (ADS). Exposed for layout capture/thumbnail code
    /// that needs to grab the dock area's pixmap. Nullable during early
    /// init before setup_ui completes.
    ads::CDockManager* dock_manager() const { return dock_manager_; }

    /// Whether this frame is "active for work" — Phase 8 / decision 9.1.
    /// Best-effort flag derived from `!isMinimized()` and `isVisible()`.
    /// Used by DataHub to gate fanout of `pause_when_inactive` topics so
    /// expensive update streams (sparklines, ticks) stop firing into a
    /// frame the user can't see.
    ///
    /// We deliberately do NOT try to detect virtual-desktop occupancy or
    /// other-window occlusion — both are platform-specific and the design
    /// doc (9.1) calls them out as best-effort-only. Minimised + hidden
    /// covers the obvious cases.
    bool is_active_for_work() const;

  signals:
    /// Emitted when `is_active_for_work()` flips. DataHub subscribes to
    /// this to invalidate its per-owner active-frame cache; widgets with
    /// their own throttling logic can also key off it.
    ///
    /// Fires from `changeEvent(WindowStateChange)` and `showEvent` /
    /// `hideEvent`. The state at signal emission is what
    /// `is_active_for_work()` returns immediately after.
    void active_for_work_changed(bool active);

    /// Phase 6 trim: emitted when this frame moves between physical screens
    /// (user dragged the window onto a different monitor). Subscribers that
    /// cache paint-rendered content at the old DPR (e.g. paint-cached
    /// QPixmap widgets, custom GL surfaces) should invalidate their cache
    /// here so the next paint redraws at the new DPR cleanly.
    ///
    /// Implemented by connecting QWindow::screenChanged to a forwarding
    /// slot in the constructor — fires once per screen transition.
    void screen_changed(QScreen* new_screen);

  public:
    // ── Action handler API (Phase 4 ActionRegistry integration) ─────────────
    //
    // The handlers in core/actions/builtin_actions.cpp call into these
    // methods on the focused frame. Each is a thin facade over what was
    // previously an inline lambda inside the constructor's keyboard-binding
    // block; lifting them out lets the registry route actions to the live
    // focused frame instead of capturing `this` at bind time.

    /// Flip focus mode (chrome hide/show).
    void toggle_focus_mode();

    /// Forward `refresh` to whichever panel currently has focus inside this
    /// frame. No-op if no panel is focused or it doesn't expose `refresh()`.
    void refresh_focused_panel();

    /// Open the Phase 6 Component Browser dialog scoped to this frame.
    void open_component_browser();

    /// Phase 10 trim: toggle the per-frame `DebugOverlay`. Lazily
    /// constructed on first invocation; subsequent calls flip visibility.
    /// The full Phase 10 overlay (per-panel render times, thread pool
    /// stats, paint-time histogram) is deferred — this is the minimal
    /// "tell me what frame I'm on and how busy DataHub is" surface.
    void toggle_debug_overlay();

    /// Phase 9: toggle the per-frame CommandBar (line-edit at the bottom).
    /// Lazily constructed on first invocation. Fincept-style "just type"
    /// surface that reads ActionRegistry and CommandParser.
    void toggle_command_bar();

    /// Phase 9: surface the CommandPalette (Ctrl+K modal) over this frame.
    /// Centred on this frame, not the primary screen, so multi-monitor
    /// users see it where they're looking.
    void open_command_palette();

  protected:
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    int window_id_ = 0;

    /// Lazily-minted, process-stable UUID for this frame. See frame_uuid()
    /// for the rationale. mutable so the const accessor can mint on first
    /// call without needing a non-const path.
    mutable WindowId frame_uuid_;

    /// Cached value of `is_active_for_work()` — used by changeEvent to emit
    /// the `active_for_work_changed` signal only on actual state flips, not
    /// every redundant WindowStateChange (Qt fires multiple per minimise).
    /// Initialised to true (constructor sets a visible frame as active).
    bool last_active_for_work_ = true;

    /// Phase 10 trim: lazily-constructed debug overlay. nullptr until the
    /// user first invokes `toggle_debug_overlay()` — saves the widget +
    /// per-second QTimer cost for users who never enable it.
    ui::DebugOverlay* debug_overlay_ = nullptr;

    /// Phase 9: lazily-constructed CommandBar. Hidden by default.
    QPointer<QWidget> command_bar_;

    // ADS dock system
    ads::CDockManager* dock_manager_ = nullptr;
    DockScreenRouter* dock_router_ = nullptr;
    ui::DockToolBar* dock_toolbar_ = nullptr;
    ui::DockStatusBar* dock_status_bar_ = nullptr;
    ui::PushpinBar* pushpin_bar_ = nullptr; // owned by ui::ToolBar (inline in main toolbar)
    ui::TabBar* tab_bar_ = nullptr;

    // View state
    bool focus_mode_ = false;
    bool always_on_top_ = false;
    AiChatBubble* chat_bubble_ = nullptr;

    // Debounced persistence of dock layout on add/replace/remove via command bar.
    // Without this, layout changes only survive clean shutdown (closeEvent),
    // so a crash or kill loses the user's dock setup.
    QTimer* dock_layout_save_timer_ = nullptr;
    bool suppress_layout_save_ = false;
    void schedule_dock_layout_save();

    void setup_docking_mode();
    void setup_dock_screens();
    void update_window_title();
};

} // namespace fincept
