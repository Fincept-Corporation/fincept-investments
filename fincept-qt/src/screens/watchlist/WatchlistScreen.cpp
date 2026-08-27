#include "screens/watchlist/WatchlistScreen.h"

#include "core/events/EventBus.h"
#include "core/logging/Logger.h"
#include "core/session/ScreenStateManager.h"
#include "core/symbol/SymbolContext.h"
#include "core/symbol/SymbolDragSource.h"
#include "datahub/DataHub.h"
#include "datahub/DataHubMetaTypes.h"
#include "services/backtesting/BacktestingService.h"
#include "ui/formatting/NumberFormat.h"
#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeySequence>
#include <QMessageBox>
#include <QPointer>
#include <QSet>
#include <QShortcut>
#include <QShowEvent>
#include <QSplitter>
#include <QTextStream>
#include <QVBoxLayout>

namespace fincept::screens {

using namespace fincept::ui;

// ── Style builders (read live theme tokens) ─────────────────────────────────

static QString accent_btn_style() {
    QColor acc(colors::AMBER());
    auto rgb = QString("%1,%2,%3").arg(acc.red()).arg(acc.green()).arg(acc.blue());
    return QString("QPushButton { background:rgba(%1,0.1); color:%2; "
                   "border:1px solid %3; padding:0 12px; height:24px; "
                   "font-size:%4px; font-weight:700; font-family:%5; }"
                   "QPushButton:hover { background:%2; color:%6; }")
        .arg(rgb)
        .arg(colors::AMBER())
        .arg(colors::AMBER_DIM())
        .arg(fonts::TINY)
        .arg(fonts::DATA_FAMILY())
        .arg(colors::BG_BASE());
}

static QString std_btn_style() {
    return QString("QPushButton { background:%1; color:%2; border:1px solid %3; "
                   "padding:0 10px; height:24px; "
                   "font-size:%4px; font-weight:700; font-family:%5; }"
                   "QPushButton:hover { color:%6; background:%7; }")
        .arg(colors::BG_RAISED())
        .arg(colors::TEXT_SECONDARY())
        .arg(colors::BORDER_DIM())
        .arg(fonts::TINY)
        .arg(fonts::DATA_FAMILY())
        .arg(colors::TEXT_PRIMARY())
        .arg(colors::BG_HOVER());
}

static QString danger_btn_style() {
    QColor neg(colors::NEGATIVE());
    auto rgb = QString("%1,%2,%3").arg(neg.red()).arg(neg.green()).arg(neg.blue());
    return QString("QPushButton { background:rgba(%1,0.1); color:%2; "
                   "border:1px solid rgba(%1,0.3); padding:0 10px; height:24px; "
                   "font-size:%3px; font-weight:700; font-family:%4; }"
                   "QPushButton:hover { background:%2; color:%5; }")
        .arg(rgb)
        .arg(colors::NEGATIVE())
        .arg(fonts::TINY)
        .arg(fonts::DATA_FAMILY())
        .arg(colors::TEXT_PRIMARY());
}

static QString input_style() {
    QColor acc(colors::AMBER());
    auto acc_rgb = QString("%1,%2,%3").arg(acc.red()).arg(acc.green()).arg(acc.blue());
    return QString("QLineEdit { background:%1; color:%2; border:1px solid %3; "
                   "padding:3px 6px; font-size:%4px; font-family:%5; height:28px; }"
                   "QLineEdit:focus { border-color:%6; }"
                   "QLineEdit::selection { background:%7; color:%8; }")
        .arg(colors::BG_BASE())
        .arg(colors::TEXT_PRIMARY())
        .arg(colors::BORDER_DIM())
        .arg(fonts::SMALL)
        .arg(fonts::DATA_FAMILY())
        .arg(colors::BORDER_BRIGHT())
        .arg(colors::AMBER())
        .arg(colors::BG_BASE());
}

static QString list_style() {
    return QString("QListWidget { background:%1; border:none; font-family:%2; font-size:%3px; }"
                   "QListWidget::item { padding:6px 12px; color:%4; border-bottom:1px solid %5; height:26px; }"
                   "QListWidget::item:selected { background:%6; }"
                   "QListWidget::item:hover { background:%6; }")
        .arg(colors::BG_SURFACE())
        .arg(fonts::DATA_FAMILY())
        .arg(fonts::TINY)
        .arg(colors::TEXT_PRIMARY())
        .arg(colors::BORDER_DIM())
        .arg(colors::BG_HOVER());
}

// ── Constructor ──────────────────────────────────────────────────────────────

WatchlistScreen::WatchlistScreen(QWidget* parent) : QWidget(parent) {
    build_ui();
    load_watchlists();

    connect(&ThemeManager::instance(), &ThemeManager::theme_changed, this,
            [this](const ThemeTokens&) { refresh_theme(); });
    refresh_theme();
}

void WatchlistScreen::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!current_wl_id_.isEmpty() && !stocks_.isEmpty())
        hub_resubscribe_stocks();
    subscribe_mcp_events();
}

