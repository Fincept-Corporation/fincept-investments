#include "app/WindowFrame.h"

#include "app/DockScreenRouter.h"
#include "app/MonitorPickerDialog.h"
#include "app/TerminalShell.h"
#include "core/actions/ActionRegistry.h"
#include "core/actions/builtin_actions.h"
#include "core/config/ProfileManager.h"
#include "core/events/EventBus.h"
#include "core/keys/KeyConfigManager.h"
#include "core/keys/WindowCycler.h"
#include "core/layout/LayoutCatalog.h"
#include "core/layout/WorkspaceShell.h"
#include "core/logging/Logger.h"
#include "core/panel/PanelMaterialiser.h"
#include "core/session/SessionManager.h"
#include "core/symbol/IGroupLinked.h"
#include "core/symbol/SymbolGroup.h"
#include "core/window/WindowRegistry.h"
#include "screens/about/AboutScreen.h"
#include "screens/agent_config/AgentConfigScreen.h"
#include "screens/ai_chat/AiChatBubble.h"
#include "screens/ai_chat/AiChatScreen.h"
#include "screens/ai_quant_lab/AIQuantLabScreen.h"
#include "screens/akshare/AkShareScreen.h"
#include "screens/algo_trading/AlgoTradingScreen.h"
#include "screens/alpha_arena/AlphaArenaScreen.h"
#include "screens/alt_investments/AltInvestmentsScreen.h"
#include "screens/asia_markets/AsiaMarketsScreen.h"
#include "screens/backtesting/BacktestingScreen.h"
#include "screens/code_editor/CodeEditorScreen.h"
#include "screens/common/ComingSoonScreen.h"
#include "screens/common/IStatefulScreen.h"
#include "screens/crypto_center/CryptoCenterScreen.h"
#include "screens/crypto_trading/CryptoTradingScreen.h"
#include "screens/dashboard/DashboardScreen.h"
#include "screens/data_mapping/DataMappingScreen.h"
#include "screens/data_sources/DataSourcesScreen.h"
#include "screens/dbnomics/DBnomicsScreen.h"
#include "screens/derivatives/DerivativesScreen.h"
#include "screens/docs/DocsScreen.h"
#include "screens/economics/EconomicsScreen.h"
#include "screens/equity_research/EquityResearchScreen.h"
#include "screens/equity_trading/EquityTradingScreen.h"
#include "screens/excel/ExcelScreen.h"
#include "screens/file_manager/FileManagerScreen.h"
#include "screens/fno/FnoScreen.h"
#include "screens/geopolitics/GeopoliticsScreen.h"
#include "screens/gov_data/GovDataScreen.h"
#include "screens/info/ContactScreen.h"
#include "screens/info/HelpScreen.h"
#include "screens/info/PrivacyScreen.h"
#include "screens/info/TermsScreen.h"
#include "screens/info/TrademarksScreen.h"
#include "screens/ma_analytics/MAAnalyticsScreen.h"
#include "screens/maritime/MaritimeScreen.h"
#include "screens/markets/MarketsScreen.h"
#include "screens/mcp_servers/McpServersScreen.h"
#include "screens/news/NewsScreen.h"
#include "screens/node_editor/NodeEditorScreen.h"
#include "screens/notes/NotesScreen.h"
#include "screens/polymarket/PolymarketScreen.h"
#include "screens/portfolio/PortfolioScreen.h"
#include "screens/relationship_map/RelationshipMapScreen.h"
#include "screens/report_builder/ReportBuilderScreen.h"
#include "screens/settings/SettingsScreen.h"
#include "screens/surface_analytics/SurfaceAnalyticsScreen.h"
#include "screens/trade_viz/TradeVizScreen.h"
#include "screens/watchlist/WatchlistScreen.h"
#include "services/llm/LlmService.h"
#include "services/updater/UpdateService.h"
#include "storage/repositories/SettingsRepository.h"
#include "trading/instruments/InstrumentService.h"
#include "ui/command/CommandPalette.h"
#include "ui/command/QuickCommandBar.h"
#include "ui/components/ComponentBrowserDialog.h"
#include "ui/debug/DebugOverlay.h"
#include "ui/navigation/DockStatusBar.h"
#include "ui/navigation/DockToolBar.h"
#include "ui/navigation/FKeyBar.h"
#include "ui/navigation/StatusBar.h"
#include "ui/navigation/ToolBar.h"
#include "ui/pushpins/PushpinBar.h"
#include "ui/theme/Theme.h"
#include "ui/widgets/EnterprisePromo.h"
#include "ui/workspace/LayoutOpenDialog.h"
#include "ui/workspace/LayoutSaveAsDialog.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMessageBox>
#include <QPalette>
#include <QPointer>
#include <QScreen>
#include <QSet>
#include <QShortcut>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWindow>

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>
#include <FloatingDockContainer.h>
#include <algorithm>

