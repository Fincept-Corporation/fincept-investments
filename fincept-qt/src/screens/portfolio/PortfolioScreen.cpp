// src/screens/portfolio/PortfolioScreen.cpp
//
// Core lifecycle: ctor, show/hide/resize events, refresh_theme, find_holding,
// save_state/restore_state, current_symbol, on_group_symbol_changed.
// Other concerns:
//   - PortfolioScreen_Layout.cpp   — build_ui + build_* + update_main_view_data
//   - PortfolioScreen_Handlers.cpp — service-event slots + user-action slots
#include "screens/portfolio/PortfolioScreen.h"

#include "core/session/ScreenStateManager.h"
#include "core/symbol/SymbolContext.h"
#include "core/symbol/SymbolRef.h"
#include "screens/portfolio/PortfolioBlotter.h"
#include "screens/portfolio/PortfolioCommandBar.h"
#include "screens/portfolio/PortfolioDetailWrapper.h"
#include "screens/portfolio/PortfolioDialogs.h"
#include "screens/portfolio/PortfolioFFNView.h"
#include "screens/portfolio/PortfolioHeatmap.h"
#include "screens/portfolio/PortfolioInsightsPanel.h"
#include "screens/portfolio/PortfolioOrderPanel.h"
#include "screens/portfolio/PortfolioPanelHeader.h"
#include "screens/portfolio/PortfolioPerfChart.h"
#include "screens/portfolio/PortfolioSectorPanel.h"
#include "screens/portfolio/PortfolioStatsRibbon.h"
#include "screens/portfolio/PortfolioStatusBar.h"
#include "screens/portfolio/PortfolioTxnPanel.h"
#include "services/file_manager/FileManagerService.h"
#include "services/portfolio/PortfolioService.h"
#include "storage/repositories/SettingsRepository.h"
#include "ui/theme/Theme.h"

#include <QEvent>
#include <QFileDialog>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>

#include <memory>

