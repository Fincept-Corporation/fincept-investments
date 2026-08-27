// src/screens/agent_config/AgentConfigScreen.cpp
#include "screens/agent_config/AgentConfigScreen.h"

#include "core/events/EventBus.h"
#include "core/logging/Logger.h"
#include "core/session/ScreenStateManager.h"
#include "screens/agent_config/AgentChatPanel.h"
#include "screens/agent_config/AgenticTasksPanel.h"
#include "screens/agent_config/AgentsViewPanel.h"
#include "screens/agent_config/CreateAgentPanel.h"
#include "screens/agent_config/PlannerViewPanel.h"
#include "screens/agent_config/SystemViewPanel.h"
#include "screens/agent_config/TeamsViewPanel.h"
#include "screens/agent_config/ToolsViewPanel.h"
#include "screens/agent_config/WorkflowsViewPanel.h"
#include "services/agents/AgentService.h"
#include "storage/repositories/SettingsRepository.h"
#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QShowEvent>
#include <QVBoxLayout>

namespace fincept::screens {

// ── View mode metadata ───────────────────────────────────────────────────────

struct ViewMeta {
    services::AgentViewMode mode;
    const char* label;
};

// QT_TRANSLATE_NOOP marks these literals for lupdate without translating at
// initialization time — actual lookup happens in make_nav_btn() / status-view
// update via tr(), so the active translator is consulted on every render.
static constexpr ViewMeta kViews[] = {
    {services::AgentViewMode::Agents, QT_TRANSLATE_NOOP("fincept::screens::AgentConfigScreen", "AGENTS")},
    {services::AgentViewMode::Create, QT_TRANSLATE_NOOP("fincept::screens::AgentConfigScreen", "CREATE")},
    {services::AgentViewMode::Teams, QT_TRANSLATE_NOOP("fincept::screens::AgentConfigScreen", "TEAMS")},
    {services::AgentViewMode::Workflows, QT_TRANSLATE_NOOP("fincept::screens::AgentConfigScreen", "WORKFLOWS")},
    {services::AgentViewMode::Planner, QT_TRANSLATE_NOOP("fincept::screens::AgentConfigScreen", "PLANNER")},
    {services::AgentViewMode::Tools, QT_TRANSLATE_NOOP("fincept::screens::AgentConfigScreen", "TOOLS")},
    {services::AgentViewMode::Chat, QT_TRANSLATE_NOOP("fincept::screens::AgentConfigScreen", "CHAT")},
    {services::AgentViewMode::System, QT_TRANSLATE_NOOP("fincept::screens::AgentConfigScreen", "SYSTEM")},
    {services::AgentViewMode::Agentic, QT_TRANSLATE_NOOP("fincept::screens::AgentConfigScreen", "AGENTIC")},
};

// ── Constructor ──────────────────────────────────────────────────────────────

AgentConfigScreen::AgentConfigScreen(QWidget* parent) : QWidget(parent) {
    setObjectName("AgentConfigScreen");
    build_ui();
    setup_service_connections();
    connect(&ui::ThemeManager::instance(), &ui::ThemeManager::theme_changed, this,
            [this](const ui::ThemeTokens&) { update(); });

    // Read the Agentic-Mode setting and apply visibility. The toggle widget
    // (DeveloperSection) publishes settings.agentic_mode_changed via EventBus
    // on user change; we react in-place without restart.
    {
        auto r = fincept::SettingsRepository::instance().get(QStringLiteral("agentic_mode_enabled"),
                                                             QStringLiteral("false"));
        agentic_mode_enabled_ = r.is_ok() && r.value() == QStringLiteral("true");
    }
    connect(&fincept::EventBus::instance(), &fincept::EventBus::eventPublished, this,
            [this](const QString& event, const QVariantMap& data) {
                if (event == QStringLiteral("settings.agentic_mode_changed")) {
                    agentic_mode_enabled_ = data.value("enabled").toBool();
                    apply_agentic_visibility();
                }
            });
    apply_agentic_visibility();
}

// ── UI construction ──────────────────────────────────────────────────────────

void AgentConfigScreen::build_ui() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    build_nav_bar(root);

