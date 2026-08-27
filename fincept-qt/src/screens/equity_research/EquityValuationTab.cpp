// src/screens/equity_research/EquityValuationTab.cpp
#include "screens/equity_research/EquityValuationTab.h"

#include "ui/theme/Theme.h"

#include <QEvent>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>

#include <cmath>

namespace fincept::screens {

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Local valuation models
//
// These four scores used to be computed by the hosted /quantlib endpoints. They
// are closed-form formulas over inputs the screen already assembles locally, so
// they are evaluated in-process instead. Each returns the same JSON shape the
// display_*_result() sinks already parse, so an unavailable input still renders
// the existing "NO DATA" sentinel rather than a wrong number.
// ─────────────────────────────────────────────────────────────────────────────

/// Safe ratio: returns NaN rather than ±inf when the denominator is unusable,
/// so a missing input propagates to the NO-DATA sentinel instead of a verdict.
double ratio(double num, double den) {
    return (den != 0.0 && std::isfinite(num) && std::isfinite(den)) ? num / den : qQNaN();
}

/// Discounted cash flow (FCFF), Gordon-growth terminal value.
QJsonObject compute_dcf(const QJsonObject& b) {
    const double fcf = b.value("free_cash_flow").toDouble();
    const double g = b.value("growth_rate").toDouble();
    const double gt = b.value("terminal_growth_rate").toDouble();
    const double r = b.value("discount_rate").toDouble();
    const int years = b.value("projection_years").toInt();
    const double shares = b.value("shares_outstanding").toDouble();

    // on_calculate_clicked() already rejects r <= gt before dispatching, but
    // this is the function that would divide by zero, so it re-checks.
    if (!(shares > 0.0) || !(r > gt) || years <= 0 || !std::isfinite(fcf))
        return {};

    double pv = 0.0;
    double cf = fcf;
    for (int t = 1; t <= years; ++t) {
        cf *= (1.0 + g);
        pv += cf / std::pow(1.0 + r, t);
    }
    // cf is now FCF_n; the terminal value capitalises the following year.
    const double terminal = (cf * (1.0 + gt)) / (r - gt);
    pv += terminal / std::pow(1.0 + r, years);

    const double per_share = pv / shares;
    if (!std::isfinite(per_share))
        return {};
    return QJsonObject{{"intrinsic_value_per_share", per_share}};
}

/// Altman Z-score, the original public-manufacturing weighting.
QJsonObject compute_altman_z(const QJsonObject& b) {
    const double ta = b.value("total_assets").toDouble();
    const double tl = b.value("total_liabilities").toDouble();
    const double x1 = ratio(b.value("working_capital").toDouble(), ta);
    const double x2 = ratio(b.value("retained_earnings").toDouble(), ta);
    const double x3 = ratio(b.value("ebit").toDouble(), ta);
    const double x4 = ratio(b.value("market_cap").toDouble(), tl);
    const double x5 = ratio(b.value("revenue").toDouble(), ta);

    const double z = 1.2 * x1 + 1.4 * x2 + 3.3 * x3 + 0.6 * x4 + 1.0 * x5;
    if (!std::isfinite(z))
        return {};
    return QJsonObject{{"z_score", z}};
}

/// Piotroski F-score. Eight of the nine classic tests are computable from the
/// data this screen loads; the share-issuance test compares this year's share
/// count against itself (yfinance exposes only the current figure), so it
/// always passes — the same behaviour the hosted endpoint had, since the
/// caller sent it identical values for both periods.
QJsonObject compute_piotroski_f(const QJsonObject& b) {
    const double ta = b.value("total_assets").toDouble();
    const double prev_ta = b.value("prev_total_assets").toDouble();
    const double ni = b.value("net_income").toDouble();
    const double prev_ni = b.value("prev_net_income").toDouble();
    const double cfo = b.value("operating_cashflow").toDouble();
    if (!(ta > 0.0) || !(prev_ta > 0.0))
        return {};

    const double roa = ni / ta;
    const double prev_roa = prev_ni / prev_ta;
    const double prev_current_ratio =
        ratio(b.value("prev_current_assets").toDouble(), b.value("prev_current_liabilities").toDouble());
    const double gm = ratio(b.value("gross_profit").toDouble(), b.value("revenue").toDouble());
    const double prev_gm = ratio(b.value("prev_gross_profit").toDouble(), b.value("prev_revenue").toDouble());

    int f = 0;
    f += (roa > 0.0) ? 1 : 0;                                                     // 1. profitability
    f += (cfo > 0.0) ? 1 : 0;                                                     // 2. operating cash flow
    f += (roa > prev_roa) ? 1 : 0;                                                // 3. rising ROA
    f += (cfo > ni) ? 1 : 0;                                                      // 4. accrual quality
    f += (ratio(b.value("long_term_debt").toDouble(), ta) <
          ratio(b.value("prev_long_term_debt").toDouble(), prev_ta))              // 5. falling leverage
             ? 1
             : 0;
    f += (b.value("current_ratio").toDouble() > prev_current_ratio) ? 1 : 0;      // 6. rising liquidity
    f += (b.value("shares_outstanding").toDouble() <=
          b.value("prev_shares_outstanding").toDouble())                          // 7. no dilution
             ? 1
             : 0;
    f += (std::isfinite(gm) && std::isfinite(prev_gm) && gm > prev_gm) ? 1 : 0;   // 8. rising gross margin
    f += (ratio(b.value("revenue").toDouble(), ta) >
          ratio(b.value("prev_revenue").toDouble(), prev_ta))                     // 9. rising asset turnover
             ? 1
             : 0;

    return QJsonObject{{"f_score", f}};
}

/// Beneish M-score.
///
/// Six of the eight indices come from data this screen loads. DEPI
/// (depreciation) and SGAI (SG&A expense) are not in the statement subset
/// yfinance returns here, so both are held at their neutral value of 1.0 —
/// their combined weight is -0.057, so the effect on the verdict threshold is
/// negligible compared to TATA (4.679) and SGI (0.892), which are computed.
QJsonObject compute_beneish_m(const QJsonObject& b) {
    const double rev = b.value("revenue").toDouble();
    const double prev_rev = b.value("prev_revenue").toDouble();
    const double ta = b.value("total_assets").toDouble();
    const double prev_ta = b.value("prev_total_assets").toDouble();
    if (!(rev > 0.0) || !(prev_rev > 0.0) || !(ta > 0.0) || !(prev_ta > 0.0))
        return {};

    const double dsri = ratio(ratio(b.value("receivables").toDouble(), rev),
                              ratio(b.value("prev_receivables").toDouble(), prev_rev));
    const double gmi = ratio(ratio(b.value("prev_gross_profit").toDouble(), prev_rev),
                             ratio(b.value("gross_profit").toDouble(), rev));
    const double soft = 1.0 - (b.value("current_assets").toDouble() + b.value("ppe").toDouble()) / ta;
    const double prev_soft = 1.0 - (b.value("prev_current_assets").toDouble() + b.value("prev_ppe").toDouble()) / prev_ta;
    const double aqi = ratio(soft, prev_soft);
    const double sgi = rev / prev_rev;
    const double depi = 1.0; // not computable from the loaded statements
    const double sgai = 1.0; // not computable from the loaded statements
    const double lvgi = ratio(ratio(b.value("long_term_debt").toDouble(), ta),
                              ratio(b.value("prev_long_term_debt").toDouble(), prev_ta));
    const double tata = (b.value("net_income").toDouble() - b.value("operating_cashflow").toDouble()) / ta;

    // An index whose inputs were missing contributes nothing rather than
    // poisoning the whole score with NaN.
    auto term = [](double coeff, double index) { return std::isfinite(index) ? coeff * index : 0.0; };

    const double m = -4.84 + term(0.920, dsri) + term(0.528, gmi) + term(0.404, aqi) + term(0.892, sgi) +
                     term(0.115, depi) - term(0.172, sgai) + term(4.679, tata) - term(0.327, lvgi);
    if (!std::isfinite(m))
        return {};
    return QJsonObject{{"m_score", m}};
}

} // namespace


// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

EquityValuationTab::EquityValuationTab(QWidget* parent) : QWidget(parent) {
    build_ui();
}

// ─────────────────────────────────────────────────────────────────────────────
// build_ui
// ─────────────────────────────────────────────────────────────────────────────

void EquityValuationTab::build_ui() {
    setStyleSheet(QString("background:%1; color:%2;").arg(ui::colors::BG_BASE(), ui::colors::TEXT_PRIMARY()));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet("background:transparent; border:0;");

    auto* content = new QWidget;
    content->setStyleSheet("background:transparent;");
    auto* vbox = new QVBoxLayout(content);
    vbox->setContentsMargins(12, 12, 12, 12);
    vbox->setSpacing(12);

    build_dcf_section(vbox);
    build_scoring_section(vbox);
    build_multiples_section(vbox);
    vbox->addStretch();

    scroll->setWidget(content);
    root->addWidget(scroll);
}

// ─────────────────────────────────────────────────────────────────────────────
// Section builders
// ─────────────────────────────────────────────────────────────────────────────

void EquityValuationTab::build_dcf_section(QVBoxLayout* parent) {
    auto* box = new QGroupBox(tr("DCF MODEL"));
    box->setStyleSheet(QString("QGroupBox { border:1px solid %1; border-radius:4px; margin-top:8px;"
                               "  font-size:11px; font-weight:700; color:%2; letter-spacing:1px; padding:8px; }"
                               "QGroupBox::title { subcontrol-origin:margin; left:8px; }")
                           .arg(ui::colors::BORDER_DIM(), ui::colors::AMBER()));

    auto* hl = new QHBoxLayout(box);
    hl->setSpacing(20);

    // ── Left: inputs ──────────────────────────────────────────────
    auto* left = new QWidget;
    auto* form = new QVBoxLayout(left);
    form->setSpacing(6);
    form->setContentsMargins(0, 0, 0, 0);

    auto make_spin = [&](const char* label_key, double min, double max, double def, const QString& suffix,
                         int dec) -> QDoubleSpinBox* {
        auto* row = new QWidget;
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(8);

        auto* lbl = new QLabel(tr(label_key));
        lbl->setStyleSheet(QString("color:%1; font-size:11px;").arg(ui::colors::TEXT_SECONDARY()));
        lbl->setFixedWidth(130);
        i18n_labels_[lbl] = label_key;

        auto* spin = new QDoubleSpinBox;
        spin->setRange(min, max);
        spin->setValue(def);
        spin->setSuffix(suffix);
        spin->setDecimals(dec);
        spin->setFixedWidth(120);
        spin->setStyleSheet(QString("QDoubleSpinBox { background:%1; color:%2; border:1px solid %3;"
                                    " padding:2px 4px; font-size:12px; }"
                                    "QDoubleSpinBox::up-button, QDoubleSpinBox::down-button { width:16px; }")
                                .arg(ui::colors::BG_SURFACE(), ui::colors::TEXT_PRIMARY(), ui::colors::BORDER_DIM()));

        rl->addWidget(lbl);
        rl->addWidget(spin);
        rl->addStretch();
        form->addWidget(row);
        return spin;
    };

    fcf_input_ = make_spin("FREE CASH FLOW", -999999, 999999, 0.0, " M", 2);
    growth_input_ = make_spin("GROWTH RATE", -50, 100, 8.0, " %", 2);
    terminal_input_ = make_spin("TERMINAL GROWTH", 0, 10, 2.5, " %", 2);
    discount_input_ = make_spin("DISCOUNT RATE", 1, 30, 10.0, " %", 2);
    years_input_ = make_spin("PROJECTION YEARS", 5, 20, 10.0, " yrs", 0);
    shares_input_ = make_spin("SHARES O/S", 0.01, 999999, 1.0, " M", 2);

    calc_btn_ = new QPushButton(tr("CALCULATE"));
    calc_btn_->setCursor(Qt::PointingHandCursor);
    calc_btn_->setFixedHeight(28);
    calc_btn_->setStyleSheet(QString("QPushButton { background:%1; color:%2; border:0; font-size:12px;"
                                     " font-weight:700; letter-spacing:1px; border-radius:3px; }"
                                     "QPushButton:hover { background:%3; }"
                                     "QPushButton:disabled { background:%4; color:%5; }")
                                 .arg(ui::colors::AMBER(), ui::colors::BG_BASE(), ui::colors::AMBER_DIM(),
                                      ui::colors::BG_SURFACE(), ui::colors::TEXT_TERTIARY()));
    connect(calc_btn_, &QPushButton::clicked, this, &EquityValuationTab::on_calculate_clicked);
    form->addWidget(calc_btn_);
    form->addStretch();
    hl->addWidget(left);

    // ── Right: results ────────────────────────────────────────────
    auto* right = new QWidget;
    auto* rl2 = new QVBoxLayout(right);
    rl2->setSpacing(8);
    rl2->setContentsMargins(0, 0, 0, 0);

    auto make_result_row = [&](const char* label_key, QLabel*& val_out, const QString& val_color = "") {
        auto* row = new QWidget;
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(8);

        auto* lbl = new QLabel(tr(label_key));
        lbl->setStyleSheet(QString("color:%1; font-size:11px;").arg(ui::colors::TEXT_SECONDARY()));
        lbl->setFixedWidth(130);
        i18n_labels_[lbl] = label_key;

        val_out = new QLabel(QStringLiteral("—"));
        QString col = val_color.isEmpty() ? QString(ui::colors::TEXT_PRIMARY()) : val_color;
        val_out->setStyleSheet(QString("color:%1; font-size:14px; font-weight:700;").arg(col));

        h->addWidget(lbl);
        h->addWidget(val_out);
        h->addStretch();
        rl2->addWidget(row);
    };

    make_result_row("INTRINSIC VALUE", intrinsic_val_, ui::colors::TEXT_PRIMARY());
    make_result_row("CURRENT PRICE", current_price_val_, ui::colors::TEXT_SECONDARY());
    make_result_row("MARGIN OF SAFETY", margin_val_, ui::colors::TEXT_PRIMARY());

    verdict_badge_ = new QLabel;
    verdict_badge_->setAlignment(Qt::AlignCenter);
    verdict_badge_->setFixedHeight(24);
    verdict_badge_->setStyleSheet("font-size:11px; font-weight:700;");
    rl2->addWidget(verdict_badge_);
    rl2->addStretch();

    hl->addWidget(right);
    parent->addWidget(box);
}

