// src/screens/docs/DocsScreen_Pages_Trading.cpp
//
// Trading and quant analytics docs pages: Crypto/Paper/Algo Trading,
// Backtesting, Equity Research, Surface Analytics, Derivatives, Portfolio,
// M&A Analytics, AI Quant Lab, QuantLib, AI Chat, Agent Config, Alpha Arena.
//
// Part of the partial-class split of DocsScreen.cpp.

#include "screens/docs/DocsScreen.h"
#include "screens/docs/DocsScreen_internal.h"
#include "ui/theme/Theme.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

namespace fincept::screens {

using fincept::screens::docs_internal::SCROLL_SS;

QWidget* DocsScreen::page_crypto_trading() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet(SCROLL_SS());

    auto* page = new QWidget(this);
    page->setStyleSheet(QString("background: %1;").arg(ui::colors::BG_BASE()));
    auto* vl = new QVBoxLayout(page);
    vl->setContentsMargins(20, 16, 20, 20);
    vl->setSpacing(12);

    vl->addWidget(make_heading(tr("CRYPTO TRADING")));
    vl->addWidget(make_muted_label(tr("Multi-exchange crypto trading terminal")));

    auto* sep = new QFrame;
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet(QString("color: %1;").arg(ui::colors::BORDER_DIM()));
    vl->addWidget(sep);

    vl->addWidget(
        make_section_panel("■", tr("OVERVIEW"),
                           tr("A full-featured crypto trading terminal supporting 10+ exchanges with real-time "
                              "WebSocket feeds. Features OHLC charts, order book depth, order entry with market/"
                              "limit/stop orders, watchlist, and a paper trading engine for risk-free practice."),
                           ui::colors::AMBER));

    vl->addWidget(make_section_panel("■", tr("SUPPORTED EXCHANGES"),
                                     tr("Kraken  |  Binance  |  Bybit  |  OKX  |  Coinbase  |  Bitget\n"
                                        "Gate  |  KuCoin  |  MEXC  |  HTX\n\n"
                                        "Each exchange supports: spot trading, real-time orderbook, OHLC candles, "
                                        "ticker data, and trade history via WebSocket."),
                                     ui::colors::POSITIVE));

    vl->addWidget(
        make_section_panel("■", tr("PANELS"),
                           tr("■  Command Bar — Exchange selector, symbol input, paper/live toggle, API config\n"
                              "■  Ticker Bar — Real-time price, 24h change, high/low, volume\n"
                              "■  Watchlist — 8+ default pairs with live prices\n"
                              "■  OHLC Chart — Candlestick chart with multiple timeframes\n"
                              "■  Order Entry — Market, Limit, Stop-Limit with SL/TP\n"
                              "■  Order Book — Live bid/ask depth visualization\n"
                              "■  Bottom Panel — Open orders, order history, positions, balances"),
                           ui::colors::INFO));

    vl->addWidget(make_section_panel("■", tr("PAPER TRADING"),
                                     tr("Built-in paper trading engine with:\n"
                                        "■  Simulated order matching with realistic fills\n"
                                        "■  Virtual portfolio tracking (default $100,000 USDT)\n"
                                        "■  P&L tracking per position\n"
                                        "■  Order history persistence\n"
                                        "■  Switch between Paper and Live mode with one click"),
                                     ui::colors::CYAN));

    vl->addWidget(make_skill_panel(
        tr("Start in Paper mode. Pick BTC/USDT, place a small market buy, watch it fill. Learn the order book."),
        tr("Try limit orders at support levels. Use stop-loss. Track P&L across multiple positions."),
        tr("Switch to Live mode with API keys. Use multiple exchanges. Implement SL/TP strategies."),
        tr("Run simultaneous positions across exchanges. Use order flow data for scalping. Combine with algo "
           "strategies.")));

    vl->addStretch();
    scroll->setWidget(page);
    return scroll;
}

