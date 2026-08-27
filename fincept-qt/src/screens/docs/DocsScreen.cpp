// Documentation Screen — comprehensive in-app terminal guide.
//
// Core lifecycle: ctor, sidebar/page assembly (build_sidebar,
// build_content_pages), and navigate_to. The make_* helpers that compose
// every page also live here. The page bodies are split across:
//   - DocsScreen_Pages_Intro.cpp    — welcome/dashboard/markets/news/watchlist
//   - DocsScreen_Pages_Trading.cpp  — crypto / paper / algo / backtest / AI quant
//   - DocsScreen_Pages_Data.cpp     — data sources, geopolitics, tools, settings
// Shared QSS helpers live in DocsScreen_internal.h.
#include "screens/docs/DocsScreen.h"

#include "screens/docs/DocsScreen_internal.h"
#include "ui/theme/Theme.h"

#include <QCoreApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLineEdit>
#include <QScrollArea>
#include <QShortcut>
#include <QSplitter>
#include <QTreeWidgetItemIterator>

namespace fincept::screens {

using fincept::screens::docs_internal::SCROLL_SS;
static const char* FONT = "'Consolas','Courier New',monospace";

static QString SIDEBAR_SS() {
    return QString("QTreeWidget { background: %1; color: %2; border: none;"
                   "  font-size: 12px; font-family: 'Consolas','Courier New',monospace;"
                   "  outline: none; }"
                   "QTreeWidget::item { height: 26px; padding-left: 6px; border: none; }"
                   "QTreeWidget::item:hover { background: %3; color: %4; }"
                   "QTreeWidget::item:selected { background: %5; color: %6; }"
                   "QTreeWidget::branch { background: %1; }"
                   "QTreeWidget::branch:hover { background: %3; }"
                   "QTreeWidget::branch:has-children:!has-siblings:closed,"
                   "QTreeWidget::branch:closed:has-children:has-siblings"
                   " { border-image: none; image: none; }"
                   "QTreeWidget::branch:open:has-children:!has-siblings,"
                   "QTreeWidget::branch:open:has-children:has-siblings"
                   " { border-image: none; image: none; }"
                   "QScrollBar:vertical { width: 5px; background: transparent; }"
                   "QScrollBar::handle:vertical { background: %7; }"
                   "QScrollBar::handle:vertical:hover { background: %8; }"
                   "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }")
        .arg(ui::colors::BG_SURFACE(), ui::colors::TEXT_SECONDARY(), ui::colors::BG_RAISED(),
             ui::colors::TEXT_PRIMARY(), ui::colors::BG_HOVER(), ui::colors::AMBER(), ui::colors::BORDER_MED(),
             ui::colors::BORDER_BRIGHT());
}

// SCROLL_SS lives in DocsScreen_internal.h so the page-builder TUs can reuse it.

// ============================================================================
// Helpers
// ============================================================================

QLabel* DocsScreen::make_heading(const QString& text) {
    auto* lbl = new QLabel(text);
    lbl->setStyleSheet(QString("color: %1; font-size: 14px; font-weight: bold; letter-spacing: 0.5px;"
                               " background: transparent; font-family: 'Consolas','Courier New',monospace;"
                               " padding: 4px 0;")
                           .arg(ui::colors::AMBER()));
    lbl->setWordWrap(true);
    return lbl;
}

QLabel* DocsScreen::make_body_label(const QString& text) {
    auto* lbl = new QLabel(text);
    lbl->setStyleSheet(QString("color: %1; font-size: 12px; background: transparent; line-height: 1.5;"
                               " font-family: 'Consolas','Courier New',monospace;")
                           .arg(ui::colors::TEXT_PRIMARY()));
    lbl->setWordWrap(true);
    return lbl;
}

QLabel* DocsScreen::make_muted_label(const QString& text) {
    auto* lbl = new QLabel(text);
    lbl->setStyleSheet(QString("color: %1; font-size: 11px; background: transparent;"
                               " font-family: 'Consolas','Courier New',monospace;")
                           .arg(ui::colors::TEXT_SECONDARY()));
    lbl->setWordWrap(true);
    return lbl;
}

QWidget* DocsScreen::make_section_panel(const QString& icon, const QString& title, const QString& body,
                                        const QString& accent_color) {
    auto* panel = new QWidget(this);
    panel->setStyleSheet(
        QString("background: %1; border: 1px solid %2;").arg(ui::colors::BG_SURFACE(), ui::colors::BORDER_DIM()));
    auto* vl = new QVBoxLayout(panel);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->setSpacing(0);

    // Header
    auto* hdr = new QLabel(QString("%1  %2").arg(icon, title));
    hdr->setStyleSheet(QString("color: %1; font-size: 11px; font-weight: bold; letter-spacing: 0.5px;"
                               " background: %2; padding: 8px 12px; border-bottom: 1px solid %3;"
                               " font-family: 'Consolas','Courier New',monospace;")
                           .arg(accent_color, ui::colors::BG_RAISED(), ui::colors::BORDER_DIM()));
    vl->addWidget(hdr);

    // Body
    auto* content = new QLabel(body);
    content->setStyleSheet(QString("color: %1; font-size: 12px; background: transparent; padding: 10px 12px;"
                                   " font-family: 'Consolas','Courier New',monospace; line-height: 1.6;")
                               .arg(ui::colors::TEXT_PRIMARY()));
    content->setWordWrap(true);
    vl->addWidget(content);

    return panel;
}

QWidget* DocsScreen::make_skill_panel(const QString& beginner, const QString& intermediate, const QString& advanced,
                                      const QString& pro) {
    auto* panel = new QWidget(this);
    panel->setStyleSheet(
        QString("background: %1; border: 1px solid %2;").arg(ui::colors::BG_SURFACE(), ui::colors::BORDER_DIM()));
    auto* vl = new QVBoxLayout(panel);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->setSpacing(0);

    auto* hdr = new QLabel(tr("SKILL LEVELS"));
    hdr->setStyleSheet(QString("color: %1; font-size: 11px; font-weight: bold; letter-spacing: 0.5px;"
                               " background: %2; padding: 8px 12px; border-bottom: 1px solid %3;"
                               " font-family: 'Consolas','Courier New',monospace;")
                           .arg(ui::colors::AMBER(), ui::colors::BG_RAISED(), ui::colors::BORDER_DIM()));
    vl->addWidget(hdr);

    struct Level {
        QString tag;
        const char* color;
        const QString& text;
    };
    Level levels[] = {
        {tr("BEGINNER"), ui::colors::POSITIVE(), beginner},
        {tr("INTERMEDIATE"), ui::colors::INFO(), intermediate},
        {tr("ADVANCED"), ui::colors::AMBER(), advanced},
        {tr("PRO"), ui::colors::NEGATIVE(), pro},
    };

    for (const auto& lvl : levels) {
        auto* row = new QWidget(this);
        row->setStyleSheet(
            QString("background: transparent; border-bottom: 1px solid %1;").arg(ui::colors::BG_RAISED()));
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(12, 8, 12, 8);
        rl->setSpacing(10);

        auto* badge = new QLabel(lvl.tag);
        badge->setFixedWidth(105);
        badge->setStyleSheet(QString("color: %1; font-size: 10px; font-weight: bold; background: transparent;"
                                     " letter-spacing: 0.5px; font-family: 'Consolas','Courier New',monospace;")
                                 .arg(lvl.color));
        rl->addWidget(badge);

        auto* desc = new QLabel(lvl.text);
        desc->setStyleSheet(QString("color: %1; font-size: 11px; background: transparent;"
                                    " font-family: 'Consolas','Courier New',monospace;")
                                .arg(ui::colors::TEXT_PRIMARY()));
        desc->setWordWrap(true);
        rl->addWidget(desc, 1);

        vl->addWidget(row);
    }
    return panel;
}

QWidget* DocsScreen::make_tip_box(const QString& text, const QString& color) {
    auto* box = new QLabel(text);
    box->setStyleSheet(QString("color: %1; font-size: 11px; background: rgba(%2, 0.08);"
                               " border: 1px solid rgba(%2, 0.3); padding: 8px 12px;"
                               " font-family: 'Consolas','Courier New',monospace;")
                           .arg(color, color));
    box->setWordWrap(true);
    return box;
}

QWidget* DocsScreen::make_page(const QString& title, const QString& subtitle,
                               const std::vector<std::pair<QString, QString>>& sections) {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet(SCROLL_SS());

    auto* page = new QWidget(this);
    page->setStyleSheet(QString("background: %1;").arg(ui::colors::BG_BASE()));
    auto* vl = new QVBoxLayout(page);
    vl->setContentsMargins(20, 16, 20, 20);
    vl->setSpacing(12);

    // Page title
    vl->addWidget(make_heading(title));
    if (!subtitle.isEmpty()) {
        vl->addWidget(make_muted_label(subtitle));
    }

    // Separator
    auto* sep = new QFrame;
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet(QString("color: %1;").arg(ui::colors::BORDER_DIM()));
    vl->addWidget(sep);

    // Sections
    for (const auto& [heading, body] : sections) {
        vl->addWidget(make_section_panel("■", heading, body, ui::colors::AMBER));
    }

    vl->addStretch();
    scroll->setWidget(page);
    return scroll;
}

void DocsScreen::build_sidebar() {
    sidebar_ = new QTreeWidget;
    sidebar_->setHeaderHidden(true);
    sidebar_->setStyleSheet(SIDEBAR_SS());
    sidebar_->setIndentation(16);
    sidebar_->setAnimated(true);
    sidebar_->setRootIsDecorated(true);

    auto add_item = [&](QTreeWidgetItem* parent, const QString& label, const QString& id) {
        auto* item = new QTreeWidgetItem(parent, {label});
        item->setData(0, Qt::UserRole, id);
        return item;
    };

    auto add_category = [&](const QString& label) {
        auto* cat = new QTreeWidgetItem(sidebar_, {label});
        cat->setFlags(cat->flags() & ~Qt::ItemIsSelectable);
        QFont f(FONT, 10);
        f.setBold(true);
        cat->setFont(0, f);
        cat->setForeground(0, QColor(ui::colors::AMBER.get()));
        cat->setExpanded(true);
        return cat;
    };

    // ── Getting Started ──────────────────────────────────────────────────────
    auto* start = add_category(tr("GETTING STARTED"));
    add_item(start, tr("Welcome"), "welcome");
    add_item(start, tr("First Steps"), "getting_started");
    add_item(start, tr("Keyboard Shortcuts"), "shortcuts");

    // ── Core Screens ─────────────────────────────────────────────────────────
    auto* core = add_category(tr("CORE SCREENS"));
    add_item(core, tr("Dashboard"), "dashboard");
    add_item(core, tr("Markets"), "markets");
    add_item(core, tr("News"), "news");
    add_item(core, tr("Watchlist"), "watchlist");

    // ── Trading ──────────────────────────────────────────────────────────────
    auto* trading = add_category(tr("TRADING"));
    add_item(trading, tr("Crypto Trading"), "crypto_trading");
    add_item(trading, tr("Paper Trading"), "paper_trading");
    add_item(trading, tr("Algo Trading"), "algo_trading");
    add_item(trading, tr("Backtesting"), "backtesting");

    // ── Research & Analytics ─────────────────────────────────────────────────
    auto* research = add_category(tr("RESEARCH & ANALYTICS"));
    add_item(research, tr("Equity Research"), "equity_research");
    add_item(research, tr("Surface Analytics"), "surface_analytics");
    add_item(research, tr("Derivatives"), "derivatives");
    add_item(research, tr("Portfolio"), "portfolio");
    add_item(research, tr("M&A Analytics"), "ma_analytics");

    // ── AI & Quantitative ────────────────────────────────────────────────────
    auto* ai = add_category(tr("AI & QUANTITATIVE"));
    add_item(ai, tr("AI Quant Lab"), "ai_quant_lab");
    add_item(ai, tr("AI Chat"), "ai_chat");
    add_item(ai, tr("Agent Studio"), "agent_config");
    add_item(ai, tr("Alpha Arena"), "alpha_arena");

    // ── Data Sources ─────────────────────────────────────────────────────────
    auto* data_cat = add_category(tr("DATA SOURCES"));
    add_item(data_cat, tr("DBnomics"), "dbnomics");
    add_item(data_cat, tr("Economics"), "economics");
    add_item(data_cat, tr("AkShare Data"), "akshare");
    add_item(data_cat, tr("Government Data"), "gov_data");

    // ── Geopolitics & Alt ────────────────────────────────────────────────────
    auto* geo = add_category(tr("GEOPOLITICS & ALT"));
    add_item(geo, tr("Geopolitics"), "geopolitics");
    add_item(geo, tr("Maritime"), "maritime");
    add_item(geo, tr("Prediction Markets"), "polymarket");
    add_item(geo, tr("Alt Investments"), "alt_investments");

    // ── Tools ────────────────────────────────────────────────────────────────
    auto* tools = add_category(tr("TOOLS"));
    add_item(tools, tr("Report Builder"), "report_builder");
    add_item(tools, tr("Node Editor"), "node_editor");
    add_item(tools, tr("Code Editor"), "code_editor");
    add_item(tools, tr("Excel"), "excel");
    add_item(tools, tr("Notes"), "notes");
    add_item(tools, tr("MCP Servers"), "mcp_servers");
    add_item(tools, tr("Data Mapping"), "data_mapping");

    // ── Account ──────────────────────────────────────────────────────────────
    auto* account = add_category(tr("ACCOUNT"));
    add_item(account, tr("Settings"), "settings");

    // Navigation
    connect(sidebar_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
        if (!current)
            return;
        QString id = current->data(0, Qt::UserRole).toString();
        if (!id.isEmpty()) {
            navigate_to(id);
        }
    });
}