void WatchlistScreen::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    hub_unsubscribe_all();
    unsubscribe_mcp_events();
    if (rebuild_timer_)
        rebuild_timer_->stop(); // P3: no work while hidden
}

void WatchlistScreen::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    QWidget::changeEvent(event);
}

void WatchlistScreen::retranslateUi() {
    if (sidebar_title_)
        sidebar_title_->setText(tr("WATCHLISTS"));
    if (wl_count_)
        wl_count_->setText(tr("%1 lists").arg(watchlists_.size()));

    // Top bar
    if (panel_title_) {
        if (current_wl_id_.isEmpty()) {
            panel_title_->setText(tr("Select a watchlist"));
        } else {
            // Watchlist name itself is user data — only the empty state is translatable.
            for (const auto& wl : watchlists_) {
                if (wl.id == current_wl_id_) {
                    panel_title_->setText(wl.name.toUpper());
                    break;
                }
            }
        }
    }
    if (stock_count_ && !current_wl_id_.isEmpty())
        stock_count_->setText(tr("%1 symbols").arg(stocks_.size()));
    if (refresh_btn_)
        refresh_btn_->setText(tr("REFRESH"));
    if (del_wl_btn_)
        del_wl_btn_->setText(tr("DELETE LIST"));
    if (import_csv_btn_)
        import_csv_btn_->setText(tr("IMPORT CSV"));
    if (export_csv_btn_)
        export_csv_btn_->setText(tr("EXPORT CSV"));

    // Add bar
    if (add_label_)
        add_label_->setText(tr("ADD:"));
    if (add_input_)
        add_input_->setPlaceholderText(tr("AAPL, MSFT, TSLA..."));
    if (add_btn_)
        add_btn_->setText(tr("ADD"));
    if (remove_btn_)
        remove_btn_->setText(tr("REMOVE SELECTED"));

    // Table headers — reapply so the live header row reflects the new language.
    if (table_) {
        table_->set_headers(
            {tr("SYMBOL"), tr("NAME"), tr("PRICE"), tr("CHANGE"), tr("CHG %"), tr("HIGH"), tr("LOW"), tr("VOLUME")});
    }
}

// ── MCP-driven UI sync ──────────────────────────────────────────────────────
// MCP watchlist tools publish watchlist.created / watchlist.deleted /
// watchlist.updated when the LLM mutates watchlists via AI Chat or
// Finagent. We always reload from DB — the events carry only id/symbol,
// not enough to update the table incrementally.

void WatchlistScreen::subscribe_mcp_events() {
    if (!mcp_event_subs_.isEmpty())
        return; // idempotent

    QPointer<WatchlistScreen> self = this;
    auto on_watchlists_changed = [self](const QVariantMap&) {
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self]() {
                if (!self)
                    return;
                self->load_watchlists();
                // load_watchlists() preserves current_wl_id_ where possible;
                // load_stocks() reloads the right-pane table.
                self->load_stocks();
            },
            Qt::QueuedConnection);
    };

    auto& bus = EventBus::instance();
    mcp_event_subs_.append(bus.subscribe(this, "watchlist.created", on_watchlists_changed));
    mcp_event_subs_.append(bus.subscribe(this, "watchlist.deleted", on_watchlists_changed));
    mcp_event_subs_.append(bus.subscribe(this, "watchlist.updated", on_watchlists_changed));
}

void WatchlistScreen::unsubscribe_mcp_events() {
    auto& bus = EventBus::instance();
    for (auto id : mcp_event_subs_)
        bus.unsubscribe(id);
    mcp_event_subs_.clear();
}

void WatchlistScreen::build_ui() {
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    splitter_ = new QSplitter(Qt::Horizontal);

    splitter_->addWidget(build_sidebar());
    splitter_->addWidget(build_main_panel());
    splitter_->setStretchFactor(0, 0);
    splitter_->setStretchFactor(1, 1);

    root->addWidget(splitter_);
}

// ── Sidebar ──────────────────────────────────────────────────────────────────

QWidget* WatchlistScreen::build_sidebar() {
    sidebar_ = new QWidget(this);
    sidebar_->setMinimumWidth(180);
    sidebar_->setMaximumWidth(280);

    auto* lay = new QVBoxLayout(sidebar_);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    // Panel header
    sidebar_header_ = new QWidget(this);
    sidebar_header_->setFixedHeight(34);
    auto* hl = new QHBoxLayout(sidebar_header_);
    hl->setContentsMargins(12, 0, 8, 0);
    hl->setSpacing(6);

    sidebar_title_ = new QLabel(tr("WATCHLISTS"));
    hl->addWidget(sidebar_title_);
    hl->addStretch();

    add_wl_btn_ = new QPushButton("+");
    add_wl_btn_->setFixedSize(24, 24);
    connect(add_wl_btn_, &QPushButton::clicked, this, &WatchlistScreen::on_add_watchlist);
    hl->addWidget(add_wl_btn_);

    lay->addWidget(sidebar_header_);

    // Watchlist list
    wl_list_ = new QListWidget;
    connect(wl_list_, &QListWidget::currentRowChanged, this, &WatchlistScreen::on_watchlist_selected);
    lay->addWidget(wl_list_);

    // Footer count
    wl_count_ = new QLabel(tr("0 lists"));
    wl_count_->setFixedHeight(26);
    wl_count_->setAlignment(Qt::AlignCenter);
    lay->addWidget(wl_count_);

    return sidebar_;
}