QWidget* DocsScreen::page_paper_trading() {
    return make_page(
        tr("PAPER TRADING"), tr("Risk-free simulated trading engine"),
        {
            {tr("OVERVIEW"), tr("The paper trading engine simulates real market execution with a virtual portfolio. "
                                "Available in Crypto Trading screen via the Paper/Live toggle. Your paper portfolio "
                                "starts with $100,000 USDT and supports all order types.")},
            {tr("KEY FEATURES"), tr("■  Realistic order matching with market simulation\n"
                                    "■  Market, Limit, and Stop-Limit order types\n"
                                    "■  Stop-loss and take-profit support\n"
                                    "■  Per-position P&L tracking\n"
                                    "■  Full order history with timestamps\n"
                                    "■  Portfolio balance and equity tracking\n"
                                    "■  Persistent state — resumes between sessions")},
            {tr("REAL-WORLD USAGE"), tr("■  Strategy testing: Validate a new strategy without risking capital\n"
                                        "■  Learning: Understand order types, market mechanics, P&L calculation\n"
                                        "■  Confidence building: Track your paper P&L before going live\n"
                                        "■  Backtesting comparison: Compare paper results with backtest predictions")},
            {tr("SKILL LEVELS"), tr("BEGINNER: Place market orders, understand fills, track basic P&L\n"
                                    "INTERMEDIATE: Use limit orders at key levels, implement stop-losses\n"
                                    "ADVANCED: Develop a consistent paper trading journal and strategy\n"
                                    "PRO: Use paper trading for A/B testing new strategies before live deployment")},
        });
}

QWidget* DocsScreen::page_algo_trading() {
    return make_page(
        tr("ALGO TRADING"), tr("Algorithmic strategy builder, scanner, and deployment"),
        {
            {tr("OVERVIEW"), tr("The Algo Trading screen provides a complete environment for building, testing, "
                                "and deploying algorithmic trading strategies. It includes a strategy builder, "
                                "market scanner, and deployment dashboard.")},
            {tr("KEY FEATURES"), tr("■  Strategy Builder — Define entry/exit rules, position sizing, risk limits\n"
                                    "■  Market Scanner — Scan markets for strategy-matching conditions\n"
                                    "■  Deployment Dashboard — Monitor live strategy performance\n"
                                    "■  Strategy Library — 50+ pre-built strategies as templates\n"
                                    "■  Risk Management — Max drawdown, position limits, daily loss limits\n"
                                    "■  Real-time monitoring with P&L and trade logs")},
            {tr("REAL-WORLD USAGE"), tr("■  Momentum strategies: Scan for breakouts, deploy auto-entry\n"
                                        "■  Mean reversion: Detect overbought/oversold conditions\n"
                                        "■  Pairs trading: Monitor spread and auto-execute when divergence detected\n"
                                        "■  Market making: Automated bid/ask placement with spread capture")},
            {tr("SKILL LEVELS"),
             tr("BEGINNER: Explore pre-built strategies, understand entry/exit logic\n"
                "INTERMEDIATE: Modify strategy parameters, run scanner on your watchlist\n"
                "ADVANCED: Build custom strategies, combine multiple signals, set risk limits\n"
                "PRO: Deploy multi-strategy portfolios, optimize parameters, run live with real capital")},
        });
}

QWidget* DocsScreen::page_backtesting() {
    return make_page(
        tr("BACKTESTING"), tr("Historical strategy testing with 6 providers and 50+ strategies"),
        {
            {tr("OVERVIEW"), tr("The Backtesting screen lets you test trading strategies against historical data "
                                "using multiple backtesting providers. Supports 9 commands, 50+ built-in strategies, "
                                "and detailed performance analytics.")},
            {tr("PROVIDERS"), tr("■  Backtrader — Python-based, full-featured backtesting engine\n"
                                 "■  Zipline — Quantopian's backtesting library\n"
                                 "■  VectorBT — Vectorized backtesting for high performance\n"
                                 "■  QuantConnect (Lean) — Cloud-based institutional backtesting\n"
                                 "■  Custom Python — Write your own backtesting scripts\n"
                                 "■  Built-in Engine — Simple built-in for quick tests")},
            {tr("KEY METRICS"), tr("■  Total Return, CAGR, Max Drawdown\n"
                                   "■  Sharpe Ratio, Sortino Ratio, Calmar Ratio\n"
                                   "■  Win Rate, Profit Factor, Average Win/Loss\n"
                                   "■  Trade count, Average holding period\n"
                                   "■  Equity curve, Drawdown chart, Trade distribution")},
            {tr("SKILL LEVELS"),
             tr("BEGINNER: Run a pre-built SMA crossover strategy on SPY, read the performance report\n"
                "INTERMEDIATE: Compare multiple strategies, optimize parameters, test on different assets\n"
                "ADVANCED: Build multi-asset strategies, walk-forward optimization, Monte Carlo simulation\n"
                "PRO: Custom backtesting engines, survivorship bias correction, transaction cost modeling")},
        });
}