// ============================================================================
// Content pages
// ============================================================================

void DocsScreen::build_content_pages() {
    pages_ = new QStackedWidget;
    pages_->setStyleSheet(QString("background: %1;").arg(ui::colors::BG_BASE()));

    auto add = [&](const QString& id, QWidget* page) {
        int idx = pages_->addWidget(page);
        page_index_[id] = idx;
    };

    // Getting Started
    add("welcome", page_welcome());
    add("getting_started", page_getting_started());
    add("shortcuts", page_keyboard_shortcuts());

    // Core
    add("dashboard", page_dashboard());
    add("markets", page_markets());
    add("news", page_news());
    add("watchlist", page_watchlist());

    // Trading
    add("crypto_trading", page_crypto_trading());
    add("paper_trading", page_paper_trading());
    add("algo_trading", page_algo_trading());
    add("backtesting", page_backtesting());

    // Research
    add("equity_research", page_equity_research());
    add("surface_analytics", page_surface_analytics());
    add("derivatives", page_derivatives());
    add("portfolio", page_portfolio());
    add("ma_analytics", page_ma_analytics());

    // AI
    add("ai_quant_lab", page_ai_quant_lab());
    add("ai_chat", page_ai_chat());
    add("agent_config", page_agent_config());
    add("alpha_arena", page_alpha_arena());

    // Data
    add("dbnomics", page_dbnomics());
    add("economics", page_economics());
    add("akshare", page_akshare());
    add("gov_data", page_gov_data());

    // Geopolitics
    add("geopolitics", page_geopolitics());
    add("maritime", page_maritime());
    add("polymarket", page_polymarket());
    add("alt_investments", page_alt_investments());

    // Tools
    add("report_builder", page_report_builder());
    add("node_editor", page_node_editor());
    add("code_editor", page_code_editor());
    add("excel", page_excel());
    add("notes", page_notes());
    add("mcp_servers", page_mcp_servers());
    add("data_mapping", page_data_mapping());

    // Account
    add("settings", page_settings());
}