// ── Main Panel ───────────────────────────────────────────────────────────────

QWidget* WatchlistScreen::build_main_panel() {
    main_panel_ = new QWidget(this);

    auto* lay = new QVBoxLayout(main_panel_);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    // Top bar: title + controls
    top_bar_ = new QWidget(this);
    top_bar_->setFixedHeight(34);
    auto* tl = new QHBoxLayout(top_bar_);
    tl->setContentsMargins(14, 0, 14, 0);
    tl->setSpacing(8);

    panel_title_ = new QLabel(tr("Select a watchlist"));
    tl->addWidget(panel_title_);

    tl->addStretch();

    stock_count_ = new QLabel;
    tl->addWidget(stock_count_);

    refresh_btn_ = new QPushButton(tr("REFRESH"));
    connect(refresh_btn_, &QPushButton::clicked, this, &WatchlistScreen::on_refresh);
    tl->addWidget(refresh_btn_);

    del_wl_btn_ = new QPushButton(tr("DELETE LIST"));
    connect(del_wl_btn_, &QPushButton::clicked, this, &WatchlistScreen::on_delete_watchlist);
    del_wl_btn_->setEnabled(false);
    tl->addWidget(del_wl_btn_);

    import_csv_btn_ = new QPushButton(tr("IMPORT CSV"));
    connect(import_csv_btn_, &QPushButton::clicked, this, &WatchlistScreen::on_import_csv);
    import_csv_btn_->setEnabled(false);
    tl->addWidget(import_csv_btn_);

    export_csv_btn_ = new QPushButton(tr("EXPORT CSV"));
    connect(export_csv_btn_, &QPushButton::clicked, this, &WatchlistScreen::on_export_csv);
    export_csv_btn_->setEnabled(false);
    tl->addWidget(export_csv_btn_);

    auto* backtest_btn = new QPushButton(tr("BACKTEST"));
    connect(backtest_btn, &QPushButton::clicked, this, [this]() {
        if (stocks_.isEmpty())
            return;
        QJsonArray symbols;
        for (const auto& s : stocks_)
            symbols.append(s.symbol);
        QJsonObject config;
        config["symbols"] = symbols;
        services::backtest::BacktestingService::instance().set_pending_portfolio_config(config);
        EventBus::instance().publish("nav.switch_screen", {{"screen_id", QString("backtesting")}});
    });
    tl->addWidget(backtest_btn);

    lay->addWidget(top_bar_);

    // Add stock bar
    add_bar_ = new QWidget(this);
    add_bar_->setFixedHeight(34);
    auto* al = new QHBoxLayout(add_bar_);
    al->setContentsMargins(14, 0, 14, 0);
    al->setSpacing(6);

    add_label_ = new QLabel(tr("ADD:"));
    al->addWidget(add_label_);

    add_input_ = new QLineEdit;
    add_input_->setPlaceholderText(tr("AAPL, MSFT, TSLA..."));
    add_input_->setFixedHeight(28);
    al->addWidget(add_input_, 1);

    add_btn_ = new QPushButton(tr("ADD"));
    connect(add_btn_, &QPushButton::clicked, this, &WatchlistScreen::on_add_stock);
    connect(add_input_, &QLineEdit::returnPressed, this, &WatchlistScreen::on_add_stock);
    al->addWidget(add_btn_);

    remove_btn_ = new QPushButton(tr("REMOVE SELECTED"));
    connect(remove_btn_, &QPushButton::clicked, this, &WatchlistScreen::on_remove_stock);
    al->addWidget(remove_btn_);

    lay->addWidget(add_bar_);

    // Table — the main data area
    table_ = new ui::DataTable;
    table_->set_headers(
        {tr("SYMBOL"), tr("NAME"), tr("PRICE"), tr("CHANGE"), tr("CHG %"), tr("HIGH"), tr("LOW"), tr("VOLUME")});
    table_->set_column_widths({100, 160, 100, 90, 80, 90, 90, 110});
    table_->setSortingEnabled(true); // opt-in: WatchlistScreen stamps numeric EditRole values
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);

    table_->setAccessibleName(tr("Watchlist quotes"));

    // When the user selects a row, publish its symbol into the linked group.
    // Use itemSelectionChanged rather than cellClicked so keyboard navigation
    // also propagates.
    connect(table_, &QTableWidget::itemSelectionChanged, this, &WatchlistScreen::publish_selection_to_group);

    // Delete on a selected row removes the symbol (with the same confirmation
    // as the REMOVE SELECTED button). Scoped to the table so it can't fire
    // while the user is typing in the ADD box.
    auto* del_sc = new QShortcut(QKeySequence::Delete, table_);
    del_sc->setContext(Qt::WidgetShortcut);
    connect(del_sc, &QShortcut::activated, this, &WatchlistScreen::on_remove_stock);

    // Drag-out: hold-and-drag a symbol row to broadcast the ticker to any
    // panel. The provider callback reads the current row at drag-start so
    // keyboard row-changes are reflected without reinstalling the filter.
    symbol_dnd::installDragSource(table_->viewport(), [this]() { return current_symbol(); }, link_group_);

    // Table + empty-state guidance share one slot; only one is visible.
    auto* table_stack = new QWidget(main_panel_);
    auto* ts_lay = new QVBoxLayout(table_stack);
    ts_lay->setContentsMargins(0, 0, 0, 0);
    ts_lay->setSpacing(0);
    ts_lay->addWidget(table_, 1);

    empty_label_ = new QLabel(table_stack);
    empty_label_->setAlignment(Qt::AlignCenter);
    empty_label_->setWordWrap(true);
    empty_label_->setVisible(false);
    ts_lay->addWidget(empty_label_, 1);

    lay->addWidget(table_stack, 1);

    // Drop: dropping a symbol onto the watchlist body adds it to the current
    // watchlist. Happens on the main_panel_ so the drop target is generous
    // (header, table, sidebar edge all count).
    symbol_dnd::installDropFilter(main_panel_, [this](const SymbolRef& ref, SymbolGroup) {
        if (current_wl_id_.isEmpty() || !ref.is_valid())
            return;
        fincept::WatchlistRepository::instance().add_stock(current_wl_id_, ref.symbol);
        load_stocks();
    });

    return main_panel_;
}

