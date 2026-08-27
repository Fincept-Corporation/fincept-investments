// src/ui/navigation/CommandBar.cpp
//
// Core: command/asset-type registries, search/scoring, ctor + styling,
// draft persistence, key-action setup, event filter, dropdown placement,
// theme refresh. The slot dispatch and per-mode rendering live in:
//   - CommandBar_Input.cpp        — on_text_changed / on_return_pressed / etc.
//   - CommandBar_Suggestions.cpp  — slash / dock-verb completion lists + parsers
//   - CommandBar_Assets.cpp       — /stock-style asset search mode
// Shared constants (kMaxResults) live in CommandBar_internal.h.
#include "ui/navigation/CommandBar.h"

#include "core/events/EventBus.h"
#include "core/keys/KeyConfigManager.h"
#include "core/session/ScreenStateManager.h"
#include "network/http/HttpClient.h"
#include "ui/navigation/CommandBar_internal.h"
#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include <QApplication>
#include <QEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QListWidgetItem>
#include <QRegularExpression>
#include <QScreen>
#include <QVBoxLayout>

namespace fincept::ui {

using fincept::ui::commandbar_internal::kMaxResults;

// ── styling ─────────────────────────────────────────────────────────────────

static QString input_ss() {
    return QString("QLineEdit{"
                   "  background:%1;"
                   "  color:%2;"
                   "  border:1px solid %3;"
                   "  padding:2px 8px 2px 24px;"
                   "  font-size:12px;"
                   "  font-family:'Consolas',monospace;"
                   "}"
                   "QLineEdit:focus{"
                   "  border:1px solid %4;"
                   "}")
        .arg(colors::BG_RAISED.get())
        .arg(colors::TEXT_PRIMARY.get())
        .arg(colors::BORDER_MED.get())
        .arg(colors::AMBER.get());
}

static QString drop_ss() {
    return QString("QFrame{"
                   "  background:%1;"
                   "  border:1px solid %2;"
                   "  border-top:none;"
                   "}")
        .arg(colors::BG_SURFACE.get())
        .arg(colors::BORDER_MED.get());
}

static QString list_ss() {
    return QString("QListWidget{"
                   "  background:transparent;"
                   "  border:none;"
                   "  outline:none;"
                   "}"
                   "QListWidget::item{"
                   "  padding:0px;"
                   "  border:none;"
                   "  background:transparent;"
                   "}"
                   "QListWidget::item:selected{"
                   "  background:%1;"
                   "  border-left:3px solid %2;"
                   "}")
        .arg(colors::BG_RAISED.get())
        .arg(colors::AMBER.get());
}

// ── yfinance symbol conversion ───────────────────────────────────────────────

/// Convert exchange + symbol to yfinance-compatible ticker.
/// The /market/search API returns clean symbols (e.g. "RELIANCE") with exchange
/// as a separate field. yfinance needs suffixed tickers for non-US exchanges.

void CommandBar::build_commands() {
    commands_ = {
        // Primary tabs
        {"dashboard",
         tr("Dashboard"),
         "Main overview screen",
         {"dash", "home", "overview", "main"},
         "F1",
         {"dashboard", "home", "summary"}},
        {"markets",
         tr("Markets"),
         "Live market data",
         {"mkts", "markets", "market", "live"},
         "F2",
         {"markets", "stocks", "quotes", "prices"}},
        {"news",
         tr("News"),
         "Financial news feed",
         {"news", "headlines", "feed"},
         "F3",
         {"news", "headlines", "articles"}},
        {"portfolio",
         tr("Portfolio"),
         "Portfolio management",
         {"port", "portfolio", "pf", "holdings"},
         "F4",
         {"portfolio", "holdings", "positions"}},
        {"backtesting",
         tr("Backtesting"),
         "Strategy backtesting",
         {"bktest", "backtest", "bt"},
         "F5",
         {"backtest", "strategy", "historical"}},
        {"watchlist",
         tr("Watchlist"),
         "Manage watchlists",
         {"watch", "watchlist", "wl"},
         "F6",
         {"watchlist", "favorites", "track"}},
        {"crypto_trading",
         tr("Crypto Trading"),
         "Cryptocurrency trading",
         {"trade", "trading", "crypto", "kraken"},
         "F9",
         {"trading", "crypto", "exchange"}},
        {"ai_chat",
         tr("AI Chat"),
         "AI assistant",
         {"ai", "chat", "assistant", "bot"},
         "F10",
         {"ai", "chat", "assistant"}},
        {"notes", tr("Notes"), "Notes and reports", {"notes", "note"}, "F11", {"notes", "reports", "documents"}},
        {"settings",
         tr("Settings"),
         "Application settings",
         {"settings", "prefs", "config"},
         "",
         {"settings", "preferences"}},
        // Trading & Portfolio
        {"equity_trading",
         tr("Equity Trading"),
         "Stock trading interface",
         {"eqtrade", "stocks", "equities"},
         "",
         {"equity", "stocks", "trading"}},
        {"algo_trading",
         tr("Algo Trading"),
         "Algorithmic trading",
         {"algo", "algotrading"},
         "",
         {"algo", "algorithmic", "automated"}},
        {"alpha_arena",
         tr("Alpha Arena"),
         "Trading competition platform",
         {"alpha", "arena", "alphaarena"},
         "",
         {"alpha", "competition", "leaderboard"}},
        {"polymarket",
         tr("Prediction Markets"),
         "Polymarket + Kalshi prediction markets",
         {"poly", "polymarket", "kalshi", "prediction"},
         "",
         {"polymarket", "kalshi", "prediction", "markets"}},
        {"derivatives",
         tr("Derivatives"),
         "Single-contract pricing calculator (BSM, swaps, CDS)",
         {"deriv", "derivatives", "calc", "calculator"},
         "",
         {"derivatives", "pricing", "calculator", "bsm"}},
        {"fno",
         tr("F&O"),
         "Sensibull-style option chain, strategy builder, OI analytics",
         {"fno", "options", "chain", "strategy", "sensibull"},
         "",
         {"options", "chain", "strategy", "payoff", "oi", "fno", "futures"}},
        // Research
        {"equity_research",
         tr("Equity Research"),
         "Equity research tools",
         {"rsrch", "research", "equity"},
         "",
         {"research", "equity", "fundamental"}},
        {"screener",
         tr("Screener"),
         "Stock screener",
         {"scrn", "screener", "filter", "scan"},
         "",
         {"screener", "filter", "scan"}},
        {"ma_analytics",
         tr("M&A Analytics"),
         "Mergers and acquisitions",
         {"ma", "mna", "mergers"},
         "",
         {"ma", "mergers", "acquisitions"}},
        {"alt_investments",
         tr("Alt. Investments"),
         "Alternative investment analysis",
         {"altinv", "alt", "alternatives"},
         "",
         {"alternative", "investments", "hedge"}},
        {"surface_analytics",
         tr("Surface Analytics"),
         "Options vol surface & correlation",
         {"surface", "volsurface", "3dviz", "3d"},
         "",
         {"surface", "volatility", "correlation", "pca"}},
        // Economics & Data
        {"economics",
         tr("Economics"),
         "Economic indicators",
         {"econ", "economics", "indicators"},
         "",
         {"economics", "macro", "indicators"}},
        {"gov_data",
         tr("GOVT Data"),
         "Government securities & open data",
         {"govt", "gov", "government", "treasury"},
         "",
         {"government", "treasury", "bonds"}},
        {"dbnomics",
         tr("DBnomics"),
         "Economic database",
         {"dbn", "dbnomics", "database"},
         "",
         {"dbnomics", "database", "economic"}},
        {"akshare",
         tr("AKShare Data"),
         "Chinese financial data",
         {"aks", "akshare", "chinese"},
         "",
         {"akshare", "chinese", "china"}},
        {"asia_markets",
         tr("Asia Markets"),
         "Asian market data",
         {"asia", "apac", "asian"},
         "",
         {"asia", "asian", "apac"}},
        // Geopolitics
        {"geopolitics",
         tr("Geopolitics"),
         "Geopolitical analysis",
         {"geo", "geopolitics", "politics"},
         "",
         {"geopolitics", "politics", "global"}},
        {"maritime",
         tr("Maritime"),
         "Maritime intelligence",
         {"marine", "maritime", "shipping"},
         "",
         {"maritime", "shipping", "vessels"}},
        {"relationship_map",
         tr("Relationship Map"),
         "Entity relationship mapping",
         {"relmap", "relationships", "map"},
         "",
         {"relationship", "map", "network"}},
        // AI / Quant
        {"ai_quant_lab",
         tr("AI Quant Lab"),
         "Quantitative analysis lab",
         {"quantlab", "quant", "lab"},
         "",
         {"quant", "quantitative", "lab"}},
        {"agent_config",
         tr("Agent Config"),
         "Configure AI agents",
         {"agents", "agent", "config"},
         "",
         {"agents", "ai", "configuration"}},
        // Tools
        {"mcp_servers",
         tr("MCP Servers"),
         "Model Context Protocol servers",
         {"mcp", "servers"},
         "",
         {"mcp", "servers", "protocol"}},
        {"node_editor",
         tr("Node Editor"),
         "Visual workflow editor",
         {"nodes", "workflow", "node"},
         "",
         {"nodes", "workflow", "visual"}},
        {"code_editor",
         tr("Code Editor"),
         "Code development",
         {"code", "editor", "dev"},
         "",
         {"code", "editor", "programming"}},
        {"excel",
         tr("Excel"),
         "Excel workbook integration",
         {"excel", "spreadsheet", "xls"},
         "",
         {"excel", "spreadsheet", "workbook"}},
        {"report_builder",
         tr("Report Builder"),
         "Create reports",
         {"report", "reports", "builder"},
         "",
         {"report", "builder", "document"}},
        {"data_sources",
         tr("Data Sources"),
         "Manage data sources",
         {"datasrc", "datasources", "sources"},
         "",
         {"data", "sources", "connections"}},
        {"data_mapping",
         tr("Data Mapping"),
         "API integration & schema transform",
         {"datamap", "mapping", "schema"},
         "",
         {"data", "mapping", "schema", "api"}},
        {"trade_viz",
         tr("Trade Visualization"),
         "Trade flow visualization",
         {"tradeviz", "tradegraph"},
         "",
         {"trade", "visualization", "flow"}},
        // Community / Info
        {"about",
         tr("About"),
         "About Fincept Terminal",
         {"about", "info", "version"},
         "",
         {"about", "information", "version"}},
    };
}

// ── asset type registry ──────────────────────────────────────────────────────

void CommandBar::build_asset_types() {
    asset_types_ = {
        {"/stock", "stock", tr("Stock"), tr("Search stocks by symbol or company name")},
        {"/fund", "fund", tr("Fund"), tr("Search mutual funds and ETFs")},
        {"/dr", "dr", tr("DR"), tr("Search depositary receipts (ADR/GDR)")},
        {"/index", "index", tr("Index"), tr("Search market indices")},
        {"/forex", "forex", tr("Forex"), tr("Search forex currency pairs")},
        {"/crypto", "crypto", tr("Crypto"), tr("Search cryptocurrencies")},
        {"/futures", "futures", tr("Futures"), tr("Search futures contracts")},
        {"/bond", "bond", tr("Bond"), tr("Search bonds and fixed income")},
        {"/economic", "economic", tr("Economic"), tr("Search economic indicators")},
    };
}

// ── screen command search ────────────────────────────────────────────────────

QVector<ScreenCommand> CommandBar::search(const QString& query) const {
    if (query.trimmed().isEmpty())
        return {};

    const QString q = query.trimmed().toLower();

    struct Scored {
        int score;
        const ScreenCommand* cmd;
    };
    QVector<Scored> scored;

    for (const auto& cmd : commands_) {
        int score = 0;
        for (const auto& alias : cmd.aliases)
            if (alias.toLower() == q) {
                score = 100;
                break;
            }
        if (!score) {
            for (const auto& alias : cmd.aliases)
                if (alias.toLower().startsWith(q)) {
                    score = std::max(score, 80);
                }
            if (cmd.name.toLower().contains(q))
                score = std::max(score, 70);
            for (const auto& alias : cmd.aliases)
                if (alias.toLower().contains(q))
                    score = std::max(score, 60);
            if (cmd.description.toLower().contains(q))
                score = std::max(score, 40);
            for (const auto& kw : cmd.keywords)
                if (kw.toLower().contains(q))
                    score = std::max(score, 30);
        }
        if (score > 0)
            scored.append({score, &cmd});
    }

    std::sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) { return a.score > b.score; });

    QVector<ScreenCommand> result;
    for (int i = 0; i < std::min((int)scored.size(), kMaxResults); ++i)
        result.append(*scored[i].cmd);
    return result;
}

