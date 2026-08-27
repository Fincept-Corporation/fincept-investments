// src/screens/docs/DocsScreen_Pages_Data.cpp
//
// Data-source, geopolitics, and tools/settings docs pages: DBnomics,
// Economics, AkShare, Gov Data, Geopolitics, Maritime, Polymarket,
// Alt Investments, Report Builder, Node Editor, Code Editor, Excel, Notes,
// MCP Servers, Data Mapping, Settings, Profile.
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

QWidget* DocsScreen::page_dbnomics() {
    return make_page(
        tr("DBNOMICS"), tr("Access 100+ data providers with 500K+ economic datasets"),
        {
            {tr("OVERVIEW"), tr("DBnomics provides access to the world's largest open economic database. Browse "
                                "100+ data providers (IMF, World Bank, OECD, ECB, Fed, etc.) with 500K+ datasets "
                                "covering macroeconomics, finance, demographics, and trade.")},
            {tr("KEY FEATURES"), tr("■  Provider browser — 100+ statistical agencies and central banks\n"
                                    "■  Dataset search — Full-text search across 500K+ datasets\n"
                                    "■  Series explorer — Browse individual time series with metadata\n"
                                    "■  Single view — Detailed chart + data table for one series\n"
                                    "■  Comparison view — 2x2 chart grid comparing multiple series\n"
                                    "■  CSV export — Download any series for external analysis\n"
                                    "■  Loading animations and progressive data loading")},
            {tr("REAL-WORLD USAGE"), tr("■  Macro research: GDP growth, inflation, employment data by country\n"
                                        "■  Central bank analysis: Interest rates, money supply, balance sheets\n"
                                        "■  Trade analysis: Import/export data, trade balances, tariffs\n"
                                        "■  Demographics: Population, labor force, migration data")},
            {tr("SKILL LEVELS"), tr("BEGINNER: Browse providers, look up US GDP or inflation data\n"
                                    "INTERMEDIATE: Compare series across countries, export data for analysis\n"
                                    "ADVANCED: Build macro dashboards, track leading indicators\n"
                                    "PRO: Integrate DBnomics data into quantitative macro trading models")},
        });
}

QWidget* DocsScreen::page_economics() {
    return make_page(
        tr("ECONOMICS"), tr("Macroeconomic data and analysis tools"),
        {
            {tr("OVERVIEW"), tr("The Economics screen provides comprehensive macroeconomic data visualization "
                                "and analysis tools for tracking economic indicators, central bank policies, "
                                "and global economic trends.")},
            {tr("KEY FEATURES"), tr("■  Economic indicators dashboard — GDP, CPI, unemployment, PMI\n"
                                    "■  Central bank tracker — Fed, ECB, BOJ, BOE rate decisions\n"
                                    "■  Economic calendar — Upcoming releases with forecasts\n"
                                    "■  Historical comparisons — Indicator trends over time\n"
                                    "■  Country comparison — Side-by-side economic profiles")},
            {tr("REAL-WORLD USAGE"), tr("■  Macro trading: Position ahead of rate decisions\n"
                                        "■  Risk management: Monitor recession indicators\n"
                                        "■  FX trading: Track rate differentials for carry trades\n"
                                        "■  Asset allocation: Shift sectors based on economic cycle")},
            {tr("SKILL LEVELS"), tr("BEGINNER: Check upcoming economic events, understand key indicators\n"
                                    "INTERMEDIATE: Track leading vs lagging indicators, spot economic trends\n"
                                    "ADVANCED: Build economic models, forecast indicator releases\n"
                                    "PRO: Multi-factor macro models, economic surprise index trading")},
        });
}

QWidget* DocsScreen::page_akshare() {
    return make_page(tr("AKSHARE DATA"), tr("Chinese and Asian market data via AkShare"),
                     {
                         {tr("OVERVIEW"),
                          tr("AkShare provides access to Chinese financial market data including A-shares, "
                             "Hong Kong stocks, Chinese futures, and macro data from Chinese statistical agencies.")},
                         {tr("KEY FEATURES"), tr("■  A-Share market data — SSE, SZSE listed companies\n"
                                                 "■  Hong Kong market data — HKEX listings\n"
                                                 "■  Chinese futures — commodity and financial futures\n"
                                                 "■  Macro data — NBS statistics, PBOC data\n"
                                                 "■  Fund data — Mutual funds, ETFs listed in China")},
                         {tr("REAL-WORLD USAGE"), tr("■  China market exposure: Research A-share opportunities\n"
                                                     "■  Emerging market analysis: Compare Chinese vs global markets\n"
                                                     "■  Commodity trading: Track Chinese commodity demand\n"
                                                     "■  Macro research: Monitor Chinese economic indicators")},
                         {tr("SKILL LEVELS"), tr("BEGINNER: Browse Chinese market indices and major stocks\n"
                                                 "INTERMEDIATE: Compare A-share sectors, track PBOC policy\n"
                                                 "ADVANCED: Chinese commodity-equity correlation analysis\n"
                                                 "PRO: Cross-market arbitrage between A-shares, H-shares, and ADRs")},
                     });
}