// ── Theme refresh ───────────────────────────────────────────────────────────

void WatchlistScreen::refresh_theme() {
    // Root
    setStyleSheet(QString("background:%1;").arg(colors::BG_BASE()));

    // Splitter
    if (splitter_)
        splitter_->setStyleSheet(QString("QSplitter::handle { background:%1; width:1px; }").arg(colors::BORDER_DIM()));

    // Sidebar
    if (sidebar_)
        sidebar_->setStyleSheet(
            QString("background:%1; border-right:1px solid %2;").arg(colors::BG_SURFACE(), colors::BORDER_DIM()));

    if (sidebar_header_)
        sidebar_header_->setStyleSheet(
            QString("background:%1; border-bottom:1px solid %2;").arg(colors::BG_RAISED(), colors::BORDER_DIM()));

    if (sidebar_title_)
        sidebar_title_->setStyleSheet(QString("color:%1; font-size:%2px; font-weight:700; letter-spacing:0.5px; "
                                              "font-family:%3; background:transparent;")
                                          .arg(colors::AMBER())
                                          .arg(fonts::TINY)
                                          .arg(fonts::DATA_FAMILY()));

    if (add_wl_btn_)
        add_wl_btn_->setStyleSheet(accent_btn_style());

    if (wl_list_)
        wl_list_->setStyleSheet(list_style());

    if (wl_count_)
        wl_count_->setStyleSheet(
            QString("background:%1; color:%2; font-size:%3px; border-top:1px solid %4; font-family:%5;")
                .arg(colors::BG_SURFACE())
                .arg(colors::TEXT_TERTIARY())
                .arg(fonts::TINY)
                .arg(colors::BORDER_DIM())
                .arg(fonts::DATA_FAMILY()));

    // Main panel
    if (main_panel_)
        main_panel_->setStyleSheet(QString("background:%1;").arg(colors::BG_BASE()));

    if (top_bar_)
        top_bar_->setStyleSheet(
            QString("background:%1; border-bottom:1px solid %2;").arg(colors::BG_RAISED(), colors::BORDER_DIM()));

    if (panel_title_)
        panel_title_->setStyleSheet(
            QString("color:%1; font-size:%2px; font-weight:700; font-family:%3; background:transparent;")
                .arg(colors::TEXT_PRIMARY())
                .arg(fonts::SMALL)
                .arg(fonts::DATA_FAMILY()));

    if (stock_count_)
        stock_count_->setStyleSheet(QString("color:%1; font-size:%2px; font-family:%3; background:transparent;")
                                        .arg(colors::TEXT_TERTIARY())
                                        .arg(fonts::TINY)
                                        .arg(fonts::DATA_FAMILY()));

    if (refresh_btn_)
        refresh_btn_->setStyleSheet(std_btn_style());

    if (del_wl_btn_)
        del_wl_btn_->setStyleSheet(danger_btn_style());

    if (import_csv_btn_)
        import_csv_btn_->setStyleSheet(std_btn_style());

    if (export_csv_btn_)
        export_csv_btn_->setStyleSheet(std_btn_style());

    // Add bar
    if (add_bar_)
        add_bar_->setStyleSheet(
            QString("background:%1; border-bottom:1px solid %2;").arg(colors::BG_SURFACE(), colors::BORDER_DIM()));

    if (add_label_)
        add_label_->setStyleSheet(QString("color:%1; font-size:%2px; font-weight:700; letter-spacing:0.5px; "
                                          "font-family:%3; background:transparent;")
                                      .arg(colors::TEXT_SECONDARY())
                                      .arg(fonts::TINY)
                                      .arg(fonts::DATA_FAMILY()));

    if (add_input_)
        add_input_->setStyleSheet(input_style());

    if (add_btn_)
        add_btn_->setStyleSheet(accent_btn_style());

    if (remove_btn_)
        remove_btn_->setStyleSheet(danger_btn_style());

    if (empty_label_)
        empty_label_->setStyleSheet(QString("color:%1; font-size:%2px; font-family:%3; background:transparent;")
                                        .arg(colors::TEXT_TERTIARY())
                                        .arg(fonts::SMALL)
                                        .arg(fonts::DATA_FAMILY()));
}