    view_stack_ = new QStackedWidget;
    view_stack_->setObjectName("AgentViewStack");
    root->addWidget(view_stack_, 1);

    // No placeholders — panels are added to the stack on first navigation.
    // ensure_panel_built() appends them and sets currentWidget directly.

    build_status_bar(root);
}

void AgentConfigScreen::build_nav_bar(QVBoxLayout* root) {
    auto* nav_bar = new QWidget(this);
    nav_bar->setObjectName("AgentNavBar");
    nav_bar->setFixedHeight(40);
    nav_bar->setStyleSheet(QString("QWidget#AgentNavBar { background: %1; border-bottom: 1px solid %2; }")
                               .arg(ui::colors::BG_RAISED(), ui::colors::BORDER_DIM()));

    auto* hl = new QHBoxLayout(nav_bar);
    hl->setContentsMargins(12, 0, 12, 0);
    hl->setSpacing(0);

    title_label_ = new QLabel(tr("AGENT STUDIO"));
    title_label_->setStyleSheet(
        QString("color:%1;font-size:13px;font-weight:700;letter-spacing:2px;padding-right:16px;")
            .arg(ui::colors::AMBER()));
    hl->addWidget(title_label_);

    auto* sep = new QFrame;
    sep->setFrameShape(QFrame::VLine);
    sep->setStyleSheet(QString("color:%1;").arg(ui::colors::BORDER_MED()));
    sep->setFixedWidth(1);
    hl->addWidget(sep);

    for (const auto& vm : kViews) {
        auto* btn = make_nav_btn(vm.label, vm.mode);
        if (vm.mode == services::AgentViewMode::Agentic)
            agentic_nav_idx_ = nav_buttons_.size();
        hl->addWidget(btn);
        nav_buttons_.append(btn);
    }

    hl->addStretch();

    agent_count_label_ = new QLabel;
    agent_count_label_->setStyleSheet(
        QString("color:%1;font-size:11px;padding:2px 8px;background:%2;border-radius:2px;")
            .arg(ui::colors::AMBER(), ui::colors::BG_SURFACE()));
    hl->addWidget(agent_count_label_);

    root->addWidget(nav_bar);
}

void AgentConfigScreen::build_status_bar(QVBoxLayout* root) {
    auto* bar = new QWidget(this);
    bar->setFixedHeight(24);
    bar->setStyleSheet(
        QString("background:%1;border-top:1px solid %2;").arg(ui::colors::BG_RAISED(), ui::colors::BORDER_DIM()));

    auto* hl = new QHBoxLayout(bar);
    hl->setContentsMargins(12, 0, 12, 0);

    status_label_ = new QLabel(tr("READY"));
    status_label_->setStyleSheet(QString("color:%1;font-size:10px;").arg(ui::colors::TEXT_TERTIARY()));
    hl->addWidget(status_label_);
    hl->addStretch();

    view_label_ = new QLabel;
    view_label_->setObjectName("AgentStatusView");
    view_label_->setStyleSheet(QString("color:%1;font-size:10px;").arg(ui::colors::AMBER()));
    hl->addWidget(view_label_);

    root->addWidget(bar);

    connect(view_stack_, &QStackedWidget::currentChanged, this, [this](int) {
        // Translate the mode label through tr() each time — picks up the
        // active locale automatically.
        const int idx = static_cast<int>(current_view_);
        if (idx >= 0 && idx < static_cast<int>(std::size(kViews)))
            view_label_->setText(tr(kViews[idx].label));
    });
}

void AgentConfigScreen::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    QWidget::changeEvent(event);
}

