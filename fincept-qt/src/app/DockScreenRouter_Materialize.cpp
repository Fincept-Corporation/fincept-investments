// src/app/DockScreenRouter_Materialize.cpp
//
// Dock-widget construction + lazy materialisation: create_dock_widget,
// prepare_dock_widget, materialize_now, materialize_screen.
//
// Part of the partial-class split of DockScreenRouter.cpp.

#include "app/DockScreenRouter.h"
#include "app/WindowFrame.h"
#include "core/components/PopularityTracker.h"
#include "core/keys/WindowCycler.h"
#include "core/logging/Logger.h"
#include "core/panel/PanelHandle.h"
#include "core/panel/PanelRegistry.h"
#include "core/session/ScreenStateManager.h"
#include "core/session/SessionManager.h"
#include "core/symbol/IGroupLinked.h"
#include "core/symbol/SymbolContext.h"
#include "core/symbol/SymbolGroupRegistry.h"
#include "core/symbol/SymbolRef.h"
#include "core/window/WindowRegistry.h"
#include "screens/common/IStatefulScreen.h"
#include "ui/theme/Theme.h"
#include "ui/widgets/GroupBadge.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QVBoxLayout>

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>
#include <DockWidgetTab.h>

namespace fincept {

ads::CDockWidget* DockScreenRouter::create_dock_widget(const QString& id) {
    if (dock_widgets_.contains(id))
        return dock_widgets_[id];

    auto* dw = new ads::CDockWidget(manager_, title_for_id(id));
    dw->setObjectName(id);

    // Phase 5 polish: DockWidgetFloatable removed alongside the global
    // DoubleClickUndocksWidget flag (see WindowFrame::setup_docking_mode).
    // Tear-off goes through the right-click menu's "Tear off into new
    // window" entry, which uses the close-and-recreate path that
    // preserves the panel UUID + state.
    //
    // Pinnable is intentionally excluded too: the auto-hide pin button
    // converts panels to collapsible sidebars, which causes them to
    // disappear when another panel is opened alongside them.
    dw->setFeatures(ads::CDockWidget::DockWidgetMovable | ads::CDockWidget::DockWidgetClosable |
                    ads::CDockWidget::DockWidgetFocusable);

    // Use dock-widget minimum size (not content size) so screens with large
    // internal canvases (like DashboardCanvas) don't prevent side-by-side splits.
    dw->setMinimumSizeHintMode(ads::CDockWidget::MinimumSizeHintFromDockWidget);
    dw->setMinimumWidth(200);

    // Register the dock widget early so attach_group_badge_to_tab() (called
    // from wrap_with_group_badge below) can resolve the tab via dock_widgets_.
    dock_widgets_[id] = dw;

    // Phase 4b: mint a PanelInstanceId for this string id and register a
    // PanelHandle with PanelRegistry. The handle's frame_id comes from
    // the parent WindowFrame (the router's QObject parent). Two open
    // panels of the same screen type get distinct UUIDs because they go
    // through duplicate_panel() which calls create_dock_widget with a
    // synthetic `<base>#dup<N>` id — each gets its own UUID.
    if (!instance_ids_.contains(id)) {
        WindowId frame_id; // null if the router's parent isn't a WindowFrame
        int win_id = 0;
        if (auto* frame = qobject_cast<WindowFrame*>(parent())) {
            frame_id = frame->frame_uuid();
            win_id = frame->window_id();
        }

        // Derive the instance id DETERMINISTICALLY from window+dock id rather
        // than a fresh random uuid. The dock id is stable across restarts (the
        // ADS layout restores the same ids), so the same panel reuses the same
        // PanelInstanceId every launch — which is what lets ScreenStateManager
        // find the panel's saved UI state again (restore_by_uuid keys on this).
        // A random generate() minted a new id each launch, so per-screen state
        // (FNO underlying/expiry, equity symbol/watchlist, …) was written under
        // an id that never matched on the next run and was effectively lost.
        // window id is included so the same dock id in two windows stays unique.
        const PanelInstanceId panel_uuid = PanelInstanceId::from_name(QStringLiteral("w%1:%2").arg(win_id).arg(id));
        instance_ids_.insert(id, panel_uuid);

        // Strip any "#dup<N>" suffix from the type id so two watchlists
        // share a type_id of "watchlist" while having distinct instance_ids.
        QString type_id = id;
        const int dup_idx = type_id.indexOf("#dup");
        if (dup_idx > 0)
            type_id = type_id.left(dup_idx);

        auto* handle = new PanelHandle(panel_uuid, type_id, title_for_id(type_id), frame_id);
        handle->set_dock_widget(dw);
        handle->set_state(PanelHandle::State::Active); // dock widget is created
        PanelRegistry::instance().register_panel(handle);
    }

    // Screen widgets must allow shrinking so ADS can create side-by-side splits.
    // Factory screens get a lightweight placeholder here; the real widget is swapped
    // in on first navigation via materialize_screen().
    if (screens_.contains(id)) {
        screens_[id]->setMinimumWidth(0);
        dw->setWidget(wrap_with_group_badge(id, screens_[id]));
    } else {
        auto* placeholder = new QWidget;
        // Match the app's dark background so placeholders don't flash
        // white/system-gray during the brief window between show() and
        // deferred screen materialization.
        QPalette pal = placeholder->palette();
        pal.setColor(QPalette::Window, QColor(ui::colors::BG_BASE()));
        placeholder->setPalette(pal);
        placeholder->setAutoFillBackground(true);
        auto* lbl = new QLabel(title_for_id(id));
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setStyleSheet(QString("color:%1;font-size:14px;font-weight:500;").arg(QString(ui::colors::TEXT_DIM())));
        auto* vl = new QVBoxLayout(placeholder);
        vl->addWidget(lbl);
        dw->setWidget(placeholder);
        // Attach a decorative `[·]` badge for factory screens too — once the
        // real screen materialises, attach_group_badge_to_tab is idempotent
        // and won't add a second badge, so the existing one stays in place.
        attach_group_badge_to_tab(id, placeholder);
    }

    // Intercept double-click on the title label inside CDockWidgetTab to allow
    // inline rename. We use the label (objectName "dockWidgetTabLabel"), NOT the
    // tab frame — ADS already uses tab frame double-click to undock/float.
    //
    // Phase 12 — also install the filter on the tab widget itself so right-click
    // anywhere on the tab (including the icon / padding) opens the context
    // menu. The event filter distinguishes by event type, not watched object.
    if (auto* tab = dw->tabWidget()) {
        tab->installEventFilter(this);
        tab->setProperty("dock_id", id);
        tab->setContextMenuPolicy(Qt::DefaultContextMenu);
        if (auto* lbl = tab->findChild<QLabel*>("dockWidgetTabLabel")) {
            lbl->installEventFilter(this);
            lbl->setProperty("dock_id", id);
        }
    }

    // Restore a previously saved custom title (user may have renamed this tab).
    const QString saved_title = load_tab_title(id);
    if (!saved_title.isEmpty())
        dw->setWindowTitle(saved_title);

    // Persist the title whenever the user renames it via the inline editor.
    connect(dw, &ads::CDockWidget::titleChanged, this, [this, id](const QString& title) {
        if (title != title_for_id(id)) // only save if user actually changed it
            save_tab_title(id, title);
    });

    // Materialize on first show; save state on hide.
    connect(dw, &ads::CDockWidget::visibilityChanged, this, [this, id](bool visible) {
        if (visible) {
            // Bulk-register paths (ensure_all_registered, prepare_dock_widget)
            // transiently make the widget visible just to register it with the
            // manager before hiding it again with toggleView(false). Skip the
            // factory call in that window — the user hasn't asked to see this
            // panel yet, and materialising every registered screen at startup
            // is the cold-boot bug we're guarding against.
            if (suppress_visibility_materialize_)
                return;
            materialize_screen(id);
        } else {
            save_screen_state(id);
        }
    });

    // dock_widgets_[id] was registered earlier (before wrap_with_group_badge)
    // so attach_group_badge_to_tab could resolve the tab during construction.
    return dw;
}

void DockScreenRouter::prepare_dock_widget(const QString& id) {
    // Phase 8: pre-create the dock widget shell for restoreState matching.
    // Idempotent — a no-op if the widget already exists (likely registered
    // earlier by ensure_all_registered() at WindowFrame construction).
    if (dock_widgets_.contains(id))
        return;
    if (!factories_.contains(id) && !screens_.contains(id)) {
        LOG_WARN("DockRouter", QString("prepare_dock_widget: unknown id '%1'").arg(id));
        return;
    }
    auto* dw = create_dock_widget(id);
    if (!dw)
        return;

    // CRITICAL: register with the dock manager so restoreState can find
    // the widget by objectName via CDockManager::dockWidgetsMap. Without
    // this, the saved layout silently fails to restore. Mirrors
    // ensure_all_registered's "tab into shared area, hide" pattern so
    // we don't generate N vertical splits during the pre-restore pass.
    if (manager_) {
        // Same rationale as ensure_all_registered: the transient
        // visibilityChanged(true) the addDockWidget emits must not
        // materialise the screen factory before the layout is restored.
        suppress_visibility_materialize_ = true;
        manager_->addDockWidget(ads::CenterDockWidgetArea, dw);
        dw->toggleView(false); // hidden until restoreState repositions it
        suppress_visibility_materialize_ = false;
    }
}

void DockScreenRouter::materialize_now(const QString& id) {
    materialize_screen(id);
}

void DockScreenRouter::materialize_screen(const QString& id) {
    if (screens_.contains(id))
        return;

    auto fit = factories_.find(id);
    if (fit == factories_.end())
        return;

    LOG_INFO("DockRouter", "Lazy-constructing screen: " + id);
    QWidget* screen = nullptr;
    try {
        screen = fit.value()();
    } catch (const std::exception& e) {
        LOG_ERROR("DockRouter", QString("Factory threw for '%1': %2").arg(id, e.what()));
    } catch (...) {
        LOG_ERROR("DockRouter", QString("Factory threw unknown exception for '%1'").arg(id));
    }
    if (!screen) {
        LOG_ERROR("DockRouter", "Factory returned null for: " + id);
        return;
    }
    screen->setMinimumWidth(0);
    // Phase 5: keep the factory in factories_ even after first materialise.
    // Pre-Phase-5 we erased it as a memory tidy-up, but tear-off / move-to-
    // window need to re-create the widget on a different router, which
    // requires the factory to still be available. The factory is a small
    // std::function — no meaningful memory cost. duplicate_panel's
    // factory-reinstall workaround (line ~626) becomes unnecessary too.

    auto* dw = dock_widgets_.value(id, nullptr);
    if (!dw)
        return;

    QWidget* old = dw->widget();
    dw->setWidget(wrap_with_group_badge(id, screen));
    if (old) {
        old->setParent(nullptr);
        old->deleteLater();
    }
    screens_[id] = screen;
    restore_screen_state(id);
}

// ── Tab title persistence ─────────────────────────────────────────────────────

} // namespace fincept