// ── Data Loading ─────────────────────────────────────────────────────────────

void WatchlistScreen::load_watchlists() {
    auto r = fincept::WatchlistRepository::instance().list_all();
    if (r.is_err()) {
        LOG_ERROR("Watchlist", "Failed to load watchlists");
        watchlists_.clear();
    } else {
        watchlists_ = r.value();
    }

    // Ensure at least one default watchlist exists. We deliberately leave it
    // empty rather than seeding US large-caps — those weren't user-chosen and
    // confused the AI Chat flow ("I added RITES" appeared to fail because the
    // user saw the stale starter set after restart).
    if (watchlists_.isEmpty()) {
        QColor acc(colors::AMBER());
        auto cr = fincept::WatchlistRepository::instance().create("Default", acc.name());
        if (cr.is_ok())
            watchlists_.append(cr.value());
    }

    wl_list_->blockSignals(true);
    wl_list_->clear();
    for (const auto& wl : watchlists_) {
        wl_list_->addItem(wl.name);
    }
    wl_list_->blockSignals(false);

    wl_count_->setText(tr("%1 lists").arg(watchlists_.size()));

    if (watchlists_.isEmpty())
        return;

    // Keep the user on the list they were viewing. This is reloaded from a
    // cloud pull and from every MCP watchlist.* event, and the old code always
    // jumped back to row 0 — so an LLM adding a symbol yanked the user out of
    // whichever list they had open.
    int row = 0;
    if (!current_wl_id_.isEmpty()) {
        for (int i = 0; i < watchlists_.size(); ++i) {
            if (watchlists_[i].id == current_wl_id_) {
                row = i;
                break;
            }
        }
    }
    if (wl_list_->currentRow() == row)
        on_watchlist_selected(row); // same row → currentRowChanged won't fire
    else
        wl_list_->setCurrentRow(row);
}

void WatchlistScreen::load_stocks() {
    if (current_wl_id_.isEmpty())
        return;

    auto r = fincept::WatchlistRepository::instance().get_stocks(current_wl_id_);
    if (r.is_err()) {
        stocks_.clear();
    } else {
        stocks_ = r.value();
    }

    stock_count_->setText(tr("%1 symbols").arg(stocks_.size()));
    fetch_quotes();
}

void WatchlistScreen::fetch_quotes() {
    if (stocks_.isEmpty()) {
        table_->clear_data();
        hub_unsubscribe_all();
        update_empty_state();
        return;
    }

    hub_resubscribe_stocks();
    // Render placeholder rows synchronously so a newly-added symbol appears
    // in the table immediately. Real prices fill in as the hub delivers
    // quotes via the subscription callbacks.
    rebuild_from_cache();
}

void WatchlistScreen::schedule_table_rebuild() {
    if (!rebuild_timer_) {
        rebuild_timer_ = new QTimer(this);
        rebuild_timer_->setSingleShot(true);
        rebuild_timer_->setInterval(60); // one frame-ish; absorbs a whole hub burst
        connect(rebuild_timer_, &QTimer::timeout, this, &WatchlistScreen::rebuild_from_cache);
    }
    if (!rebuild_timer_->isActive())
        rebuild_timer_->start();
}

void WatchlistScreen::rebuild_from_cache() {
    QVector<services::QuoteData> quotes;
    quotes.reserve(stocks_.size());
    for (const auto& s : stocks_) {
        if (row_cache_.contains(s.symbol))
            quotes.append(row_cache_.value(s.symbol));
    }
    if (quotes.isEmpty()) {
        // No data yet — show placeholder rows.
        table_->setSortingEnabled(false);
        table_->clear_data();
        for (const auto& s : stocks_) {
            table_->add_row({s.symbol, s.name, "--", "--", "--", "--", "--", "--"});
        }
        table_->setSortingEnabled(true);
        update_empty_state();
        return;
    }
    populate_table(quotes);
    update_empty_state();
}

void WatchlistScreen::update_empty_state() {
    if (!empty_label_ || !table_)
        return;
    // An empty watchlist used to render as a blank grid with no explanation.
    const bool show = current_wl_id_.isEmpty() || stocks_.isEmpty();
    empty_label_->setVisible(show);
    table_->setVisible(!show);
    if (!show)
        return;
    empty_label_->setText(current_wl_id_.isEmpty()
                              ? tr("Select a watchlist on the left, or press + to create one.")
                              : tr("This watchlist is empty.\n\nType one or more tickers in the ADD box above\n"
                                   "(comma-separated), or drag a symbol in from another panel."));
}