void AgentConfigScreen::retranslateUi() {
    if (title_label_)
        title_label_->setText(tr("AGENT STUDIO"));
    // Nav buttons — apply the QT_TR_NOOP-marked label through tr() in order
    // so the table-driven mode mapping survives the language switch.
    for (int i = 0; i < nav_buttons_.size() && i < static_cast<int>(std::size(kViews)); ++i)
        nav_buttons_[i]->setText(tr(kViews[i].label));
    // Refresh the status-view label (right side of the status bar) with the
    // newly-translated mode name for whatever panel is currently visible.
    if (view_label_) {
        const int idx = static_cast<int>(current_view_);
        if (idx >= 0 && idx < static_cast<int>(std::size(kViews)))
            view_label_->setText(tr(kViews[idx].label));
    }
    // The free-form status label (READY / ERROR / DONE …) is state-driven;
    // we only restore the default "READY" idle state. Any active error or
    // execution result will be overwritten by the next AgentService signal.
    if (status_label_ && status_label_->text().isEmpty())
        status_label_->setText(tr("READY"));
    if (agent_count_label_)
        agent_count_label_->setText(tr("%1 agents").arg(last_agent_count_));
}

QPushButton* AgentConfigScreen::make_nav_btn(const QString& text, services::AgentViewMode mode) {
    auto* btn = new QPushButton(text);
    btn->setCheckable(true);
    btn->setChecked(mode == services::AgentViewMode::Agents);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setStyleSheet(
        QString("QPushButton { background: transparent; color: %1; font-size: 11px; font-weight: 600; "
                "letter-spacing: 1px; padding: 8px 12px; border: none; border-bottom: 2px solid transparent; }"
                "QPushButton:hover { color: %2; }"
                "QPushButton:checked { color: %2; border-bottom: 2px solid %2; }")
            .arg(ui::colors::TEXT_SECONDARY(), ui::colors::AMBER()));

    connect(btn, &QPushButton::clicked, this, [this, mode]() { set_view(mode); });
    return btn;
}

// ── Lazy panel construction ──────────────────────────────────────────────────

void AgentConfigScreen::ensure_panel_built(services::AgentViewMode mode) {
    const int idx = static_cast<int>(mode);
    if (panel_built_[idx])
        return;
    panel_built_[idx] = true;

    QWidget* panel = nullptr;
    switch (mode) {
        case services::AgentViewMode::Agents:
            agents_panel_ = new AgentsViewPanel;
            panel = agents_panel_;
            break;
        case services::AgentViewMode::Create:
            create_panel_ = new CreateAgentPanel;
            panel = create_panel_;
            break;
        case services::AgentViewMode::Teams:
            teams_panel_ = new TeamsViewPanel;
            panel = teams_panel_;
            break;
        case services::AgentViewMode::Workflows:
            workflows_panel_ = new WorkflowsViewPanel;
            panel = workflows_panel_;
            break;
        case services::AgentViewMode::Planner:
            planner_panel_ = new PlannerViewPanel;
            panel = planner_panel_;
            break;
        case services::AgentViewMode::Tools:
            tools_panel_ = new ToolsViewPanel;
            panel = tools_panel_;
            break;
        case services::AgentViewMode::Chat:
            chat_panel_ = new AgentChatPanel;
            panel = chat_panel_;
            break;
        case services::AgentViewMode::System:
            system_panel_ = new SystemViewPanel;
            panel = system_panel_;
            break;
        case services::AgentViewMode::Agentic:
            agentic_panel_ = new AgenticTasksPanel;
            panel = agentic_panel_;
            break;
    }

    // Add panel to stack — no placeholder removal needed, index tracked via pointer
    view_stack_->addWidget(panel);

    // Wire cross-panel signals now that relevant panels may be available
    wire_cross_panel_signals();

    LOG_INFO("AgentConfigScreen", QString("Built panel: %1").arg(kViews[idx].label));
}