// ── constructor ──────────────────────────────────────────────────────────────

CommandBar::CommandBar(QWidget* parent) : QWidget(parent) {
    build_commands();
    build_asset_types();
    setFixedHeight(32);

    auto* hl = new QHBoxLayout(this);
    hl->setContentsMargins(0, 4, 0, 4);
    hl->setSpacing(0);

    input_ = new QLineEdit(this);
    input_->setFixedHeight(24);
    input_->setPlaceholderText(tr("> Enter Command or /type ..."));
    input_->setStyleSheet(input_ss());
    input_->installEventFilter(this);
    hl->addWidget(input_);

    // Dropdown — true top-level floating window so it appears above all widgets
    dropdown_ = new QFrame(nullptr);
    dropdown_->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    dropdown_->setAttribute(Qt::WA_ShowWithoutActivating);
    dropdown_->setStyleSheet(drop_ss());

    auto* vl = new QVBoxLayout(dropdown_);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->setSpacing(0);

    list_ = new QListWidget(dropdown_);
    list_->setStyleSheet(list_ss());
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    list_->setMouseTracking(true);
    list_->setFocusPolicy(Qt::NoFocus);
    vl->addWidget(list_);
    dropdown_->hide();

    // Debounce timer for API search
    search_debounce_ = new QTimer(this);
    search_debounce_->setSingleShot(true);
    connect(search_debounce_, &QTimer::timeout, this, [this]() {
        if (!pending_query_.isEmpty())
            fire_asset_search(pending_query_);
    });

    connect(input_, &QLineEdit::textChanged, this, &CommandBar::on_text_changed);
    connect(input_, &QLineEdit::textChanged, this, [this](const QString&) { schedule_draft_save(); });
    connect(input_, &QLineEdit::returnPressed, this, &CommandBar::on_return_pressed);
    connect(list_, &QListWidget::itemClicked, this, &CommandBar::on_item_clicked);
    connect(list_, &QListWidget::itemEntered, this, &CommandBar::on_item_entered);

    // Hide dropdown when app loses focus
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        if (!now || (now != input_ && !dropdown_->isAncestorOf(now) && now != list_))
            hide_dropdown();
    });

    connect(&ThemeManager::instance(), &ThemeManager::theme_changed, this,
            [this](const ThemeTokens&) { refresh_theme(); });

    retranslateUi();

    setup_key_actions();

    // ── Draft persistence ────────────────────────────────────────────────────
    // Debounce so every keystroke doesn't hit SQLite — 400 ms quiet period.
    draft_save_timer_ = new QTimer(this);
    draft_save_timer_->setSingleShot(true);
    draft_save_timer_->setInterval(400);
    connect(draft_save_timer_, &QTimer::timeout, this, &CommandBar::save_draft_now);

    // Restore last draft after the full UI tree is wired up so text-changed
    // logic (mode transitions, dropdown) re-establishes the right state.
    QTimer::singleShot(0, this, &CommandBar::restore_draft);
}