void WatchlistScreen::hub_resubscribe_stocks() {
    auto& hub = datahub::DataHub::instance();
    // Stocks set is dynamic (user selected another watchlist or added/removed
    // a stock). Drop every prior subscription owned by this screen.
    hub.unsubscribe(this);
    hub_active_ = false;
    row_cache_.clear();

    if (stocks_.isEmpty())
        return;

    QStringList topics;
    topics.reserve(stocks_.size());
    for (const auto& s : stocks_) {
        const QString sym = s.symbol;
        const QString topic = QStringLiteral("market:quote:") + sym;
        topics.append(topic);
        hub.subscribe(this, topic, [this, sym](const QVariant& v) {
            if (!v.canConvert<services::QuoteData>())
                return;
            row_cache_.insert(sym, v.value<services::QuoteData>());
            // Do NOT rebuild the whole table here. A hub delivery burst fans out
            // one callback per symbol, so a 50-symbol watchlist used to run 50
            // full clear_data()+50-row repopulations back-to-back (2500 row
            // constructions) and lose the user's sort/selection each time.
            // Coalesce into a single rebuild at the end of the burst.
            schedule_table_rebuild();
        });
    }
    LOG_DEBUG("Watchlist", QString("subscribed + requesting %1 quote topics").arg(topics.size()));
    // force=true: watchlist symbols change on user edit; bypass min_interval
    // so newly-added tickers resolve immediately instead of waiting for the
    // scheduler tick.
    hub.request(topics, /*force=*/true);
    hub_active_ = true;
}

void WatchlistScreen::hub_unsubscribe_all() {
    if (!hub_active_)
        return;
    datahub::DataHub::instance().unsubscribe(this);
    hub_active_ = false;
}

void WatchlistScreen::populate_table(const QVector<services::QuoteData>& quotes) {
    // Disable sorting during population to prevent per-row re-sorting
    // (avoids both visual flickering and O(n log n) overhead per insert).
    table_->setSortingEnabled(false);
    table_->clear_data();

    // Build a map for quick lookup
    QMap<QString, services::QuoteData> quote_map;
    for (const auto& q : quotes) {
        quote_map[q.symbol] = q;
    }

    for (const auto& s : stocks_) {
        auto it = quote_map.find(s.symbol);
        if (it != quote_map.end()) {
            const auto& q = it.value();
            table_->add_row({q.symbol, q.name.isEmpty() ? s.name : q.name, QString("$%1").arg(q.price, 0, 'f', 2),
                             QString("%1%2").arg(q.change >= 0 ? "+" : "").arg(q.change, 0, 'f', 2),
                             QString("%1%2%").arg(q.change_pct >= 0 ? "+" : "").arg(q.change_pct, 0, 'f', 2),
                             QString("$%1").arg(q.high, 0, 'f', 2), QString("$%1").arg(q.low, 0, 'f', 2),
                             fincept::ui::formatting::format_compact_volume(static_cast<qint64>(q.volume))});

            int row = table_->rowCount() - 1;

            // Stamp numeric EditRole values so Qt sorts by magnitude,
            // not by the display string ("$2.5M" vs "$999K" etc.).
            table_->set_cell_numeric(row, 2, q.price);      // PRICE
            table_->set_cell_numeric(row, 3, q.change);     // CHANGE
            table_->set_cell_numeric(row, 4, q.change_pct); // CHG %
            table_->set_cell_numeric(row, 5, q.high);       // HIGH
            table_->set_cell_numeric(row, 6, q.low);        // LOW
            table_->set_cell_numeric(row, 7, q.volume);     // VOLUME

            // Green = good, Red = bad
            QString chg_color = q.change_pct >= 0 ? colors::POSITIVE : colors::NEGATIVE;
            table_->set_cell_color(row, 3, chg_color);
            table_->set_cell_color(row, 4, chg_color);
        } else {
            table_->add_row({s.symbol, s.name, "--", "--", "--", "--", "--", "--"});
        }
    }

    // Re-enable sorting — Qt will apply the current sort column/order once.
    table_->setSortingEnabled(true);
}

// ── Slots ────────────────────────────────────────────────────────────────────

void WatchlistScreen::on_watchlist_selected(int row) {
    if (row < 0 || row >= watchlists_.size())
        return;
    current_wl_id_ = watchlists_[row].id;
    panel_title_->setText(watchlists_[row].name.toUpper());
    load_stocks();
    ScreenStateManager::instance().notify_changed(this);
    if (del_wl_btn_)
        del_wl_btn_->setEnabled(true);
    if (import_csv_btn_)
        import_csv_btn_->setEnabled(true);
    if (export_csv_btn_)
        export_csv_btn_->setEnabled(true);
}

void WatchlistScreen::on_add_watchlist() {
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("New Watchlist"), tr("Name:"), QLineEdit::Normal, "", &ok);
    if (!ok || name.trimmed().isEmpty())
        return;

    auto r = fincept::WatchlistRepository::instance().create(name.trimmed());
    if (r.is_ok()) {
        load_watchlists();
        // Select the new one (last in list)
        wl_list_->setCurrentRow(watchlists_.size() - 1);
    }
}

