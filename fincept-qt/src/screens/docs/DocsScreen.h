#pragma once
#include <QEvent>
#include <QLabel>
#include <QListWidget>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

class QSplitter;
class QLineEdit;

namespace fincept::screens {

/// Comprehensive in-app documentation browser for all terminal features.
/// Organized by category with skill-level guidance (Beginner → Pro).
class DocsScreen : public QWidget {
    Q_OBJECT
  public:
    explicit DocsScreen(QWidget* parent = nullptr);

  protected:
    void changeEvent(QEvent* event) override;

  private:
    void build_sidebar();
    void build_content_pages();
    void navigate_to(const QString& section_id);
    /// Filter the sidebar tree by topic/category text (live, from the search box).
    void apply_search(const QString& text);
    /// Recompute the "N TOPICS | M CATEGORIES" chip from the actual tree.
    void update_topic_count();

    /// Re-apply tr() lookups. Static documentation content (sidebar tree + all
    /// pages) is rebuilt from scratch on QEvent::LanguageChange — caching the
    /// hundreds of labels that make up the docs would dominate the source.
    /// The current section is preserved across the rebuild by section_id.
    void retranslateUi();

    // ── Helpers ──────────────────────────────────────────────────────────────
    QWidget* make_page(const QString& title, const QString& subtitle,
                       const std::vector<std::pair<QString, QString>>& sections);
    QWidget* make_section_panel(const QString& icon, const QString& title, const QString& body,
                                const QString& accent_color);
    QWidget* make_skill_panel(const QString& beginner, const QString& intermediate, const QString& advanced,
                              const QString& pro);
    QWidget* make_tip_box(const QString& text, const QString& color);
    QLabel* make_heading(const QString& text);
    QLabel* make_body_label(const QString& text);
    QLabel* make_muted_label(const QString& text);

    // ── Content page builders ────────────────────────────────────────────────
    QWidget* page_welcome();
    QWidget* page_getting_started();
    QWidget* page_keyboard_shortcuts();

    // Core screens
    QWidget* page_dashboard();
    QWidget* page_markets();
    QWidget* page_news();
    QWidget* page_watchlist();

    // Trading
    QWidget* page_crypto_trading();
    QWidget* page_paper_trading();
    QWidget* page_algo_trading();
    QWidget* page_backtesting();

    // Research & Analytics
    QWidget* page_equity_research();
    QWidget* page_surface_analytics();
    QWidget* page_derivatives();
    QWidget* page_portfolio();
    QWidget* page_ma_analytics();

    // AI & Quantitative
    QWidget* page_ai_quant_lab();
    QWidget* page_ai_chat();
    QWidget* page_agent_config();
    QWidget* page_alpha_arena();

    // Data Sources
    QWidget* page_dbnomics();
    QWidget* page_economics();
    QWidget* page_akshare();
    QWidget* page_gov_data();

    // Geopolitics & Alt
    QWidget* page_geopolitics();
    QWidget* page_maritime();
    QWidget* page_polymarket();
    QWidget* page_alt_investments();

    // Tools
    QWidget* page_report_builder();
    QWidget* page_node_editor();
    QWidget* page_code_editor();
    QWidget* page_excel();
    QWidget* page_notes();
    QWidget* page_mcp_servers();
    QWidget* page_data_mapping();

    // Community
    QWidget* page_settings();

    // ── Members ──────────────────────────────────────────────────────────────
    QSplitter* splitter_ = nullptr; // owns sidebar_ + pages_; reused on rebuild
    QTreeWidget* sidebar_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QLabel* breadcrumb_ = nullptr;
    // Command-bar widgets (cached for retranslateUi)
    QLabel* cmd_title_ = nullptr;
    QLabel* cmd_count_ = nullptr;
    QLineEdit* search_input_ = nullptr;
    QMap<QString, int> page_index_; // section_id → stacked widget index
};

} // namespace fincept::screens