void DocsScreen::navigate_to(const QString& section_id) {
    auto it = page_index_.find(section_id);
    if (it != page_index_.end()) {
        pages_->setCurrentIndex(it.value());
    }
}

// ── Topic count + search ─────────────────────────────────────────────────────

void DocsScreen::update_topic_count() {
    if (!cmd_count_ || !sidebar_)
        return;
    // Counted, not hardcoded — the literal said "35 TOPICS" while the tree had
    // 38, and would drift again on the next page added.
    int topics = 0;
    const int categories = sidebar_->topLevelItemCount();
    for (QTreeWidgetItemIterator it(sidebar_); *it; ++it)
        if (!(*it)->data(0, Qt::UserRole).toString().isEmpty())
            ++topics;
    cmd_count_->setText(tr("%1 TOPICS  |  %2 CATEGORIES").arg(topics).arg(categories));
}

void DocsScreen::apply_search(const QString& text) {
    if (!sidebar_)
        return;
    const QString needle = text.trimmed();

    // Empty query restores the full tree.
    if (needle.isEmpty()) {
        for (int i = 0; i < sidebar_->topLevelItemCount(); ++i) {
            auto* cat = sidebar_->topLevelItem(i);
            cat->setHidden(false);
            cat->setExpanded(true);
            for (int j = 0; j < cat->childCount(); ++j)
                cat->child(j)->setHidden(false);
        }
        return;
    }

    for (int i = 0; i < sidebar_->topLevelItemCount(); ++i) {
        auto* cat = sidebar_->topLevelItem(i);
        const bool cat_hit = cat->text(0).contains(needle, Qt::CaseInsensitive);
        int visible_children = 0;
        for (int j = 0; j < cat->childCount(); ++j) {
            auto* child = cat->child(j);
            const bool hit = cat_hit || child->text(0).contains(needle, Qt::CaseInsensitive) ||
                             child->data(0, Qt::UserRole).toString().contains(needle, Qt::CaseInsensitive);
            child->setHidden(!hit);
            if (hit)
                ++visible_children;
        }
        cat->setHidden(visible_children == 0);
        cat->setExpanded(true);
    }
}

