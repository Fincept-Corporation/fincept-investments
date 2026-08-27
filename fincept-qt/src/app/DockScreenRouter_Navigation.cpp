// src/app/DockScreenRouter_Navigation.cpp
//
// User-driven panel navigation: navigate (open/exclusive), tab_into,
// add_alongside, remove_screen, replace_screen, ensure_all_registered.
// These are the entry points the rest of the app calls when the user picks
// a screen from the command palette / dock toolbar / etc.
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

void DockScreenRouter::navigate(const QString& id, bool exclusive) {
    LOG_INFO("DockRouter", QString(">>> navigate('%1', exclusive=%2) opened_areas=%3")
                               .arg(id)
                               .arg(exclusive)
                               .arg(manager_ ? manager_->openedDockAreas().size() : 0));
    // Bump popularity so the Component Browser surfaces frequently-used
    // panels at the top. Safe for unknown ids — PopularityTracker no-ops
    // on failure and we still fall into the unknown-screen branch below.
    PopularityTracker::instance().increment(id);

    auto* dw = find_dock_widget(id);

    bool needs_add = false;
    if (!dw) {
        // Screen not yet in dock manager — check if we know how to create it
        if (!factories_.contains(id) && !screens_.contains(id)) {
            LOG_WARN("DockRouter", "Unknown screen: " + id);
            return;
        }
        dw = create_dock_widget(id);
        needs_add = true;
    } else if (!dw->dockManager()) {
        // Widget exists but was never added to the dock manager (pre-created
        // by ensure_all_registered for state restore, but restoreState was
        // skipped or didn't place this widget).
        needs_add = true;
    }

    if (screens_.contains(id) && dw->widget() != screens_[id]) {
        QWidget* old = dw->widget();
        dw->setWidget(screens_[id]);
        if (old) {
            old->setParent(nullptr);
            old->deleteLater();
        }
    }

    materialize_screen(id);

    if (exclusive) {
        for (auto* area : manager_->openedDockAreas()) {
            for (auto* other : area->openedDockWidgets()) {
                if (other != dw)
                    other->toggleView(false);
            }
        }
        for (auto* other : manager_->dockWidgetsMap()) {
            if (other != dw && other->isAutoHide())
                other->toggleView(false);
        }
    }

    // Determine whether this widget needs to be physically placed into a new
    // split area, or whether it already has its own dedicated area.
    //
    // ensure_all_registered() pre-creates every dock widget and groups them all
    // into a SINGLE shared dock area (all hidden). We detect this by checking
    // whether dw's area contains other dock widgets (shared) vs being solo.
    auto* existing_area = dw->dockAreaWidget();
    const bool area_is_poisoned = existing_area && existing_area->dockWidgets().size() > 1;
    const bool must_place = needs_add || !existing_area || area_is_poisoned;

    if (exclusive) {
        // Exclusive: replace whatever's open with this single panel in the
        // central area.
        manager_->addDockWidget(ads::CenterDockWidgetArea, dw);

    } else if (must_place) {
        // Phase 4 grid removal: ADS handles arbitrary splits natively. We
        // tab new panels into the *focused* dock area when one exists, and
        // create a fresh central area otherwise. The user can split panels
        // into a side-by-side layout via ADS drag-drop on the tab bar (the
        // existing right-click "Float Panel" / "Tear off into new window"
        // entries also remain). The pre-Phase-4 hard-coded
        // "right-of-TL → below-TL → right-of-BL" cycle was a v1
        // simplification; this is the cleaner Fincept-grade behaviour
        // (everything tabs into centre by default; user splits explicitly).
        // Product behaviour: a newly-added panel auto-tiles into the next grid
        // slot rather than stacking as a tab. Place it anywhere first so it's in
        // a layout, then re-grid all open panels (newest lands in the next slot:
        // 2→left|right, 3→full-width bottom, 4→2x2, 5+→tab into bottom-right).
        const auto opened = manager_->openedDockAreas();
        ads::CDockAreaWidget* target = nullptr;
        if (auto* focused = manager_->focusedDockWidget()) {
            target = focused->dockAreaWidget();
        }
        if (!target && !opened.isEmpty()) {
            target = opened.last();
        }
        if (target)
            manager_->addDockWidget(ads::CenterDockWidgetArea, dw, target);
        else
            manager_->addDockWidget(ads::CenterDockWidgetArea, dw);
        retile_grid(dw);

    } else {
        // Widget already has a dedicated area — just bring it to the front.
        LOG_DEBUG("DockRouter", QString("[%1] already placed, raising").arg(id));
    }

    // Always call toggleView(true) — addDockWidget() marks the widget as
    // not-closed but does NOT make the tab visible. Without this unconditional
    // call, the tab stays hidden because isClosed() returns false and the
    // previous conditional `if (isClosed()) toggleView(true)` was skipped.
    dw->toggleView(true);
    // ADS does not restore CDockWidgetTab visibility when toggleView(true)
    // re-opens a previously hidden widget — the tab stays hidden from the
    // ensure_all_registered() phase where all widgets were toggleView(false)'d.
    // Force the tab visible so the label text appears in the title bar.
    if (auto* tab = dw->tabWidget()) {
        if (tab->isHidden())
            tab->setVisible(true);
    }
    dw->raise();
    dw->setAsCurrentTab();

    apply_ads_theme();

    current_id_ = id;
    if (auto* frame = qobject_cast<WindowFrame*>(parent()))
        SessionManager::instance().set_last_screen(frame->window_id(), id);
    LOG_INFO("DockRouter", "Navigated to: " + id);
    emit screen_changed(id);
}