QWidget* DocsScreen::page_gov_data() {
    return make_page(
        tr("GOVERNMENT DATA"), tr("Official government statistical data"),
        {
            {tr("OVERVIEW"), tr("Access official government data from statistical agencies worldwide including "
                                "census data, economic surveys, trade statistics, and regulatory filings.")},
            {tr("KEY FEATURES"), tr("■  Multi-country government data sources\n"
                                    "■  Census and demographic data\n"
                                    "■  Trade and commerce statistics\n"
                                    "■  Regulatory filings and disclosures\n"
                                    "■  Historical data series with long time horizons")},
            {tr("REAL-WORLD USAGE"), tr("■  Research: Access primary source economic data\n"
                                        "■  Policy analysis: Track government spending and fiscal policy\n"
                                        "■  Demographics: Population and labor force trends\n"
                                        "■  Trade analysis: Import/export flows by country and sector")},
            {tr("SKILL LEVELS"), tr("BEGINNER: Browse available datasets, download basic reports\n"
                                    "INTERMEDIATE: Cross-reference government data with market data\n"
                                    "ADVANCED: Build predictive models using government leading indicators\n"
                                    "PRO: Automated policy tracking, fiscal surprise detection")},
        });
}

// ============================================================================
// Geopolitics & Alt
// ============================================================================

QWidget* DocsScreen::page_geopolitics() {
    return make_page(
        tr("GEOPOLITICS"), tr("Conflict monitoring, HDX data, trade analysis, and relationship mapping"),
        {
            {tr("OVERVIEW"), tr("The Geopolitics screen provides a comprehensive view of global geopolitical events "
                                "with conflict monitoring, humanitarian data (HDX), trade flow analysis, and entity "
                                "relationship mapping for understanding geopolitical dynamics.")},
            {tr("KEY FEATURES"), tr("■  Conflict monitor — Active global conflicts with severity tracking\n"
                                    "■  HDX integration — Humanitarian Data Exchange for crisis data\n"
                                    "■  Trade analysis — Sanctions, tariffs, trade flow disruptions\n"
                                    "■  Relationship mapping — Entity relationships and influence networks\n"
                                    "■  Risk scoring — Country-level geopolitical risk assessments\n"
                                    "■  AI agent — Geopolitics-specialized analysis agent")},
            {tr("REAL-WORLD USAGE"), tr("■  Risk management: Monitor geopolitical risks to your portfolio\n"
                                        "■  Commodity trading: Track supply disruptions from conflicts\n"
                                        "■  FX trading: Geopolitical risk premium in currency pricing\n"
                                        "■  ESG investing: Humanitarian impact assessment")},
            {tr("SKILL LEVELS"), tr("BEGINNER: Monitor the conflict dashboard, understand risk scores\n"
                                    "INTERMEDIATE: Correlate geopolitical events with market movements\n"
                                    "ADVANCED: Build geopolitical risk models, trade around events\n"
                                    "PRO: Multi-factor geopolitical alpha, automated event-driven trading")},
        });
}

QWidget* DocsScreen::page_maritime() {
    return make_page(
        tr("MARITIME"), tr("Maritime shipping and trade flow data"),
        {
            {tr("OVERVIEW"), tr("The Maritime screen provides shipping and trade flow data for monitoring global "
                                "supply chains, commodity transport, and maritime trade patterns.")},
            {tr("KEY FEATURES"), tr("■  Vessel tracking — AIS data for major shipping routes\n"
                                    "■  Port analytics — Throughput, congestion, wait times\n"
                                    "■  Trade flows — Commodity shipping volumes by route\n"
                                    "■  Supply chain indicators — Shipping cost indices\n"
                                    "■  Geopolitical overlay — Chokepoint risk assessment")},
            {tr("REAL-WORLD USAGE"), tr("■  Commodity trading: Track oil tanker movements for supply signals\n"
                                        "■  Supply chain: Monitor port congestion for inflation signals\n"
                                        "■  Shipping stocks: Analyze Baltic Dry Index and freight rates\n"
                                        "■  Geopolitical risk: Chokepoint monitoring (Suez, Strait of Hormuz)")},
            {tr("SKILL LEVELS"), tr("BEGINNER: Browse shipping routes, understand trade flow basics\n"
                                    "INTERMEDIATE: Track commodity shipments, correlate with futures prices\n"
                                    "ADVANCED: Build supply chain disruption models\n"
                                    "PRO: Satellite-verified shipping data for alternative data alpha")},
        });
}