namespace fincept::screens {

PortfolioScreen::PortfolioScreen(QWidget* parent) : QWidget(parent) {
    build_ui();
    refresh_theme(); // Apply theme-aware font sizes and colors on first build

    // Connect to PortfolioService signals
    auto& svc = services::PortfolioService::instance();
    connect(&svc, &services::PortfolioService::portfolios_loaded, this, &PortfolioScreen::on_portfolios_loaded);
    connect(&svc, &services::PortfolioService::summary_loaded, this, &PortfolioScreen::on_summary_loaded);
    connect(&svc, &services::PortfolioService::summary_error, this, &PortfolioScreen::on_summary_error);
    connect(&svc, &services::PortfolioService::metrics_computed, this, &PortfolioScreen::on_metrics_computed);
    connect(&svc, &services::PortfolioService::portfolio_created, this, &PortfolioScreen::on_portfolio_created);
    connect(&svc, &services::PortfolioService::portfolio_deleted, this, &PortfolioScreen::on_portfolio_deleted);
    connect(&svc, &services::PortfolioService::asset_added, this, &PortfolioScreen::on_asset_changed);
    connect(&svc, &services::PortfolioService::asset_sold, this, &PortfolioScreen::on_asset_changed);
    connect(&svc, &services::PortfolioService::snapshots_loaded, this, &PortfolioScreen::on_snapshots_loaded);
    connect(&svc, &services::PortfolioService::transactions_loaded, this, [this](QVector<portfolio::Transaction> txns) {
        if (txn_panel_)
            txn_panel_->set_transactions(txns);
    });
    connect(&svc, &services::PortfolioService::correlation_computed, this, [this](QHash<QString, double> matrix) {
        if (sector_panel_)
            sector_panel_->set_correlation(matrix);
        if (detail_wrapper_)
            detail_wrapper_->update_correlation(matrix);
    });
    connect(&svc, &services::PortfolioService::spy_history_loaded, this,
            [this](QStringList /*dates*/, QVector<double> closes) {
                // Feed the FFN view's BENCHMARK (SPY) column — it computes
                // total return / CAGR / vol / Sharpe / max-DD from the closes.
                if (ffn_view_)
                    ffn_view_->set_benchmark(closes);
                // Recompute metrics now that SPY data is available for OLS beta.
                // The chart consumes the per-symbol benchmark_history_loaded
                // signal below — SPY there may differ from this Beta fetch.
                if (summary_loaded_)
                    services::PortfolioService::instance().compute_metrics(current_summary_);
            });
    connect(&svc, &services::PortfolioService::benchmark_history_loaded, this,
            [this](QString symbol, QStringList dates, QVector<double> closes) {
                // Hand the chart whichever benchmark was actually requested
                // (SPY for USD, ^GSPTSE for CAD, etc.) so the overlay label and
                // currency-normalisation are correct.
                if (!perf_chart_ || !summary_loaded_)
                    return;
                const QString want =
                    services::PortfolioService::default_benchmark_for_currency(current_summary_.portfolio.currency);
                if (symbol != want)
                    return; // ignore the secondary SPY-for-Beta fetch
                perf_chart_->set_benchmark_history(symbol, dates, closes);
            });
    connect(&svc, &services::PortfolioService::risk_free_rate_loaded, this, [this](double /*rate*/) {
        // Recompute metrics with updated risk-free rate for Sharpe
        if (summary_loaded_)
            services::PortfolioService::instance().compute_metrics(current_summary_);
    });
    // After yfinance backfill lands, refresh snapshots and metrics so Beta/MDD
    // populate without requiring a manual refresh.
    connect(&svc, &services::PortfolioService::history_backfilled, this, [this](QString portfolio_id, int point_count) {
        if (point_count <= 0 || !summary_loaded_ || portfolio_id != selected_id_)
            return;
        services::PortfolioService::instance().load_snapshots(portfolio_id);
        services::PortfolioService::instance().compute_metrics(current_summary_);
    });

    // Restore persisted refresh interval (P17)
    {
        auto r = SettingsRepository::instance().get("portfolio.refresh_interval_ms");
        if (r.is_ok() && !r.value().isEmpty()) {
            bool ok = false;
            const int saved = r.value().toInt(&ok);
            if (ok && saved >= 10000) // sanity: minimum 10 s
                refresh_interval_ms_ = saved;
        }
    }

    // Refresh timer (P3: only set interval, don't start)
    refresh_timer_ = new QTimer(this);
    refresh_timer_->setInterval(refresh_interval_ms_);
    connect(refresh_timer_, &QTimer::timeout, this, &PortfolioScreen::request_refresh);
    command_bar_->set_refresh_interval(refresh_interval_ms_);

    // Load portfolios
    svc.load_portfolios();

    // Theme change: refresh all child component styles
    connect(&ui::ThemeManager::instance(), &ui::ThemeManager::theme_changed, this,
            [this](const ui::ThemeTokens&) { refresh_theme(); });
}

void PortfolioScreen::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refresh_timer_->start();
    status_bar_->start_clock();
    if (blotter_ && !current_summary_.portfolio.broker_account_id.isEmpty())
        blotter_->hub_resubscribe_broker_quotes(current_summary_.portfolio.broker_account_id);
}

void PortfolioScreen::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    refresh_timer_->stop();
    status_bar_->stop_clock();
}

// ── Slots ────────────────────────────────────────────────────────────────────

void PortfolioScreen::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    QWidget::changeEvent(event);
}

