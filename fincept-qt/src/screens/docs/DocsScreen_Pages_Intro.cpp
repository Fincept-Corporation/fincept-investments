// src/screens/docs/DocsScreen_Pages_Intro.cpp
//
// Intro and orientation pages: Welcome, Getting Started, Keyboard Shortcuts,
// Dashboard, Markets, News, Watchlist.
//
// Part of the partial-class split of DocsScreen.cpp.

#include "screens/docs/DocsScreen.h"
#include "screens/docs/DocsScreen_internal.h"
#include "ui/theme/Theme.h"

#include <QCoreApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

namespace fincept::screens {

using fincept::screens::docs_internal::SCROLL_SS;

// ============================================================================
// Page builders — Welcome & Getting Started
// ============================================================================

QWidget* DocsScreen::page_welcome() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet(SCROLL_SS());

    auto* page = new QWidget(this);
    page->setStyleSheet(QString("background: %1;").arg(ui::colors::BG_BASE()));
    auto* vl = new QVBoxLayout(page);
    vl->setContentsMargins(20, 16, 20, 20);
    vl->setSpacing(12);

    vl->addWidget(make_heading(tr("FINCEPT TERMINAL  —  DOCUMENTATION")));
    // Version comes from the running application — the hardcoded "v4.0.0" was
    // already stale against the shipped 4.0.1.
    vl->addWidget(make_muted_label(
        tr("v%1  |  Native C++ Financial Intelligence Terminal").arg(QCoreApplication::applicationVersion())));

    auto* sep = new QFrame;
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet(QString("color: %1;").arg(ui::colors::BORDER_DIM()));
    vl->addWidget(sep);

    vl->addWidget(
        make_section_panel("■", tr("WHAT IS FINCEPT TERMINAL?"),
                           tr("Fincept Terminal is a professional-grade desktop financial intelligence platform "
                              "built in native C++ with Qt6. It provides institutional-quality market data, "
                              "trading capabilities, quantitative research tools, and AI-powered analytics — "
                              "all in a single unified terminal interface.\n\n"
                              "With 50+ integrated screens, real-time WebSocket feeds, embedded Python analytics, "
                              "and 16 broker integrations, Fincept Terminal bridges the gap between "
                              "retail and institutional tooling."),
                           ui::colors::AMBER));

    vl->addWidget(make_section_panel("■", tr("KEY CAPABILITIES"),
                                     tr("■  Real-time market data across equities, crypto, forex, commodities\n"
                                        "■  Multi-exchange crypto trading (Kraken, HyperLiquid, Binance, etc.)\n"
                                        "■  Paper trading engine with simulated order matching\n"
                                        "■  1300+ Python analytics scripts (equity, portfolio, derivatives)\n"
                                        "■  18-module QuantLib quantitative analysis suite (590+ endpoints)\n"
                                        "■  AI Quant Lab with ML models, factor discovery, HFT, RL trading\n"
                                        "■  Multiple AI agent frameworks (Geopolitics, Economic, Hedge Fund)\n"
                                        "■  Visual node editor for workflow automation\n"
                                        "■  DBnomics access to 100+ data providers, 500K+ datasets\n"
                                        "■  Surface analytics for derivatives, fixed income, credit, risk\n"
                                        "■  Report builder with drag-and-drop components\n"
                                        "■  Backtesting with 6 providers and 50+ strategies\n"
                                        "■  Algorithmic trading with strategy builder and scanner"),
                                     ui::colors::POSITIVE));

    vl->addWidget(make_section_panel("■", tr("WHO IS THIS FOR?"),
                                     tr("■  Retail traders seeking institutional-quality tools\n"
                                        "■  Quantitative researchers and data scientists\n"
                                        "■  Portfolio managers and financial analysts\n"
                                        "■  Finance students learning markets and analytics\n"
                                        "■  Algorithmic trading developers\n"
                                        "■  Crypto traders needing multi-exchange access\n"
                                        "■  Economics researchers working with global datasets"),
                                     ui::colors::INFO));

    vl->addWidget(make_section_panel("■", tr("NAVIGATION"),
                                     tr("Use the sidebar on the left to browse documentation by category. "
                                        "Each section covers a terminal feature with:\n\n"
                                        "■  Overview — what the feature does\n"
                                        "■  Key Features — detailed capabilities\n"
                                        "■  Real-World Usage — practical applications\n"
                                        "■  Skill Levels — Beginner through Pro guidance"),
                                     ui::colors::TEXT_SECONDARY));

    vl->addStretch();
    scroll->setWidget(page);
    return scroll;
}