QWidget* AgentConfigScreen::panel_widget(services::AgentViewMode mode) const {
    switch (mode) {
        case services::AgentViewMode::Agents:
            return agents_panel_;
        case services::AgentViewMode::Create:
            return create_panel_;
        case services::AgentViewMode::Teams:
            return teams_panel_;
        case services::AgentViewMode::Workflows:
            return workflows_panel_;
        case services::AgentViewMode::Planner:
            return planner_panel_;
        case services::AgentViewMode::Tools:
            return tools_panel_;
        case services::AgentViewMode::Chat:
            return chat_panel_;
        case services::AgentViewMode::System:
            return system_panel_;
        case services::AgentViewMode::Agentic:
            return agentic_panel_;
    }
    return nullptr;
}

void AgentConfigScreen::apply_agentic_visibility() {
    if (agentic_nav_idx_ < 0 || agentic_nav_idx_ >= nav_buttons_.size())
        return;
    nav_buttons_[agentic_nav_idx_]->setVisible(agentic_mode_enabled_);
    // If the user toggles agentic mode off while the panel is current, fall
    // back to the chat view so they don't sit on a hidden tab.
    if (!agentic_mode_enabled_ && current_view_ == services::AgentViewMode::Agentic)
        set_view(services::AgentViewMode::Chat);
}

void AgentConfigScreen::wire_cross_panel_signals() {
    // AGENTS → TEAMS: "Add to Team" button
    if (agents_panel_ && teams_panel_)
        connect(agents_panel_, &AgentsViewPanel::add_agent_to_team, teams_panel_, &TeamsViewPanel::add_agent_from_panel,
                Qt::UniqueConnection);

    // TOOLS → AGENTS: live selection propagation
    if (tools_panel_ && agents_panel_)
        connect(tools_panel_, &ToolsViewPanel::tools_selection_changed, agents_panel_,
                &AgentsViewPanel::apply_tools_selection, Qt::UniqueConnection);

    // TOOLS → AGENTS: reload agent tools from DB immediately after assign
    if (tools_panel_ && agents_panel_)
        connect(
            tools_panel_, &ToolsViewPanel::tool_assigned, agents_panel_,
            [this](const QString& /*id*/, const QStringList& tools) { agents_panel_->apply_tools_selection(tools); },
            Qt::UniqueConnection);

    // TOOLS → CREATE: selection propagation
    if (tools_panel_ && create_panel_)
        connect(tools_panel_, &ToolsViewPanel::tools_selection_changed, create_panel_,
                &CreateAgentPanel::apply_tools_selection, Qt::UniqueConnection);

    // TOOLS → TEAMS: selection propagation
    if (tools_panel_ && teams_panel_)
        connect(tools_panel_, &ToolsViewPanel::tools_selection_changed, teams_panel_,
                &TeamsViewPanel::apply_tools_selection, Qt::UniqueConnection);

    // SERVICE → SYSTEM: refresh when agents/tools change
    if (system_panel_) {
        connect(
            &services::AgentService::instance(), &services::AgentService::agents_discovered, system_panel_,
            [this](QVector<services::AgentInfo>, QVector<services::AgentCategory>) {
                system_panel_->on_agents_changed();
            },
            Qt::UniqueConnection);

        connect(
            &services::AgentService::instance(), &services::AgentService::tools_loaded, system_panel_,
            [this](services::AgentToolsInfo info) { system_panel_->on_tools_changed(info); }, Qt::UniqueConnection);

        // LLM config saved/deleted → refresh LLM list in System tab
        connect(
            &services::AgentService::instance(), &services::AgentService::config_saved, system_panel_,
            [this]() { system_panel_->on_llm_config_changed(); }, Qt::UniqueConnection);

        connect(
            &services::AgentService::instance(), &services::AgentService::config_deleted, system_panel_,
            [this]() { system_panel_->on_llm_config_changed(); }, Qt::UniqueConnection);
    }
}

// ── View switching ───────────────────────────────────────────────────────────