QWidget* DocsScreen::page_polymarket() {
    return make_page(
        tr("POLYMARKET"), tr("Prediction markets and event probability trading"),
        {
            {tr("OVERVIEW"), tr("The Polymarket screen provides access to prediction market data, allowing you to "
                                "track event probabilities and trade on real-world outcomes.")},
            {tr("KEY FEATURES"), tr("■  Event browser — Political, economic, sports, crypto events\n"
                                    "■  Probability charts — Historical probability movement\n"
                                    "■  Volume and liquidity data\n"
                                    "■  Category filtering and search\n"
                                    "■  Correlation with traditional markets")},
            {tr("REAL-WORLD USAGE"), tr("■  Election trading: Track political event probabilities\n"
                                        "■  Hedging: Use prediction markets to hedge event risk\n"
                                        "■  Sentiment gauge: Market-implied probabilities as sentiment\n"
                                        "■  Research: Compare prediction market accuracy with polls")},
            {tr("SKILL LEVELS"), tr("BEGINNER: Browse events, understand probability pricing\n"
                                    "INTERMEDIATE: Track probability changes, identify momentum\n"
                                    "ADVANCED: Arbitrage between prediction markets and traditional assets\n"
                                    "PRO: Build prediction market-enhanced trading models")},
        });
}

QWidget* DocsScreen::page_alt_investments() {
    return make_page(
        tr("ALTERNATIVE INVESTMENTS"), tr("Private equity, hedge funds, real assets, and alternatives data"),
        {
            {tr("OVERVIEW"), tr("The Alternative Investments screen covers non-traditional asset classes including "
                                "private equity, hedge funds, real estate, commodities, and other alternatives.")},
            {tr("KEY FEATURES"), tr("■  PE/VC deal flow and fund performance\n"
                                    "■  Hedge fund strategy tracking\n"
                                    "■  Real estate market data (REITs, cap rates)\n"
                                    "■  Commodity index tracking\n"
                                    "■  Correlation analysis with traditional assets")},
            {tr("REAL-WORLD USAGE"), tr("■  Portfolio diversification with alternative assets\n"
                                        "■  Hedge fund replication strategy development\n"
                                        "■  Real asset allocation for inflation hedging\n"
                                        "■  Private market opportunity screening")},
            {tr("SKILL LEVELS"), tr("BEGINNER: Understand alternative asset classes and their role\n"
                                    "INTERMEDIATE: Analyze REIT performance, commodity correlations\n"
                                    "ADVANCED: Factor-based alternative investment allocation\n"
                                    "PRO: Private market deal analysis, hedge fund replication, liquid alternatives")},
        });
}

// ============================================================================
// Tools
// ============================================================================

QWidget* DocsScreen::page_report_builder() {
    return make_page(tr("REPORT BUILDER"), tr("Drag-and-drop financial report creation"),
                     {
                         {tr("OVERVIEW"), tr("The Report Builder lets you create professional financial reports with a "
                                             "drag-and-drop interface. Combine charts, tables, text, and data from any "
                                             "terminal screen into presentation-ready documents.")},
                         {tr("KEY FEATURES"), tr("■  Component toolbar — Charts, tables, text blocks, images, headers\n"
                                                 "■  Drag-and-drop canvas with grid snapping\n"
                                                 "■  Properties panel — Customize colors, fonts, data sources\n"
                                                 "■  Multi-page support with page navigation\n"
                                                 "■  Export to PDF, PNG, or print directly\n"
                                                 "■  Template library for common report types")},
                         {tr("REAL-WORLD USAGE"), tr("■  Client reports: Weekly market summary for advisory clients\n"
                                                     "■  Investment memos: Structured investment thesis documents\n"
                                                     "■  Risk reports: Portfolio risk metrics for compliance\n"
                                                     "■  Research reports: Published equity research format")},
                         {tr("SKILL LEVELS"), tr("BEGINNER: Create a simple one-page report with a chart and text\n"
                                                 "INTERMEDIATE: Multi-page reports with tables and data binding\n"
                                                 "ADVANCED: Templated report generation, automated weekly reports\n"
                                                 "PRO: Branded client deliverables, automated report pipelines")},
                     });
}

