// ToolSelfTest.cpp — see ToolSelfTest.h.

#include "mcp/ToolSelfTest.h"

#include "mcp/McpProvider.h"
#include "mcp/ToolRetriever.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace fincept::mcp {
namespace {

// ── Eval corpus ────────────────────────────────────────────────────────────
//
// Realistic things a user would ask the AI chat to do, each mapped to the
// tool(s) that should satisfy it. A case is a HIT if ANY listed tool appears
// in the retriever's top-K. Phrasing deliberately avoids the tool's own name
// where possible — we're testing semantic recall, not string match.
//
// Grounded in the live catalog (see `--dump-tools`). Acceptable-set lists
// every tool that genuinely answers the intent so legitimately-equivalent
// tools don't count as misses, but the *intended* tool always leads the list.
struct EvalCase {
    const char* intent;
    QStringList expected;
};

std::vector<EvalCase> corpus() {
    return {
        // ── portfolio (named portfolios) ──
        {"create a new investment portfolio to track my long-term holdings", {"create_portfolio"}},
        {"add 10 shares of AAPL bought at 225 to my portfolio", {"add_portfolio_asset", "add_holding"}},
        {"show me all my portfolios", {"list_portfolios"}},
        {"what assets are in my portfolio", {"get_portfolio_assets", "get_holdings", "get_portfolio"}},
        {"record a buy transaction for 5 shares of MSFT", {"add_transaction", "add_portfolio_asset"}},
        {"delete the portfolio I no longer use", {"delete_portfolio"}},
        {"what are my current stock holdings and average cost", {"get_holdings", "get_portfolio_assets"}},

        // ── paper trading ──
        {"place a simulated buy order for 100 shares of TSLA", {"pt_place_order"}},
        {"create a paper trading account to practice", {"pt_create_portfolio"}},
        {"what are my open paper trading positions", {"pt_get_positions", "pt_get_portfolio"}},
        {"cancel my pending paper order", {"pt_cancel_order"}},

        // ── live broker trading ──
        {"place a real order to buy 50 shares through my broker", {"live_place_order", "live_smart_order"}},
        {"what positions do I hold at my broker right now", {"live_get_positions", "live_get_holdings"}},
        {"cancel my open broker order", {"live_cancel_order", "live_cancel_all_orders"}},
        {"check my available trading funds and margin", {"live_get_funds"}},
        {"close all my open positions immediately", {"live_close_all_positions", "live_close_position"}},
        {"get the live option chain for NIFTY", {"live_get_option_chain"}},

        // ── markets / quotes ──
        {"what is the current price of Apple stock", {"get_quote", "live_get_quote"}},
        {"get historical daily prices for GOOGL over the last year", {"get_history", "get_equity_historical"}},
        {"find the ticker symbol for Microsoft", {"lookup_symbol", "search_equity_symbols"}},

        // ── watchlist ──
        {"add NVDA to my watchlist", {"add_to_watchlist"}},
        {"create a watchlist for semiconductor stocks", {"create_watchlist"}},
        {"show me my watchlists", {"get_watchlists"}},
        {"remove TSLA from my watchlist", {"remove_from_watchlist"}},

        // ── news ──
        {"show me the latest market news", {"get_news", "get_top_news"}},
        {"search for recent news about Tesla", {"search_news", "get_equity_news"}},
        {"summarize today's top headlines", {"summarize_news_headlines", "get_news_summary"}},
        {"alert me when there is breaking news about oil prices", {"add_news_monitor"}},

        // ── notes ──
        {"save a note about my trade idea for AMD", {"create_note"}},
        {"show me my saved notes", {"get_notes"}},

        // ── navigation ──
        {"open the portfolio tab", {"navigate_to_tab"}},
        {"what tabs or screens are available", {"list_tabs", "list_available_screen_ids"}},

        // ── crypto ──
        {"get the current bitcoin price on the exchange", {"get_ticker", "get_quote"}},
        {"show me the order book depth for ETH", {"get_order_book"}},

        // ── equity research ──
        {"pull the latest financial statements for Apple", {"get_equity_financials", "edgar_get_financials"}},
        {"what is the analyst sentiment on NVDA", {"get_equity_sentiment"}},
        {"show technical indicators for the SPY", {"get_equity_technicals"}},
        {"who are the peer companies of Microsoft", {"get_equity_peers"}},

        // ── SEC EDGAR ──
        {"find the latest 10-K annual report for Tesla",
         {"edgar_10k_sections", "edgar_10k_full_text", "edgar_10k_search", "edgar_find_company"}},
        {"show me recent insider trading at Apple", {"edgar_insider_transactions", "edgar_insider_summary"}},
        {"what 13F holdings does Berkshire Hathaway report", {"edgar_13f_holdings", "edgar_13f_top_holdings"}},
        {"look up the CIK number for a company", {"edgar_resolve_cik", "edgar_find_company"}},

        // ── report builder ──
        {"add a chart component to my research report", {"report_add_component"}},
        {"export my report to PDF", {"report_export_pdf"}},
        {"start a new report from a template", {"report_apply_template"}},

        // ── dashboard ──
        {"add a market heatmap widget to my dashboard", {"add_dashboard_widget"}},
        {"apply a preset dashboard layout", {"apply_dashboard_template"}},

        // ── M&A analytics ──
        {"run a DCF valuation model", {"ma_dcf", "ma_synergies_dcf"}},
        {"build an LBO model for this deal", {"ma_lbo_model"}},
        {"compute the accretion dilution of the merger", {"ma_accretion_dilution"}},

        // ── agents ──
        {"run an AI agent to analyze a stock for me",
         {"run_stock_analysis_agent", "run_agent", "run_agent_structured"}},
        {"list the available AI agents", {"list_agents", "list_agent_configs"}},

        // ── excel ──
        {"set the value of cell B2 in my spreadsheet", {"set_excel_cell"}},
        {"read the data from my excel sheet", {"get_excel_sheet_data"}},

        // ── files ──
        {"list the files I have saved", {"list_files"}},
        {"read the contents of a file", {"read_file_contents"}},

        // ── settings ──
        {"switch my active LLM provider", {"set_active_llm"}},
        {"change an application setting", {"set_setting"}},

        // ── profile / account ──

        // ── workspace ──
        {"tile my open panels in a 2x2 grid", {"tile_panels_2x2"}},
        {"save my current window layout", {"save_current_layout", "snapshot_workspace_now"}},

        // ── economics / data ──
        {"find economic time series data on GDP", {"search_dbnomics", "list_dbnomics_series"}},

        // ── mcp servers / data sources ──
        {"list my connected MCP servers", {"list_mcp_servers"}},
        {"what data source connectors are available", {"ds_list_connectors", "ds_connectors_by_category"}},
    };
}

void out(const QString& s) {
    const QByteArray b = s.toUtf8();
    std::fwrite(b.constData(), 1, static_cast<std::size_t>(b.size()), stdout);
    std::fputc('\n', stdout);
}

// 1-based rank of the first acceptable tool in `results`, or 0 if none present.
int first_hit_rank(const std::vector<ToolMatch>& results, const QStringList& expected) {
    for (int i = 0; i < static_cast<int>(results.size()); ++i) {
        if (expected.contains(results[static_cast<std::size_t>(i)].name))
            return i + 1;
    }
    return 0;
}

QString top_names(const std::vector<ToolMatch>& results, int k) {
    QStringList names;
    for (int i = 0; i < static_cast<int>(results.size()) && i < k; ++i)
        names << results[static_cast<std::size_t>(i)].name;
    return names.isEmpty() ? QStringLiteral("(none)") : names.join(QStringLiteral(", "));
}

} // namespace