// ============================================================================
// Constructor — Main layout
// ============================================================================

DocsScreen::DocsScreen(QWidget* parent) : QWidget(parent) {
    setObjectName("docsScreen");
    setStyleSheet(QString("QWidget#docsScreen { background: %1; }").arg(ui::colors::BG_BASE()));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── Command bar ──────────────────────────────────────────────────────────
    auto* cmd = new QWidget(this);
    cmd->setStyleSheet(QString("background: %1; border-bottom: 1px solid %2;")
                           .arg(ui::colors::BG_SURFACE(), ui::colors::BORDER_DIM()));
    cmd->setFixedHeight(32);
    auto* cmd_hl = new QHBoxLayout(cmd);
    cmd_hl->setContentsMargins(10, 0, 10, 0);
    cmd_hl->setSpacing(10);

    cmd_title_ = new QLabel(tr("DOCUMENTATION"));
    cmd_title_->setStyleSheet(QString("color: %1; font-size: 12px; font-weight: bold; letter-spacing: 1px;"
                                      " background: transparent; font-family: 'Consolas','Courier New',monospace;")
                                  .arg(ui::colors::AMBER()));
    cmd_hl->addWidget(cmd_title_);

    auto* sep = new QLabel("|");
    sep->setStyleSheet(QString("color: %1; background: transparent; font-family: 'Consolas',monospace;")
                           .arg(ui::colors::BORDER_BRIGHT()));
    cmd_hl->addWidget(sep);

    // Brand + version string — shown verbatim, not translated. Read from the
    // running application rather than hardcoded: the literal here said v4.0.0
    // while the app shipped as 4.0.1.
    breadcrumb_ = new QLabel(QStringLiteral("FINCEPT TERMINAL v%1").arg(QCoreApplication::applicationVersion()));
    breadcrumb_->setStyleSheet(QString("color: %1; font-size: 11px; font-weight: bold;"
                                       " background: transparent; letter-spacing: 0.5px;"
                                       " font-family: 'Consolas','Courier New',monospace;")
                                   .arg(ui::colors::TEXT_SECONDARY()));
    cmd_hl->addWidget(breadcrumb_);

    // ── Search ────────────────────────────────────────────────────────────────
    // 38 topics with no way to search them. Filters the sidebar tree live;
    // Ctrl+F focuses it.
    search_input_ = new QLineEdit;
    search_input_->setFixedWidth(220);
    search_input_->setClearButtonEnabled(true);
    search_input_->setPlaceholderText(tr("Search topics…  (Ctrl+F)"));
    search_input_->setAccessibleName(tr("Search documentation topics"));
    search_input_->setStyleSheet(QString("QLineEdit { background: %1; color: %2; border: 1px solid %3;"
                                         "  padding: 2px 6px; font-size: 11px;"
                                         "  font-family: 'Consolas','Courier New',monospace; }"
                                         "QLineEdit:focus { border-color: %4; }")
                                     .arg(ui::colors::BG_BASE(), ui::colors::TEXT_PRIMARY(),
                                          ui::colors::BORDER_DIM(), ui::colors::BORDER_BRIGHT()));
    connect(search_input_, &QLineEdit::textChanged, this, &DocsScreen::apply_search);
    cmd_hl->addWidget(search_input_);

    cmd_hl->addStretch();

    cmd_count_ = new QLabel;
    cmd_count_->setStyleSheet(QString("color: %1; font-size: 11px; background: transparent;"
                                      " font-family: 'Consolas','Courier New',monospace;")
                                  .arg(ui::colors::TEXT_TERTIARY()));
    cmd_hl->addWidget(cmd_count_);

    root->addWidget(cmd);

    // ── Splitter: sidebar + content ──────────────────────────────────────────
    build_sidebar();
    build_content_pages();
    update_topic_count();

    auto* find_sc = new QShortcut(QKeySequence::Find, this);
    find_sc->setContext(Qt::WidgetWithChildrenShortcut);
    connect(find_sc, &QShortcut::activated, this, [this]() {
        search_input_->setFocus();
        search_input_->selectAll();
    });

    splitter_ = new QSplitter(Qt::Horizontal);
    splitter_->setStyleSheet(QString("QSplitter { background: %1; }"
                                     "QSplitter::handle { background: %2; width: 1px; }")
                                 .arg(ui::colors::BG_BASE(), ui::colors::BORDER_DIM()));
    splitter_->addWidget(sidebar_);
    splitter_->addWidget(pages_);
    splitter_->setStretchFactor(0, 0);
    splitter_->setStretchFactor(1, 1);
    splitter_->setSizes({220, 800});

    root->addWidget(splitter_, 1);

    // Default to welcome page
    navigate_to("welcome");
}

