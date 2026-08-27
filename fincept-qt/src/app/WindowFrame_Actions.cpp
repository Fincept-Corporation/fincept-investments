// src/app/WindowFrame_Actions.cpp
//
// Public action handlers — focus mode, command palette, debug overlay,
// component browser, and the periodic dock-layout save.
// These are invoked from the ActionRegistry (builtin_actions.cpp) or
// directly from menu/shortcut hooks.
//
// Part of the partial-class split of WindowFrame.cpp.

#include "app/DockScreenRouter.h"
#include "app/WindowFrame.h"
#include "core/logging/Logger.h"
#include "core/session/SessionManager.h"
#include "ui/command/CommandPalette.h"
#include "ui/command/QuickCommandBar.h"
#include "ui/components/ComponentBrowserDialog.h"
#include "ui/debug/DebugOverlay.h"
#include "ui/navigation/DockStatusBar.h"
#include "ui/navigation/DockToolBar.h"
#include "ui/theme/Theme.h"

#include <QToolBar>

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>

namespace fincept {

void WindowFrame::toggle_focus_mode() {
    focus_mode_ = !focus_mode_;
    if (dock_toolbar_)
        dock_toolbar_->setVisible(!focus_mode_);
    if (dock_status_bar_)
        dock_status_bar_->setVisible(!focus_mode_);
}

void WindowFrame::refresh_focused_panel() {
    if (!dock_manager_)
        return;
    auto* focused = dock_manager_->focusedDockWidget();
    if (focused && focused->widget())
        QMetaObject::invokeMethod(focused->widget(), "refresh", Qt::QueuedConnection);
}

void WindowFrame::open_component_browser() {
    auto* dlg = new ui::ComponentBrowserDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &ui::ComponentBrowserDialog::component_chosen, this, [this](const QString& id) {
        if (dock_router_ && !id.isEmpty())
            dock_router_->navigate(id);
    });
    dlg->show();
}

void WindowFrame::toggle_debug_overlay() {
    // Lazy construction so users who never enable the overlay don't pay
    // for the QTimer + QLabel cost.
    if (!debug_overlay_) {
        debug_overlay_ = new ui::DebugOverlay(this);
        // Parent it to the main window so resizes auto-trigger reposition
        // via our resizeEvent. The overlay uses
        // WA_TransparentForMouseEvents so it never blocks dock interaction.
    }
    debug_overlay_->toggle_visible();
}

void WindowFrame::toggle_command_bar() {
    // Phase 9: lazy construction. Wraps a `ui::CommandBar` in a bottom
    // QToolBar so QMainWindow lays it out above the status bar.
    if (!command_bar_) {
        auto* bar = new ui::QuickCommandBar(this);
        auto* host = new QToolBar(this);
        host->setObjectName("commandBarHost");
        host->setMovable(false);
        host->setFloatable(false);
        host->setAllowedAreas(Qt::BottomToolBarArea);
        host->addWidget(bar);
        addToolBar(Qt::BottomToolBarArea, host);
        host->setVisible(false);
        host->setProperty("commandBarInner", QVariant::fromValue(static_cast<QWidget*>(bar)));
        command_bar_ = host;
    }
    if (command_bar_) {
        const bool was_visible = command_bar_->isVisible();
        command_bar_->setVisible(!was_visible);
        if (!was_visible) {
            if (auto* inner = command_bar_->property("commandBarInner").value<QWidget*>()) {
                if (auto* cb = qobject_cast<ui::QuickCommandBar*>(inner))
                    cb->surface();
            }
        }
    }
}

void WindowFrame::open_command_palette() {
    ui::CommandPalette::show_for(this);
}

void WindowFrame::schedule_dock_layout_save() {
    if (suppress_layout_save_ || !dock_layout_save_timer_)
        return;
    dock_layout_save_timer_->start();
}

} // namespace fincept