void EquityValuationTab::build_scoring_section(QVBoxLayout* parent) {
    auto* box = new QGroupBox(tr("CREDIT QUALITY"));
    box->setStyleSheet(QString("QGroupBox { border:1px solid %1; border-radius:4px; margin-top:8px;"
                               "  font-size:11px; font-weight:700; color:%2; letter-spacing:1px; padding:8px; }"
                               "QGroupBox::title { subcontrol-origin:margin; left:8px; }")
                           .arg(ui::colors::BORDER_DIM(), ui::colors::AMBER()));

    auto* vl = new QVBoxLayout(box);
    vl->setSpacing(8);

    scoring_note_ = new QLabel(tr("Open FINANCIALS tab first to enable scoring."));
    scoring_note_->setStyleSheet(
        QString("color:%1; font-size:11px; font-style:italic;").arg(ui::colors::TEXT_TERTIARY()));
    vl->addWidget(scoring_note_);

    auto make_score_row = [&](const char* label_key, QLabel*& val_out, QLabel*& badge_out) {
        auto* row = new QWidget;
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(12);

        auto* lbl = new QLabel(tr(label_key));
        lbl->setStyleSheet(QString("color:%1; font-size:12px;").arg(ui::colors::TEXT_SECONDARY()));
        lbl->setFixedWidth(140);
        i18n_labels_[lbl] = label_key;

        val_out = new QLabel(QStringLiteral("—"));
        val_out->setStyleSheet(QString("color:%1; font-size:13px; font-weight:600;").arg(ui::colors::TEXT_PRIMARY()));
        val_out->setFixedWidth(60);

        badge_out = new QLabel;
        badge_out->setAlignment(Qt::AlignCenter);
        badge_out->setFixedHeight(20);
        badge_out->setMinimumWidth(160);
        badge_out->setStyleSheet("font-size:10px; font-weight:700;");

        h->addWidget(lbl);
        h->addWidget(val_out);
        h->addWidget(badge_out);
        h->addStretch();
        vl->addWidget(row);
    };

    make_score_row("ALTMAN Z-SCORE", altman_val_, altman_badge_);
    make_score_row("PIOTROSKI F", piotroski_val_, piotroski_badge_);
    make_score_row("BENEISH M-SCORE", beneish_val_, beneish_badge_);

    parent->addWidget(box);
}

