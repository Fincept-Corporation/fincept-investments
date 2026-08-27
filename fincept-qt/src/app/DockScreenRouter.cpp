// src/app/DockScreenRouter.cpp
//
// Core: screen/factory registration, title_for_id, group badges,
// find_dock_widget, all_screen_ids, eventFilter, panel_uuid_for, screen-state
// save/restore, apply_ads_theme. Other concerns:
//   - DockScreenRouter_Navigation.cpp  — navigate / tab_into / add_alongside ...
//   - DockScreenRouter_PanelOps.cpp    — duplicate / tear-off / tile / move
//   - DockScreenRouter_Materialize.cpp — dock-widget construction + lazy materialise
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
#include <QCoreApplication>
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

QString DockScreenRouter::title_for_id(const QString& id) {
    // Map stays in English source so it remains the canonical lookup key.
    // The returned value is wrapped in tr() at call time so the live
    // QTranslator picks up the user's language without us having to
    // rebuild the static map on every language switch.
    static const QHash<QString, const char*> titles = {
        {"dashboard", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Dashboard")},
        {"markets", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Markets")},
        {"crypto_trading", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Crypto Trading")},
        {"equity_trading", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Equity Trading")},
        {"algo_trading", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Algo Trading")},
        {"backtesting", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Backtesting")},
        {"portfolio", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Portfolio")},
        {"watchlist", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Watchlist")},
        {"news", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "News")},
        {"ai_chat", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "AI Chat")},
        {"equity_research", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Equity Research")},
        {"economics", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Economics")},
        {"dbnomics", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "DBnomics")},
        {"akshare", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "AkShare Data")},
        {"asia_markets", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Asia Markets")},
        {"geopolitics", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Geopolitics")},
        {"gov_data", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Gov Data")},
        {"maritime", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Maritime")},
        {"polymarket", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Prediction Markets")},
        {"relationship_map", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Relationship Map")},
        {"derivatives", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Derivatives")},
        {"alt_investments", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Alt Investments")},
        {"ma_analytics", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "M&A Analytics")},
        {"surface_analytics", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Surface Analytics")},
        {"ai_quant_lab", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "AI Quant Lab")},
        {"alpha_arena", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Alpha Arena")},
        {"agent_config", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Agent Config")},
        {"mcp_servers", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "MCP Servers")},
        {"node_editor", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Node Editor")},
        {"code_editor", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Code Editor")},
        {"excel", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Excel")},
        {"report_builder", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Report Builder")},
        {"trade_viz", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Trade Viz")},
        {"data_sources", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Data Sources")},
        {"data_mapping", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Data Mapping")},
        {"file_manager", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "File Manager")},
        {"notes", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Notes")},
        {"docs", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Docs")},
        {"about", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "About")},
        {"settings", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Settings")},
        {"contact", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Contact")},
        {"terms", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Terms")},
        {"privacy", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Privacy")},
        {"trademarks", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Trademarks")},
        {"help", QT_TRANSLATE_NOOP("fincept::DockScreenRouter", "Help")},
    };
    auto it = titles.find(id);
    if (it == titles.end())
        return id;
    return QCoreApplication::translate("fincept::DockScreenRouter", it.value());
}

DockScreenRouter::DockScreenRouter(ads::CDockManager* manager, QObject* parent) : QObject(parent), manager_(manager) {
    // Route SymbolContext broadcasts to the matching IGroupLinked screens.
    // One central listener per router — cheaper and easier to reason about
    // than per-screen connections, and it gives us a single filter point
    // for source-suppression (skip re-delivery to the publisher).
    connect(&SymbolContext::instance(), &SymbolContext::group_symbol_changed, this,
            &DockScreenRouter::on_group_symbol_changed_external);
}

void DockScreenRouter::on_group_symbol_changed_external(SymbolGroup g, const SymbolRef& ref, QObject* source) {
    for (auto it = group_linked_.begin(); it != group_linked_.end(); ++it) {
        IGroupLinked* linked = it.value();
        if (!linked || linked->group() != g)
            continue;
        auto* as_obj = dynamic_cast<QObject*>(linked);
        if (as_obj && as_obj == source)
            continue; // skip feedback to the publisher
        linked->on_group_symbol_changed(ref);
    }
}

QWidget* DockScreenRouter::wrap_with_group_badge(const QString& id, QWidget* screen) {
    // The badge now lives inside the ADS tab title (next to close/float),
    // not inside the screen content area. Wrapping happens in
    // attach_group_badge_to_tab() after the CDockWidget is constructed —
    // this function is now a pass-through that just records the linked
    // pointer if the screen opts in. Existing call sites are unchanged.
    if (!screen)
        return screen;
    if (auto* linked = dynamic_cast<IGroupLinked*>(screen))
        group_linked_[id] = linked;
    attach_group_badge_to_tab(id, screen);
    return screen;
}

void DockScreenRouter::attach_group_badge_to_tab(const QString& id, QWidget* screen) {
    auto* dw = dock_widgets_.value(id, nullptr);
    if (!dw)
        return;
    auto* tab = dw->tabWidget();
    if (!tab)
        return;
    auto* layout = qobject_cast<QBoxLayout*>(tab->layout());
    if (!layout)
        return; // ADS layout not built yet — caller will retry on materialise

    // If a badge already exists (placeholder phase, or duplicate call), tear
    // it down so we can re-wire with the real screen's linked pointer. The
    // re-create is cheaper than juggling state in the existing widget.
    if (auto* existing = tab->findChild<ui::GroupBadge*>("dockTabBadge")) {
        layout->removeWidget(existing);
        existing->deleteLater();
    }

    auto* linked = dynamic_cast<IGroupLinked*>(screen);

    auto* badge = new ui::GroupBadge(tab);
    badge->setObjectName("dockTabBadge");
    badge->set_group_silent(linked ? linked->group() : SymbolGroup::None);
    if (!linked) {
        // Non-linked panel: still show the badge so the control is uniform,
        // but disable interaction — there is no IGroupLinked target.
        badge->setEnabled(false);
        badge->setToolTip(QStringLiteral("This panel doesn't support symbol groups"));
    }
    // Insert at index 0 — before the icon/title label.
    layout->insertWidget(0, badge);
    layout->insertSpacing(1, 6);

    if (!linked) {
        // No further wiring; the tab badge is decorative for this panel.
        group_badges_[id] = badge;
        return;
    }

    connect(badge, &ui::GroupBadge::group_change_requested, this, [linked, badge](SymbolGroup g) {
        linked->set_group(g);
        badge->set_group_silent(g);
        // If the new group already has an active symbol, push it
        // into the newly-linked screen immediately. Otherwise seed
        // the group from the screen's current symbol.
        const SymbolRef group_sym = SymbolContext::instance().group_symbol(g);
        if (group_sym.is_valid()) {
            linked->on_group_symbol_changed(group_sym);
        } else if (g != SymbolGroup::None) {
            const SymbolRef own = linked->current_symbol();
            if (own.is_valid())
                SymbolContext::instance().set_group_symbol(g, own, dynamic_cast<QObject*>(linked));
        }
    });

    group_badges_[id] = badge;
}

void DockScreenRouter::register_screen(const QString& id, QWidget* screen) {
    // Store the screen widget — the CDockWidget is only created on first navigate().
    // This prevents all eagerly registered screens from appearing at startup.
    screens_[id] = screen;
    factories_.remove(id);
}

void DockScreenRouter::register_factory(const QString& id, ScreenFactory factory) {
    factories_[id] = std::move(factory);
}

ads::CDockWidget* DockScreenRouter::find_dock_widget(const QString& id) const {
    return dock_widgets_.value(id, nullptr);
}

QStringList DockScreenRouter::all_screen_ids() const {
    QStringList ids;
    ids.reserve(dock_widgets_.size() + factories_.size());
    for (auto it = dock_widgets_.cbegin(); it != dock_widgets_.cend(); ++it)
        ids.append(it.key());
    for (auto it = factories_.cbegin(); it != factories_.cend(); ++it) {
        if (!ids.contains(it.key()))
            ids.append(it.key());
    }
    return ids;
}

bool DockScreenRouter::eventFilter(QObject* obj, QEvent* event) {
    // Phase 12 — right-click context menu on any installed tab/label. Handled
    // first so we don't gate on QLabel-only below (the tab widget itself is
    // also a watched object now).
    if (event->type() == QEvent::ContextMenu) {
        const QString id = obj->property("dock_id").toString();
        if (!id.isEmpty() && dock_widgets_.contains(id)) {
            auto* cm = static_cast<QContextMenuEvent*>(event);
            show_tab_context_menu(id, cm->globalPos());
            return true;
        }
    }

    if (event->type() != QEvent::MouseButtonDblClick)
        return QObject::eventFilter(obj, event);

    auto* lbl = qobject_cast<QLabel*>(obj);
    if (!lbl)
        return QObject::eventFilter(obj, event);

    const QString id = lbl->property("dock_id").toString();
    auto* dw = dock_widgets_.value(id, nullptr);
    if (!dw)
        return QObject::eventFilter(obj, event);

    // Show a QLineEdit overlaid on the label for inline rename.
    // Parent it to the label's parent (the tab frame) so geometry is correct.
    QWidget* tab_frame = lbl->parentWidget();
    auto* editor = new QLineEdit(tab_frame);
    editor->setText(dw->windowTitle());
    editor->selectAll();
    editor->setFrame(false);
    editor->setStyleSheet("QLineEdit{"
                          "  background:#1a1a1a;"
                          "  color:#ffffff;"
                          "  border:1px solid #d97706;"
                          "  padding:0 4px;"
                          "  font-size:11px;"
                          "  font-family:'Consolas',monospace;"
                          "}");
    // Map label rect into the tab frame's coordinate space
    editor->setGeometry(tab_frame->mapFromGlobal(lbl->mapToGlobal(QPoint(0, 0))).x(), 2, lbl->width(),
                        tab_frame->height() - 4);
    editor->show();
    editor->raise();
    editor->setFocus();

    auto commit = [dw, editor]() {
        const QString name = editor->text().trimmed();
        if (!name.isEmpty()) {
            // setWindowTitle on CDockWidget propagates to the tab label via
            // Qt's QEvent::WindowTitleChange handling inside CDockWidget.
            dw->setWindowTitle(name);
        }
        editor->deleteLater();
    };

    connect(editor, &QLineEdit::returnPressed, editor, commit);
    connect(editor, &QLineEdit::editingFinished, editor, commit);

    // Cancel on Escape via a local event filter
    struct EscFilter : QObject {
        QLineEdit* ed;
        explicit EscFilter(QLineEdit* e) : QObject(e), ed(e) {}
        bool eventFilter(QObject*, QEvent* e) override {
            if (e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape) {
                ed->deleteLater();
                return true;
            }
            return false;
        }
    };
    editor->installEventFilter(new EscFilter(editor));

    return true; // consume double-click so ADS doesn't float the widget
}

DockScreenRouter::~DockScreenRouter() {
    // Phase 4b cleanup: unregister every panel this router owns so the
    // PanelRegistry doesn't keep stale entries pointing at this frame
    // after WindowFrame destruction.
    for (auto it = instance_ids_.constBegin(); it != instance_ids_.constEnd(); ++it)
        PanelRegistry::instance().unregister_panel(it.value());
    instance_ids_.clear();
}

PanelInstanceId DockScreenRouter::panel_uuid_for(const QString& id) const {
    return instance_ids_.value(id);
}

void DockScreenRouter::save_tab_title(const QString& id, const QString& title) {
    SessionManager::instance().save_tab_state("title/" + id, {{"title", title}});
}

QString DockScreenRouter::load_tab_title(const QString& id) const {
    const auto state = SessionManager::instance().load_tab_state("title/" + id);
    return state.value("title").toString();
}

// ── Screen state persistence (IStatefulScreen opt-in) ────────────────────────

void DockScreenRouter::save_screen_state(const QString& id) {
    auto* screen = screens_.value(id, nullptr);
    if (!screen)
        return;
    auto* stateful = dynamic_cast<screens::IStatefulScreen*>(screen);
    if (!stateful)
        return;

    // Phase 4c: prefer the UUID-keyed save path so two open panels of the
    // same screen type (e.g. two watchlists) get distinct rows in
    // screen_state. Fall back to the legacy screen-key path only if no
    // PanelInstanceId has been minted for this id — that would indicate
    // a screen registered before create_dock_widget() ran (shouldn't
    // happen in practice; defensive).
    const PanelInstanceId uuid = panel_uuid_for(id);
    if (!uuid.is_null()) {
        ScreenStateManager::instance().save_now_by_uuid(stateful, uuid.to_string());
    } else {
        LOG_DEBUG("DockRouter",
                  QString("save_screen_state('%1'): no PanelInstanceId, falling back to screen-key path").arg(id));
        ScreenStateManager::instance().save_now(stateful);
    }
}

void DockScreenRouter::restore_screen_state(const QString& id) {
    auto* screen = screens_.value(id, nullptr);
    if (!screen)
        return;
    auto* stateful = dynamic_cast<screens::IStatefulScreen*>(screen);
    if (!stateful)
        return;

    // Phase 4c: load via the UUID-keyed path; restore_by_uuid falls back to
    // the legacy screen_key path internally on first run for users
    // upgrading, so no state is lost across the migration.
    const PanelInstanceId uuid = panel_uuid_for(id);
    if (!uuid.is_null()) {
        ScreenStateManager::instance().restore_by_uuid(stateful, uuid.to_string(), /*fallback_to_screen_key=*/true);
    } else {
        LOG_DEBUG("DockRouter",
                  QString("restore_screen_state('%1'): no PanelInstanceId, falling back to screen-key path").arg(id));
        ScreenStateManager::instance().restore(stateful);
    }
}

int DockScreenRouter::flush_all_screen_states() {
    int flushed = 0;
    // screens_ holds every MATERIALIZED screen widget (closed-but-not-yet-
    // dematerialised included). Synchronously persist each stateful one so a
    // tab that's still on screen at quit doesn't lose its config. Closed tabs
    // were already saved through the visibilityChanged(false) hide path.
    for (auto it = screens_.cbegin(); it != screens_.cend(); ++it) {
        auto* stateful = dynamic_cast<screens::IStatefulScreen*>(it.value());
        if (!stateful)
            continue;
        const PanelInstanceId uuid = panel_uuid_for(it.key());
        if (!uuid.is_null())
            ScreenStateManager::instance().save_now_by_uuid_sync(stateful, uuid.to_string());
        else
            ScreenStateManager::instance().save_now_sync(stateful);
        ++flushed;
    }
    LOG_INFO("DockRouter", QString("flush_all_screen_states: persisted %1 screen(s)").arg(flushed));
    return flushed;
}

void DockScreenRouter::apply_ads_theme() {
    // ADS's CSS (build_ads_qss on CDockManager) handles all ADS widget styling.
    // This function ensures tab widgets and labels don't have stale widget-level
    // QSS that could interfere with ADS's CSS cascade, and forces hidden tabs
    // visible (they can get stuck hidden after ensure_all_registered()).
    for (auto* dw : manager_->dockWidgetsMap()) {
        if (auto* tab = dw->tabWidget()) {
            if (!tab->styleSheet().isEmpty())
                tab->setStyleSheet(QString());
            if (tab->isHidden() && !dw->isClosed())
                tab->setVisible(true);
            for (auto* lbl : tab->findChildren<QLabel*>()) {
                if (!lbl->styleSheet().isEmpty())
                    lbl->setStyleSheet(QString());
            }
        }
    }
}

} // namespace fincept
