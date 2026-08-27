// src/app/DockScreenRouter_PanelOps.cpp
//
// Panel-level operations: cycle_focused_panel, duplicate_panel,
// show_tab_context_menu, tile_2x2, adopt_panel_instance,
// move_panel_to_frame, tear_off_to_new_frame.
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
#include <QPoint>
#include <QVBoxLayout>

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>
#include <DockWidgetTab.h>
#include <algorithm>

namespace fincept {

void DockScreenRouter::cycle_focused_panel(bool forward) {
    if (!manager_)
        return;
    // Build an ordered list of currently-visible dock widgets — we cycle
    // through those, not the entire registration catalogue.
    QList<ads::CDockWidget*> visible;
    visible.reserve(dock_widgets_.size());
    for (auto it = dock_widgets_.cbegin(); it != dock_widgets_.cend(); ++it) {
        ads::CDockWidget* dw = it.value();
        if (dw && !dw->isClosed())
            visible.append(dw);
    }
    if (visible.size() < 2)
        return;

    // Find the currently-focused dock widget. ADS exposes focusedDockWidget()
    // but it may be null if the focused QWidget isn't inside any dock —
    // fall back to the dock containing current_id_.
    ads::CDockWidget* current = manager_->focusedDockWidget();
    if (!current)
        current = dock_widgets_.value(current_id_, nullptr);

    int idx = visible.indexOf(current);
    if (idx < 0)
        idx = 0;
    else
        idx = (idx + (forward ? 1 : -1) + visible.size()) % visible.size();

    ads::CDockWidget* next = visible.at(idx);
    next->raise();
    next->setAsCurrentTab();
    if (auto* w = next->widget())
        w->setFocus(Qt::ShortcutFocusReason);
    current_id_ = next->objectName();
    emit screen_changed(current_id_);
}

ads::CDockWidget* DockScreenRouter::duplicate_panel(const QString& id) {
    if (!manager_)
        return nullptr;

    // Strip any existing #dup<N> suffix so duplicates-of-duplicates all derive
    // from the same original factory — prevents unbounded id drift.
    QString base = id;
    const int hash = id.indexOf('#');
    if (hash > 0)
        base = id.left(hash);

    // We need a factory to produce a fresh widget — the eagerly-registered
    // path gives one instance that we can't clone safely.
    auto fit = factories_.find(base);
    // Special case: the original factory was consumed on first materialise,
    // so factories_ may no longer contain `base`. In that case reconstruct
    // from a registered rebuild helper (we don't have one yet) — fail with a
    // warning so the user sees a concrete reason rather than a silent no-op.
    if (fit == factories_.end()) {
        // Only eager (one-instance) screens reach here. Duplication is not
        // supported for them in this phase.
        LOG_WARN("DockRouter", QString("duplicate_panel('%1'): factory not available — duplication requires "
                                       "register_factory() and the original factory is consumed on first materialise. "
                                       "Re-register a factory before duplication to support this path.")
                                   .arg(base));
        return nullptr;
    }

    // Allocate a unique suffixed id. Scan existing ids to avoid collisions.
    int n = 2;
    QString dup_id;
    do {
        dup_id = QString("%1#dup%2").arg(base).arg(n++);
    } while (dock_widgets_.contains(dup_id) || factories_.contains(dup_id));

    // Build the duplicate via a fresh factory copy so navigate() can
    // materialize it on-demand through the existing path. Re-install the
    // base factory too so further duplicates remain possible.
    ScreenFactory factory_copy = fit.value();
    factories_[dup_id] = factory_copy;
    factories_[base] = factory_copy;

    // Title: "<Original Title> 2", "<Original Title> 3", ...
    const int dup_index = n - 1;
    // Populate title for the synthetic id so the tab label reads cleanly.
    // title_for_id() is static — we can't extend it at runtime, so we store
    // the custom title via the existing user-rename path.
    save_tab_title(dup_id, QString("%1 %2").arg(title_for_id(base)).arg(dup_index));

    // Create the dock widget. This path also wraps the widget in a
    // GroupBadge container if the source screen implements IGroupLinked,
    // materialises via the factory, and adds the dock widget to the manager.
    auto* dw = create_dock_widget(dup_id);
    if (!dw) {
        LOG_WARN("DockRouter", QString("duplicate_panel: create_dock_widget failed for %1").arg(dup_id));
        return nullptr;
    }
    materialize_screen(dup_id);

    // Copy IStatefulScreen state from source → duplicate so the user doesn't
    // re-type the ticker etc. Source is the inner screen widget, not the
    // group-badge container — screens_ always stores the inner widget.
    auto* src_screen = screens_.value(base, nullptr);
    auto* dup_screen = screens_.value(dup_id, nullptr);
    if (src_screen && dup_screen) {
        auto* src_stateful = dynamic_cast<screens::IStatefulScreen*>(src_screen);
        auto* dup_stateful = dynamic_cast<screens::IStatefulScreen*>(dup_screen);
        if (src_stateful && dup_stateful)
            dup_stateful->restore_state(src_stateful->save_state());
    }

    // Place the duplicate next to the original as a tabbed sibling so the
    // user sees it immediately without manually docking.
    auto* src_dw = dock_widgets_.value(base, nullptr);
    if (src_dw && src_dw->dockAreaWidget()) {
        manager_->addDockWidgetTabToArea(dw, src_dw->dockAreaWidget());
    } else {
        manager_->addDockWidget(ads::CenterDockWidgetArea, dw);
    }
    dw->setAsCurrentTab();
    dw->raise();

    LOG_INFO("DockRouter", QString("Duplicated '%1' → '%2'").arg(base, dup_id));
    return dw;
}

// ── Phase 12 — tab right-click context menu ──────────────────────────────────

void DockScreenRouter::show_tab_context_menu(const QString& id, const QPoint& global_pos) {
    auto* dw = dock_widgets_.value(id, nullptr);
    if (!dw)
        return;

    QMenu menu;
    menu.setStyleSheet("QMenu{background:#111827;color:#e5e7eb;border:1px solid #374151;padding:4px 0;}"
                       "QMenu::item{padding:5px 24px 5px 12px;}"
                       "QMenu::item:selected{background:#1f2937;color:#d97706;}"
                       "QMenu::item:disabled{color:#6b7280;}"
                       "QMenu::separator{background:#374151;height:1px;margin:4px 8px;}");

    // Rename — reuses the inline editor code path. Simpler to pop a modal
    // prompt here; the inline editor requires the label's geometry which is
    // awkward to invoke from a menu click.
    auto* act_rename = menu.addAction("Rename Tab…");
    connect(act_rename, &QAction::triggered, this, [this, id, dw]() {
        bool ok = false;
        const QString name =
            QInputDialog::getText(nullptr, "Rename Tab", "New name:", QLineEdit::Normal, dw->windowTitle(), &ok);
        if (ok && !name.trimmed().isEmpty()) {
            dw->setWindowTitle(name.trimmed());
            save_tab_title(id, name.trimmed());
        }
    });

    // Duplicate — only enabled for factory-registered screens. We can't tell
    // from here reliably without re-running the factory lookup, so we flag the
    // item as enabled and let duplicate_panel no-op with a log warning on
    // unsupported screens.
    auto* act_dup = menu.addAction("Duplicate Panel");
    connect(act_dup, &QAction::triggered, this, [this, id]() { duplicate_panel(id); });

    menu.addSeparator();

    // Float or re-dock. ADS's setFloating() has no matching setDocked(); to
    // re-dock a floating widget we close its view and re-open it, which
    // causes ADS to re-attach it to the main container.
    const bool floating = dw->isFloating();
    auto* act_float = menu.addAction(floating ? "Re-dock Panel" : "Float Panel");
    connect(act_float, &QAction::triggered, this, [this, dw, floating]() {
        if (floating) {
            dw->toggleView(false);
            dw->toggleView(true);
            if (manager_ && manager_->dockAreaCount() == 0)
                manager_->addDockWidget(ads::CenterDockWidgetArea, dw);
        } else {
            dw->setFloating();
        }
    });

    // Always-on-top — only meaningful when the dock is a top-level floating
    // window. For docked tabs, the flag lives on the WindowFrame; we don't
    // expose it here to avoid confusion.
    if (floating) {
        if (auto* fc = dw->floatingDockContainer()) {
            const bool on = fc->windowFlags().testFlag(Qt::WindowStaysOnTopHint);
            auto* act_top = menu.addAction("Always on Top");
            act_top->setCheckable(true);
            act_top->setChecked(on);
            connect(act_top, &QAction::triggered, this, [fc, on]() {
                Qt::WindowFlags f = fc->windowFlags();
                if (on)
                    f &= ~Qt::WindowStaysOnTopHint;
                else
                    f |= Qt::WindowStaysOnTopHint;
                fc->setWindowFlags(f);
                fc->show();
            });
        }
    }

    menu.addSeparator();

    // Link to Group (Phase 1) — only for screens implementing IGroupLinked.
    // The inner screen is in screens_[id]; the container wrapping happens at
    // dw->widget(), so cast from the stored screen to test the interface.
    if (auto* screen = screens_.value(id, nullptr)) {
        if (auto* linked = dynamic_cast<IGroupLinked*>(screen)) {
            auto* group_menu = menu.addMenu("Link to Group");
            group_menu->setStyleSheet(menu.styleSheet());
            const SymbolGroup current = linked->group();
            auto& registry = SymbolGroupRegistry::instance();

            auto* unlink = group_menu->addAction("(None)");
            unlink->setCheckable(true);
            unlink->setChecked(current == SymbolGroup::None);
            connect(unlink, &QAction::triggered, this, [this, id]() {
                if (auto* s = screens_.value(id, nullptr)) {
                    if (auto* l = dynamic_cast<IGroupLinked*>(s))
                        l->set_group(SymbolGroup::None);
                }
                QPointer<ui::GroupBadge> badge = group_badges_.value(id);
                if (badge)
                    badge->set_group_silent(SymbolGroup::None);
            });
            group_menu->addSeparator();

            // Only enabled slots are selectable; disabled ones are surfaced
            // as greyed-out entries so the full inventory stays visible.
            for (SymbolGroup g : all_symbol_groups()) {
                const bool en = registry.enabled(g);
                QString label = QStringLiteral("%1  (%2)").arg(registry.name(g)).arg(symbol_group_letter(g));
                if (!en)
                    label += QStringLiteral("  — disabled");
                auto* a = group_menu->addAction(label);
                a->setCheckable(true);
                a->setChecked(g == current);
                a->setEnabled(en);
                connect(a, &QAction::triggered, this, [this, id, g]() {
                    auto* s = screens_.value(id, nullptr);
                    auto* l = s ? dynamic_cast<IGroupLinked*>(s) : nullptr;
                    if (!l)
                        return;
                    l->set_group(g);
                    QPointer<ui::GroupBadge> badge = group_badges_.value(id);
                    if (badge)
                        badge->set_group_silent(g);
                    // Seed the newly-linked group with the screen's current
                    // symbol (if any) so the other group panels follow it.
                    const SymbolRef own = l->current_symbol();
                    if (own.is_valid())
                        SymbolContext::instance().set_group_symbol(g, own, dynamic_cast<QObject*>(l));
                });
            }
        }
    }

    menu.addSeparator();

    // Phase 5: tear off into a new WindowFrame. Always offered; the call
    // itself logs + no-ops if the panel can't be torn off (eager screen
    // without a factory).
    auto* act_tear_off = menu.addAction("Tear off into new window");
    connect(act_tear_off, &QAction::triggered, this, [this, id]() { tear_off_to_new_frame(id); });

    // Phase 5: move to another open WindowFrame. Submenu lists every
    // other frame by display number; current frame is excluded. Hidden
    // entirely if there's only one frame (nothing to move to).
    {
        const auto frames = WindowRegistry::instance().frames();
        WindowFrame* this_frame = qobject_cast<WindowFrame*>(parent());
        // Count peers other than `this` to decide if the submenu is worth showing.
        int peers = 0;
        for (auto* f : frames)
            if (f && f != this_frame)
                ++peers;
        if (peers > 0) {
            auto* move_menu = menu.addMenu("Move to window");
            move_menu->setStyleSheet(menu.styleSheet());
            int display_num = 1;
            for (auto* f : frames) {
                if (!f)
                    continue;
                if (f == this_frame) {
                    ++display_num;
                    continue;
                }
                const QString label = QString("Window %1").arg(display_num);
                auto* a = move_menu->addAction(label);
                connect(a, &QAction::triggered, this, [this, id, f]() {
                    if (f && f->dock_router())
                        move_panel_to_frame(id, f->dock_router());
                });
                ++display_num;
            }
        }
    }

    menu.addSeparator();

    // Copy tab title to clipboard — small but handy for sharing a panel id
    // over chat / for scripting.
    auto* act_copy = menu.addAction("Copy Tab Title");
    connect(act_copy, &QAction::triggered, this, [dw]() { QApplication::clipboard()->setText(dw->windowTitle()); });

    // Close tab.
    auto* act_close = menu.addAction("Close Tab");
    connect(act_close, &QAction::triggered, this, [dw]() { dw->toggleView(false); });

    menu.exec(global_pos);
}

int DockScreenRouter::prune_hidden_panels() {
    if (!manager_)
        return 0;
    // dockWidgetsMap() returns a value copy, so removing while iterating the
    // snapshot is safe; collect first regardless to keep intent obvious.
    QList<ads::CDockWidget*> closed;
    for (auto* dw : manager_->dockWidgetsMap()) {
        if (dw && dw->isClosed())
            closed.append(dw);
    }
    for (auto* dw : closed) {
        // removeDockWidget() detaches the widget, drops it from the manager's
        // map and collapses its now-empty area. It does NOT delete the widget,
        // so the screen and its IStatefulScreen state stay alive in our
        // dock_widgets_ map for re-add on the next navigate().
        manager_->removeDockWidget(dw);
    }
    if (!closed.isEmpty())
        LOG_DEBUG("DockRouter", QString("prune_hidden_panels: dropped %1 closed area(s)").arg(closed.size()));
    return static_cast<int>(closed.size());
}

void DockScreenRouter::tile_2x2() {
    // Public `layout.tile_2x2` command: re-grid every open panel in its current
    // reading order. Delegates to the shared auto-grid implementation.
    retile_grid(nullptr);
}

void DockScreenRouter::retile_grid(ads::CDockWidget* newest) {
    // Detach every open dock widget then re-add with explicit ADS area hints to
    // force the product's auto-grid. Quadrant fill order:
    //   1 panel  → full
    //   2 panels → left | right
    //   3 panels → top row [left | right] + full-width bottom
    //   4 panels → 2x2 (3rd → bottom-left, 4th → bottom-right)
    //   5+       → tab into bottom-right
    if (!manager_)
        return;

    // Collect open panels, excluding `newest` (appended last so it lands in the
    // next/last slot). Existing panels are ordered by current on-screen position
    // (top→bottom, then left→right) so a re-tile preserves their reading order
    // instead of reshuffling them alphabetically by id.
    QList<ads::CDockWidget*> open;
    for (auto* dw : manager_->dockWidgetsMap()) {
        if (dw && !dw->isClosed() && dw != newest)
            open.append(dw);
    }
    std::sort(open.begin(), open.end(), [](ads::CDockWidget* a, ads::CDockWidget* b) {
        const QPoint pa = a->dockAreaWidget() ? a->dockAreaWidget()->mapToGlobal(QPoint(0, 0)) : QPoint();
        const QPoint pb = b->dockAreaWidget() ? b->dockAreaWidget()->mapToGlobal(QPoint(0, 0)) : QPoint();
        if (pa.y() != pb.y())
            return pa.y() < pb.y();
        return pa.x() < pb.x();
    });
    if (newest && !newest->isClosed())
        open.append(newest);

    if (open.isEmpty()) {
        LOG_DEBUG("DockRouter", "retile_grid: no open panels");
        return;
    }
    const int n = open.size();
    LOG_INFO("DockRouter", QString("retile_grid: arranging %1 panel(s)").arg(n));

    for (auto* dw : open)
        manager_->removeDockWidget(dw);

    ads::CDockAreaWidget* tl = nullptr;
    ads::CDockAreaWidget* tr = nullptr;
    ads::CDockAreaWidget* bl = nullptr;
    ads::CDockAreaWidget* br = nullptr;
    ads::CDockAreaWidget* bottom = nullptr;

    for (int i = 0; i < n; ++i) {
        ads::CDockWidget* dw = open.at(i);
        if (i == 0) {
            tl = manager_->addDockWidget(ads::CenterDockWidgetArea, dw);
        } else if (i == 1) {
            tr = manager_->addDockWidget(ads::RightDockWidgetArea, dw, tl);
        } else if (i == 2) {
            if (n == 3)
                bottom = manager_->addDockWidget(ads::BottomDockWidgetArea, dw); // full-width bottom
            else
                bl = manager_->addDockWidget(ads::BottomDockWidgetArea, dw, tl); // bottom-left
        } else if (i == 3) {
            br = manager_->addDockWidget(ads::RightDockWidgetArea, dw, bl); // bottom-right
        } else {
            ads::CDockAreaWidget* target = br ? br : (bottom ? bottom : (bl ? bl : (tr ? tr : tl)));
            if (target)
                manager_->addDockWidget(ads::CenterDockWidgetArea, dw, target);
            else
                manager_->addDockWidget(ads::CenterDockWidgetArea, dw);
        }
        dw->toggleView(true);
    }
    (void)tr;
    (void)bottom;
}

// ── Phase 5: tear-off + cross-frame move ────────────────────────────────────

void DockScreenRouter::adopt_panel_instance(const QString& id, PanelInstanceId instance_id) {
    if (id.isEmpty() || instance_id.is_null())
        return;

    instance_ids_.insert(id, instance_id);

    // Update the existing PanelHandle so PanelRegistry::find_by_frame
    // returns the right results for this frame's owning WindowFrame.
    if (auto* handle = PanelRegistry::instance().find(instance_id)) {
        WindowId frame_id;
        if (auto* frame = qobject_cast<WindowFrame*>(parent()))
            frame_id = frame->frame_uuid();
        handle->set_frame_id(frame_id);
        // dock_widget pointer will be repointed when create_dock_widget
        // runs in this router and assigns the freshly-created widget.
        handle->set_dock_widget(nullptr);
        handle->set_state(PanelHandle::State::Spawning);
    }
}

bool DockScreenRouter::move_panel_to_frame(const QString& id, DockScreenRouter* target) {
    if (!target || target == this) {
        LOG_DEBUG("DockRouter", QString("move_panel_to_frame('%1'): target invalid (null=%2 same=%3)")
                                    .arg(id)
                                    .arg(target == nullptr)
                                    .arg(target == this));
        return false;
    }
    auto* dw = dock_widgets_.value(id, nullptr);
    if (!dw) {
        LOG_WARN("DockRouter", QString("move_panel_to_frame('%1'): no dock widget").arg(id));
        return false;
    }
    if (target->dock_widgets_.contains(id)) {
        // Collision: the target already has a panel under this string id.
        // We could synthesise <id>#movedN but for v1 we refuse so the user
        // gets clear feedback. Caller (context menu / action) should
        // surface a toast / status message.
        LOG_WARN("DockRouter",
                 QString("move_panel_to_frame('%1'): target already has this panel id — refusing").arg(id));
        return false;
    }
    if (!factories_.contains(id) && !screens_.contains(id)) {
        // No factory and no eager screen — can't recreate on the target.
        LOG_WARN("DockRouter",
                 QString("move_panel_to_frame('%1'): no factory available, can't recreate on target").arg(id));
        return false;
    }

    LOG_INFO("DockRouter", QString("Moving panel '%1' to another frame").arg(id));

    // 1. Flush + sync-save state under the panel's UUID. The save itself
    //    is async (QtConcurrent), so there's a tight race where the target's
    //    restore could read a slightly stale row. SQLite WAL writes are
    //    sub-ms and widget materialise is 10-100ms, so practically the
    //    race almost never fires. Phase 8 formalises sync flush.
    auto* screen = screens_.value(id, nullptr);
    if (auto* stateful = dynamic_cast<screens::IStatefulScreen*>(screen)) {
        stateful->flush_pending_state();
        const PanelInstanceId uuid = instance_ids_.value(id);
        if (!uuid.is_null())
            ScreenStateManager::instance().save_now_by_uuid(stateful, uuid.to_string());
        else
            ScreenStateManager::instance().save_now(stateful);
    }

    // 2. Capture the UUID + factory so we can transfer them to the target.
    const PanelInstanceId panel_uuid = instance_ids_.value(id);
    ScreenFactory factory_copy;
    if (factories_.contains(id))
        factory_copy = factories_.value(id);

    // 3. Tear down the panel on this router. removeDockWidget unregisters
    //    from the manager; deleteLater destroys the widget. We purposely
    //    skip our own save_screen_state() because we already did the
    //    UUID-keyed save above — the visibility-changed handler would
    //    otherwise try a second save against a half-torn-down widget.
    manager_->removeDockWidget(dw);
    dw->deleteLater();
    dock_widgets_.remove(id);
    if (auto* sw = screens_.take(id))
        sw->deleteLater();
    factories_.remove(id);
    instance_ids_.remove(id);

    // 4. Set the target up. Transfer the factory + UUID before navigate()
    //    so target's create_dock_widget() reuses the existing UUID instead
    //    of minting a new one (and PanelRegistry's existing handle is
    //    re-used, frame_id repointed by adopt_panel_instance).
    if (factory_copy)
        target->factories_.insert(id, factory_copy);
    if (!panel_uuid.is_null())
        target->adopt_panel_instance(id, panel_uuid);

    // 5. Materialise on target. navigate() will create the dock widget,
    //    materialise the screen, and restore_screen_state will read the
    //    UUID-keyed row written in step 1.
    target->navigate(id);

    LOG_INFO("DockRouter", QString("Moved panel '%1' (uuid=%2) to target frame").arg(id, panel_uuid.to_string()));
    return true;
}

bool DockScreenRouter::tear_off_to_new_frame(const QString& id) {
    if (!dock_widgets_.contains(id)) {
        LOG_WARN("DockRouter", QString("tear_off_to_new_frame('%1'): no such panel").arg(id));
        return false;
    }
    if (!factories_.contains(id) && !screens_.contains(id)) {
        LOG_WARN("DockRouter", QString("tear_off_to_new_frame('%1'): no factory available, can't tear off").arg(id));
        return false;
    }

    // Spawn a new frame on the next monitor. Reuses WindowCycler's smart
    // placement so tear-offs land somewhere sensible (decision 5.6).
    WindowCycler::instance().new_window_on_next_monitor();

    // Find the freshly-created frame. WindowCycler::new_window_on_next_monitor
    // doesn't return a pointer, but the new frame is the most-recently-added
    // entry in WindowRegistry.
    auto frames = WindowRegistry::instance().frames();
    if (frames.isEmpty()) {
        LOG_ERROR("DockRouter", "tear_off_to_new_frame: spawned frame not found in registry");
        return false;
    }
    // The newly-created frame has the highest window_id, so it's last in
    // the (ascending) sorted list.
    WindowFrame* new_frame = frames.last();
    if (!new_frame || !new_frame->dock_router()) {
        LOG_ERROR("DockRouter", "tear_off_to_new_frame: spawned frame has no router");
        return false;
    }

    return move_panel_to_frame(id, new_frame->dock_router());
}
} // namespace fincept