QWidget* DocsScreen::page_getting_started() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet(SCROLL_SS());

    auto* page = new QWidget(this);
    page->setStyleSheet(QString("background: %1;").arg(ui::colors::BG_BASE()));
    auto* vl = new QVBoxLayout(page);
    vl->setContentsMargins(20, 16, 20, 20);
    vl->setSpacing(12);

    vl->addWidget(make_heading(tr("GETTING STARTED")));
    vl->addWidget(make_muted_label(tr("Your first steps with Fincept Terminal")));

    auto* sep = new QFrame;
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet(QString("color: %1;").arg(ui::colors::BORDER_DIM()));
    vl->addWidget(sep);

    vl->addWidget(make_section_panel("1", tr("LAUNCH"),
                                     tr("Fincept Terminal opens straight onto the Dashboard — your home base. "
                                        "There is no account and no sign-in: everything runs locally against "
                                        "your own data and your own API keys.\n\n"
                                        "■  Configure market data providers in Settings → Data Sources\n"
                                        "■  Configure an AI provider in Settings → LLM Config\n"
                                        "■  Connect a broker in Settings → Credentials"),
                                     ui::colors::POSITIVE));

    vl->addWidget(make_section_panel("2", tr("THE INTERFACE"),
                                     tr("The terminal has four main zones:\n\n"
                                        "TOOLBAR (top) — File, Navigate, View, Help menus + clock\n"
                                        "TAB BAR — 14 primary tabs: Dashboard, Markets, Crypto, Portfolio, etc.\n"
                                        "CONTENT AREA — The active screen fills this zone\n"
                                        "STATUS BAR (bottom) — Version, market indicators, connection status\n\n"
                                        "Use the Navigate menu (in toolbar) to access 30+ additional screens "
                                        "organized by category: Markets & Data, Trading, Research, Tools, etc."),
                                     ui::colors::INFO));

    vl->addWidget(make_section_panel("3", tr("KEYBOARD SHORTCUTS"),
                                     tr("F11  — Toggle fullscreen\n"
                                        "F10  — Focus mode (hide tab/status bars for maximum screen space)\n"
                                        "F5   — Refresh current screen\n"
                                        "Ctrl+P — Take screenshot (saved to home directory)"),
                                     ui::colors::AMBER));

    vl->addWidget(make_section_panel("4", tr("SUBSCRIPTION PLANS"),
                                     tr("Fincept Terminal offers tiered access:\n\n"
                                        "■  FREE — Basic market data, limited screens, paper trading\n"
                                        "■  PRO — Full market data, all screens, real trading, AI chat\n"
                                        "■  ENTERPRISE — Everything + API access, priority support\n\n"
                                        "Manage your plan from Settings or the Pricing screen."),
                                     ui::colors::AMBER));

    vl->addWidget(
        make_skill_panel(tr("Explore the Dashboard, set up a watchlist, browse market data"),
                         tr("Configure broker connections, set up paper trading, explore analytics"),
                         tr("Deploy algo strategies, use QuantLib suite, build custom workflows"),
                         tr("Multi-agent AI systems, custom MCP servers, HFT backtesting, node editor automation")));

    vl->addStretch();
    scroll->setWidget(page);
    return scroll;
}

QWidget* DocsScreen::page_keyboard_shortcuts() {
    return make_page(
        tr("KEYBOARD SHORTCUTS"), tr("Global shortcuts and navigation keys"),
        {
            {tr("GLOBAL SHORTCUTS"), tr("F11  — Toggle fullscreen mode\n"
                                        "F10  — Toggle focus mode (hides tab bar and status bar)\n"
                                        "F5   — Refresh the current screen data\n"
                                        "Ctrl+P — Capture screenshot (saved to ~/FinceptScreenshot_*.png)")},
            {tr("FILE MENU"), tr("New Workspace — Create a fresh workspace layout\n"
                                 "Open Workspace — Load a saved workspace\n"
                                 "Save Workspace — Persist current layout\n"
                                 "Import Data — Import external data files\n"
                                 "Export Data — Export current view data\n"
                                 "Refresh All — Refresh all active data feeds")},
            {tr("NAVIGATE MENU"), tr("Access 30+ screens organized in sub-menus:\n"
                                     "■  Markets & Data — Screener, Economics, DBnomics, AkShare, Gov Data\n"
                                     "■  Trading & Portfolio — Equity Trading, Derivatives, Watchlist\n"
                                     "■  Research — Equity Research, M&A, Geopolitics, Surface Analytics\n"
                                     "■  Tools — Report Builder, Node Editor, Code Editor, Excel, Notes")},
            {tr("VIEW MENU"), tr("Fullscreen (F11) — Use full monitor space\n"
                                 "Focus Mode (F10) — Hide chrome for maximum content area\n"
                                 "Refresh (F5) — Reload current screen\n"
                                 "Screenshot (Ctrl+P) — Capture to file")},
        });
}