void PortfolioScreen::retranslateUi() {
    // PortfolioScreen owns the empty/loading state labels and the POSITIONS
    // header. Child widgets (command bar, stats ribbon, etc.) handle their
    // own retranslate via QEvent::LanguageChange delivered to each top-level
    // widget by Qt.

    if (empty_title_label_)
        empty_title_label_->setText(tr("PORTFOLIO WORKSPACE"));
    if (empty_sub_label_)
        empty_sub_label_->setText(tr("Create, import, or explore a sample portfolio to get started."));

    if (empty_create_card_.title)
        empty_create_card_.title->setText(tr("CREATE NEW"));
    if (empty_create_card_.subtitle)
        empty_create_card_.subtitle->setText(tr("Start a fresh portfolio. Name it, pick a currency, "
                                                "and add holdings one at a time."));
    if (empty_import_card_.title)
        empty_import_card_.title->setText(tr("IMPORT JSON"));
    if (empty_import_card_.subtitle)
        empty_import_card_.subtitle->setText(tr("Load an existing portfolio from an exported JSON file. "
                                                "Merge into an existing portfolio or create a new one."));
    if (empty_demo_card_.title)
        empty_demo_card_.title->setText(tr("LOAD DEMO"));
    if (empty_demo_card_.subtitle)
        empty_demo_card_.subtitle->setText(tr("Preview the workspace with a sample diversified portfolio "
                                              "of 12 major equities."));

    if (loading_label_)
        loading_label_->setText(tr("Loading portfolio data…"));
    if (positions_title_label_)
        positions_title_label_->setText(tr("POSITIONS"));
    if (positions_filter_edit_)
        positions_filter_edit_->setPlaceholderText(tr("Filter positions…"));
}

void PortfolioScreen::refresh_theme() {
    setStyleSheet(QString("background:%1;").arg(ui::colors::BG_BASE()));

    if (command_bar_)
        command_bar_->refresh_theme();
    if (stats_ribbon_)
        stats_ribbon_->refresh_theme();
    if (perf_chart_)
        perf_chart_->refresh_theme();
    if (heatmap_)
        heatmap_->refresh_theme();
    if (blotter_)
        blotter_->refresh_theme();
    if (txn_panel_)
        txn_panel_->refresh_theme();
}

void PortfolioScreen::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    reposition_order_panel();

    // Keep the insights dock (and its scrim) glued to the right edge when
    // the window is resized.
    if (insights_panel_ && command_bar_) {
        const int top = command_bar_->height();
        const int bottom_reserve = status_bar_ ? status_bar_->height() : 0;
        const int h = qMax(200, height() - top - bottom_reserve);
        if (insights_scrim_ && insights_scrim_->isVisible())
            insights_scrim_->setGeometry(0, top, width(), h);
        if (insights_panel_->isVisible()) {
            insights_panel_->setFixedHeight(h);
            insights_panel_->move(width() - insights_panel_->width(), top);
        }
    }
}

const portfolio::HoldingWithQuote* PortfolioScreen::find_holding(const QString& symbol) const {
    for (const auto& h : current_summary_.holdings) {
        if (h.symbol == symbol)
            return &h;
    }
    return nullptr;
}

QVariantMap PortfolioScreen::save_state() const {
    QVariantMap state{{"portfolio_id", selected_id_}, {"symbol", selected_symbol_}};
    if (positions_filter_edit_)
        state["filter"] = positions_filter_edit_->text();
    return state;
}

void PortfolioScreen::restore_state(const QVariantMap& state) {
    const QString id = state.value("portfolio_id").toString();
    const QString sym = state.value("symbol").toString();
    if (!id.isEmpty())
        on_portfolio_selected(id);
    if (!sym.isEmpty())
        selected_symbol_ = sym;
    if (positions_filter_edit_ && state.contains("filter"))
        positions_filter_edit_->setText(state.value("filter").toString());
}

// ── IGroupLinked ─────────────────────────────────────────────────────────────

SymbolRef PortfolioScreen::current_symbol() const {
    if (selected_symbol_.isEmpty())
        return {};
    return SymbolRef::equity(selected_symbol_);
}

void PortfolioScreen::on_group_symbol_changed(const SymbolRef& ref) {
    if (!ref.is_valid())
        return;
    // Only react if the symbol is actually held — otherwise the group is
    // pointing at a ticker the user can't act on here, and silently
    // selecting a phantom would be misleading.
    if (!find_holding(ref.symbol))
        return;
    if (selected_symbol_ == ref.symbol)
        return;
    selected_symbol_ = ref.symbol;
    if (heatmap_)
        heatmap_->set_selected_symbol(ref.symbol);
    if (blotter_)
        blotter_->set_selected_symbol(ref.symbol);
    if (order_panel_)
        order_panel_->set_holding(find_holding(ref.symbol));
}

} // namespace fincept::screens