// ============================================================================
// Re-translation
// ============================================================================
// The sidebar tree and every documentation page are static content composed of
// hundreds of labels. Rather than caching each one, we rebuild both on a
// language change and restore the section the user was reading.

void DocsScreen::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    QWidget::changeEvent(event);
}

void DocsScreen::retranslateUi() {
    // Command bar
    if (cmd_title_)
        cmd_title_->setText(tr("DOCUMENTATION"));
    if (search_input_)
        search_input_->setPlaceholderText(tr("Search topics…  (Ctrl+F)"));

    // Rebuild sidebar + content pages so their tr() strings pick up the new
    // language. Preserve the currently displayed section across the rebuild.
    if (!splitter_ || !sidebar_ || !pages_)
        return;

    QString current_id;
    if (auto* cur = sidebar_->currentItem())
        current_id = cur->data(0, Qt::UserRole).toString();
    if (current_id.isEmpty()) {
        // Fall back to whatever page is on top of the stack.
        const int idx = pages_->currentIndex();
        for (auto it = page_index_.cbegin(); it != page_index_.cend(); ++it)
            if (it.value() == idx) {
                current_id = it.key();
                break;
            }
    }

    QTreeWidget* old_sidebar = sidebar_;
    QStackedWidget* old_pages = pages_;
    const QList<int> sizes = splitter_->sizes();

    page_index_.clear();
    build_sidebar();       // assigns a fresh sidebar_
    build_content_pages(); // assigns a fresh pages_

    splitter_->insertWidget(0, sidebar_);
    splitter_->insertWidget(1, pages_);
    splitter_->setStretchFactor(0, 0);
    splitter_->setStretchFactor(1, 1);
    if (sizes.size() == 2)
        splitter_->setSizes(sizes);

    old_sidebar->deleteLater();
    old_pages->deleteLater();

    update_topic_count();
    if (search_input_)
        apply_search(search_input_->text()); // re-apply the active filter to the new tree

    // Restore the section the user was on (also re-selects the sidebar row via
    // the currentItemChanged → navigate_to wiring).
    if (!current_id.isEmpty()) {
        navigate_to(current_id);
        for (QTreeWidgetItemIterator it(sidebar_); *it; ++it) {
            if ((*it)->data(0, Qt::UserRole).toString() == current_id) {
                sidebar_->setCurrentItem(*it);
                break;
            }
        }
    } else {
        navigate_to("welcome");
    }
}

} // namespace fincept::screens