// ============================================================================
// Research & Analytics
// ============================================================================

QWidget* DocsScreen::page_equity_research() {
    return make_page(
        tr("EQUITY RESEARCH"), tr("Comprehensive stock analysis and fundamental research"),
        {
            {tr("OVERVIEW"),
             tr("The Equity Research screen provides institutional-grade fundamental analysis tools "
                "including financial statements, valuation models, peer comparison, and analyst estimates.")},
            {tr("KEY FEATURES"), tr("■  Financial statements — Income, Balance Sheet, Cash Flow (quarterly/annual)\n"
                                    "■  Valuation models — DCF, comparable analysis, precedent transactions\n"
                                    "■  Peer comparison — Compare metrics across competitors\n"
                                    "■  Analyst estimates — Consensus EPS, revenue, price targets\n"
                                    "■  Technical overlays — Combine fundamental with technical analysis\n"
                                    "■  Export to report builder for presentation-ready output")},
            {tr("REAL-WORLD USAGE"), tr("■  Due diligence before investing in a new position\n"
                                        "■  Quarterly earnings analysis and comparison to estimates\n"
                                        "■  Sector analysis with peer group comparison\n"
                                        "■  Valuation-driven investment thesis development")},
            {tr("SKILL LEVELS"),
             tr("BEGINNER: Look up basic financials (revenue, earnings, P/E ratio) for a stock\n"
                "INTERMEDIATE: Compare peers, analyze quarterly trends, read cash flow statements\n"
                "ADVANCED: Build DCF models, identify valuation discrepancies, create investment theses\n"
                "PRO: Multi-factor screening + fundamental overlay, integrate with algo trading signals")},
        });
}

QWidget* DocsScreen::page_surface_analytics() {
    return make_page(
        tr("SURFACE ANALYTICS"), tr("35 financial surfaces across derivatives, fixed income, FX, credit, and more"),
        {
            {tr("OVERVIEW"), tr("Surface Analytics provides 3D visualization of financial surfaces including "
                                "volatility surfaces, yield curves, credit spreads, and more. Supports 35 surface "
                                "types across 7 asset classes with real-time and historical data.")},
            {tr("SURFACE CATEGORIES (35 TOTAL)"),
             tr("EQUITY DERIVATIVES (7): Vol surface, local vol, implied distribution, skew, "
                "term structure, risk reversal, butterfly spread\n\n"
                "FIXED INCOME (5): Yield curve, forward curve, swap spread, basis swap, "
                "inflation breakeven\n\n"
                "FX (5): Vol surface, risk reversal, butterfly, cross-currency basis, carry\n\n"
                "CREDIT (5): CDS spread, default probability, recovery rate, basis, "
                "capital structure\n\n"
                "COMMODITIES (5): Forward curve, vol surface, calendar spread, crack spread, "
                "basis\n\n"
                "RISK (4): Correlation, covariance, VaR, stress test\n\n"
                "MACRO (4): GDP growth, inflation, rates, employment")},
            {tr("REAL-WORLD USAGE"), tr("■  Options trading: Analyze vol surface for mispriced options\n"
                                        "■  Fixed income: Monitor yield curve shape for recession signals\n"
                                        "■  Risk management: Correlation and VaR surfaces for portfolio risk\n"
                                        "■  Relative value: Compare surfaces across maturities and strikes")},
            {tr("SKILL LEVELS"), tr("BEGINNER: View pre-built surfaces, understand 3D visualization axes\n"
                                    "INTERMEDIATE: Compare surface snapshots over time, identify trends\n"
                                    "ADVANCED: Use surface analytics for options trading edge, relative value\n"
                                    "PRO: Real-time surface monitoring, custom Databento feeds, surface arbitrage")},
        });
}