void AgentConfigScreen::set_view(services::AgentViewMode mode) {
    const int idx = static_cast<int>(mode);

    // Build the panel on first visit (P2: lazy construction)
    ensure_panel_built(mode);

    // Use setCurrentWidget — safe regardless of insertion order in stack
    QWidget* panel = panel_widget(mode);
    if (panel)
        view_stack_->setCurrentWidget(panel);
    current_view_ = mode;

    for (int i = 0; i < nav_buttons_.size(); ++i)
        nav_buttons_[i]->setChecked(i == idx);

    fincept::ScreenStateManager::instance().notify_changed(this);
}

// ── Service-level connections (global signals, no per-panel wiring) ──────────

void AgentConfigScreen::setup_service_connections() {
    auto& svc = services::AgentService::instance();

    connect(&svc, &services::AgentService::agents_discovered, this,
            [this](QVector<services::AgentInfo> agents, QVector<services::AgentCategory>) {
                last_agent_count_ = agents.size();
                agent_count_label_->setText(tr("%1 agents").arg(last_agent_count_));
            });

    connect(&svc, &services::AgentService::error_occurred, this, [this](const QString& ctx, const QString& msg) {
        status_label_->setText(tr("ERROR [%1]: %2").arg(ctx, msg.left(60)));
        status_label_->setStyleSheet(QString("color:%1;font-size:10px;").arg(ui::colors::NEGATIVE()));
    });

    connect(&svc, &services::AgentService::agent_result, this, [this](services::AgentExecutionResult r) {
        if (r.success) {
            status_label_->setText(tr("DONE (%1ms)").arg(r.execution_time_ms));
            status_label_->setStyleSheet(QString("color:%1;font-size:10px;").arg(ui::colors::POSITIVE()));
        } else {
            status_label_->setText(tr("FAILED: %1").arg(r.error.left(60)));
            status_label_->setStyleSheet(QString("color:%1;font-size:10px;").arg(ui::colors::NEGATIVE()));
        }
    });

    // Task 4 & 9: when an agent is saved/deleted in CREATE, refresh the agent
    // list so AGENTS tab and TEAMS available list stay in sync.
    connect(&svc, &services::AgentService::config_saved, this, []() {
        services::AgentService::instance().clear_cache();
        services::AgentService::instance().discover_agents();
    });
    connect(&svc, &services::AgentService::config_deleted, this, []() {
        services::AgentService::instance().clear_cache();
        services::AgentService::instance().discover_agents();
    });
}

// ── Visibility ───────────────────────────────────────────────────────────────

void AgentConfigScreen::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (first_show_) {
        first_show_ = false;
        // Build the default (AGENTS) panel and kick off discovery once
        ensure_panel_built(services::AgentViewMode::Agents);
        services::AgentService::instance().discover_agents();
    }
}

void AgentConfigScreen::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
}

// ── IStatefulScreen ──────────────────────────────────────────────────────────

QVariantMap AgentConfigScreen::save_state() const {
    QVariantMap state{{"view", static_cast<int>(current_view_)}};
    if (create_panel_)
        state["create_draft"] = create_panel_->save_draft();
    if (agents_panel_)
        state["agents_draft"] = agents_panel_->save_draft();
    if (workflows_panel_)
        state["workflows_draft"] = workflows_panel_->save_draft();
    return state;
}

void AgentConfigScreen::restore_state(const QVariantMap& state) {
    const int v = state.value("view", -1).toInt();
    if (v < 0)
        return;
    set_view(static_cast<services::AgentViewMode>(v));
    if (create_panel_ && state.contains("create_draft"))
        create_panel_->restore_draft(state.value("create_draft").toMap());
    if (agents_panel_ && state.contains("agents_draft"))
        agents_panel_->restore_draft(state.value("agents_draft").toMap());
    if (workflows_panel_ && state.contains("workflows_draft"))
        workflows_panel_->restore_draft(state.value("workflows_draft").toMap());
}

} // namespace fincept::screens