namespace fincept {

namespace {

} // namespace

int WindowFrame::next_window_id() {
    // Seed from the max of (persisted window IDs, live window IDs) so a new
    // window never reuses an ID that already owns saved geometry/dock layout.
    // Live IDs matter too: during a single session we may have deleted a
    // secondary window without clearing its QSettings keys yet, and we still
    // must not collide with any WindowFrame currently on screen.
    int max_id = 0;
    const auto saved = SessionManager::instance().load_window_ids();
    for (int id : saved)
        max_id = std::max(max_id, id);
    // Live frames (Phase 1: WindowRegistry replaces the topLevelWidgets walk).
    for (int id : WindowRegistry::instance().frame_ids())
        max_id = std::max(max_id, id);
    return max_id + 1;
}

WindowFrame::WindowFrame(int window_id, QWidget* parent, const WindowId& adopted_uuid)
    : QMainWindow(parent), window_id_(window_id) {
    // Adopt the saved UUID before any panel is created — see header rationale.
    // Null adopted_uuid = mint a fresh one lazily on first frame_uuid() call.
    if (!adopted_uuid.is_null())
        frame_uuid_ = adopted_uuid;
    // Phase 8: seed the dynamic property DataHub uses to gate
    // pause_when_inactive topics. Initial state = active (we wouldn't be
    // constructing the frame if we didn't intend to show it). changeEvent
    // updates this on every minimise/restore.
    setProperty("fincept.active_for_work", true);

    // Phase 6 trim: connect QWindow::screenChanged so paint-cached widgets
    // can invalidate at the new DPR. The QWindow is created lazily on
    // first show — queue the connect via QMetaObject::invokeMethod with
    // QueuedConnection so it runs after the event loop has had a chance
    // to construct the window handle.
    QMetaObject::invokeMethod(
        this,
        [this]() {
            if (auto* wh = windowHandle()) {
                connect(wh, &QWindow::screenChanged, this, [this](QScreen* scr) { emit screen_changed(scr); });
            }
        },
        Qt::QueuedConnection);

    // Show active profile in title bar when using a non-default profile.
    // "Fincept Terminal" is the product brand and is intentionally not
    // translated — the bracketed profile name is the only variable part.
    const QString profile = ProfileManager::instance().active();
    setWindowTitle(profile == "default" ? QStringLiteral("Fincept Terminal")
                                        : QStringLiteral("Fincept Terminal [%1]").arg(profile));
    // Load icon from the embedded Windows resource (IDI_ICON1 in app.rc).
    // Falls back to the .ico beside the executable on other platforms.
    QIcon app_icon;
#if defined(Q_OS_WIN)
    app_icon = QApplication::windowIcon(); // set by app.rc at link time
    if (app_icon.isNull()) {
        // Fallback: look next to the executable (works after windeployqt)
        const QString ico = QCoreApplication::applicationDirPath() + "/fincept.ico";
        if (QFile::exists(ico))
            app_icon = QIcon(ico);
    }
#else
    {
        const QString ico = QCoreApplication::applicationDirPath() + "/fincept.ico";
        if (QFile::exists(ico))
            app_icon = QIcon(ico);
    }
#endif
    if (!app_icon.isNull())
        setWindowIcon(app_icon);
    // Minimum size must be small enough to allow OS window-snap (half/third screen).
    // On a 1920-wide display, left-snap = 960px. Use 800x500 as minimum so snapping
    // works on any 1080p+ monitor without the window bouncing back to centre.
    setMinimumSize(800, 500);

    // Primary windows (id 0) restore saved geometry from last session.
    // Secondary windows (id > 0) restore their own saved geometry if available,
    // otherwise use smart multi-monitor placement (handled by the caller after show()).
    const QByteArray saved_geom = SessionManager::instance().load_geometry(window_id_);
    bool geometry_applied = false;
    if (!saved_geom.isEmpty() && restoreGeometry(saved_geom)) {
        // Verify the restored frame actually intersects a currently-connected
        // screen — if the user unplugged a monitor between sessions, Qt will
        // happily hand back off-screen coordinates and the window becomes
        // invisible/unreachable. Guard against that by falling through to
        // smart placement if no intersection exists.
        const QRect frame = frameGeometry();
        bool on_some_screen = false;
        const auto screens = QGuiApplication::screens();
        for (QScreen* s : screens) {
            if (s->availableGeometry().intersects(frame)) {
                on_some_screen = true;
                break;
            }
        }
        geometry_applied = on_some_screen;
    }

    if (!geometry_applied) {
        // Prefer the screen the window lived on last session (by name); fall
        // back to primary if that screen is gone.
        QScreen* target = nullptr;
        const QString saved_screen = SessionManager::instance().load_screen_name(window_id_);
        if (!saved_screen.isEmpty()) {
            for (QScreen* s : QGuiApplication::screens()) {
                if (s->name() == saved_screen) {
                    target = s;
                    break;
                }
            }
        }
        if (!target)
            target = QApplication::primaryScreen();
        if (target) {
            QRect geom = target->availableGeometry();
            resize(geom.width() * 9 / 10, geom.height() * 9 / 10);
            QPoint center = geom.center() - rect().center();
            if (window_id_ == 0) {
                move(center);
            } else {
                // Cascade secondary windows so they don't stack exactly
                // on top of the primary when multiple windows fall back
                // to the same screen (e.g. external monitors disconnected).
                int offset = window_id_ * 30;
                QPoint pos = center + QPoint(offset, offset);
                pos.setX(std::max(pos.x(), geom.x()));
                pos.setY(std::max(pos.y(), geom.y()));
                pos.setX(std::min(pos.x(), geom.right() - width()));
                pos.setY(std::min(pos.y(), geom.bottom() - height()));
                move(pos);
            }
        }
    }

    // Restore always-on-top before the first show() so the OS hint is
    // applied cleanly. Cannot use set_always_on_top() here — it would call
    // show() which only works after the widget has a fully built-out layout.
    always_on_top_ = SessionManager::instance().load_window_flag(window_id_, "always_on_top", false);
    if (always_on_top_)
        setWindowFlags(windowFlags() | Qt::WindowStaysOnTopHint);

    // ── ADS Docking mode ─────────────────────────────────────────────────────
    // The dock wrapper is the one and only central widget. There is no auth /
    // chat-mode / lock overlay to switch between any more, so the frame skips
    // the QStackedWidget indirection entirely.
    setup_docking_mode();
    setCentralWidget(dock_manager_->parentWidget());

    dock_toolbar_ = new ui::DockToolBar(this);
    addToolBar(Qt::TopToolBarArea, dock_toolbar_);

    // Pushpin chips are embedded directly inside the main toolbar (see
    // ToolBar::ToolBar). Visibility/enabled state now follows the parent
    // toolbar — no explicit gating needed.
    pushpin_bar_ = dock_toolbar_->inner()->pushpin_bar();

    dock_status_bar_ = new ui::DockStatusBar(this);
    setStatusBar(dock_status_bar_);

    auto* toolbar = dock_toolbar_->inner();

    // ── Window registry wiring ──────────────────────────────────────────────
    // Layout/workspace persistence lives in core/layout (LayoutCatalog +
    // WorkspaceShell + WorkspaceSnapshotRing). Title bar reads the active
    // layout name from WorkspaceShell::current_name() inside
    // update_window_title(), so no signal wiring is needed here.
    WindowCycler::instance().register_window(this);
    connect(this, &QObject::destroyed, this, [this]() { WindowCycler::instance().unregister_window(this); });

    dock_router_ = new DockScreenRouter(dock_manager_, this);
    setup_dock_screens();

    // Tab bar navigation: open the selected screen exclusively — closes all other
    // panels and fills the full area. Each tab is a fresh independent screen,
    // not nested into whatever was previously open.
    connect(tab_bar_, &ui::TabBar::tab_changed, this,
            [this](const QString& id) { dock_router_->navigate(id, true); });
    connect(dock_router_, &DockScreenRouter::screen_changed, tab_bar_, &ui::TabBar::set_active);

    // Update the main window title bar to reflect the current screen name.
    connect(dock_router_, &DockScreenRouter::screen_changed, this, [this](const QString&) { update_window_title(); });

    // Per-screen tool filter wiring removed (Tool RAG / Tier-0 model).
    // The LLM now sees a 6-tool Tier-0 prefix on every turn and discovers
    // the rest via tool_list — making per-screen scoping unnecessary and
    // counterproductive (it would prevent the LLM from finding tools the
    // user might want regardless of which screen happens to be active).

    // Chat bubble — floats over dock_manager_ content area
    chat_bubble_ = new AiChatBubble(dock_manager_);
    {
        auto r = SettingsRepository::instance().get("appearance.show_chat_bubble");
        bool show = !r.is_ok() || r.value() != "false";
        chat_bubble_->setVisible(show);
        if (show)
            chat_bubble_->raise();
    }

    // Re-raise and reposition the bubble after each navigation — dock geometry
    // shifts when panels open/close, so the bubble needs to re-anchor itself.
    connect(dock_router_, &DockScreenRouter::screen_changed, this, [this](const QString&) {
        if (!chat_bubble_)
            return;
        auto r = SettingsRepository::instance().get("appearance.show_chat_bubble");
        bool show = !r.is_ok() || r.value() != "false";
        chat_bubble_->setVisible(show);
        if (show) {
            chat_bubble_->reposition();
            chat_bubble_->raise();
        }
    });

    connect(toolbar, &ui::ToolBar::navigate_to, this,
            [this](const QString& id) { dock_router_->navigate(id, true); });

    // Debounced dock-layout save: ADS emits a burst of signals during
    // add/replace/remove; coalesce into one write ~500 ms after the last change
    // so rapid edits don't hammer QSettings and so crashes preserve layout.
    dock_layout_save_timer_ = new QTimer(this);
    dock_layout_save_timer_->setSingleShot(true);
    dock_layout_save_timer_->setInterval(500);
    connect(dock_layout_save_timer_, &QTimer::timeout, this, [this]() {
        if (!dock_manager_)
            return;
        // Drop hidden/closed panels before serialising so the saved layout is
        // exactly the visible grid — otherwise closed areas accumulate into
        // dozens of phantom splits that corrupt the restored arrangement.
        // prune emits dockWidgetRemoved/viewToggled (→ schedule_dock_layout_save);
        // suppress that re-entrant restart while we save the freshly-pruned tree.
        suppress_layout_save_ = true;
        if (dock_router_)
            dock_router_->prune_hidden_panels();
        suppress_layout_save_ = false;
        // SessionManager.save_dock_layout fans out to WorkspaceSnapshotRing
        // (60s rate-limited auto snapshot). That covers what the legacy
        // 5-min WorkspaceManager autosave used to do, with better cadence.
        SessionManager::instance().save_dock_layout(window_id_, dock_manager_->saveState());
    });

    connect(toolbar, &ui::ToolBar::dock_command, this,
            [this](const QString& action, const QString& primary, const QString& secondary) {
                if (action == "add")
                    dock_router_->add_alongside(primary, secondary);
                else if (action == "remove")
                    dock_router_->remove_screen(primary);
                else if (action == "replace")
                    dock_router_->replace_screen(primary, secondary);
                schedule_dock_layout_save();
            });

    // MCP navigation tool → dock_router (cross-thread safe)
    EventBus::instance().subscribe(this, "nav.switch_screen", [this](const QVariantMap& nav_data) {
        QString screen_id = nav_data["screen_id"].toString();
        // Optional: callers that want a clean "take me there" switch (replace the
        // current view) rather than the default auto-tiled split set exclusive=true.
        const bool exclusive = nav_data.value("exclusive", false).toBool();
        if (!screen_id.isEmpty())
            QMetaObject::invokeMethod(
                dock_router_,
                [this, screen_id, exclusive]() { dock_router_->navigate(screen_id, exclusive); },
                Qt::QueuedConnection);
    });

    // Equity Research "BUY/SELL" → reuse the Equity Trading order ticket without
    // leaving Research. We materialise the trading screen (hidden — no tab switch)
    // if it doesn't exist yet, then open its app-modal ticket, which pops over the
    // current tab. The trading screen owns the form + the paper/live placement path.
    EventBus::instance().subscribe(this, "equity.open_order_ticket", [this](const QVariantMap& d) {
        const QString symbol = d.value("symbol").toString();
        if (symbol.isEmpty())
            return;
        const QString exchange = d.value("exchange").toString();
        const QStringList match_exchanges = d.value("match_exchanges").toStringList();
        const bool is_buy = d.value("is_buy").toBool();
        const double price = d.value("price").toDouble();
        QMetaObject::invokeMethod(
            this,
            [this, symbol, exchange, match_exchanges, is_buy, price]() {
                if (!dock_router_)
                    return;
                const QString id = QStringLiteral("equity_trading");
                if (!dock_router_->screen_widget(id))
                    dock_router_->materialize_now(id); // create hidden, don't raise
                if (auto* trading = qobject_cast<screens::EquityTradingScreen*>(dock_router_->screen_widget(id)))
                    trading->open_external_order_ticket(symbol, exchange, match_exchanges, is_buy, price);
            },
            Qt::QueuedConnection);
    });

    // ── Keyboard shortcuts via ActionRegistry (Phase 4) ───────────────────────
    //
    // Every global action is now defined once in core/actions/builtin_actions.cpp
    // and registered with ActionRegistry. KeyConfigManager still owns the
    // QAction-per-KeyAction (so live re-binding via Settings → Keybindings
    // continues to work), but each frame routes the QAction's `triggered`
    // signal through ActionRegistry::invoke(action_id, ctx) instead of the
    // pre-Phase-4 captured-`this` lambdas.
    //
    // The captured-`this` form had a latent bug in multi-window setups: an
    // action triggered from frame B would still execute against frame A's
    // state (focus_mode_, etc.) because `this` was fixed at bind
    // time. The registry resolves the focused frame live at invoke time.
    auto& km = KeyConfigManager::instance();
    auto& reg = ActionRegistry::instance();
    for (KeyAction a : km.all_actions()) {
        const QString action_id = actions::action_id_for(a);
        if (action_id.isEmpty())
            continue; // per-screen action; handled elsewhere
        if (!reg.contains(action_id)) {
            LOG_WARN("WindowFrame",
                     QString("KeyAction → action_id mapping references unregistered id: %1").arg(action_id));
            continue;
        }
        auto* act = km.action(a);
        if (!act)
            continue;
        // Window-scoped so only the focused frame fires.
        act->setShortcutContext(Qt::WindowShortcut);
        addAction(act);
        connect(act, &QAction::triggered, this, [action_id]() {
            // Build a fresh context every invocation so the focused frame
            // is resolved live rather than captured. This is the bug-fix
            // that motivated the refactor.
            CommandContext ctx;
            ctx.shell = &TerminalShell::instance();
            ctx.focused_frame = WindowCycler::instance().focused_frame();
            // ctx.focused_panel left null — Phase 7 wires PanelRegistry's
            // focused-panel lookup once panels are UUID-keyed.
            auto r = ActionRegistry::instance().invoke(action_id, ctx);
            if (r.is_err()) {
                LOG_DEBUG("WindowFrame",
                          QString("Action %1 returned error: %2").arg(action_id, QString::fromStdString(r.error())));
            }
        });
    }

    // Toolbar actions
    connect(toolbar, &ui::ToolBar::action_triggered, this, [this](const QString& action) {
        if (action == "browse_components") {
            // Same path as the keyboard shortcut so the behavior stays in one
            // place — the KeyConfigManager action will be triggered directly.
            if (auto* act = KeyConfigManager::instance().action(KeyAction::BrowseComponents))
                act->trigger();
            return;
        }
        if (action == "new_window") {
            // Ask the user which monitor when more than one is connected.
            // pick() returns the only screen if there's just one, and
            // nullptr if the user cancels the dialog — in which case no
            // window is created.
            QScreen* target = MonitorPickerDialog::pick(this, this->screen());
            if (!target)
                return;
            auto* w = new WindowFrame(WindowFrame::next_window_id());
            w->setAttribute(Qt::WA_DeleteOnClose);
            const QRect sg = target->availableGeometry();
            w->resize(sg.width() * 9 / 10, sg.height() * 9 / 10);
            w->move(sg.center() - w->rect().center());
            w->show();
            w->raise();
            w->activateWindow();
        } else if (action == "close_window") {
            // Same as the titlebar close button — WA_DeleteOnClose handles
            // teardown, closeEvent persists this window's layout. If this is
            // the last window, the app honours general.on_last_window_close
            // (quit or surface the Launchpad), exactly like a manual close.
            close();
        } else if (action == "close_all_windows") {
            // Snapshot the registry first: each close() schedules the frame's
            // destruction (WA_DeleteOnClose → deleteLater) which unregisters it,
            // so iterating the live registry would be unsafe. The copy is stable.
            const auto frames = WindowRegistry::instance().frames();
            for (WindowFrame* w : frames)
                if (w)
                    w->close();
        } else if (action.startsWith("move_to_monitor:")) {
            // Move this window to the named monitor. We resolve by QScreen::name
            // (not index) because index ordering flips on plug/unplug events.
            const QString target_name = action.mid(QStringLiteral("move_to_monitor:").size());
            QScreen* target = nullptr;
            for (QScreen* s : QGuiApplication::screens()) {
                if (s->name() == target_name) {
                    target = s;
                    break;
                }
            }
            if (!target) {
                LOG_WARN("WindowFrame", QString("Move to monitor: screen '%1' not found").arg(target_name));
                return;
            }
            move_to_screen(target);
        } else if (action.startsWith("panel_")) {
            // Float a screen — in ADS mode, navigate dock_router; in legacy, spawn window
            static const QMap<QString, QPair<QString, QString>> panel_map = {
                {"panel_dashboard", {"Dashboard", "dashboard"}},
                {"panel_watchlist", {"Watchlist", "watchlist"}},
                {"panel_news", {"News Feed", "news"}},
                {"panel_portfolio", {"Portfolio", "portfolio"}},
                {"panel_markets", {"Markets", "markets"}},
                {"panel_crypto", {"Crypto Trading", "crypto_trading"}},
                {"panel_equity", {"Equity Trading", "equity_trading"}},
                {"panel_algo", {"Algo Trading", "algo_trading"}},
                {"panel_research", {"Equity Research", "equity_research"}},
                {"panel_economics", {"Economics", "economics"}},
                {"panel_geopolitics", {"Geopolitics", "geopolitics"}},
                {"panel_ai_chat", {"AI Chat", "ai_chat"}},
            };
            if (panel_map.contains(action)) {
                const auto [title, route] = panel_map[action];
                if (dock_router_)
                    dock_router_->navigate(route);
            }
        } else if (action == "perspective_save") {
            if (dock_manager_) {
                bool ok = false;
                const QString name = QInputDialog::getText(this, tr("Save Layout"), tr("Layout name:"),
                                                           QLineEdit::Normal, QString(), &ok);
                if (ok && !name.trimmed().isEmpty()) {
                    dock_manager_->addPerspective(name.trimmed());
                    LOG_INFO("WindowFrame", QString("Saved perspective: %1").arg(name.trimmed()));
                }
            }
        } else if (action.startsWith("perspective_")) {
            // Quick Switch — navigate directly to a preset screen layout
            // 2 screens = 1:1 side-by-side, 4 screens = 2x2 grid
            static const QMap<QString, QStringList> view_screens = {
                // Trading
                {"perspective_trading", {"crypto_trading", "watchlist", "markets", "news"}},
                {"perspective_equity", {"equity_trading", "watchlist"}},
                {"perspective_algo", {"algo_trading", "backtesting"}},
                // Research
                {"perspective_research", {"equity_research", "markets", "screener", "news"}},
                {"perspective_derivatives", {"derivatives", "surface_analytics"}},
                {"perspective_fno", {"fno", "surface_analytics", "equity_trading"}},
                {"perspective_ma", {"ma_analytics", "news"}},
                // Portfolio
                {"perspective_portfolio", {"portfolio", "markets", "watchlist", "news"}},
                // News & Markets
                {"perspective_news", {"news", "markets"}},
                {"perspective_markets", {"markets", "watchlist"}},
                // Economics & Data
                {"perspective_economics", {"economics", "dbnomics", "gov_data", "asia_markets"}},
                {"perspective_data", {"data_sources", "data_mapping"}},
                // Geopolitics
                {"perspective_geopolitics", {"geopolitics", "maritime", "relationship_map", "news"}},
                // AI & Quant
                {"perspective_quant", {"ai_quant_lab", "backtesting", "markets"}},
                {"perspective_ai", {"ai_chat", "agent_config"}},
                // Tools
                {"perspective_tools", {"code_editor", "node_editor"}},
            };
            const auto it = view_screens.find(action);
            if (it != view_screens.end() && dock_router_) {
                const QStringList& screens = it.value();
                if (!screens.isEmpty()) {
                    dock_router_->navigate(screens[0], true); // first exclusive
                    for (int i = 1; i < screens.size(); ++i)
                        dock_router_->navigate(screens[i]); // fill grid
                }
                LOG_INFO("WindowFrame", QString("Quick Switch: %1 (%2 panels)").arg(action).arg(screens.size()));
            }
        } else if (action == "fullscreen") {
            if (isFullScreen())
                showNormal();
            else
                showFullScreen();
        } else if (action == "focus_mode") {
            focus_mode_ = !focus_mode_;
            if (dock_toolbar_)
                dock_toolbar_->setVisible(!focus_mode_);
            if (dock_status_bar_)
                dock_status_bar_->setVisible(!focus_mode_);
        } else if (action == "always_on_top") {
            set_always_on_top(!always_on_top_);
        } else if (action == "refresh") {
            if (dock_manager_) {
                auto* focused = dock_manager_->focusedDockWidget();
                if (focused && focused->widget())
                    QMetaObject::invokeMethod(focused->widget(), "refresh", Qt::QueuedConnection);
            }
        } else if (action == "layout_new" || action == "layout_save_as") {
            // Both routes prompt for a name then save. layout.new = blank
            // workspace under that name; layout.save_as = capture-current
            // under that name. Handlers in builtin_actions distinguish.
            ui::LayoutSaveAsDialog dlg(this);
            if (dlg.exec() == QDialog::Accepted) {
                CommandContext ctx;
                ctx.shell = &TerminalShell::instance();
                ctx.focused_frame = this;
                ctx.args.insert(QStringLiteral("name"), dlg.name());
                const QString action_id =
                    (action == "layout_new") ? QStringLiteral("layout.new") : QStringLiteral("layout.save_as");
                auto r = ActionRegistry::instance().invoke(action_id, ctx);
                if (r.is_err())
                    LOG_WARN("WindowFrame", QString("%1 failed: %2").arg(action_id, QString::fromStdString(r.error())));
            }
        } else if (action == "layout_open") {
            ui::LayoutOpenDialog dlg(this);
            if (dlg.exec() == QDialog::Accepted && !dlg.selected_id().is_null()) {
                CommandContext ctx;
                ctx.shell = &TerminalShell::instance();
                ctx.focused_frame = this;
                ctx.args.insert(QStringLiteral("name"), dlg.selected_id().to_string());
                auto r = ActionRegistry::instance().invoke(QStringLiteral("layout.switch"), ctx);
                if (r.is_err())
                    LOG_WARN("WindowFrame", QString("layout.switch failed: %1").arg(QString::fromStdString(r.error())));
            }
        } else if (action == "layout_save") {
            // If a layout is currently loaded, save under its name. Otherwise
            // prompt for one (treat as save-as with no initial name).
            const QString cur = layout::WorkspaceShell::current_name();
            QString target = cur;
            if (target.isEmpty()) {
                ui::LayoutSaveAsDialog dlg(this);
                if (dlg.exec() != QDialog::Accepted) {
                    return;
                }
                target = dlg.name();
            }
            CommandContext ctx;
            ctx.shell = &TerminalShell::instance();
            ctx.focused_frame = this;
            ctx.args.insert(QStringLiteral("name"), target);
            auto r = ActionRegistry::instance().invoke(QStringLiteral("layout.save"), ctx);
            if (r.is_err())
                LOG_WARN("WindowFrame", QString("layout.save failed: %1").arg(QString::fromStdString(r.error())));
        } else if (action == "import_data") {
            QString path = QFileDialog::getOpenFileName(this, tr("Import Layout"), QDir::homePath(),
                                                        tr("Fincept Layout (*.flayout *.fwsp);;All Files (*)"));
            if (!path.isEmpty()) {
                auto r = LayoutCatalog::instance().import_from(path);
                if (r.is_err())
                    QMessageBox::warning(this, tr("Import Failed"), QString::fromStdString(r.error()));
            }
        } else if (action == "export_data") {
            const LayoutId cur_id = layout::WorkspaceShell::current_id();
            if (cur_id.is_null()) {
                QMessageBox::information(this, tr("Export Layout"), tr("Open or save a layout first, then export it."));
            } else {
                QString path = QFileDialog::getSaveFileName(this, tr("Export Layout"), QDir::homePath(),
                                                            tr("Fincept Layout (*.flayout)"));
                if (!path.isEmpty()) {
                    auto r = LayoutCatalog::instance().export_to(cur_id, path);
                    if (r.is_err())
                        QMessageBox::warning(this, tr("Export Failed"), QString::fromStdString(r.error()));
                }
            }
        } else if (action == "check_updates") {
            // Help ▸ Check for Updates. silent=false so the user gets a result
            // either way — a menu-triggered check that says nothing reads as broken.
            services::UpdateService::instance().check_for_updates(/*silent=*/false);
        } else if (action == "screenshot") {
            QScreen* scr = this->screen();
            if (!scr)
                scr = QApplication::primaryScreen();
            QPixmap px = scr->grabWindow(winId());
            QString path = QFileDialog::getSaveFileName(
                this, tr("Save Screenshot"),
                QDir::homePath() + "/fincept_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".png",
                tr("PNG Images (*.png)"));
            if (!path.isEmpty()) {
                px.save(path, "PNG");
                LOG_INFO("WindowFrame", QString("Screenshot saved: %1").arg(path));
            }
        }
    });

    // Toolbar UPGRADE → Enterprise (the private edition) promo dialog.
    connect(toolbar, &ui::ToolBar::upgrade_clicked, this, [this]() { ui::UpgradeDialog::show_now(this); });

    // Restore window state (maximised, toolbar positions, etc.)
    // Note: restoreState() also restores QToolBar visibility, which can
    // hide the toolbar from stale/corrupt saved state. We force it visible
    // afterward unless the user is intentionally in focus mode.
    const QByteArray saved_state = SessionManager::instance().load_state(window_id_);
    if (!saved_state.isEmpty())
        restoreState(saved_state);

    // The shell is always visible now (no auth/lock overlay to hide it);
    // focus mode is the only thing that hides the chrome.
    if (dock_toolbar_)
        dock_toolbar_->setVisible(!focus_mode_);
    if (dock_status_bar_)
        dock_status_bar_->setVisible(!focus_mode_);

    // Restore ADS dock layout — must happen after all screens are registered
    // but BEFORE the initial navigate, so we don't create a widget that
    // conflicts with restoreState's layout.
    // Layout version: bump this whenever the dock layout format changes to
    // automatically discard stale/corrupt saved state from previous versions.
    // v5: closed panels are now pruned before save (prune_hidden_panels), so
    // pre-v5 blobs — bloated with dozens of phantom closed single-widget areas
    // that reshuffle the restored grid — are discarded rather than restored.
    static constexpr int kDockLayoutVersion = 5;
    bool dock_restored = false;

    if (dock_manager_) {
        QSettings persp_settings;
        SessionManager::instance().load_perspectives(persp_settings);
        dock_manager_->loadPerspectives(persp_settings);

        // Suppress the debounced dock-layout save during bulk registration
        // and restoreState. These operations fire dockWidgetAdded and
        // viewToggled dozens of times — each restarts the 500ms timer, and
        // the eventual save would just re-write the state we're loading.
        suppress_layout_save_ = true;

        const int saved_version = SessionManager::instance().dock_layout_version(window_id_);
        if (saved_version == kDockLayoutVersion) {
            const QByteArray saved_dock = SessionManager::instance().load_dock_layout(window_id_);
            if (!saved_dock.isEmpty()) {
                dock_router_->ensure_all_registered();
                // Also suppress screen construction during restoreState —
                // ADS toggles each restored widget visible, which fires
                // visibilityChanged→materialize_screen. Constructing all
                // visible screens inline blocks the first paint for seconds.
                // Screens are materialized progressively after show().
                dock_router_->set_suppress_materialize(true);
                dock_restored = dock_manager_->restoreState(saved_dock);
                dock_router_->set_suppress_materialize(false);

                // Sanity check: a genuinely corrupt blob restores into dozens of
                // areas (the pre-prune bug produced 28-33). A legit layout now
                // tops out around the 4-panel auto-grid plus a few floating /
                // torn-off panels, so 6 was too tight — it would nuke a valid
                // grid+float layout. prune_hidden_panels() keeps saved blobs at
                // visible-panel count, so this is only a backstop for true bloat.
                if (dock_restored && dock_manager_->openedDockAreas().size() > 16) {
                    LOG_WARN("WindowFrame", QString("Dock layout corrupt: %1 open areas — resetting")
                                                .arg(dock_manager_->openedDockAreas().size()));
                    dock_restored = false;
                }
            }
        } else if (saved_version != 0) {
            LOG_INFO("WindowFrame", QString("Dock layout version mismatch (saved %1, expected %2) — resetting")
                                        .arg(saved_version)
                                        .arg(kDockLayoutVersion));
        }

        if (!dock_restored) {
            // Close all dock widgets that may have been opened by a failed restore
            for (auto* dw : dock_manager_->dockWidgetsMap())
                dw->toggleView(false);
        }

        suppress_layout_save_ = false;

        // Save the current version so next startup knows the format.
        SessionManager::instance().set_dock_layout_version(window_id_, kDockLayoutVersion);
    }

    // Recovery / "Continue from last session" path: WorkspaceShell::apply
    // spawns this frame with a non-null adopted_uuid and will call
    // apply_layout(fl) immediately after construction returns. Skip the
    // default dashboard navigate so we don't (a) leave a stray dashboard
    // dock widget for ADS restoreState to flag-as-unassigned, and
    // (b) pay the cost of constructing DashboardScreen just to discard it.
    // If apply_layout subsequently fails, the caller is responsible for
    // falling back to a default screen.
    if (!dock_restored && adopted_uuid.is_null()) {
        // Defer navigation so the window can paint its chrome (toolbar,
        // tab bar, status bar) before the first screen factory runs.
        // DashboardScreen construction can take 200-500ms; without this
        // the user sees a blank frame for that entire duration.
        QTimer::singleShot(0, this, [this]() { dock_router_->navigate("dashboard"); });
        LOG_INFO("WindowFrame", "Deferred default dashboard navigate to after first paint");
    } else if (!dock_restored) {
        LOG_INFO("WindowFrame", QString("Deferring default navigate — frame spawned with adopted uuid %1 "
                                        "(apply_layout will populate)")
                                    .arg(adopted_uuid.to_string()));
    } else {
        // Dock layout restored — tabs are positioned but screens are
        // still placeholders (materialize was suppressed above). Defer
        // construction: materialize the active panel first so the user
        // sees content quickly, then fill in remaining visible panels.
        const QString last = SessionManager::instance().last_screen(window_id_);
        QTimer::singleShot(0, this, [this, last]() {
            if (!dock_router_ || !dock_manager_)
                return;
            // Active panel first — user sees populated content immediately
            if (!last.isEmpty()) {
                dock_router_->materialize_now(last);
                if (auto* dw = dock_router_->find_dock_widget(last)) {
                    if (!dw->isClosed()) {
                        dw->raise();
                        dw->setAsCurrentTab();
                    }
                }
                tab_bar_->set_active(last);
            }

            // Remaining visible panels — ONE PER EVENT-LOOP TURN. Doing the
            // whole loop in a single tick re-froze the window the instant
            // the first panel painted: a 4-panel layout constructed three
            // heavy screens back to back with no chance to repaint between
            // them. Snapshot the ids first so the walk is stable while the
            // dock map changes underneath it.
            QStringList pending;
            for (const auto& entry : dock_manager_->dockWidgetsMap().toStdMap()) {
                if (entry.first == last)
                    continue;
                if (entry.second && !entry.second->isClosed())
                    pending << entry.first;
            }
            if (pending.isEmpty())
                return;

            // Copyable, self-rescheduling functor: materialise the head,
            // then post itself again with the tail. QPointer + the `frame`
            // context object mean a closed window simply drops the rest.
            struct MaterialiseWalk {
                QPointer<WindowFrame> frame;
                QStringList todo;
                void operator()() const {
                    if (!frame || todo.isEmpty())
                        return;
                    auto* router = frame->dock_router();
                    if (!router)
                        return;
                    // The user may have closed this panel on an earlier tick.
                    if (auto* dw = router->find_dock_widget(todo.front()); dw && !dw->isClosed())
                        router->materialize_now(todo.front());
                    const QStringList rest = todo.mid(1);
                    if (!rest.isEmpty())
                        QTimer::singleShot(0, frame.data(), MaterialiseWalk{frame, rest});
                }
            };
            QTimer::singleShot(0, this, MaterialiseWalk{this, pending});
        });
    }

}

void WindowFrame::update_window_title() {
    // Brand stays in English; only the workspace/screen suffix (translated
    // via DockScreenRouter::title_for_id) responds to language changes.
    QString title = QStringLiteral("Fincept Terminal");

    const QString profile = ProfileManager::instance().active();
    if (profile != "default")
        title += QStringLiteral(" [%1]").arg(profile);

    // Layout name set by WorkspaceShell::apply on every successful
    // layout switch / cold-boot restore. Empty until first apply.
    const QString ws = layout::WorkspaceShell::current_name();
    if (!ws.isEmpty())
        title += QString(" — %1").arg(ws);

    if (dock_router_) {
        const QString id = dock_router_->current_screen_id();
        if (!id.isEmpty())
            title += QString(" — %1").arg(DockScreenRouter::title_for_id(id));
    }

    setWindowTitle(title);
}

void WindowFrame::set_always_on_top(bool on) {
    if (always_on_top_ == on)
        return;
    always_on_top_ = on;
    Qt::WindowFlags flags = windowFlags();
    if (on)
        flags |= Qt::WindowStaysOnTopHint;
    else
        flags &= ~Qt::WindowStaysOnTopHint;
    setWindowFlags(flags);
    // setWindowFlags hides the window on most platforms; re-show it so the
    // change is visible. Preserve maximised/fullscreen state across the toggle.
    if (isVisible() || true) // always re-show; isVisible is false after the flag change
        show();
    SessionManager::instance().save_window_flag(window_id_, "always_on_top", on);
    LOG_INFO("WindowFrame", QString("Window %1 always-on-top = %2").arg(window_id_).arg(on ? "on" : "off"));
}

void WindowFrame::move_to_screen(QScreen* target) {
    if (!target || screen() == target)
        return;

    // Preserve maximised/fullscreen state across the move: Qt can't directly
    // relocate a maximised window to a different screen, so we restore →
    // move → re-apply the state.
    const bool was_maximised = isMaximized();
    const bool was_fullscreen = isFullScreen();
    if (was_maximised || was_fullscreen)
        showNormal();

    const QRect sg = target->availableGeometry();
    QSize new_size = size();
    if (new_size.width() > sg.width() || new_size.height() > sg.height())
        new_size = QSize(sg.width() * 9 / 10, sg.height() * 9 / 10);
    resize(new_size);
    move(sg.center() - QPoint(new_size.width() / 2, new_size.height() / 2));

    if (was_fullscreen)
        showFullScreen();
    else if (was_maximised)
        showMaximized();

    LOG_INFO("WindowFrame", QString("Moved window %1 to monitor '%2'").arg(window_id_).arg(target->name()));
}

WindowId WindowFrame::frame_uuid() const {
    if (frame_uuid_.is_null())
        frame_uuid_ = WindowId::generate();
    return frame_uuid_;
}

void WindowFrame::adopt_frame_uuid(const WindowId& id) {
    if (id.is_null()) {
        LOG_WARN("WindowFrame", "adopt_frame_uuid: null id ignored");
        return;
    }
    if (!frame_uuid_.is_null() && frame_uuid_ != id) {
        // The frame already minted (or adopted) a different UUID — too late,
        // any panels created so far carry the old frame_id. Log and bail
        // rather than corrupting the PanelRegistry view.
        LOG_WARN("WindowFrame", QString("adopt_frame_uuid: refusing to overwrite existing %1 with %2")
                                    .arg(frame_uuid_.to_string(), id.to_string()));
        return;
    }
    frame_uuid_ = id;
}

// ── Phase 6 final: capture / apply layout ───────────────────────────────────

bool WindowFrame::is_active_for_work() const {
    // Only isMinimized() drives the active/inactive flag. The previous
    // isVisible() check fired false-positives during window construction:
    // Qt sends a WindowStateChange BEFORE show() returns, at which point
    // isVisible() is still false. changeEvent() then flips
    // fincept.active_for_work to false, and any DataHub topic with
    // pause_when_inactive=true (market:quote:*, market:sparkline:*)
    // silently drops slot fan-out for newly-subscribed dashboard widgets.
    // The watchdog then renders "No data yet" even though publishes are
    // arriving. Navigating away and back works around it because by then
    // the window is visible and the property is true again.
    //
    // Everything else (virtual-desktop occupancy, occlusion by other apps,
    // alt-tab status) is intentionally NOT detected — platform-specific and
    // unreliable.
    return !isMinimized();
}

namespace {
// Phase 8 / decision 9.2: DataHub reads this dynamic property via
// QObject::property() to decide whether to fan out pause_when_inactive
// topics. Using a dynamic property (rather than a typed accessor) keeps
// the datahub layer free of any "app/WindowFrame.h" include — DataHub
// only needs Qt and the property name.
constexpr const char* kActiveForWorkProp = "fincept.active_for_work";
} // namespace

void WindowFrame::closeEvent(QCloseEvent* event) {
    // Synchronously persist every open tab's UI state BEFORE anything else.
    // Panels visible at quit never get visibilityChanged(false), so their
    // per-screen save would otherwise never fire — and an async save here
    // would race process exit. This guarantees each tab reopens with the
    // exact config it had (FNO underlying/expiry/broker, equity symbol/
    // watchlist, active sub-tab, …).
    if (dock_router_)
        dock_router_->flush_all_screen_states();
    // Force an immediate snapshot bypassing the 60s rate limit so a real
    // shutdown always has fresh state to restore from. Per Phase 6 the
    // legacy WorkspaceManager autosave is gone — the snapshot ring is the
    // single persistence path.
    SessionManager::instance().flush_snapshot_now();
    SessionManager::instance().save_geometry(window_id_, saveGeometry(), saveState());
    // Record the monitor this window is currently on so startup can place
    // it back on the same screen even if saveGeometry's absolute coordinates
    // would otherwise land off-screen (e.g. monitor disconnected, DPI changed).
    if (QScreen* scr = screen())
        SessionManager::instance().save_screen_name(window_id_, scr->name());
    if (dock_manager_) {
        // Persist only the visible grid — drop closed panels so the saved
        // layout doesn't carry phantom areas that corrupt the restored
        // arrangement (see DockScreenRouter::prune_hidden_panels).
        suppress_layout_save_ = true;
        if (dock_router_)
            dock_router_->prune_hidden_panels();
        suppress_layout_save_ = false;
        SessionManager::instance().save_dock_layout(window_id_, dock_manager_->saveState());
        QSettings tmp;
        dock_manager_->savePerspectives(tmp);
        SessionManager::instance().save_perspectives(tmp);
    }

    // ── Persist the window set for next launch ─────────────────────────────
    //
    // During app quit, multiple closeEvents fire before any window is
    // destroyed (WA_DeleteOnClose uses deleteLater). If each saves "all
    // except self", the last writer wins with an arbitrary subset — e.g.
    // window 0 saves [1,2], then window 2 saves [0,1], and on restart
    // main.cpp creates window 0 (unconditional) + 0,1 from the list =
    // phantom window. Fix: accumulate closing window IDs in a process-
    // level batch; a deferred handler runs after all closeEvents in the
    // current event-loop iteration and saves the correct surviving set.
    //
    // Synchronous fallback: save all IDs (including self) immediately so
    // that if the deferred timer doesn't fire during shutdown, we still
    // have a complete session snapshot. The deferred handler overwrites
    // with the precise surviving set if it does fire.
    {
        static QSet<int> s_closing_batch;
        static bool s_drain_queued = false;
        s_closing_batch.insert(window_id_);

        QList<int> all_ids = WindowRegistry::instance().frame_ids();
        std::sort(all_ids.begin(), all_ids.end());
        SessionManager::instance().save_window_ids(all_ids);

        if (!s_drain_queued) {
            s_drain_queued = true;
            QTimer::singleShot(0, qApp, []() {
                s_drain_queued = false;
                QList<int> all = WindowRegistry::instance().frame_ids();
                QList<int> surviving;
                for (int id : all) {
                    if (!s_closing_batch.contains(id))
                        surviving.append(id);
                }
                // All windows closing (app quit) → save the full set so
                // next launch can restore the complete session.
                if (surviving.isEmpty())
                    surviving = all;
                std::sort(surviving.begin(), surviving.end());
                SessionManager::instance().save_window_ids(surviving);
                s_closing_batch.clear();
            });
        }
    }

    // Phase 2: force a workspace snapshot at close. The save_* calls above
    // each request a snapshot but the ring rate-limits to 60s — closing a
    // window after rapid changes could otherwise miss the latest state.
    // Forced flush bypasses the rate limit. Cheap (one INSERT).
    SessionManager::instance().flush_snapshot_now();

    event->accept();
}

void WindowFrame::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    if (chat_bubble_)
        chat_bubble_->reposition();
    if (debug_overlay_ && debug_overlay_->isVisible())
        debug_overlay_->reposition();
}

void WindowFrame::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    // Refresh the title bar when the active translator changes, so the
    // workspace/screen suffix picks up the new locale immediately.
    if (event->type() == QEvent::LanguageChange) {
        update_window_title();
        return;
    }
    if (event->type() != QEvent::WindowStateChange)
        return;

    // Phase 8 / decision 9.1: emit active_for_work_changed when the flag
    // actually flips. Fires on minimise (true → false) AND restore
    // (false → true), so DataHub (and any other subscriber) can flush
    // pending updates when the user brings the window forward. We compute
    // is_active_for_work() now — Qt has already updated windowState by
    // the time WindowStateChange fires.
    {
        const bool now_active = is_active_for_work();
        if (now_active != last_active_for_work_) {
            last_active_for_work_ = now_active;
            // Update the dynamic property DataHub reads. Setting it BEFORE
            // emitting the signal so any DataHub subscriber that reacts to
            // the signal sees the up-to-date property.
            setProperty(kActiveForWorkProp, now_active);
            emit active_for_work_changed(now_active);
        }
    }
}

} // namespace fincept