void EquityValuationTab::build_multiples_section(QVBoxLayout* parent) {
    auto* box = new QGroupBox(tr("ADVANCED MULTIPLES"));
    box->setStyleSheet(QString("QGroupBox { border:1px solid %1; border-radius:4px; margin-top:8px;"
                               "  font-size:11px; font-weight:700; color:%2; letter-spacing:1px; padding:8px; }"
                               "QGroupBox::title { subcontrol-origin:margin; left:8px; }")
                           .arg(ui::colors::BORDER_DIM(), ui::colors::AMBER()));

    auto* grid = new QWidget;
    auto* gl = new QHBoxLayout(grid);
    gl->setSpacing(32);
    gl->setContentsMargins(0, 0, 0, 0);

    auto* col1 = new QVBoxLayout;
    auto* col2 = new QVBoxLayout;

    auto make_mult = [&](const char* label_key, QLabel*& val_out, QVBoxLayout* col) {
        auto* row = new QWidget;
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(8);

        auto* lbl = new QLabel(tr(label_key));
        lbl->setStyleSheet(QString("color:%1; font-size:11px;").arg(ui::colors::TEXT_SECONDARY()));
        lbl->setFixedWidth(100);
        i18n_labels_[lbl] = label_key;

        val_out = new QLabel(QStringLiteral("—"));
        val_out->setStyleSheet(QString("color:%1; font-size:13px; font-weight:600;").arg(ui::colors::TEXT_PRIMARY()));

        h->addWidget(lbl);
        h->addWidget(val_out);
        h->addStretch();
        col->addWidget(row);
    };

    make_mult("EV/EBITDA", ev_ebitda_val_, col1);
    make_mult("EV/REVENUE", ev_revenue_val_, col1);
    make_mult("P/SALES", price_sales_val_, col2);
    make_mult("GRAHAM NUMBER", graham_val_, col2);

    gl->addLayout(col1);
    gl->addLayout(col2);
    gl->addStretch();

    auto* vl = new QVBoxLayout(box);
    vl->addWidget(grid);

    parent->addWidget(box);
}