void CommandBar::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    QWidget::changeEvent(event);
}

void CommandBar::retranslateUi() {
    if (input_)
        input_->setPlaceholderText(tr("> Enter Command or /type ..."));
    // The command/asset-type registries cache their tr() display labels at
    // build time. Re-run the builders so a language switch refreshes the
    // names/descriptions; the dropdown rebuilds from these on the next keystroke.
    build_commands();
    build_asset_types();
}

// ── Draft persistence (cache.db screen_state[key="command_bar"]) ────────────

void CommandBar::restore_draft() {
    const QVariantMap s = ScreenStateManager::instance().load_direct("command_bar", kDraftStateVersion);
    if (s.isEmpty())
        return;

    // Restore parser bookkeeping BEFORE setting text — on_text_changed() will
    // see the right mode_ and dock_*/active_asset_type_ values.
    active_asset_type_ = s.value("active_asset_type").toString();
    dock_primary_id_ = s.value("dock_primary_id").toString();
    dock_verb_ = s.value("dock_verb").toString();
    mode_ = static_cast<Mode>(s.value("mode", static_cast<int>(Mode::Screen)).toInt());

    const QString text = s.value("text").toString();
    if (text.isEmpty())
        return;

    // QLineEdit::setText() fires textChanged() which re-runs the parser.
    // That will rebuild the correct dropdown for the restored mode.
    input_->setText(text);
    input_->setCursorPosition(text.length());
}

