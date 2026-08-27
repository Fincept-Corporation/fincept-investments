// SecuritySection.cpp — AI tool permissions.

#include "screens/settings/SecuritySection.h"

#include "core/logging/Logger.h"
#include "mcp/McpProvider.h"
#include "screens/settings/SettingsRowHelpers.h"
#include "screens/settings/SettingsStyles.h"
#include "ui/theme/Theme.h"

#include <QLabel>
#include <QScrollArea>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QString>
#include <QVBoxLayout>

namespace fincept::screens {

namespace {
// make_row() builds the label (and optional description) QLabel internally;
// grab them back from the row's direct child QLabels so retranslateUi() can
// re-apply text. Order is deterministic: title label first, description second.
void capture_row_labels(QWidget* row, QLabel** label_out, QLabel** desc_out = nullptr) {
    const auto labels = row->findChildren<QLabel*>(QString(), Qt::FindDirectChildrenOnly);
    if (!labels.isEmpty() && label_out)
        *label_out = labels.at(0);
    if (labels.size() > 1 && desc_out)
        *desc_out = labels.at(1);
}

// Long-form explanation for the destructive-tools grant. One definition so
// build_ui() and retranslateUi() cannot drift apart.
QString destructive_tools_desc() {
    return SecuritySection::tr(
        "Off by default. With this off, AI chat and agents can read your terminal but cannot change anything "
        "— no file writes, no spreadsheet, note, watchlist or portfolio edits, no dashboard or workspace "
        "changes, no script execution. Turning it on lets them act for you; you can turn it back off at any "
        "time. Live trading orders and access to your saved broker credentials stay blocked either way. "
        "Applies immediately.");
}
} // namespace

SecuritySection::SecuritySection(QWidget* parent) : QWidget(parent) {
    build_ui();
}

void SecuritySection::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    reload();
}

void SecuritySection::build_ui() {
    using namespace settings_styles;
    using namespace settings_helpers;

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet(QString("QScrollArea { border: none; background: transparent; }"
                                  "QScrollBar:vertical { background: %1; width: 6px; }"
                                  "QScrollBar::handle:vertical { background: %2; }"
                                  "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }")
                              .arg(ui::colors::BG_SURFACE(), ui::colors::BORDER_MED()));

    auto* page = new QWidget(this);
    auto* vl = new QVBoxLayout(page);
    vl->setContentsMargins(24, 24, 24, 24);
    vl->setSpacing(8);

    // ── AI TOOL PERMISSIONS ───────────────────────────────────────────────────
    //
    // Backs `mcp/allow_destructive_tools`. Every MCP tool flagged
    // `is_destructive` — spreadsheet and file writes, dashboard / workspace /
    // layout mutations, notes, watchlist, portfolio, report builder, agent
    // execution, run_python_script — fails closed unless this is on. It is the
    // ONLY place that grant can be made, so without it the whole "let the
    // assistant drive the terminal" surface is permanently read-only.
    //
    // McpProvider::set_destructive_allowed persists it (AppConfig, same
    // mechanism LoggingSection and VoiceConfigSection use) and updates the live
    // session in one call — there is no second copy of this setting to keep in
    // sync, and no Save button to half-apply it.
    title_ai_tools_ = new QLabel(tr("AI TOOL PERMISSIONS"));
    title_ai_tools_->setStyleSheet(section_title_ss());
    vl->addWidget(title_ai_tools_);
    vl->addWidget(make_sep());
    vl->addSpacing(8);

    sec_allow_destructive_ = new QCheckBox(tr("Allow AI agents to modify files, workspaces and data"));
    sec_allow_destructive_->setStyleSheet(check_ss());
    sec_allow_destructive_->setAccessibleName(tr("Allow AI agents to modify files, workspaces and data"));
    auto* row_destructive = make_row(tr("Destructive AI Tools"), sec_allow_destructive_, destructive_tools_desc());
    capture_row_labels(row_destructive, &row_destructive_lbl_, &row_destructive_desc_);
    vl->addWidget(row_destructive);

    connect(sec_allow_destructive_, &QCheckBox::toggled, this, [](bool checked) {
        mcp::McpProvider::set_destructive_allowed(checked);
        LOG_INFO("Settings",
                 QString("Destructive AI tools %1 from Security settings").arg(checked ? "enabled" : "disabled"));
    });

    vl->addStretch();
    scroll->setWidget(page);
    root->addWidget(scroll);
}

void SecuritySection::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    QWidget::changeEvent(event);
}

void SecuritySection::retranslateUi() {
    if (title_ai_tools_)
        title_ai_tools_->setText(tr("AI TOOL PERMISSIONS"));
    if (sec_allow_destructive_) {
        sec_allow_destructive_->setText(tr("Allow AI agents to modify files, workspaces and data"));
        sec_allow_destructive_->setAccessibleName(tr("Allow AI agents to modify files, workspaces and data"));
    }
    if (row_destructive_lbl_)
        row_destructive_lbl_->setText(tr("Destructive AI Tools"));
    if (row_destructive_desc_)
        row_destructive_desc_->setText(destructive_tools_desc());
}

void SecuritySection::reload() {
    if (!sec_allow_destructive_)
        return;
    // Read back through McpProvider rather than the raw setting key: it is the
    // owner of `mcp/allow_destructive_tools` and of the session grant seeded
    // from it, so this can never show a state the tool gate does not actually
    // apply. (Its per-call agent token path is thread-local and always false
    // here on the GUI thread.)
    const QSignalBlocker b(sec_allow_destructive_);
    sec_allow_destructive_->setChecked(mcp::McpProvider::destructive_allowed());
}

} // namespace fincept::screens