QWidget* DocsScreen::page_derivatives() {
    return make_page(
        tr("DERIVATIVES PRICING"), tr("Bond, equity option, FX option, swap, and CDS pricing"),
        {
            {tr("OVERVIEW"), tr("The Derivatives screen provides pricing models for major derivative types using "
                                "embedded Python analytics. Supports bonds, equity options, FX options, interest "
                                "rate swaps, and credit default swaps.")},
            {tr("SUPPORTED INSTRUMENTS"), tr("■  Bonds — Price, yield, duration, convexity, spread\n"
                                             "■  Equity Options — Black-Scholes, binomial, Monte Carlo pricing\n"
                                             "■  FX Options — Garman-Kohlhagen, barrier options, digital options\n"
                                             "■  Interest Rate Swaps — Fixed-float, basis swaps, forward starting\n"
                                             "■  Credit Default Swaps — Spread pricing, default probability, CVA")},
            {tr("REAL-WORLD USAGE"), tr("■  Options traders: Price options, calculate Greeks, find mispricing\n"
                                        "■  Fixed income: Price bonds, analyze yield sensitivity, duration matching\n"
                                        "■  Risk management: Calculate CVA, price CDS for credit hedging\n"
                                        "■  Treasury: Swap valuation for interest rate risk management")},
            {tr("SKILL LEVELS"), tr("BEGINNER: Price a simple call option with Black-Scholes, understand Greeks\n"
                                    "INTERMEDIATE: Compare pricing models, analyze sensitivity to volatility\n"
                                    "ADVANCED: Price exotic options, build structured products, calculate CVA\n"
                                    "PRO: Custom pricing models, volatility surface calibration, Greeks hedging")},
        });
}

QWidget* DocsScreen::page_portfolio() {
    return make_page(
        tr("PORTFOLIO"), tr("Multi-portfolio management with analytics and AI agent"),
        {
            {tr("OVERVIEW"), tr("The Portfolio screen supports multiple portfolios with holdings tracking, performance "
                                "analytics, sector heatmap, historical chart, blotter, and an AI agent panel for "
                                "intelligent portfolio insights.")},
            {tr("KEY FEATURES"), tr("■  Multiple named portfolios with separate tracking\n"
                                    "■  Holdings table — symbol, quantity, cost basis, market value, P&L\n"
                                    "■  Sector heatmap — Visual allocation by sector\n"
                                    "■  Performance chart — Time series of portfolio returns\n"
                                    "■  Trade blotter — Complete transaction history\n"
                                    "■  FFN analytics — Advanced factor analysis\n"
                                    "■  AI Agent — Natural language portfolio insights")},
            {tr("REAL-WORLD USAGE"), tr("■  Track your actual brokerage positions with real-time P&L\n"
                                        "■  Compare multiple portfolio strategies side by side\n"
                                        "■  Monitor sector allocation drift and rebalance\n"
                                        "■  Use AI agent to ask questions about your portfolio performance")},
            {tr("SKILL LEVELS"),
             tr("BEGINNER: Create a portfolio, add holdings, track basic P&L\n"
                "INTERMEDIATE: Use sector heatmap for diversification, review trade blotter\n"
                "ADVANCED: Multiple portfolios for different strategies, FFN analytics\n"
                "PRO: AI agent for portfolio optimization, factor exposure analysis, risk budgeting")},
        });
}