// ============================================================================
// Core Screens
// ============================================================================

QWidget* DocsScreen::page_dashboard() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet(SCROLL_SS());

    auto* page = new QWidget(this);
    page->setStyleSheet(QString("background: %1;").arg(ui::colors::BG_BASE()));
    auto* vl = new QVBoxLayout(page);
    vl->setContentsMargins(20, 16, 20, 20);
    vl->setSpacing(12);

    vl->addWidget(make_heading(tr("DASHBOARD")));
    vl->addWidget(make_muted_label(tr("Your home base — customizable widget grid with live market data")));

    auto* sep = new QFrame;
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet(QString("color: %1;").arg(ui::colors::BORDER_DIM()));
    vl->addWidget(sep);

    vl->addWidget(make_section_panel(
        "■", tr("OVERVIEW"),
        tr("The Dashboard is your primary workspace. It features a draggable widget grid "
           "where you can arrange market widgets, a scrolling ticker bar showing live prices, "
           "a market pulse panel with sector performance, and a status bar showing connection state."),
        ui::colors::AMBER));

    vl->addWidget(make_section_panel("■", tr("AVAILABLE WIDGETS"),
                                     tr("■  Stock Quote — Real-time price, change, volume for any symbol\n"
                                        "■  Watchlist — Your tracked symbols with live updates\n"
                                        "■  Top Movers — Biggest gainers and losers of the session\n"
                                        "■  Market Sentiment — Bull/bear indicators and fear/greed index\n"
                                        "■  News — Latest headlines with sentiment tagging\n"
                                        "■  Economic Calendar — Upcoming economic events and releases\n"
                                        "■  Sector Heatmap — Visual sector performance map\n"
                                        "■  Performance — Portfolio return tracking\n"
                                        "■  Risk Metrics — VaR, Sharpe, beta, drawdown indicators\n"
                                        "■  Screener — Quick stock screener with filters\n"
                                        "■  Quote Table — Multi-symbol comparison table\n"
                                        "■  Quick Trade — One-click trade entry\n"
                                        "■  Indices — Major index tracking (S&P 500, NASDAQ, DOW)\n"
                                        "■  Forex — Currency pair rates\n"
                                        "■  Crypto — Top cryptocurrency prices\n"
                                        "■  Commodities — Gold, oil, silver, natural gas\n"
                                        "■  Portfolio Summary — Holdings overview with allocation"),
                                     ui::colors::POSITIVE));

    vl->addWidget(
        make_section_panel("■", tr("REAL-WORLD USAGE"),
                           tr("■  Morning routine: Check top movers, review overnight news, scan economic calendar\n"
                              "■  Active trading: Pin stock quote + quick trade widgets, monitor watchlist\n"
                              "■  Portfolio management: Use portfolio summary + risk metrics + performance\n"
                              "■  Sector rotation: Combine sector heatmap + top movers + indices"),
                           ui::colors::INFO));

    vl->addWidget(make_skill_panel(
        tr("Start with default layout. Add a Stock Quote widget for a symbol you follow. Watch the ticker bar."),
        tr("Customize your grid layout. Add multiple watchlists for different sectors. Use the market pulse panel."),
        tr("Build specialized layouts for different strategies (day trading vs swing). Use risk metrics + performance "
           "together."),
        tr("Create multi-monitor layouts. Combine dashboard with algo trading feeds. Use economic calendar for "
           "event-driven setups.")));

    vl->addStretch();
    scroll->setWidget(page);
    return scroll;
}