QWidget* DocsScreen::page_node_editor() {
    return make_page(
        tr("NODE EDITOR"), tr("Visual workflow automation with drag-and-drop nodes"),
        {
            {tr("OVERVIEW"), tr("The Node Editor provides a visual programming environment for building "
                                "data processing and trading workflows by connecting nodes.")},
            {tr("KEY FEATURES"), tr("■  Drag-and-drop node placement\n"
                                    "■  Data source nodes — Market data, news, economic indicators\n"
                                    "■  Processing nodes — Filters, transformations, calculations\n"
                                    "■  Action nodes — Alerts, trades, report generation\n"
                                    "■  Workflow persistence and sharing\n"
                                    "■  Real-time execution monitoring")},
            {tr("REAL-WORLD USAGE"), tr("■  Alert systems: Price crosses MA → send notification\n"
                                        "■  Data pipelines: Fetch → filter → analyze → report\n"
                                        "■  Trading automation: Signal → validation → order → monitor\n"
                                        "■  Research workflows: Data collection → processing → visualization")},
            {tr("SKILL LEVELS"), tr("BEGINNER: Connect a data source to a simple alert node\n"
                                    "INTERMEDIATE: Multi-step workflows with conditional branching\n"
                                    "ADVANCED: Complex trading workflows with risk checks\n"
                                    "PRO: Production automation pipelines, multi-feed aggregation")},
        });
}

QWidget* DocsScreen::page_code_editor() {
    return make_page(tr("CODE EDITOR"), tr("Built-in code editor for trading strategies and scripts"),
                     {
                         {tr("OVERVIEW"), tr("The Code Editor provides a built-in development environment for writing "
                                             "and testing Python trading strategies and analytics scripts.")},
                         {tr("KEY FEATURES"), tr("■  Syntax highlighting for Python\n"
                                                 "■  Script execution with output panel\n"
                                                 "■  Access to terminal data via APIs\n"
                                                 "■  Template library for common patterns\n"
                                                 "■  Integration with backtesting engine")},
                         {tr("REAL-WORLD USAGE"), tr("■  Strategy development: Write and test Python strategies\n"
                                                     "■  Custom analytics: Build bespoke analysis scripts\n"
                                                     "■  Data processing: Transform and analyze raw data\n"
                                                     "■  Automation: Create scheduled data processing jobs")},
                         {tr("SKILL LEVELS"), tr("BEGINNER: Run example scripts, modify parameters\n"
                                                 "INTERMEDIATE: Write simple strategies from templates\n"
                                                 "ADVANCED: Custom analytics with pandas, numpy, scipy\n"
                                                 "PRO: Full strategy development with custom backtesting integration")},
                     });
}

QWidget* DocsScreen::page_excel() {
    return make_page(tr("EXCEL"), tr("Spreadsheet interface for financial analysis"),
                     {
                         {tr("OVERVIEW"), tr("The Excel screen provides a spreadsheet-style interface for financial "
                                             "analysis with formula support, data import/export, and charting.")},
                         {tr("KEY FEATURES"), tr("■  Spreadsheet grid with formula support\n"
                                                 "■  Data import from terminal screens\n"
                                                 "■  Financial functions (NPV, IRR, XIRR, etc.)\n"
                                                 "■  Chart creation from spreadsheet data\n"
                                                 "■  CSV import/export compatibility")},
                         {tr("REAL-WORLD USAGE"), tr("■  Financial modeling: Build DCF models in a familiar interface\n"
                                                     "■  Data analysis: Import market data for custom calculations\n"
                                                     "■  Reporting: Create tabular reports with formulas\n"
                                                     "■  Comparison: Side-by-side metric comparison tables")},
                         {tr("SKILL LEVELS"), tr("BEGINNER: Enter data, use basic formulas (SUM, AVG)\n"
                                                 "INTERMEDIATE: Financial functions, data import from terminal\n"
                                                 "ADVANCED: Complex financial models with linked cells\n"
                                                 "PRO: Automated model updates with live data feeds")},
                     });
}

QWidget* DocsScreen::page_notes() {
    return make_page(
        tr("NOTES"), tr("Persistent note-taking for research and trade journals"),
        {
            {tr("OVERVIEW"), tr("The Notes screen provides a persistent note-taking environment for maintaining "
                                "research notes, trade journals, and analysis records.")},
            {tr("KEY FEATURES"), tr("■  Rich text editing\n"
                                    "■  Multiple note organization\n"
                                    "■  Persistent storage between sessions\n"
                                    "■  Search across all notes\n"
                                    "■  Timestamp tracking for each note")},
            {tr("REAL-WORLD USAGE"), tr("■  Trade journal: Record entry/exit rationale for every trade\n"
                                        "■  Research notes: Document investment theses and analysis\n"
                                        "■  Meeting notes: Record calls with management or analysts\n"
                                        "■  Watchlist notes: Add context to symbols you're tracking")},
            {tr("SKILL LEVELS"), tr("BEGINNER: Create your first trade journal entry\n"
                                    "INTERMEDIATE: Organized note system by category (trades, research, ideas)\n"
                                    "ADVANCED: Systematic trade journaling with performance review\n"
                                    "PRO: Integrated research workflow with notes linked to positions")},
        });
}

