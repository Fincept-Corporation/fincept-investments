#include "core/actions/builtin_actions.h"

#include "app/DockScreenRouter.h"
#include "app/WindowFrame.h"
#include "core/actions/ActionRegistry.h"
#include "core/debug/StressLoad.h"
#include "core/keys/KeyConfigManager.h"
#include "core/keys/WindowCycler.h"
#include "core/layout/LayoutCatalog.h"
#include "core/layout/LayoutTypes.h"
#include "core/layout/WorkspaceMatcher.h"
#include "core/layout/WorkspaceShell.h"
#include "core/logging/Logger.h"
#include "core/symbol/IGroupLinked.h"
#include "core/symbol/SymbolContext.h"
#include "core/symbol/SymbolGroup.h"
#include "core/symbol/SymbolGroupRegistry.h"
#include "core/symbol/SymbolRef.h"
#include "core/window/WindowRegistry.h"
#include "screens/launchpad/OnboardingTour.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QGuiApplication>
#include <QPixmap>
#include <QScreen>

namespace fincept::actions {

namespace {

constexpr const char* kBuiltinTag = "BuiltinActions";

// ── Predicate helpers ────────────────────────────────────────────────────────
//
// Predicates are invoked by every surface that displays or routes the action
// (palette grey-out, hotkey gate, menu enable). They must be cheap and pure.

ActionPredicate require_focused_frame() {
    return [](const CommandContext& ctx) { return ctx.focused_frame != nullptr; };
}

ActionPredicate require_more_than_one_window() {
    return [](const CommandContext&) { return WindowRegistry::instance().frame_count() > 1; };
}

ActionPredicate require_more_than_one_monitor() {
    return [](const CommandContext&) { return QGuiApplication::screens().size() > 1; };
}

// ── Handler helpers ──────────────────────────────────────────────────────────

Result<void> handler_fullscreen(const CommandContext& ctx) {
    if (!ctx.focused_frame)
        return Result<void>::ok();
    if (ctx.focused_frame->isFullScreen())
        ctx.focused_frame->showNormal();
    else
        ctx.focused_frame->showFullScreen();
    return Result<void>::ok();
}

Result<void> handler_focus_mode(const CommandContext& ctx) {
    if (!ctx.focused_frame)
        return Result<void>::ok();
    ctx.focused_frame->toggle_focus_mode();
    return Result<void>::ok();
}

Result<void> handler_refresh(const CommandContext& ctx) {
    if (!ctx.focused_frame)
        return Result<void>::ok();
    ctx.focused_frame->refresh_focused_panel();
    return Result<void>::ok();
}

Result<void> handler_screenshot(const CommandContext& ctx) {
    if (!ctx.focused_frame)
        return Result<void>::ok();
    QScreen* scr = ctx.focused_frame->screen();
    if (!scr)
        scr = QApplication::primaryScreen();
    if (!scr)
        return Result<void>::err("No screen available for screenshot");

    QPixmap px = scr->grabWindow(ctx.focused_frame->winId());
    const QString path = QFileDialog::getSaveFileName(
        ctx.focused_frame, "Save Screenshot",
        QDir::homePath() + "/fincept_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".png",
        "PNG Images (*.png)");
    if (!path.isEmpty()) {
        px.save(path, "PNG");
        LOG_INFO(kBuiltinTag, QString("Screenshot saved: %1").arg(path));
    }
    return Result<void>::ok();
}

Result<void> handler_always_on_top(const CommandContext& ctx) {
    if (!ctx.focused_frame)
        return Result<void>::ok();
    ctx.focused_frame->set_always_on_top(!ctx.focused_frame->is_always_on_top());
    return Result<void>::ok();
}

Result<void> handler_browse_components(const CommandContext& ctx) {
    if (!ctx.focused_frame)
        return Result<void>::ok();
    ctx.focused_frame->open_component_browser();
    return Result<void>::ok();
}

Result<void> handler_toggle_debug_overlay(const CommandContext& ctx) {
    if (!ctx.focused_frame)
        return Result<void>::ok();
    ctx.focused_frame->toggle_debug_overlay();
    return Result<void>::ok();
}

// ── Phase 6: layout.* actions ───────────────────────────────────────────────

Result<void> handler_layout_save(const CommandContext& ctx) {
    const QString name = ctx.args.value(QStringLiteral("name")).toString().trimmed();
    if (name.isEmpty())
        return Result<void>::err("Missing 'name' argument");

    // Phase 6 final: capture the live shell state via WorkspaceShell.
    // If a layout with the same name already exists, preserve its UUID +
    // variant history so the save is "add a new variant / refresh the
    // frame snapshot," not a name collision.
    layout::Workspace prev;
    bool has_prev = false;
    LayoutId existing_id = LayoutCatalog::instance().find_by_name(name);
    if (!existing_id.is_null()) {
        auto load_r = LayoutCatalog::instance().load_workspace(existing_id);
        if (load_r.is_ok()) {
            prev = load_r.value();
            has_prev = true;
        }
    }
    layout::Workspace w = layout::WorkspaceShell::capture(name, "user", has_prev ? &prev : nullptr);

    auto r = LayoutCatalog::instance().save_workspace(w);
    if (r.is_err())
        return Result<void>::err(r.error());
    LOG_INFO("BuiltinActions", QString("layout.save: '%1' saved as %2 (%3 frames, %4 variants)")
                                   .arg(name, r.value().to_string())
                                   .arg(w.frames.size())
                                   .arg(w.variants.size()));
    return Result<void>::ok();
}

Result<void> handler_layout_save_as(const CommandContext& ctx) {
    return handler_layout_save(ctx); // identical for v1; UI will distinguish
}

Result<void> handler_layout_switch(const CommandContext& ctx) {
    const QString name_or_id = ctx.args.value(QStringLiteral("name")).toString().trimmed();
    if (name_or_id.isEmpty())
        return Result<void>::err("Missing 'name' argument");
    LayoutId id = LayoutCatalog::instance().find_by_name(name_or_id);
    if (id.is_null())
        id = LayoutId::from_string(name_or_id);
    if (id.is_null())
        return Result<void>::err(("No layout named " + name_or_id).toStdString());

    auto r = LayoutCatalog::instance().load_workspace(id);
    if (r.is_err())
        return Result<void>::err(r.error());

    // Phase 6 final: actually apply the workspace to live frames.
    const int applied = layout::WorkspaceShell::apply(r.value());
    LOG_INFO("BuiltinActions", QString("layout.switch: '%1' applied (%2/%3 frames restored)")
                                   .arg(r.value().name)
                                   .arg(applied)
                                   .arg(r.value().frames.size()));
    return Result<void>::ok();
}

Result<void> handler_layout_new(const CommandContext& ctx) {
    // "New" = save the current state under a fresh name, blank Workspace.
    layout::Workspace w;
    w.name = ctx.args.value(QStringLiteral("name")).toString().trimmed();
    if (w.name.isEmpty())
        w.name = QStringLiteral("New Layout");
    w.kind = "user";
    w.created_at_unix = QDateTime::currentSecsSinceEpoch();
    w.updated_at_unix = w.created_at_unix;
    auto r = LayoutCatalog::instance().save_workspace(w);
    if (r.is_err())
        return Result<void>::err(r.error());
    return Result<void>::ok();
}

Result<void> handler_layout_export(const CommandContext& ctx) {
    const QString name = ctx.args.value(QStringLiteral("name")).toString().trimmed();
    const QString path = ctx.args.value(QStringLiteral("path")).toString().trimmed();
    if (name.isEmpty() || path.isEmpty())
        return Result<void>::err("layout.export needs 'name' and 'path' arguments");
    LayoutId id = LayoutCatalog::instance().find_by_name(name);
    if (id.is_null())
        return Result<void>::err(("No layout named " + name).toStdString());
    auto r = LayoutCatalog::instance().export_to(id, path);
    if (r.is_err())
        return Result<void>::err(r.error());
    LOG_INFO("BuiltinActions", QString("layout.export: '%1' → %2").arg(name, path));
    return Result<void>::ok();
}

Result<void> handler_layout_import(const CommandContext& ctx) {
    const QString path = ctx.args.value(QStringLiteral("path")).toString().trimmed();
    if (path.isEmpty())
        return Result<void>::err("layout.import needs 'path' argument");
    auto r = LayoutCatalog::instance().import_from(path);
    if (r.is_err())
        return Result<void>::err(r.error());
    LOG_INFO("BuiltinActions", QString("layout.import: %1 → uuid %2").arg(path, r.value().to_string()));
    return Result<void>::ok();
}

Result<void> handler_layout_tile_2x2(const CommandContext& ctx) {
    // Decision 5.5: tile current frame's panels into a 2x2 grid across the
    // active monitor. Implementation in DockScreenRouter::tile_2x2.
    if (!ctx.focused_frame)
        return Result<void>::ok();
    if (auto* router = ctx.focused_frame->dock_router())
        router->tile_2x2();
    return Result<void>::ok();
}

Result<void> handler_panel_fullscreen_toggle(const CommandContext& ctx) {
    // Decision 5.10: focused panel takes over its frame, chrome hides.
    // Wire to WindowFrame::toggle_focus_mode for v1 (which already hides
    // the toolbar/status/pushpin chrome). The "panel takes over" half is
    // a Phase 6 polish (needs router-level "show only this panel" mode).
    if (!ctx.focused_frame)
        return Result<void>::ok();
    ctx.focused_frame->toggle_focus_mode();
    return Result<void>::ok();
}

Result<void> handler_cmdbar_toggle(const CommandContext& ctx) {
    if (!ctx.focused_frame)
        return Result<void>::ok();
    ctx.focused_frame->toggle_command_bar();
    return Result<void>::ok();
}

Result<void> handler_palette_open(const CommandContext& ctx) {
    if (!ctx.focused_frame)
        return Result<void>::ok();
    ctx.focused_frame->open_command_palette();
    return Result<void>::ok();
}

Result<void> handler_debug_stress(const CommandContext& ctx) {
    // Args: n (frames), m (panels per frame), k (ticks/sec). Defaults
    // mirror the design doc 9.10 example: `debug stress 4 16 256`.
    bool ok = false;
    int n = ctx.args.value(QStringLiteral("n")).toInt(&ok);
    if (!ok || n <= 0)
        n = 4;
    int m = ctx.args.value(QStringLiteral("m")).toInt(&ok);
    if (!ok || m <= 0)
        m = 16;
    int k = ctx.args.value(QStringLiteral("k")).toInt(&ok);
    if (!ok || k <= 0)
        k = 256;

    if (debug_tools::StressLoad::instance().is_running())
        debug_tools::StressLoad::instance().stop();
    else
        debug_tools::StressLoad::instance().start(n, m, k);
    return Result<void>::ok();
}

// ── Phase 5: tear-off + move-to-window ──────────────────────────────────────

QString focused_panel_id_(WindowFrame* frame) {
    // Resolve the panel id of whichever dock widget is currently focused
    // in the frame. The router's current_screen_id is the most-recently-
    // navigated panel which usually matches focus, but ADS lets the user
    // click another panel to focus it without going through navigate().
    // Fall back to current_screen_id if no focused dock widget.
    if (!frame || !frame->dock_router())
        return {};
    // Reach into the router via its public dock_widgets_ accessor — none
    // exists, so we use current_screen_id as the v1 approximation. A future
    // helper on DockScreenRouter could expose the focused panel id directly.
    return frame->dock_router()->current_screen_id();
}

Result<void> handler_tear_off(const CommandContext& ctx) {
    if (!ctx.focused_frame)
        return Result<void>::ok();
    auto* router = ctx.focused_frame->dock_router();
    if (!router)
        return Result<void>::err("Focused frame has no dock router");
    const QString id = focused_panel_id_(ctx.focused_frame);
    if (id.isEmpty())
        return Result<void>::err("No focused panel to tear off");
    return router->tear_off_to_new_frame(id) ? Result<void>::ok()
                                             : Result<void>::err("tear_off_to_new_frame returned false");
}

// ── Phase 7 polish: link.* actions ──────────────────────────────────────────
//
// These wrap IGroupLinked operations on whichever panel is currently focused
// in the active frame. Discoverability is via the right-click menu on the
// panel tab (already present); the actions exist so command-bar / palette
// surfaces (Phase 9) can reach them too. Parameter slots designed so a
// command like `link group red` resolves to set_group(SymbolGroup::A) etc.

IGroupLinked* focused_linked_(const CommandContext& ctx) {
    if (!ctx.focused_frame || !ctx.focused_frame->dock_router())
        return nullptr;
    auto* router = ctx.focused_frame->dock_router();
    const QString id = router->current_screen_id();
    if (id.isEmpty())
        return nullptr;
    auto* dw = router->find_dock_widget(id);
    if (!dw || !dw->widget())
        return nullptr;
    return dynamic_cast<IGroupLinked*>(dw->widget());
}

SymbolGroup parse_group_arg_(const QVariant& v) {
    // Accept either a single letter ("A".."J", case-insensitive, "None") or
    // an int (0=None, 1=A, ..., 10=J). Returns SymbolGroup::None for
    // unrecognised input — predicate-checked at call time.
    if (v.typeId() == QMetaType::Int) {
        const int i = v.toInt();
        if (i <= 0 || i > 10)
            return SymbolGroup::None;
        return symbol_group_from_char(QChar('A' + (i - 1)));
    }
    const QString s = v.toString().trimmed();
    if (s.isEmpty() || s.compare("none", Qt::CaseInsensitive) == 0)
        return SymbolGroup::None;
    return symbol_group_from_char(s.at(0).toUpper());
}

Result<void> handler_link_set_group(const CommandContext& ctx) {
    auto* linked = focused_linked_(ctx);
    if (!linked)
        return Result<void>::err("Focused panel does not support linking");
    const SymbolGroup g = parse_group_arg_(ctx.args.value(QStringLiteral("group")));
    if (g == SymbolGroup::None)
        return Result<void>::err("Missing or invalid 'group' argument (expect A..J)");
    if (!SymbolGroupRegistry::instance().enabled(g))
        return Result<void>::err(QString("Group %1 is disabled").arg(symbol_group_letter(g)).toStdString());

    linked->set_group(g);
    // Mirror the right-click flow: if the group already has a symbol, push it
    // immediately; otherwise seed the group from the panel's current symbol.
    const SymbolRef group_sym = SymbolContext::instance().group_symbol(g);
    if (group_sym.is_valid()) {
        linked->on_group_symbol_changed(group_sym);
    } else {
        const SymbolRef own = linked->current_symbol();
        if (own.is_valid())
            SymbolContext::instance().set_group_symbol(g, own, dynamic_cast<QObject*>(linked));
    }
    return Result<void>::ok();
}

Result<void> handler_link_clear_group(const CommandContext& ctx) {
    auto* linked = focused_linked_(ctx);
    if (!linked)
        return Result<void>::err("Focused panel does not support linking");
    linked->set_group(SymbolGroup::None);
    return Result<void>::ok();
}

Result<void> handler_link_publish_to_group(const CommandContext& ctx) {
    // Used by the command bar to broadcast a symbol into a colour group from
    // anywhere — does not require a focused linked panel. Args:
    //   group: A..J or None
    //   symbol: ticker string (asset_class/exchange not supplied via this
    //           action; SymbolRef defaults match the most common case).
    const SymbolGroup g = parse_group_arg_(ctx.args.value(QStringLiteral("group")));
    if (g == SymbolGroup::None)
        return Result<void>::err("Missing or invalid 'group' argument");
    const QString symbol = ctx.args.value(QStringLiteral("symbol")).toString().trimmed();
    if (symbol.isEmpty())
        return Result<void>::err("Missing 'symbol' argument");

    SymbolRef ref;
    ref.symbol = symbol.toUpper();
    // asset_class/exchange omitted — consumers default to equity behaviour.
    SymbolContext::instance().set_group_symbol(g, ref, nullptr);
    return Result<void>::ok();
}

Result<void> handler_move_to_frame(const CommandContext& ctx) {
    if (!ctx.focused_frame)
        return Result<void>::ok();
    auto* router = ctx.focused_frame->dock_router();
    if (!router)
        return Result<void>::err("Focused frame has no dock router");
    const QString id = focused_panel_id_(ctx.focused_frame);
    if (id.isEmpty())
        return Result<void>::err("No focused panel to move");

    // The action expects a frame_index parameter (1-based, mirroring
    // Ctrl+1..9 ergonomics). Resolve the target frame; refuse if it's
    // the same as the focused frame.
    bool ok = false;
    const int frame_index = ctx.args.value(QStringLiteral("frame_index")).toInt(&ok);
    if (!ok || frame_index < 1)
        return Result<void>::err("Missing or invalid frame_index argument");

    const auto frames = WindowRegistry::instance().frames();
    if (frame_index > frames.size())
        return Result<void>::err("frame_index out of range");
    auto* target_frame = frames.at(frame_index - 1);
    if (!target_frame || target_frame == ctx.focused_frame)
        return Result<void>::err("Target frame is the same as the focused frame");
    if (!target_frame->dock_router())
        return Result<void>::err("Target frame has no dock router");

    return router->move_panel_to_frame(id, target_frame->dock_router())
               ? Result<void>::ok()
               : Result<void>::err("move_panel_to_frame returned false");
}

Result<void> handler_focus_window_n(int n, const CommandContext&) {
    WindowCycler::instance().focus_window_by_index(n);
    return Result<void>::ok();
}

Result<void> handler_cycle_windows(bool forward, const CommandContext&) {
    WindowCycler::instance().cycle_windows(forward);
    return Result<void>::ok();
}

Result<void> handler_cycle_panels(bool forward, const CommandContext&) {
    WindowCycler::instance().cycle_panels_in_current_window(forward);
    return Result<void>::ok();
}

Result<void> handler_new_window_next_monitor(const CommandContext&) {
    WindowCycler::instance().new_window_on_next_monitor();
    return Result<void>::ok();
}

Result<void> handler_move_window_to_monitor(int n, const CommandContext&) {
    WindowCycler::instance().move_current_window_to_monitor(n);
    return Result<void>::ok();
}

Result<void> handler_replay_tour(const CommandContext& ctx) {
    // Reset the seen-flag so subsequent launches also replay until the
    // user finishes the tour, then surface it now over the focused frame
    // (or with no parent if no frame is currently focused — the tour
    // window-manages itself).
    fincept::screens::OnboardingTour::reset_seen();
    QWidget* parent = nullptr;
    if (ctx.focused_frame)
        parent = ctx.focused_frame;
    fincept::screens::OnboardingTour::show_for(parent);
    return Result<void>::ok();
}

// ── Mapping from KeyAction enum to action id strings ──────────────────────
//
// The id is what the registry, command bar, and hotkey-binding layer use.
// Pre-Phase 4 the only handle on an action was the KeyAction enum value;
// post-Phase 4 the id string is the canonical form. KeyConfigManager
// continues to own user-bound QKeySequence persistence — we just look up
// the current binding via `KeyConfigManager::key(action)` when registering.

QKeySequence current_key_for(KeyAction a) {
    return KeyConfigManager::instance().key(a);
}

void register_one(const ActionDef& def) {
    ActionRegistry::instance().register_action(def);
}

} // namespace

void register_builtins() {
    auto& reg = ActionRegistry::instance();
    if (reg.size() > 0) {
        LOG_DEBUG(kBuiltinTag, "register_builtins called twice — skipping");
        return;
    }

    // ── Frame-level chrome / view actions ──────────────────────────────────

    register_one(ActionDef{
        "frame.toggle_fullscreen",
        "Toggle Fullscreen",
        "Frame",
        {"fullscreen"},
        current_key_for(KeyAction::Fullscreen),
        require_focused_frame(),
        &handler_fullscreen,
        {},
    });

    register_one(ActionDef{
        "frame.toggle_focus_mode",
        "Toggle Focus Mode",
        "Frame",
        {"focus mode", "distraction free"},
        current_key_for(KeyAction::FocusMode),
        require_focused_frame(),
        &handler_focus_mode,
        {},
    });

    register_one(ActionDef{
        "frame.toggle_always_on_top",
        "Toggle Always On Top",
        "Frame",
        {"always on top", "pin window"},
        current_key_for(KeyAction::ToggleAlwaysOnTop),
        require_focused_frame(),
        &handler_always_on_top,
        {},
    });

    register_one(ActionDef{
        "frame.screenshot",
        "Save Screenshot",
        "Frame",
        {"screenshot", "capture"},
        current_key_for(KeyAction::Screenshot),
        require_focused_frame(),
        &handler_screenshot,
        {},
    });

    register_one(ActionDef{
        "frame.browse_components",
        "Browse Components",
        "Frame",
        {"components", "screen browser"},
        current_key_for(KeyAction::BrowseComponents),
        require_focused_frame(),
        &handler_browse_components,
        {},
    });

    // Phase 10 trim: debug overlay toggle. No default hotkey — discoverable
    // via command bar. Frame-level so each frame has its own overlay (the
    // user can leave it on for the frame they're tuning and off elsewhere).
    register_one(ActionDef{
        "debug.overlay",
        "Toggle Debug Overlay",
        "Debug",
        {"debug", "overlay", "stats"},
        QKeySequence(),
        require_focused_frame(),
        &handler_toggle_debug_overlay,
        {},
    });

    // ── Phase 6: layout.* actions ─────────────────────────────────────────

    register_one(ActionDef{
        "layout.save",
        "Save Current Layout",
        "Layout",
        {"save layout", "save workspace"},
        QKeySequence(),
        /*predicate*/ {},
        &handler_layout_save,
        {ParameterSlot{"name", "Layout name", "string", true, "", {}}},
    });

    register_one(ActionDef{
        "layout.save_as",
        "Save Layout As…",
        "Layout",
        {"save as", "save workspace as"},
        QKeySequence(),
        /*predicate*/ {},
        &handler_layout_save_as,
        {ParameterSlot{"name", "Layout name", "string", true, "", {}}},
    });

    register_one(ActionDef{
        "layout.switch",
        "Switch Layout",
        "Layout",
        {"switch layout", "load layout"},
        QKeySequence(),
        /*predicate*/ {},
        &handler_layout_switch,
        {ParameterSlot{"name", "Layout name or UUID", "string", true, "layout_name", {}}},
    });

    register_one(ActionDef{
        "layout.new",
        "New Layout",
        "Layout",
        {"new layout"},
        QKeySequence(),
        /*predicate*/ {},
        &handler_layout_new,
        {ParameterSlot{"name", "Layout name", "string", false, "", "New Layout"}},
    });

    register_one(ActionDef{
        "layout.export",
        "Export Layout",
        "Layout",
        {"export layout"},
        QKeySequence(),
        /*predicate*/ {},
        &handler_layout_export,
        {
            ParameterSlot{"name", "Layout name", "string", true, "layout_name", {}},
            ParameterSlot{"path", "Export path", "string", true, "", {}},
        },
    });

    register_one(ActionDef{
        "layout.import",
        "Import Layout",
        "Layout",
        {"import layout"},
        QKeySequence(),
        /*predicate*/ {},
        &handler_layout_import,
        {ParameterSlot{"path", "Import path", "string", true, "", {}}},
    });

    register_one(ActionDef{
        "layout.tile_2x2",
        "Tile Current Layout 2x2",
        "Layout",
        {"tile", "2x2"},
        QKeySequence(),
        require_focused_frame(),
        &handler_layout_tile_2x2,
        {},
    });

    register_one(ActionDef{
        "panel.fullscreen_toggle",
        "Fullscreen Focused Panel",
        "Panel",
        {"panel fullscreen", "zoom panel"},
        QKeySequence(),
        require_focused_frame(),
        &handler_panel_fullscreen_toggle,
        {},
    });

    // ── Phase 9: command bar + palette ──────────────────────────────────────

    register_one(ActionDef{
        "cmdbar.toggle",
        "Toggle Command Bar",
        "Command",
        {"command bar", "show command bar"},
        QKeySequence(QStringLiteral("Ctrl+\\")),
        require_focused_frame(),
        &handler_cmdbar_toggle,
        {},
    });

    register_one(ActionDef{
        "palette.open",
        "Open Command Palette",
        "Command",
        {"palette", "command palette"},
        QKeySequence(QStringLiteral("Ctrl+K")),
        require_focused_frame(),
        &handler_palette_open,
        {},
    });

    register_one(ActionDef{
        "debug.stress",
        "Toggle Stress Load (N frames × M panels × K ticks/sec)",
        "Debug",
        {"stress", "synthetic load", "perf test"},
        QKeySequence(),
        /*predicate*/ {},
        &handler_debug_stress,
        {
            ParameterSlot{"n", "Frame count", "int", false, "", 4},
            ParameterSlot{"m", "Panels per frame", "int", false, "", 16},
            ParameterSlot{"k", "Ticks per second", "int", false, "", 256},
        },
    });

    // ── Panel-level actions ────────────────────────────────────────────────

    register_one(ActionDef{
        "panel.refresh",
        "Refresh Focused Panel",
        "Panel",
        {"refresh", "reload"},
        current_key_for(KeyAction::Refresh),
        require_focused_frame(),
        &handler_refresh,
        {},
    });

    register_one(ActionDef{
        "panel.cycle_forward",
        "Cycle Panels Forward",
        "Panel",
        {"next panel"},
        current_key_for(KeyAction::CyclePanelsForward),
        require_focused_frame(),
        [](const CommandContext& ctx) { return handler_cycle_panels(true, ctx); },
        {},
    });

    register_one(ActionDef{
        "panel.cycle_back",
        "Cycle Panels Back",
        "Panel",
        {"previous panel"},
        current_key_for(KeyAction::CyclePanelsBack),
        require_focused_frame(),
        [](const CommandContext& ctx) { return handler_cycle_panels(false, ctx); },
        {},
    });

    // Phase 5: tear off the focused panel into a brand-new window. No
    // default hotkey — discoverability via the right-click menu and the
    // command bar (when Phase 9 lands).
    register_one(ActionDef{
        "panel.tear_off",
        "Tear off panel into new window",
        "Panel",
        {"tear off", "panel popout", "popout panel"},
        QKeySequence(),
        require_focused_frame(),
        &handler_tear_off,
        {},
    });

    // Phase 5: move the focused panel to another frame by display number.
    // Parameter slot frame_index (1-based) drives the target lookup. The
    // command-bar parser populates it from typed args; the right-click
    // menu invokes move_panel_to_frame directly without going through
    // the registry, so this entry is mostly for command-bar / future
    // palette discoverability.
    // ── Phase 7 polish: link.* actions ─────────────────────────────────────
    //
    // Discoverability is via the right-click panel menu (already present);
    // these registrations expose the same operations to the command bar and
    // palette (Phase 9 consumers).

    register_one(ActionDef{
        "link.set_group",
        "Link Panel to Group",
        "Link",
        {"link", "link to group", "set group"},
        QKeySequence(),
        require_focused_frame(),
        &handler_link_set_group,
        {
            ParameterSlot{
                /*name*/ "group",
                /*display*/ "Group letter (A..J or None)",
                /*type*/ "string",
                /*required*/ true,
                /*suggestion_source*/ "link_groups",
                /*default_value*/ {},
            },
        },
    });

    register_one(ActionDef{
        "link.clear_group",
        "Unlink Panel",
        "Link",
        {"unlink", "clear link"},
        QKeySequence(),
        require_focused_frame(),
        &handler_link_clear_group,
        {},
    });

    register_one(ActionDef{
        "link.publish_to_group",
        "Publish Symbol to Group",
        "Link",
        {"publish", "broadcast"},
        QKeySequence(),
        /*predicate*/ {}, // works without a focused panel — broadcasts globally
        &handler_link_publish_to_group,
        {
            ParameterSlot{"group", "Group letter (A..J)", "string", true, "link_groups", {}},
            ParameterSlot{"symbol", "Symbol ticker", "string", true, "symbol", {}},
        },
    });

    register_one(ActionDef{
        "panel.move_to_frame",
        "Move panel to window…",
        "Panel",
        {"move panel", "send to window"},
        QKeySequence(),
        require_more_than_one_window(),
        &handler_move_to_frame,
        // parameter_slots
        {
            ParameterSlot{
                /*name*/ "frame_index",
                /*display*/ "Window number",
                /*type*/ "int",
                /*required*/ true,
                /*suggestion_source*/ "open_frames",
                /*default_value*/ {},
            },
        },
    });

    // ── Window-level actions (cross-frame) ─────────────────────────────────

    register_one(ActionDef{
        "window.cycle_forward",
        "Cycle Windows Forward",
        "Window",
        {"next window"},
        current_key_for(KeyAction::CycleWindowsForward),
        require_more_than_one_window(),
        [](const CommandContext& ctx) { return handler_cycle_windows(true, ctx); },
        {},
    });

    register_one(ActionDef{
        "window.cycle_back",
        "Cycle Windows Back",
        "Window",
        {"previous window"},
        current_key_for(KeyAction::CycleWindowsBack),
        require_more_than_one_window(),
        [](const CommandContext& ctx) { return handler_cycle_windows(false, ctx); },
        {},
    });

    register_one(ActionDef{
        "window.new_on_next_monitor",
        "New Window on Next Monitor",
        "Window",
        {"new window", "spawn window"},
        current_key_for(KeyAction::NewWindowNextMonitor),
        /*predicate*/ {}, // always available — works on any monitor count
        &handler_new_window_next_monitor,
        {},
    });

    // FocusWindow1..9 — generated via a loop because the only difference is
    // the index. Each registers as window.focus_1, window.focus_2, etc.
    {
        const KeyAction focus_keys[9] = {
            KeyAction::FocusWindow1, KeyAction::FocusWindow2, KeyAction::FocusWindow3,
            KeyAction::FocusWindow4, KeyAction::FocusWindow5, KeyAction::FocusWindow6,
            KeyAction::FocusWindow7, KeyAction::FocusWindow8, KeyAction::FocusWindow9,
        };
        for (int i = 0; i < 9; ++i) {
            register_one(ActionDef{
                QString("window.focus_%1").arg(i + 1),
                QString("Focus Window %1").arg(i + 1),
                "Window",
                {QString("window %1").arg(i + 1)},
                current_key_for(focus_keys[i]),
                /*predicate*/ {}, // no-op if index out of range — registry layer doesn't need to know
                [i](const CommandContext& ctx) { return handler_focus_window_n(i, ctx); },
                {},
            });
        }
    }

    // MoveWindowToMonitor1..9 — same pattern.
    {
        const KeyAction move_keys[9] = {
            KeyAction::MoveWindowToMonitor1, KeyAction::MoveWindowToMonitor2, KeyAction::MoveWindowToMonitor3,
            KeyAction::MoveWindowToMonitor4, KeyAction::MoveWindowToMonitor5, KeyAction::MoveWindowToMonitor6,
            KeyAction::MoveWindowToMonitor7, KeyAction::MoveWindowToMonitor8, KeyAction::MoveWindowToMonitor9,
        };
        for (int i = 0; i < 9; ++i) {
            register_one(ActionDef{
                QString("window.move_to_monitor_%1").arg(i + 1),
                QString("Move Window to Monitor %1").arg(i + 1),
                "Window",
                {QString("monitor %1").arg(i + 1)},
                current_key_for(move_keys[i]),
                require_more_than_one_monitor(),
                [i](const CommandContext& ctx) { return handler_move_window_to_monitor(i, ctx); },
                {},
            });
        }
    }

    // ── Help-level actions ──────────────────────────────────────────────────

    register_one(ActionDef{
        "help.replay_tour",
        "Replay Welcome Tour",
        "Help",
        {"tour", "replay tour", "onboarding"},
        QKeySequence{}, // no default hotkey — discoverable via palette
        /*predicate*/ {},
        &handler_replay_tour,
        {},
    });

    LOG_INFO(kBuiltinTag, QString("Registered %1 builtin actions").arg(ActionRegistry::instance().size()));
}

QString action_id_for(KeyAction a) {
    // Kept in sync with the register_one() calls above. Per-screen
    // KeyActions (NavNext, NewsOpen, RunCell, etc.) intentionally return
    // empty — those bind directly inside their owning screens, not via
    // the global ActionRegistry.
    switch (a) {
        case KeyAction::Refresh:
            return QStringLiteral("panel.refresh");
        case KeyAction::FocusMode:
            return QStringLiteral("frame.toggle_focus_mode");
        case KeyAction::Fullscreen:
            return QStringLiteral("frame.toggle_fullscreen");
        case KeyAction::Screenshot:
            return QStringLiteral("frame.screenshot");
        case KeyAction::ToggleAlwaysOnTop:
            return QStringLiteral("frame.toggle_always_on_top");
        case KeyAction::BrowseComponents:
            return QStringLiteral("frame.browse_components");
        case KeyAction::CyclePanelsForward:
            return QStringLiteral("panel.cycle_forward");
        case KeyAction::CyclePanelsBack:
            return QStringLiteral("panel.cycle_back");
        case KeyAction::CycleWindowsForward:
            return QStringLiteral("window.cycle_forward");
        case KeyAction::CycleWindowsBack:
            return QStringLiteral("window.cycle_back");
        case KeyAction::NewWindowNextMonitor:
            return QStringLiteral("window.new_on_next_monitor");
        case KeyAction::FocusWindow1:
            return QStringLiteral("window.focus_1");
        case KeyAction::FocusWindow2:
            return QStringLiteral("window.focus_2");
        case KeyAction::FocusWindow3:
            return QStringLiteral("window.focus_3");
        case KeyAction::FocusWindow4:
            return QStringLiteral("window.focus_4");
        case KeyAction::FocusWindow5:
            return QStringLiteral("window.focus_5");
        case KeyAction::FocusWindow6:
            return QStringLiteral("window.focus_6");
        case KeyAction::FocusWindow7:
            return QStringLiteral("window.focus_7");
        case KeyAction::FocusWindow8:
            return QStringLiteral("window.focus_8");
        case KeyAction::FocusWindow9:
            return QStringLiteral("window.focus_9");
        case KeyAction::MoveWindowToMonitor1:
            return QStringLiteral("window.move_to_monitor_1");
        case KeyAction::MoveWindowToMonitor2:
            return QStringLiteral("window.move_to_monitor_2");
        case KeyAction::MoveWindowToMonitor3:
            return QStringLiteral("window.move_to_monitor_3");
        case KeyAction::MoveWindowToMonitor4:
            return QStringLiteral("window.move_to_monitor_4");
        case KeyAction::MoveWindowToMonitor5:
            return QStringLiteral("window.move_to_monitor_5");
        case KeyAction::MoveWindowToMonitor6:
            return QStringLiteral("window.move_to_monitor_6");
        case KeyAction::MoveWindowToMonitor7:
            return QStringLiteral("window.move_to_monitor_7");
        case KeyAction::MoveWindowToMonitor8:
            return QStringLiteral("window.move_to_monitor_8");
        case KeyAction::MoveWindowToMonitor9:
            return QStringLiteral("window.move_to_monitor_9");
        // Per-screen actions — not in ActionRegistry yet.
        case KeyAction::NavNext:
        case KeyAction::NavPrev:
        case KeyAction::NavAccept:
        case KeyAction::NavEscape:
        case KeyAction::NewsNext:
        case KeyAction::NewsPrev:
        case KeyAction::NewsOpen:
        case KeyAction::NewsClose:
        case KeyAction::RunCell:
        case KeyAction::RunAndNext:
        case KeyAction::RenameCell:
            return {};
    }
    return {};
}

} // namespace fincept::actions