void WatchlistScreen::on_delete_watchlist() {
    if (current_wl_id_.isEmpty())
        return;

    auto reply = QMessageBox::question(this, tr("Delete Watchlist"),
                                       tr("Are you sure you want to delete this watchlist and all its stocks?"),
                                       QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (reply != QMessageBox::Yes)
        return;

    fincept::WatchlistRepository::instance().remove(current_wl_id_);
    current_wl_id_.clear();
    stocks_.clear();
    table_->clear_data();
    update_empty_state();
    panel_title_->setText(tr("Select a watchlist"));
    stock_count_->clear();
    if (del_wl_btn_)
        del_wl_btn_->setEnabled(false);
    if (import_csv_btn_)
        import_csv_btn_->setEnabled(false);
    if (export_csv_btn_)
        export_csv_btn_->setEnabled(false);
    load_watchlists();
}

void WatchlistScreen::on_add_stock() {
    if (current_wl_id_.isEmpty())
        return;

    QString text = add_input_->text().trimmed().toUpper();
    if (text.isEmpty())
        return;

    auto& repo = fincept::WatchlistRepository::instance();
    for (auto& s : text.split(",")) {
        QString symbol = s.trimmed();
        if (!symbol.isEmpty()) {
            repo.add_stock(current_wl_id_, symbol);
        }
    }

    add_input_->clear();
    load_stocks();
}

void WatchlistScreen::on_remove_stock() {
    if (current_wl_id_.isEmpty())
        return;

    // Read the symbol from the selected VISUAL row's first column, not
    // stocks_[currentRow()]: sorting is enabled, so the visual row index no
    // longer maps to the insertion-ordered stocks_ vector — indexing it would
    // remove the WRONG symbol from the watchlist.
    const int row = table_->currentRow();
    auto* sym_item = (row >= 0) ? table_->item(row, 0) : nullptr;
    if (!sym_item) {
        QMessageBox::information(this, tr("Remove symbol"), tr("Select a row in the table first."));
        return;
    }
    const QString symbol = sym_item->text();
    if (symbol.isEmpty())
        return;
    // Destructive and previously one misclick away with no confirmation.
    if (QMessageBox::question(this, tr("Remove symbol"), tr("Remove %1 from this watchlist?").arg(symbol),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    fincept::WatchlistRepository::instance().remove_stock(current_wl_id_, symbol);
    load_stocks();
}

void WatchlistScreen::on_refresh() {
    if (!current_wl_id_.isEmpty()) {
        fetch_quotes();
    }
}

void WatchlistScreen::on_export_csv() {
    if (current_wl_id_.isEmpty())
        return;

    QString wl_name;
    for (const auto& wl : watchlists_) {
        if (wl.id == current_wl_id_) {
            wl_name = wl.name;
            break;
        }
    }

    if (wl_name.isEmpty())
        return;

    const QString suggested = wl_name + QStringLiteral(".csv");

    const QString path =
        QFileDialog::getSaveFileName(this, tr("Export Watchlist to CSV"), suggested, tr("CSV Files (*.csv)"));
    if (path.isEmpty())
        return;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Export failed"), tr("Could not open file for writing:\n%1").arg(path));
        return;
    }

    QTextStream out(&f);
    out.setEncoding(QStringConverter::Utf8);

    out << "SYMBOL,NAME,PRICE,CHANGE,CHG %,HIGH,LOW,VOLUME\n";

    auto csv_escape = [](const QString& s) -> QString {
        if (s.contains(',') || s.contains('"') || s.contains('\n')) {
            QString e = s;
            e.replace('"', QStringLiteral("\"\""));
            return '"' + e + '"';
        }
        return s;
    };

    for (const auto& s : stocks_) {
        const auto it = row_cache_.find(s.symbol);
        if (it == row_cache_.end()) {
            out << csv_escape(s.symbol) << ',' << csv_escape(s.name) << ",,,,,,\n";
            continue;
        }
        const auto& q = it.value();
        out << csv_escape(q.symbol) << ',' << csv_escape(q.name.isEmpty() ? s.name : q.name) << ','
            << QString::number(q.price, 'f', 2) << ',' << QString::number(q.change, 'f', 2) << ','
            << QString::number(q.change_pct, 'f', 2) << ',' << QString::number(q.high, 'f', 2) << ','
            << QString::number(q.low, 'f', 2) << ','
            << fincept::ui::formatting::format_compact_volume(static_cast<qint64>(q.volume)) << '\n';
    }
}

void WatchlistScreen::on_import_csv() {
    if (current_wl_id_.isEmpty())
        return;

    const QString path = QFileDialog::getOpenFileName(this, tr("Import CSV"), "", tr("CSV Files (*.csv)"));
    if (path.isEmpty())
        return;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Import failed"), tr("Could not open file for reading:\n%1").arg(path));
        return;
    }

    QTextStream in(&f);
    in.setEncoding(QStringConverter::Utf8);

    QString header_line = in.readLine();
    if (header_line.isEmpty())
        return;

    auto parse_csv_line = [](const QString& line) -> QStringList {
        QStringList fields;
        QString current;
        bool in_quotes = false;
        for (int i = 0; i < line.length(); ++i) {
            QChar c = line[i];
            if (c == '"') {
                if (i + 1 < line.length() && line[i + 1] == '"') {
                    current += '"';
                    ++i;
                } else {
                    in_quotes = !in_quotes;
                }
            } else if (c == ',' && !in_quotes) {
                fields.append(current);
                current.clear();
            } else {
                current += c;
            }
        }
        fields.append(current);
        return fields;
    };

    QStringList headers = parse_csv_line(header_line);
    int sym_col = -1;
    int name_col = -1;
    for (int i = 0; i < headers.size(); ++i) {
        QString h = headers[i].trimmed().toUpper();
        if (h == "SYMBOL")
            sym_col = i;
        else if (h == "NAME")
            name_col = i;
    }

    if (sym_col == -1) {
        QMessageBox::warning(this, tr("Import failed"), tr("CSV missing SYMBOL column."));
        return;
    }

    auto& repo = fincept::WatchlistRepository::instance();

    QSet<QString> existing;
    auto stocks_res = repo.get_stocks(current_wl_id_);
    if (stocks_res.is_ok()) {
        for (const auto& s : stocks_res.value()) {
            existing.insert(s.symbol.trimmed().toUpper());
        }
    }

    int imported = 0;
    int skipped = 0;

    while (!in.atEnd()) {
        QString line = in.readLine();
        if (line.trimmed().isEmpty())
            continue;

        QStringList fields = parse_csv_line(line);
        if (fields.size() <= sym_col)
            continue;

        QString sym = fields[sym_col].trimmed();
        if (sym.isEmpty())
            continue;

        QString upper_sym = sym.toUpper();
        if (existing.contains(upper_sym)) {
            skipped++;
            continue;
        }

        QString name = "";
        if (name_col != -1 && fields.size() > name_col) {
            name = fields[name_col].trimmed();
        }

        repo.add_stock(current_wl_id_, sym, name);
        existing.insert(upper_sym);
        imported++;
    }

    if (wl_list_) {
        on_watchlist_selected(wl_list_->currentRow());
    }

    QMessageBox::information(this, tr("Import Complete"),
                             tr("Imported %1, skipped %2 duplicates.").arg(imported).arg(skipped));
}

// ── IStatefulScreen ───────────────────────────────────────────────────────────

QVariantMap WatchlistScreen::save_state() const {
    return {
        {"watchlist_id", current_wl_id_},
    };
}

void WatchlistScreen::restore_state(const QVariantMap& state) {
    const QString wl_id = state.value("watchlist_id").toString();
    if (wl_id.isEmpty())
        return;

    // Find and select the matching watchlist row
    for (int i = 0; i < watchlists_.size(); ++i) {
        if (watchlists_[i].id == wl_id) {
            wl_list_->setCurrentRow(i);
            on_watchlist_selected(i);
            return;
        }
    }
    // Watchlist not found (may have been deleted) — leave default selection
}

// ── IGroupLinked ─────────────────────────────────────────────────────────────

void WatchlistScreen::on_group_symbol_changed(const SymbolRef& ref) {
    if (!table_ || !ref.is_valid())
        return;
    // Match against the table's VISUAL rows (column 0 = symbol), not the
    // insertion-ordered stocks_ vector: with sorting enabled, selectRow(stocks_
    // index) would highlight the wrong row. No-op if the ticker isn't shown.
    for (int r = 0; r < table_->rowCount(); ++r) {
        auto* it = table_->item(r, 0);
        if (it && it->text().compare(ref.symbol, Qt::CaseInsensitive) == 0) {
            QSignalBlocker block(table_); // avoid re-emitting publish
            table_->selectRow(r);
            table_->scrollToItem(it, QAbstractItemView::PositionAtCenter);
            return;
        }
    }
}

SymbolRef WatchlistScreen::current_symbol() const {
    if (!table_)
        return {};
    // Symbol from the selected visual row's column 0 (sort-safe), not
    // stocks_[currentRow()] which is wrong once the user sorts a column.
    const int r = table_->currentRow();
    auto* sym_item = (r >= 0) ? table_->item(r, 0) : nullptr;
    if (!sym_item || sym_item->text().isEmpty())
        return {};
    return SymbolRef::equity(sym_item->text());
}

void WatchlistScreen::publish_selection_to_group() {
    if (link_group_ == SymbolGroup::None || !table_)
        return;
    const int r = table_->currentRow();
    auto* sym_item = (r >= 0) ? table_->item(r, 0) : nullptr;
    if (!sym_item || sym_item->text().isEmpty())
        return;
    const SymbolRef ref = SymbolRef::equity(sym_item->text());
    SymbolContext::instance().set_group_symbol(link_group_, ref, this);
}

} // namespace fincept::screens