int dump_tools_json() {
    auto tools = McpProvider::instance().audit_all_tools();
    std::sort(tools.begin(), tools.end(), [](const auto& a, const auto& b) {
        if (a.category != b.category)
            return a.category < b.category;
        return a.name < b.name;
    });
    QJsonArray arr;
    for (const auto& t : tools) {
        arr.append(QJsonObject{
            {"name", t.name},
            {"category", t.category},
            {"description", t.description},
            {"destructive", t.is_destructive},
            {"has_handler", t.has_handler},
            {"enabled", t.enabled},
        });
    }
    QJsonObject root{{"count", static_cast<int>(arr.size())}, {"tools", arr}};
    out(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented)));
    return 0;
}

int run_tool_selftest() {
    out(QStringLiteral("\n══════════════════════════════════════════════════════════════"));
    out(QStringLiteral("  MCP TOOL SELF-TEST"));
    out(QStringLiteral("══════════════════════════════════════════════════════════════"));

    // ── Part 1: registry audit ──────────────────────────────────────────────
    auto tools = McpProvider::instance().audit_all_tools();
    out(QString(QStringLiteral("\n[1] REGISTRY AUDIT — %1 registered tools")).arg(tools.size()));

    QStringList no_handler;              // CRITICAL — tool can never run
    QStringList bad_schema;              // CRITICAL — required param not declared
    QStringList thin_desc;               // WARN — retrieval-poison
    QStringList destructive_miss;        // WARN — mutating verb but not flagged destructive
    QHash<QString, QStringList> by_desc; // duplicate-description detection

    static const QRegularExpression mutating_rx(
        QStringLiteral("(?:^|_)(delete|remove|cancel|close|clear|wipe|drop|reset|unregister)(?:_|$)"),
        QRegularExpression::CaseInsensitiveOption);

    for (const auto& t : tools) {
        if (!t.has_handler)
            no_handler << t.name;

        // Schema: type must be object, and every `required` name must be a
        // declared property — a required-but-undeclared param is a real bug
        // that makes the tool uncallable from a strict validator.
        const QJsonObject schema = t.input_schema;
        if (schema.value("type").toString() != QLatin1String("object")) {
            bad_schema << (t.name + QStringLiteral(" (type!=object)"));
        } else {
            const QJsonObject props = schema.value("properties").toObject();
            for (const auto& r : schema.value("required").toArray()) {
                const QString rn = r.toString();
                if (!props.contains(rn)) {
                    bad_schema << (t.name + QStringLiteral(" (required '") + rn +
                                   QStringLiteral("' not in properties)"));
                }
            }
        }

        if (t.description.trimmed().length() < 20)
            thin_desc << (t.name + QStringLiteral(" (\"") + t.description.trimmed() + QStringLiteral("\")"));

        if (mutating_rx.match(t.name).hasMatch() && !t.is_destructive)
            destructive_miss << t.name;

        const QString key = t.description.trimmed().toLower();
        if (!key.isEmpty())
            by_desc[key] << t.name;
    }

    auto report_list = [](const QString& label, const QStringList& items, bool critical) {
        if (items.isEmpty()) {
            out(QStringLiteral("    ✓ ") + label + QStringLiteral(": none"));
            return;
        }
        out((critical ? QStringLiteral("    ✗ ") : QStringLiteral("    ⚠ ")) + label + QStringLiteral(": ") +
            QString::number(items.size()));
        for (const auto& it : items)
            out(QStringLiteral("        - ") + it);
    };

    report_list(QStringLiteral("tools with NO handler"), no_handler, true);
    report_list(QStringLiteral("tools with invalid schema"), bad_schema, true);
    report_list(QStringLiteral("tools with thin description (<20 chars)"), thin_desc, false);
    report_list(QStringLiteral("mutating tools NOT flagged destructive"), destructive_miss, false);

    QStringList dup_desc;
    for (auto it = by_desc.cbegin(); it != by_desc.cend(); ++it) {
        if (it.value().size() > 1)
            dup_desc << (it.value().join(QStringLiteral(" = ")));
    }
    report_list(QStringLiteral("tools sharing an identical description"), dup_desc, false);

    // ── Part 2: retrieval eval ────────────────────────────────────────────────
    const auto cases = corpus();
    out(QString(QStringLiteral("\n[2] RETRIEVAL EVAL — %1 intents, top-5 via ToolRetriever (no category hint)"))
            .arg(cases.size()));

    int hit1 = 0;
    int hit5 = 0;
    double mrr = 0.0;
    QStringList misses;
    QStringList weak; // hit but not rank-1

    for (const auto& c : cases) {
        const QString intent = QString::fromUtf8(c.intent);
        auto results = ToolRetriever::instance().search(intent, 5);
        const int rank = first_hit_rank(results, c.expected);
        if (rank == 1)
            ++hit1;
        if (rank >= 1 && rank <= 5)
            ++hit5;
        if (rank >= 1)
            mrr += 1.0 / rank;

        if (rank == 0) {
            misses << (QStringLiteral("        ✗ \"") + intent + QStringLiteral("\"\n             want: ") +
                       c.expected.join(QStringLiteral("|")) + QStringLiteral("\n              got: ") +
                       top_names(results, 5));
        } else if (rank > 1) {
            weak << (QStringLiteral("        ~ rank ") + QString::number(rank) + QStringLiteral(" \"") + intent +
                     QStringLiteral("\" → ") + top_names(results, 5));
        }
    }

    const int n = static_cast<int>(cases.size());
    const double recall1 = n ? static_cast<double>(hit1) / n : 0.0;
    const double recall5 = n ? static_cast<double>(hit5) / n : 0.0;
    const double mrr_avg = n ? mrr / n : 0.0;

    if (!misses.isEmpty()) {
        out(QStringLiteral("\n    MISSES (expected tool not in top-5):"));
        for (const auto& m : misses)
            out(m);
    }
    if (!weak.isEmpty()) {
        out(QStringLiteral("\n    WEAK (found but not rank-1):"));
        for (const auto& w : weak)
            out(w);
    }

    out(QStringLiteral("\n──────────────────────────────────────────────────────────────"));
    out(QString(QStringLiteral("  recall@1 = %1   recall@5 = %2   MRR = %3"))
            .arg(recall1, 0, 'f', 3)
            .arg(recall5, 0, 'f', 3)
            .arg(mrr_avg, 0, 'f', 3));
    out(QString(QStringLiteral("  registry: %1 no-handler, %2 schema errors, %3 thin desc"))
            .arg(no_handler.size())
            .arg(bad_schema.size())
            .arg(thin_desc.size()));

    // ── Verdict ───────────────────────────────────────────────────────────────
    constexpr double kRecall5Target = 0.95;
    const bool pass = no_handler.isEmpty() && bad_schema.isEmpty() && recall5 >= kRecall5Target;
    out(QString(QStringLiteral("  VERDICT: %1  (recall@5 target %2)"))
            .arg(pass ? QStringLiteral("PASS ✓") : QStringLiteral("FAIL ✗"))
            .arg(kRecall5Target, 0, 'f', 2));
    out(QStringLiteral("══════════════════════════════════════════════════════════════\n"));
    std::fflush(stdout);

    return pass ? 0 : 1;
}

} // namespace fincept::mcp