void CommandBar::save_draft_now() {
    if (!input_)
        return;
    const QVariantMap s{
        {"text", input_->text()},
        {"mode", static_cast<int>(mode_)},
        {"active_asset_type", active_asset_type_},
        {"dock_primary_id", dock_primary_id_},
        {"dock_verb", dock_verb_},
    };
    ScreenStateManager::instance().save_direct("command_bar", s, kDraftStateVersion);
}

void CommandBar::schedule_draft_save() {
    if (draft_save_timer_)
        draft_save_timer_->start();
}

void CommandBar::setup_key_actions() {
    auto& km = KeyConfigManager::instance();

    auto* act_down = km.action(KeyAction::NavNext);
    connect(act_down, &QAction::triggered, this, [this]() {
        const int count = list_->count();
        if (count > 0)
            list_->setCurrentRow((list_->currentRow() + 1) % count);
    });

    auto* act_up = km.action(KeyAction::NavPrev);
    connect(act_up, &QAction::triggered, this, [this]() {
        const int count = list_->count();
        if (count > 0)
            list_->setCurrentRow((list_->currentRow() - 1 + count) % count);
    });

    auto* act_tab = km.action(KeyAction::NavAccept);
    connect(act_tab, &QAction::triggered, this, [this]() {
        if (!list_->currentItem())
            return;
        const QString autocomplete = list_->currentItem()->data(Qt::UserRole + 1).toString();
        if (mode_ == Mode::SlashPicker) {
            input_->setText(autocomplete);
            for (const auto& at : asset_types_) {
                if (at.slash == autocomplete) {
                    activate_asset_mode(at.api_type);
                    input_->setText(autocomplete + " ");
                    input_->setCursorPosition(input_->text().length());
                    break;
                }
            }
        } else if (mode_ == Mode::DockSecondary && !dock_primary_id_.isEmpty() && !dock_verb_.isEmpty()) {
            const QString screen_id = list_->currentItem()->data(Qt::UserRole).toString();
            if (!screen_id.isEmpty()) {
                input_->setText(dock_primary_id_ + " " + dock_verb_ + " " + screen_id);
                input_->setCursorPosition(input_->text().length());
            }
        } else if (mode_ == Mode::DockCommand && !dock_primary_id_.isEmpty()) {
            const QString verb = list_->currentItem()->data(Qt::UserRole).toString();
            dock_verb_ = verb;
            if (verb == "remove") {
                input_->setText(dock_primary_id_ + " remove");
            } else {
                mode_ = Mode::DockSecondary;
                input_->setText(dock_primary_id_ + " " + verb + " ");
            }
            input_->setCursorPosition(input_->text().length());
        } else {
            input_->setText(autocomplete);
            hide_dropdown();
        }
    });

    auto* act_esc = km.action(KeyAction::NavEscape);
    connect(act_esc, &QAction::triggered, this, [this]() {
        input_->clear();
        mode_ = Mode::Screen;
        active_asset_type_.clear();
        dock_primary_id_.clear();
        dock_verb_.clear();
        search_debounce_->stop();
        hide_dropdown();
        input_->clearFocus();
    });
}