QWidget* DocsScreen::page_ma_analytics() {
    return make_page(
        tr("M&A ANALYTICS"), tr("Merger and acquisition analysis tools"),
        {
            {tr("OVERVIEW"), tr("The M&A Analytics screen provides tools for analyzing mergers, acquisitions, "
                                "and corporate actions including deal comparison, accretion/dilution analysis, "
                                "and precedent transaction databases.")},
            {tr("KEY FEATURES"), tr("■  Deal screen — Browse recent and historical M&A transactions\n"
                                    "■  Accretion/Dilution analysis — Model EPS impact of acquisitions\n"
                                    "■  Comparable transactions — Find similar deals for valuation\n"
                                    "■  Premium analysis — Analyze deal premiums by sector\n"
                                    "■  Timeline tracking — Monitor deal progress and regulatory approvals")},
            {tr("REAL-WORLD USAGE"), tr("■  Investment banking: Model potential acquisition targets\n"
                                        "■  Event-driven trading: Analyze merger arbitrage opportunities\n"
                                        "■  Due diligence: Research comparable transactions for valuation\n"
                                        "■  Risk assessment: Track regulatory risk for pending deals")},
            {tr("SKILL LEVELS"), tr("BEGINNER: Browse recent deals, understand deal structure basics\n"
                                    "INTERMEDIATE: Compare deal multiples, analyze premiums paid\n"
                                    "ADVANCED: Build accretion/dilution models, identify merger arb opportunities\n"
                                    "PRO: Custom deal screening, multi-factor M&A prediction models")},
        });
}

// ============================================================================
// AI & Quantitative
// ============================================================================

QWidget* DocsScreen::page_ai_quant_lab() {
    return make_page(
        tr("AI QUANT LAB"), tr("18-module quantitative research platform with ML, factor discovery, and RL trading"),
        {
            {tr("OVERVIEW"), tr("The AI Quant Lab is a comprehensive quantitative research environment with 18 modules "
                                "covering machine learning, factor discovery, high-frequency trading, reinforcement "
                                "learning, and advanced statistical methods.")},
            {tr("MODULES (18)"), tr("PREDICTION: ML price prediction, time series forecasting\n"
                                    "FACTORS: Factor discovery, alpha generation, smart beta\n"
                                    "RISK: ML risk models, tail risk, regime detection\n"
                                    "HFT: High-frequency signals, microstructure analysis\n"
                                    "RL TRADING: Reinforcement learning agents for trading\n"
                                    "NLP: Sentiment analysis, news parsing, earnings call analysis\n"
                                    "ALTERNATIVE DATA: Satellite, social media, web scraping\n"
                                    "PORTFOLIO: ML portfolio optimization, dynamic allocation\n"
                                    "EXECUTION: Smart order routing, execution quality analysis\n"
                                    "And 9 more covering derivatives, crypto, macro, and cross-asset")},
            {tr("REAL-WORLD USAGE"), tr("■  Quant research: Discover new alpha factors from alternative data\n"
                                        "■  ML models: Train prediction models on historical market data\n"
                                        "■  Risk management: Regime detection for dynamic hedging\n"
                                        "■  Strategy development: RL agents that learn optimal trading policies")},
            {tr("SKILL LEVELS"), tr("BEGINNER: Explore pre-built modules, run demo predictions on sample data\n"
                                    "INTERMEDIATE: Train basic ML models, test factor strategies on historical data\n"
                                    "ADVANCED: Custom factor research, multi-model ensembles, walk-forward validation\n"
                                    "PRO: Production RL trading agents, HFT signal research, custom model deployment")},
        });
}