QWidget* DocsScreen::page_markets() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet(SCROLL_SS());

    auto* page = new QWidget(this);
    page->setStyleSheet(QString("background: %1;").arg(ui::colors::BG_BASE()));
    auto* vl = new QVBoxLayout(page);
    vl->setContentsMargins(20, 16, 20, 20);
    vl->setSpacing(12);

    vl->addWidget(make_heading(tr("MARKETS")));
    vl->addWidget(make_muted_label(tr("Global and regional market overview with auto-refresh")));

    auto* sep = new QFrame;
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet(QString("color: %1;").arg(ui::colors::BORDER_DIM()));
    vl->addWidget(sep);

    vl->addWidget(make_section_panel("■", tr("OVERVIEW"),
                                     tr("The Markets screen provides a comprehensive view of global markets organized "
                                        "by region. It displays indices, equities, forex, commodities, and crypto in "
                                        "panel-based layouts with configurable auto-refresh (default: 10 minutes)."),
                                     ui::colors::AMBER));

    vl->addWidget(make_section_panel("■", tr("KEY FEATURES"),
                                     tr("■  Regional panels — US, Europe, Asia, Global\n"
                                        "■  Auto-refresh with configurable interval\n"
                                        "■  Price, change, % change with color coding (green/red)\n"
                                        "■  Market hours and session status indicators\n"
                                        "■  Sort by name, price, change, or volume\n"
                                        "■  Click any instrument to navigate to detailed view"),
                                     ui::colors::POSITIVE));

    vl->addWidget(make_section_panel("■", tr("REAL-WORLD USAGE"),
                                     tr("■  Pre-market: Scan global indices to gauge overnight sentiment\n"
                                        "■  Cross-market analysis: Compare US vs Europe vs Asia performance\n"
                                        "■  Commodity tracking: Monitor gold, oil, natural gas alongside equities\n"
                                        "■  FX correlation: Watch currency pairs relative to equity moves"),
                                     ui::colors::INFO));

    vl->addWidget(
        make_skill_panel(tr("Browse the default view. Learn to read green (up) and red (down) color coding."),
                         tr("Compare multiple regions. Notice correlations between indices and currencies."),
                         tr("Use markets as a macro overlay for your trading decisions. Track relative strength."),
                         tr("Combine with economics data and geopolitics for macro-driven trading strategies.")));

    vl->addStretch();
    scroll->setWidget(page);
    return scroll;
}

QWidget* DocsScreen::page_news() {
    return make_page(
        tr("NEWS"), tr("Real-time news with clustering, sentiment analysis, and deviation monitoring"),
        {
            {tr("OVERVIEW"), tr("The News screen aggregates financial news from multiple sources with intelligent "
                                "clustering, sentiment analysis, and customizable keyword monitors that alert you "
                                "to significant deviations from baseline coverage patterns.")},
            {tr("KEY FEATURES"), tr("■  Category filtering — All, Markets, Economy, Tech, Crypto, Geopolitics\n"
                                    "■  Time range selection — 1H, 4H, 24H, 7D, 30D\n"
                                    "■  Sort by recency, relevance, or sentiment score\n"
                                    "■  News clustering — related articles grouped together\n"
                                    "■  Sentiment analysis — bullish/bearish/neutral tagging\n"
                                    "■  Keyword monitors — set alerts for specific topics\n"
                                    "■  Deviation detection — flags unusual coverage patterns\n"
                                    "■  Full article reader with clean formatting")},
            {tr("REAL-WORLD USAGE"), tr("■  Event-driven trading: Monitor breaking news for tradable events\n"
                                        "■  Sentiment tracking: Gauge market mood before making positions\n"
                                        "■  Due diligence: Research a company before investing\n"
                                        "■  Macro awareness: Track central bank news, policy changes")},
            {tr("SKILL LEVELS"), tr("BEGINNER: Browse headlines, filter by category, read articles\n"
                                    "INTERMEDIATE: Set up keyword monitors for your positions, use sentiment filters\n"
                                    "ADVANCED: Use deviation alerts to catch unusual news patterns early\n"
                                    "PRO: Combine news sentiment with quantitative signals for alpha generation")},
        });
}

QWidget* DocsScreen::page_watchlist() {
    return make_page(
        tr("WATCHLIST"), tr("Track your favorite instruments with live quotes"),
        {
            {tr("OVERVIEW"), tr("The Watchlist screen lets you create and manage multiple watchlists, each with "
                                "live price quotes, change indicators, and quick access to detailed analysis.")},
            {tr("KEY FEATURES"), tr("■  Multiple named watchlists (Favorites, Day Trades, Swing, etc.)\n"
                                    "■  Live price, change, % change, volume\n"
                                    "■  Add/remove symbols easily\n"
                                    "■  Color-coded performance (green/red)\n"
                                    "■  Click to navigate to detailed chart/analysis\n"
                                    "■  Persistent storage — watchlists saved between sessions")},
            {tr("REAL-WORLD USAGE"),
             tr("■  Organize by strategy: separate watchlists for day trades, swing trades, long-term\n"
                "■  Sector watchlists: group symbols by industry for sector rotation\n"
                "■  Earnings watchlist: track companies reporting this week\n"
                "■  Correlation pairs: group correlated instruments together")},
            {tr("SKILL LEVELS"), tr("BEGINNER: Create your first watchlist with 5-10 symbols you know\n"
                                    "INTERMEDIATE: Multiple watchlists organized by strategy or sector\n"
                                    "ADVANCED: Use watchlists as a pre-screened universe for your scanning\n"
                                    "PRO: Dynamic watchlists driven by screener output and quantitative filters")},
        });
}

// ============================================================================
// Trading
// ============================================================================

} // namespace fincept::screens