void DockScreenRouter::tab_into(const QString& id) {
    if (!factories_.contains(id) && !screens_.contains(id)) {
        LOG_WARN("DockRouter", "Unknown screen: " + id);
        return;
    }

    auto* dw = find_dock_widget(id);
    bool needs_add = !dw || !dw->dockManager();

    if (!dw) {
        dw = create_dock_widget(id);
        needs_add = true;
    }

    materialize_screen(id);

    // If already visible somewhere just raise it — no need to re-tab.
    if (!dw->isClosed() && dw->dockAreaWidget()) {
        dw->raise();
        dw->setAsCurrentTab();
        current_id_ = id;
        emit screen_changed(id);
        return;
    }

    // Find the focused/active dock area to tab into.
    // Prefer the focused widget's area; fall back to the first open area.
    ads::CDockAreaWidget* target_area = nullptr;
    if (auto* focused = manager_->focusedDockWidget())
        target_area = focused->dockAreaWidget();
    if (!target_area) {
        const auto opened = manager_->openedDockAreas();
        if (!opened.isEmpty())
            target_area = opened.first();
    }

    if (needs_add || !dw->dockAreaWidget()) {
        if (target_area)
            // Tab into the existing area — CenterDockWidgetArea = same area, new tab
            manager_->addDockWidget(ads::CenterDockWidgetArea, dw, target_area);
        else
            // No area open yet — open as the sole screen
            manager_->addDockWidget(ads::CenterDockWidgetArea, dw);
    }

    dw->toggleView(true);
    if (auto* tab = dw->tabWidget()) {
        if (tab->isHidden())
            tab->setVisible(true);
    }
    dw->raise();
    dw->setAsCurrentTab();

    current_id_ = id;
    if (auto* frame = qobject_cast<WindowFrame*>(parent()))
        SessionManager::instance().set_last_screen(frame->window_id(), id);
    LOG_INFO("DockRouter", "Tabbed into: " + id);
    emit screen_changed(id);
}