QWidget* DocsScreen::page_ai_chat() {
    return make_page(
        tr("AI CHAT"), tr("AI-powered financial assistant"),
        {
            {tr("OVERVIEW"), tr("The AI Chat provides a conversational interface powered by large language models "
                                "for financial analysis, market insights, and terminal assistance.")},
            {tr("KEY FEATURES"), tr("■  Natural language market queries\n"
                                    "■  Financial analysis and interpretation\n"
                                    "■  Code generation for trading strategies\n"
                                    "■  Portfolio insights and suggestions\n"
                                    "■  Configurable LLM provider and model")},
            {tr("REAL-WORLD USAGE"), tr("■  Quick analysis: \"What's driving NVDA today?\"\n"
                                        "■  Strategy help: \"Write a mean reversion strategy for crypto\"\n"
                                        "■  Learning: \"Explain how VaR is calculated\"\n"
                                        "■  Data queries: \"Compare AAPL and MSFT P/E ratios over 5 years\"")},
            {tr("SKILL LEVELS"),
             tr("BEGINNER: Ask basic market questions, get explanations of financial concepts\n"
                "INTERMEDIATE: Generate analysis code, get portfolio recommendations\n"
                "ADVANCED: Complex multi-step analysis, custom strategy development\n"
                "PRO: Multi-agent workflows, MCP server integration, automated research pipelines")},
        });
}

QWidget* DocsScreen::page_agent_config() {
    return make_page(
        tr("AGENT STUDIO"), tr("Configure and manage AI agents, teams, and workflows"),
        {
            {tr("OVERVIEW"), tr("The Agent Studio (Agent Config) provides an 8-view interface for creating, "
                                "managing, and orchestrating AI agents with specialized financial capabilities.")},
            {tr("VIEWS (8)"), tr("■  Agents — Browse and manage configured agents\n"
                                 "■  Create — Build new agents with custom capabilities\n"
                                 "■  Teams — Organize agents into collaborative teams\n"
                                 "■  Workflows — Define multi-step agent pipelines\n"
                                 "■  Planner — AI task planning and decomposition\n"
                                 "■  Tools — Manage tools available to agents\n"
                                 "■  Chat — Interact with agents in conversation\n"
                                 "■  System — System-level agent configuration")},
            {tr("AGENT TYPES"), tr("■  Geopolitics Agent — Monitors global events, conflict analysis\n"
                                   "■  Economic Agent — Macro analysis, indicator tracking\n"
                                   "■  Hedge Fund Agent — Multi-strategy portfolio management\n"
                                   "■  Trader Agent — Technical analysis and trade execution\n"
                                   "■  Investor Agent — Long-term fundamental analysis\n"
                                   "■  Deep Agent — Complex multi-step research tasks")},
            {tr("SKILL LEVELS"),
             tr("BEGINNER: Browse pre-configured agents, chat with the general assistant\n"
                "INTERMEDIATE: Create custom agents with specific tool access\n"
                "ADVANCED: Build agent teams, define multi-step workflows\n"
                "PRO: Custom MCP server integration, multi-agent orchestration, automated research")},
        });
}

QWidget* DocsScreen::page_alpha_arena() {
    return make_page(
        tr("ALPHA ARENA"), tr("Competitive alpha research and strategy ranking"),
        {
            {tr("OVERVIEW"), tr("Alpha Arena is a competitive environment where trading strategies are ranked "
                                "by performance. Submit strategies, compare against peers, and discover new "
                                "alpha sources through community insights.")},
            {tr("KEY FEATURES"), tr("■  Strategy leaderboard with real-time rankings\n"
                                    "■  Performance metrics comparison (Sharpe, returns, drawdown)\n"
                                    "■  Strategy submission and backtesting\n"
                                    "■  Community insights and discussion\n"
                                    "■  Historical performance tracking")},
            {tr("REAL-WORLD USAGE"), tr("■  Benchmark your strategies against others\n"
                                        "■  Discover new trading ideas from top performers\n"
                                        "■  Competitive motivation to improve strategy quality\n"
                                        "■  Community learning through shared insights")},
            {tr("SKILL LEVELS"), tr("BEGINNER: Browse the leaderboard, study top strategies\n"
                                    "INTERMEDIATE: Submit your first strategy, analyze performance metrics\n"
                                    "ADVANCED: Optimize strategies for ranking, study factor exposures\n"
                                    "PRO: Multi-strategy submission, alpha decay analysis, ensemble approaches")},
        });
}

// ============================================================================
// Data Sources
// ============================================================================

} // namespace fincept::screens