void CommandBar::refresh_theme() {
    if (input_)
        input_->setStyleSheet(input_ss());
    if (dropdown_)
        dropdown_->setStyleSheet(drop_ss());
    if (list_)
        list_->setStyleSheet(list_ss());
}

// ── event filter (keyboard nav in input) ─────────────────────────────────────

bool CommandBar::eventFilter(QObject* obj, QEvent* event) {
    // The Nav* QActions in KeyConfigManager (Up/Down/Tab/Esc) are created with
    // Qt::ApplicationShortcut context but never addAction()-ed to any widget
    // (WindowFrame's loop skips them because action_id_for(NavNext)=="").
    // So Qt's shortcut machinery never fires them. We intercept the keys
    // here instead, by raw key code — that's portable across Qt versions
    // / platforms and dodges the QKeySequence-equality quirks where the
    // bound sequence is constructed differently than `key|modifiers`.

    // QLineEdit also receives QEvent::ShortcutOverride for some keys before
    // KeyPress; accepting it ensures KeyPress is delivered to us instead of
    // being routed to the shortcut machinery. (Tab in particular hits
    // QWidget::focusNextPrevChild via QEvent::KeyPress; consuming it in
    // KeyPress is enough.)
    if (obj != input_)
        return QWidget::eventFilter(obj, event);

    if (event->type() != QEvent::KeyPress && event->type() != QEvent::ShortcutOverride)
        return QWidget::eventFilter(obj, event);

    auto* ke = static_cast<QKeyEvent*>(event);
    const int key = ke->key();
    const Qt::KeyboardModifiers mods = ke->modifiers() & ~Qt::KeypadModifier;

    // Map raw key to the corresponding nav handler. We only handle the
    // default bindings here; user rebindings via Settings → Keybindings
    // currently can't fire either (same orphan-QAction issue), so the
    // default bindings are the de-facto contract.
    const bool is_down = (key == Qt::Key_Down) && mods == Qt::NoModifier;
    const bool is_up = (key == Qt::Key_Up) && mods == Qt::NoModifier;
    const bool is_tab = (key == Qt::Key_Tab) && mods == Qt::NoModifier;
    const bool is_btab = (key == Qt::Key_Backtab); // Shift+Tab arrives as Backtab
    const bool is_esc = (key == Qt::Key_Escape) && mods == Qt::NoModifier;

    if (!is_down && !is_up && !is_tab && !is_btab && !is_esc)
        return QWidget::eventFilter(obj, event);

    // Escape works even when dropdown is hidden (it clears + drops focus).
    // The others only meaningfully act on a visible dropdown.
    const bool dropdown_visible = dropdown_ && dropdown_->isVisible();
    if (!dropdown_visible && !is_esc)
        return QWidget::eventFilter(obj, event);

    // For ShortcutOverride: accept it so the event is routed back to us as
    // KeyPress instead of being claimed by any global shortcut. Don't run
    // the actual handler here — that's KeyPress's job.
    if (event->type() == QEvent::ShortcutOverride) {
        event->accept();
        return true;
    }

    auto trigger = [](KeyAction a) {
        if (auto* act = KeyConfigManager::instance().action(a))
            act->trigger();
    };

    if (is_down)
        trigger(KeyAction::NavNext);
    else if (is_up)
        trigger(KeyAction::NavPrev);
    else if (is_btab)
        trigger(KeyAction::NavPrev);
    else if (is_tab)
        trigger(KeyAction::NavAccept);
    else if (is_esc)
        trigger(KeyAction::NavEscape);

    return true; // consume — keep Tab from walking focus, etc.
}

// ── slots ────────────────────────────────────────────────────────────────────

void CommandBar::execute_index(int index) {
    auto* item = list_->item(index);
    if (!item)
        return;
    emit navigate_to(item->data(Qt::UserRole).toString());
    input_->clear();
    mode_ = Mode::Screen;
    active_asset_type_.clear();
    hide_dropdown();
    input_->clearFocus();
}

void CommandBar::show_dropdown() {
    update_position();
    dropdown_->show();
    dropdown_->raise();
}

void CommandBar::hide_dropdown() {
    dropdown_->hide();
}

void CommandBar::update_position() {
    const QPoint global_pos = input_->mapToGlobal(QPoint(0, input_->height()));
    const int w = std::max(input_->width(), 360);
    const int rows = list_->count();
    const int h = std::min(rows * 32, 320);

    // dropdown_ is a top-level window — move() takes screen-global coordinates
    dropdown_->move(global_pos);
    dropdown_->resize(w, h);
}

} // namespace fincept::ui