QWidget* DocsScreen::page_mcp_servers() {
    return make_page(
        tr("MCP SERVERS"), tr("Model Context Protocol server management"),
        {
            {tr("OVERVIEW"), tr("The MCP Servers screen manages Model Context Protocol servers that extend "
                                "AI agent capabilities with external tool access and data sources.")},
            {tr("KEY FEATURES"), tr("■  Server registration and management\n"
                                    "■  Tool discovery and configuration\n"
                                    "■  Connection status monitoring\n"
                                    "■  Custom server integration\n"
                                    "■  Security and access control")},
            {tr("REAL-WORLD USAGE"), tr("■  Extend AI agents with custom data sources\n"
                                        "■  Connect to proprietary APIs and databases\n"
                                        "■  Add specialized tools for domain-specific analysis\n"
                                        "■  Build custom research pipelines with external tools")},
            {tr("SKILL LEVELS"), tr("BEGINNER: Browse available MCP servers, understand the concept\n"
                                    "INTERMEDIATE: Connect pre-configured MCP servers to agents\n"
                                    "ADVANCED: Deploy custom MCP servers for proprietary data\n"
                                    "PRO: Build production MCP infrastructure, multi-server orchestration")},
        });
}

QWidget* DocsScreen::page_data_mapping() {
    return make_page(
        tr("DATA MAPPING"), tr("Map and configure external data source connections"),
        {
            {tr("OVERVIEW"), tr("The Data Mapping screen lets you configure connections between external data "
                                "sources and terminal screens, mapping fields and transforming data formats.")},
            {tr("KEY FEATURES"), tr("■  Data source configuration\n"
                                    "■  Field mapping and transformation\n"
                                    "■  Schema validation\n"
                                    "■  Connection testing\n"
                                    "■  Scheduled refresh configuration")},
            {tr("REAL-WORLD USAGE"), tr("■  Connect proprietary data feeds to terminal widgets\n"
                                        "■  Map custom API responses to standard formats\n"
                                        "■  Integrate internal databases with terminal screens\n"
                                        "■  Configure automated data refresh schedules")},
            {tr("SKILL LEVELS"), tr("BEGINNER: Browse configured data sources\n"
                                    "INTERMEDIATE: Add new data source connections\n"
                                    "ADVANCED: Custom field mapping and transformation logic\n"
                                    "PRO: Complex multi-source data pipelines with validation")},
        });
}

// ============================================================================
// Community
// ============================================================================

QWidget* DocsScreen::page_settings() {
    return make_page(
        tr("SETTINGS"), tr("Application configuration and preferences"),
        {
            {tr("OVERVIEW"), tr("The Settings screen provides comprehensive application configuration "
                                "including display preferences, data sources, LLM configuration, MCP servers, "
                                "and integration settings.")},
            {tr("KEY FEATURES"), tr("■  Display preferences — Theme, font size, layout options\n"
                                    "■  Data configuration — API keys, refresh intervals, cache settings\n"
                                    "■  LLM Configuration — Model provider, API key, parameters\n"
                                    "■  MCP Servers — Server management and configuration\n"
                                    "■  Notification preferences\n"
                                    "■  Keyboard shortcut customization")},
            {tr("SECTIONS"), tr("■  General — Language, timezone, display density\n"
                                "■  Trading — Default exchange, order confirmations, risk limits\n"
                                "■  Data — API keys for market data providers\n"
                                "■  AI — LLM provider, model selection, temperature\n"
                                "■  MCP — Server endpoints and tool access\n"
                                "■  Notifications — Alert channels and thresholds")},
            {tr("SKILL LEVELS"), tr("BEGINNER: Set your timezone and preferred market\n"
                                    "INTERMEDIATE: Configure API keys for data providers and LLM\n"
                                    "ADVANCED: Fine-tune LLM parameters, set up MCP servers\n"
                                    "PRO: Full infrastructure configuration, custom integrations")},
        });
}

// ============================================================================
// Sidebar
// ============================================================================

} // namespace fincept::screens