void DockScreenRouter::add_alongside(const QString& primary, const QString& secondary) {
    // If primary is already open, just add secondary into the next grid slot
    // without resetting the layout. This allows building up to a 2x2 grid
    // (4 panels) incrementally via repeated "add" commands.
    auto* primary_dw = find_dock_widget(primary);
    bool primary_visible = primary_dw && !primary_dw->isClosed() && primary_dw->dockAreaWidget();

    LOG_INFO("DockRouter", QString("add_alongside: primary=%1 visible=%2 secondary=%3 opened=%4")
                               .arg(primary)
                               .arg(primary_visible)
                               .arg(secondary)
                               .arg(manager_ ? manager_->openedDockAreas().size() : 0));

    if (!primary_visible) {
        navigate(primary, true);
        primary_dw = find_dock_widget(primary);
    }

    // Phase 4 (grid removal): add_alongside is the EXPLICIT "I want a
    // split" API. Pre-Phase-4 navigate() handled the split via the
    // hard-coded grid; post-removal navigate() tabs by default. So we
    // do the split here ourselves: ensure secondary's CDockWidget exists,
    // then addDockWidget(RightDockWidgetArea, secondary, primary's area).
    if (primary_dw) {
        auto* sec_dw = find_dock_widget(secondary);
        bool needs_add = !sec_dw;
        if (!sec_dw) {
            if (!factories_.contains(secondary) && !screens_.contains(secondary)) {
                LOG_WARN("DockRouter", "add_alongside: unknown secondary screen: " + secondary);
                return;
            }
            sec_dw = create_dock_widget(secondary);
        } else if (!sec_dw->dockManager()) {
            needs_add = true;
        }
        materialize_screen(secondary);

        auto* primary_area = primary_dw->dockAreaWidget();
        if (needs_add || !sec_dw->dockAreaWidget() || sec_dw->dockAreaWidget()->dockWidgets().size() > 1) {
            if (primary_area)
                manager_->addDockWidget(ads::RightDockWidgetArea, sec_dw, primary_area);
            else
                manager_->addDockWidget(ads::CenterDockWidgetArea, sec_dw);
        }
        sec_dw->toggleView(true);
        if (auto* tab = sec_dw->tabWidget(); tab && tab->isHidden())
            tab->setVisible(true);
    }

    // Re-focus the primary so it stays the "main" panel — the user typed
    // "primary add secondary", meaning primary is what they're working in.
    primary_dw = find_dock_widget(primary);
    if (primary_dw && !primary_dw->isClosed()) {
        primary_dw->raise();
        primary_dw->setAsCurrentTab();
        current_id_ = primary;
        emit screen_changed(primary);
    }
}

void DockScreenRouter::remove_screen(const QString& primary) {
    // Close all panels except primary, which expands to fill the area.
    auto* keep = find_dock_widget(primary);
    for (auto* dw : manager_->dockWidgetsMap()) {
        if (dw != keep && !dw->isClosed())
            dw->toggleView(false);
    }
    if (keep) {
        if (keep->isClosed())
            keep->toggleView(true);
        keep->raise();
        keep->setAsCurrentTab();
        current_id_ = primary;
        emit screen_changed(primary);
    }
}

void DockScreenRouter::replace_screen(const QString& primary, const QString& secondary) {
    // Navigate to primary (ensures it exists), close everything else, then
    // navigate to secondary in exclusive mode so it fills the full area.
    Q_UNUSED(primary)
    navigate(secondary, true);
}

void DockScreenRouter::ensure_all_registered() {
    // Pre-create CDockWidgets for ALL known screens and register them
    // with the dock manager so restoreState() can find them by objectName
    // in the DockWidgetsMap. All widgets are added as tabs into a single
    // dock area (not separate splits!) and immediately closed. restoreState()
    // will reopen and reposition only those that were visible in the saved layout.
    QStringList all_ids;
    for (auto it = factories_.cbegin(); it != factories_.cend(); ++it)
        all_ids.append(it.key());
    for (auto it = screens_.cbegin(); it != screens_.cend(); ++it) {
        if (!all_ids.contains(it.key()))
            all_ids.append(it.key());
    }

    // Bulk register all factories. The transient visibilityChanged(true)
    // each addDockWidget emits would otherwise eagerly materialise every
    // screen — see suppress_visibility_materialize_ docs.
    suppress_visibility_materialize_ = true;
    ads::CDockAreaWidget* first_area = nullptr;
    for (const QString& id : all_ids) {
        if (!dock_widgets_.contains(id)) {
            auto* dw = create_dock_widget(id);
            // Add all widgets as tabs in the SAME dock area to avoid
            // creating 47 vertical splits. The first widget creates the area,
            // all subsequent ones tab into it.
            if (first_area)
                manager_->addDockWidget(ads::CenterDockWidgetArea, dw, first_area);
            else
                first_area = manager_->addDockWidget(ads::CenterDockWidgetArea, dw);
            dw->toggleView(false); // start hidden
        }
    }
    suppress_visibility_materialize_ = false;
    // Apply theme directly to all title bars and tabs created above.
    apply_ads_theme();
}

} // namespace fincept