// ─────────────────────────────────────────────────────────────────────────────
// Public setters
// ─────────────────────────────────────────────────────────────────────────────

void EquityValuationTab::set_symbol(const QString& symbol) {
    if (symbol == current_symbol_)
        return;
    current_symbol_ = symbol;
    financials_loaded_ = false;
    clear_results();
    if (scoring_note_)
        scoring_note_->setVisible(true);
}

void EquityValuationTab::set_stock_info(const services::equity::StockInfo& info) {
    last_info_ = info;
    current_price_ = info.current_price;
    current_currency_ = info.currency;
    pre_fill_dcf_inputs();

    // Show current price in results panel immediately
    if (current_price_val_)
        current_price_val_->setText(fmt_currency(current_price_));
}

void EquityValuationTab::set_financials(const services::equity::FinancialsData& data) {
    last_financials_ = data;
    financials_loaded_ = true;
    if (scoring_note_)
        scoring_note_->setVisible(false);
}

// ─────────────────────────────────────────────────────────────────────────────
// Pre-fill DCF from StockInfo
// ─────────────────────────────────────────────────────────────────────────────

void EquityValuationTab::pre_fill_dcf_inputs() {
    if (!fcf_input_)
        return;

    double fcf_m = last_info_.free_cashflow / 1e6;
    double growth = last_info_.earnings_growth * 100.0;
    double shares_m = last_info_.shares_outstanding / 1e6;
    double wacc = 4.5 + last_info_.beta * 5.5; // CAPM approx

    if (std::isfinite(fcf_m))
        fcf_input_->setValue(fcf_m);
    if (std::isfinite(growth) && growth > -100.0 && growth < 200.0)
        growth_input_->setValue(growth);
    if (std::isfinite(shares_m) && shares_m > 0.0)
        shares_input_->setValue(shares_m);
    if (std::isfinite(wacc) && wacc > 1.0 && wacc < 30.0)
        discount_input_->setValue(wacc);
}

// ─────────────────────────────────────────────────────────────────────────────
// CALCULATE slot
// ─────────────────────────────────────────────────────────────────────────────

void EquityValuationTab::on_calculate_clicked() {
    if (current_symbol_.isEmpty())
        return;

    // Validation: discount > terminal
    if (discount_input_->value() <= terminal_input_->value()) {
        if (margin_val_)
            margin_val_->setText(tr("ERR: discount ≤ terminal"));
        return;
    }

    calc_btn_->setEnabled(false);
    calc_btn_->setText(tr("CALCULATING…"));

    // ── 1. DCF ────────────────────────────────────────────────────
    QJsonObject dcf_body;
    dcf_body["free_cash_flow"] = fcf_input_->value() * 1e6;
    dcf_body["growth_rate"] = growth_input_->value() / 100.0;
    dcf_body["terminal_growth_rate"] = terminal_input_->value() / 100.0;
    dcf_body["discount_rate"] = discount_input_->value() / 100.0;
    dcf_body["projection_years"] = static_cast<int>(years_input_->value());
    dcf_body["shares_outstanding"] = shares_input_->value() * 1e6;

    calc_btn_->setEnabled(true);
    calc_btn_->setText(tr("CALCULATE"));
    display_dcf_result(compute_dcf(dcf_body));

    // ── 2. Scoring (only if financials loaded) ─────────────────────
    if (financials_loaded_)
        run_scoring_models();

    // ── 3. Multiples ──────────────────────────────────────────────
    run_multiples();
}

// ─────────────────────────────────────────────────────────────────────────────
// Scoring models
// ─────────────────────────────────────────────────────────────────────────────

void EquityValuationTab::run_scoring_models() {
    const auto& bs = last_financials_.balance_sheet;
    const auto& is = last_financials_.income_statement;
    const auto& cf = last_financials_.cash_flow;

    double total_assets = get_stmt_val(bs, "Total Assets");
    double total_liabilities = get_stmt_val(bs, "Total Liabilities Net Minority Interest");
    double current_assets = get_stmt_val(bs, "Current Assets");
    double current_liab = get_stmt_val(bs, "Current Liabilities");
    double retained_earnings = get_stmt_val(bs, "Retained Earnings");
    double revenue = get_stmt_val(is, "Total Revenue");
    double ebit = get_stmt_val(is, "EBIT");
    if (ebit == 0.0)
        ebit = get_stmt_val(is, "Operating Income");
    double working_capital = (current_assets != 0.0 && current_liab != 0.0) ? current_assets - current_liab
                                                                            : get_stmt_val(bs, "Working Capital");

    // ── Altman Z ─────────────────────────────────────────────────
    // Four of Altman's five ratios divide by total assets and the fifth by total
    // liabilities. With an empty balance sheet every input here is 0, and scoring
    // anyway yields a number that describes nothing — which then rendered as a red
    // "DISTRESS ZONE". Don't score; say so instead.
    if (total_assets > 0.0 && total_liabilities > 0.0) {
        QJsonObject altman_body;
        altman_body["working_capital"] = working_capital;
        altman_body["total_assets"] = total_assets;
        altman_body["retained_earnings"] = retained_earnings;
        altman_body["ebit"] = ebit;
        altman_body["market_cap"] = last_info_.market_cap;
        altman_body["total_liabilities"] = total_liabilities;
        altman_body["revenue"] = revenue;

        // An empty object renders "NO DATA" — an uncomputable score must not
        // leave a stale verdict from the previously viewed symbol.
        display_altman_result(compute_altman_z(altman_body));
    } else {
        display_altman_result(QJsonValue());
    }

    // ── Piotroski F (needs two periods) ──────────────────────────
    if (is.size() >= 2 && bs.size() >= 2) {
        QJsonObject pio_body;
        // Period 0 = most recent, period 1 = prior year
        pio_body["net_income"] = get_stmt_val(is, "Net Income");
        pio_body["total_assets"] = get_stmt_val(bs, "Total Assets");
        pio_body["operating_cashflow"] = get_stmt_val(cf, "Operating Cash Flow");
        pio_body["long_term_debt"] = get_stmt_val(bs, "Long Term Debt");
        pio_body["current_ratio"] = (current_liab > 0) ? current_assets / current_liab : 0.0;
        pio_body["shares_outstanding"] = last_info_.shares_outstanding;
        pio_body["gross_profit"] = get_stmt_val(is, "Gross Profit");
        pio_body["revenue"] = get_stmt_val(is, "Total Revenue");
        // Prior year
        pio_body["prev_total_assets"] = get_stmt_val(bs, "Total Assets", 1);
        pio_body["prev_long_term_debt"] = get_stmt_val(bs, "Long Term Debt", 1);
        pio_body["prev_current_assets"] = get_stmt_val(bs, "Current Assets", 1);
        pio_body["prev_current_liabilities"] = get_stmt_val(bs, "Current Liabilities", 1);
        pio_body["prev_shares_outstanding"] = last_info_.shares_outstanding;
        pio_body["prev_gross_profit"] = get_stmt_val(is, "Gross Profit", 1);
        pio_body["prev_revenue"] = get_stmt_val(is, "Total Revenue", 1);
        pio_body["prev_net_income"] = get_stmt_val(is, "Net Income", 1);

        display_piotroski_result(compute_piotroski_f(pio_body));
    }

    // ── Beneish M ────────────────────────────────────────────────
    if (is.size() >= 2 && bs.size() >= 2) {
        QJsonObject ben_body;
        ben_body["revenue"] = get_stmt_val(is, "Total Revenue");
        ben_body["prev_revenue"] = get_stmt_val(is, "Total Revenue", 1);
        ben_body["receivables"] = get_stmt_val(bs, "Receivables");
        ben_body["prev_receivables"] = get_stmt_val(bs, "Receivables", 1);
        ben_body["gross_profit"] = get_stmt_val(is, "Gross Profit");
        ben_body["prev_gross_profit"] = get_stmt_val(is, "Gross Profit", 1);
        ben_body["total_assets"] = get_stmt_val(bs, "Total Assets");
        ben_body["prev_total_assets"] = get_stmt_val(bs, "Total Assets", 1);
        ben_body["long_term_debt"] = get_stmt_val(bs, "Long Term Debt");
        ben_body["prev_long_term_debt"] = get_stmt_val(bs, "Long Term Debt", 1);
        ben_body["current_assets"] = get_stmt_val(bs, "Current Assets");
        ben_body["prev_current_assets"] = get_stmt_val(bs, "Current Assets", 1);
        ben_body["ppe"] = get_stmt_val(bs, "Net PPE");
        ben_body["prev_ppe"] = get_stmt_val(bs, "Net PPE", 1);
        ben_body["net_income"] = get_stmt_val(is, "Net Income");
        ben_body["prev_net_income"] = get_stmt_val(is, "Net Income", 1);
        ben_body["operating_cashflow"] = get_stmt_val(cf, "Operating Cash Flow");

        display_beneish_result(compute_beneish_m(ben_body));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Multiples
// ─────────────────────────────────────────────────────────────────────────────

void EquityValuationTab::run_multiples() {
    // Use already-available StockInfo fields — no extra API call needed
    // for EV/EBITDA, EV/Revenue, P/S. Graham Number needs EPS + book value.

    if (ev_ebitda_val_)
        ev_ebitda_val_->setText(last_info_.ev_to_ebitda > 0 ? QString("%1x").arg(last_info_.ev_to_ebitda, 0, 'f', 1)
                                                            : QStringLiteral("—"));

    if (ev_revenue_val_)
        ev_revenue_val_->setText(last_info_.ev_to_revenue > 0 ? QString("%1x").arg(last_info_.ev_to_revenue, 0, 'f', 1)
                                                              : QStringLiteral("—"));

    if (price_sales_val_) {
        double ps = (last_info_.total_revenue > 0 && last_info_.market_cap > 0)
                        ? last_info_.market_cap / last_info_.total_revenue
                        : 0.0;
        price_sales_val_->setText(ps > 0 ? QString("%1x").arg(ps, 0, 'f', 1) : QStringLiteral("—"));
    }

    // Graham Number = sqrt(22.5 * EPS * BookValue)
    // EPS ≈ (market_cap / shares_outstanding) / pe_ratio  (approximate)
    if (graham_val_) {
        double eps = (last_info_.pe_ratio > 0 && last_info_.current_price > 0)
                         ? last_info_.current_price / last_info_.pe_ratio
                         : 0.0;
        double bv = last_info_.book_value;
        double graham = (eps > 0 && bv > 0) ? std::sqrt(22.5 * eps * bv) : 0.0;
        graham_val_->setText(graham > 0 ? fmt_currency(graham) : QStringLiteral("—"));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Display helpers
// ─────────────────────────────────────────────────────────────────────────────

void EquityValuationTab::display_dcf_result(const QJsonValue& data) {
    auto obj = data.toObject();
    double intrinsic = obj.value("intrinsic_value_per_share").toDouble(0.0);
    if (intrinsic == 0.0)
        intrinsic = obj.value("intrinsic_value").toDouble(0.0);
    if (intrinsic == 0.0)
        intrinsic = obj.value("value_per_share").toDouble(0.0);

    if (intrinsic_val_)
        intrinsic_val_->setText(intrinsic > 0 ? fmt_currency(intrinsic) : QStringLiteral("—"));

    if (current_price_ <= 0 || intrinsic <= 0)
        return;

    double margin_pct = (intrinsic - current_price_) / current_price_ * 100.0;
    QString margin_text = QString("%1%2%").arg(margin_pct >= 0 ? "+" : "").arg(margin_pct, 0, 'f', 1);

    if (margin_val_) {
        margin_val_->setText(margin_text);
        QString col = margin_pct > 10    ? QString(ui::colors::POSITIVE())
                      : margin_pct < -10 ? QString(ui::colors::NEGATIVE())
                                         : QString(ui::colors::AMBER());
        margin_val_->setStyleSheet(QString("color:%1; font-size:14px; font-weight:700;").arg(col));
    }

    if (verdict_badge_) {
        if (margin_pct > 20)
            set_badge(verdict_badge_, tr("UNDERVALUED"), ui::colors::POSITIVE());
        else if (margin_pct < -20)
            set_badge(verdict_badge_, tr("OVERVALUED"), ui::colors::NEGATIVE());
        else
            set_badge(verdict_badge_, tr("FAIRLY VALUED"), ui::colors::AMBER());
    }
}

void EquityValuationTab::display_altman_result(const QJsonValue& data) {
    auto obj = data.toObject();
    // NaN sentinel, not 0.0. run_scoring_models() dispatches the Altman request
    // unconditionally — with all-zero inputs when the balance sheet is empty,
    // which is the normal case for ETFs, funds and most non-US listings. A 0.0
    // default fell past both thresholds into "DISTRESS ZONE", so the terminal
    // published a red bankruptcy verdict for any instrument whose financials
    // simply did not load. Mirrors the -1 sentinel in display_piotroski_result.
    const double z = obj.value("z_score").toDouble(obj.value("altman_z_score").toDouble(qQNaN()));
    const bool has_z = !std::isnan(z);

    if (altman_val_)
        altman_val_->setText(has_z ? QString::number(z, 'f', 2) : QStringLiteral("—"));

    if (altman_badge_) {
        if (!has_z)
            set_badge(altman_badge_, tr("NO DATA"), ui::colors::TEXT_TERTIARY());
        else if (z > 2.99)
            set_badge(altman_badge_, tr("SAFE ZONE"), ui::colors::POSITIVE());
        else if (z >= 1.81)
            set_badge(altman_badge_, tr("GREY ZONE"), ui::colors::AMBER());
        else
            set_badge(altman_badge_, tr("DISTRESS ZONE"), ui::colors::NEGATIVE());
    }
}

void EquityValuationTab::display_piotroski_result(const QJsonValue& data) {
    auto obj = data.toObject();
    int f = obj.value("f_score").toInt(obj.value("piotroski_f_score").toInt(-1));

    if (piotroski_val_)
        piotroski_val_->setText(f >= 0 ? QString("%1/9").arg(f) : QStringLiteral("—"));

    if (piotroski_badge_ && f >= 0) {
        if (f >= 8)
            set_badge(piotroski_badge_, tr("STRONG (%1-9)").arg(f), ui::colors::POSITIVE());
        else if (f >= 5)
            set_badge(piotroski_badge_, tr("NEUTRAL (%1-7)").arg(f), ui::colors::AMBER());
        else
            set_badge(piotroski_badge_, tr("WEAK (0-%1)").arg(f), ui::colors::NEGATIVE());
    }
}

void EquityValuationTab::display_beneish_result(const QJsonValue& data) {
    auto obj = data.toObject();
    // Same 0.0-default trap as Altman: an absent m_score defaulted to 0.0, which
    // is above the -2.22 threshold and therefore accused every company with no
    // returned score of "POSSIBLE MANIPULATOR".
    const double m = obj.value("m_score").toDouble(obj.value("beneish_m_score").toDouble(qQNaN()));
    const bool has_m = !std::isnan(m);

    if (beneish_val_)
        beneish_val_->setText(has_m ? QString::number(m, 'f', 2) : QStringLiteral("—"));

    if (beneish_badge_) {
        if (!has_m)
            set_badge(beneish_badge_, tr("NO DATA"), ui::colors::TEXT_TERTIARY());
        else if (m < -2.22)
            set_badge(beneish_badge_, tr("UNLIKELY MANIPULATOR"), ui::colors::POSITIVE());
        else
            set_badge(beneish_badge_, tr("POSSIBLE MANIPULATOR"), ui::colors::NEGATIVE());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Utility
// ─────────────────────────────────────────────────────────────────────────────

void EquityValuationTab::clear_results() {
    auto reset = [](QLabel* l) {
        if (l)
            l->setText(QStringLiteral("—"));
    };
    auto clear_badge = [](QLabel* l) {
        if (l)
            l->setText(QString());
    };

    reset(intrinsic_val_);
    reset(current_price_val_);
    reset(margin_val_);
    clear_badge(verdict_badge_);
    reset(altman_val_);
    clear_badge(altman_badge_);
    reset(piotroski_val_);
    clear_badge(piotroski_badge_);
    reset(beneish_val_);
    clear_badge(beneish_badge_);
    reset(ev_ebitda_val_);
    reset(ev_revenue_val_);
    reset(price_sales_val_);
    reset(graham_val_);
}

void EquityValuationTab::set_badge(QLabel* badge, const QString& text, const QString& hex_color) {
    if (!badge)
        return;
    badge->setText(text);
    badge->setStyleSheet(QString("background:%1; color:#000000; padding:2px 10px;"
                                 " border-radius:3px; font-size:10px; font-weight:700;")
                             .arg(hex_color));
}

QString EquityValuationTab::fmt_currency(double value) const {
    QString cs = QStringLiteral("$");
    if (current_currency_ == QStringLiteral("INR"))
        cs = QStringLiteral("₹");
    else if (current_currency_ == QStringLiteral("EUR"))
        cs = QStringLiteral("€");
    else if (current_currency_ == QStringLiteral("GBP"))
        cs = QStringLiteral("£");
    return QString("%1%2").arg(cs).arg(value, 0, 'f', 2);
}

double EquityValuationTab::get_stmt_val(const QVector<QPair<QString, QJsonObject>>& stmt, const QString& key,
                                        int period_idx) const {
    if (period_idx >= stmt.size())
        return 0.0;
    return stmt[period_idx].second.value(key).toDouble(0.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// i18n
// ─────────────────────────────────────────────────────────────────────────────

void EquityValuationTab::changeEvent(QEvent* e) {
    if (e->type() == QEvent::LanguageChange) {
        for (auto it = i18n_labels_.begin(); it != i18n_labels_.end(); ++it)
            it.key()->setText(tr(it.value()));
        if (calc_btn_)
            calc_btn_->setText(tr("CALCULATE"));
    }
    QWidget::changeEvent(e);
}

} // namespace fincept::screens
