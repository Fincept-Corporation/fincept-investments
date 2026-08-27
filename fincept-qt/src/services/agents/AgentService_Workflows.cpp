// src/services/agents/AgentService_Workflows.cpp
//
// Higher-level orchestrations: run_workflow, create_plan / execute_plan,
// portfolio analysis helpers (stock / rebalancing / risk / generic), and the
// canned plan factories.
//
// Part of the partial-class split of AgentService.cpp.

#include "core/logging/Logger.h"
#include "datahub/DataHub.h"
#include "datahub/TopicPolicy.h"
#include "mcp/McpProvider.h"
#include "mcp/McpTypes.h"
#include "mcp/TerminalMcpBridge.h"
#include "python/PythonRunner.h"
#include "services/agents/AgentService.h"
#include "services/llm/LlmService.h"
#include "storage/cache/CacheManager.h"
#include "storage/repositories/LlmConfigRepository.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QProcess>
#include <QUuid>
#include <QVariant>

#include <memory>

namespace fincept::services {

QString AgentService::run_workflow(const QString& workflow_type, const QJsonObject& params) {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    LOG_INFO("AgentService", QString("Running workflow [%1]: %2").arg(req_id.left(8), workflow_type));

    QJsonObject p = params;
    p["workflow_type"] = workflow_type;

    QPointer<AgentService> self = this;
    run_python_stdin(workflow_type, p, {}, [self, req_id](bool ok, QJsonObject result) {
        if (!self)
            return;
        AgentExecutionResult r;
        r.request_id = req_id;
        r.success = ok && result["success"].toBool(ok);
        r.execution_time_ms = result["execution_time_ms"].toInt();
        r.response = result.contains("response") ? result["response"].toString()
                                                 : QJsonDocument(result).toJson(QJsonDocument::Indented);
        r.error = result["error"].toString();
        emit self->agent_result(r);
        self->publish_agent_result(r, /*final=*/true);
    });
    return req_id;
}

// ── Planner ──────────────────────────────────────────────────────────────────

QString AgentService::create_plan(const QString& query, const QJsonObject& config) {
    LOG_INFO("AgentService", QString("Creating plan for: %1").arg(query.left(80)));

    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject params;
    params["query"] = query;
    if (!config.isEmpty())
        for (auto it = config.begin(); it != config.end(); ++it)
            params[it.key()] = it.value();

    QPointer<AgentService> self = this;
    run_python_stdin("generate_dynamic_plan", params, {}, [self, req_id](bool ok, QJsonObject result) {
        if (!self)
            return;
        if (!ok) {
            emit self->error_occurred("create_plan", result["error"].toString());
            return;
        }

        // Python returns {"success": true, "plan": {...}} — unwrap the nested plan object
        const QJsonObject planObj = result.contains("plan") ? result["plan"].toObject() : result;

        ExecutionPlan plan;
        plan.request_id = req_id;
        plan.id = planObj["id"].toString();
        plan.name = planObj["name"].toString();
        plan.description = planObj["description"].toString();
        plan.status = planObj["status"].toString("pending");
        plan.is_complete = planObj["is_complete"].toBool();

        QJsonArray steps = planObj["steps"].toArray();
        for (const auto& sv : steps) {
            QJsonObject so = sv.toObject();
            PlanStep step;
            step.id = so["id"].toString();
            step.name = so["name"].toString();
            step.step_type = so["step_type"].toString();
            step.config = so["config"].toObject();
            step.status = so["status"].toString("pending");
            QJsonArray deps = so["dependencies"].toArray();
            for (const auto& d : deps)
                step.dependencies.append(d.toString());
            plan.steps.append(step);
        }

        emit self->plan_created(plan);
    });
    return req_id;
}

QString AgentService::execute_plan(const QJsonObject& plan, const QJsonObject& config) {
    LOG_INFO("AgentService", "Executing plan");

    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject params;
    params["plan"] = plan;
    if (!config.isEmpty())
        for (auto it = config.begin(); it != config.end(); ++it)
            params[it.key()] = it.value();

    QPointer<AgentService> self = this;
    run_python_stdin("execute_plan", params, {}, [self, req_id](bool ok, QJsonObject result) {
        if (!self)
            return;
        if (!ok) {
            emit self->error_occurred("execute_plan", result["error"].toString());
            return;
        }

        ExecutionPlan plan;
        plan.request_id = req_id;
        plan.id = result["id"].toString();
        plan.status = result["status"].toString();
        plan.is_complete = result["is_complete"].toBool();
        plan.has_failed = result["has_failed"].toBool();

        QJsonArray steps = result["steps"].toArray();
        for (const auto& sv : steps) {
            QJsonObject so = sv.toObject();
            PlanStep step;
            step.id = so["id"].toString();
            step.name = so["name"].toString();
            step.status = so["status"].toString();
            step.result = so["result"].toString();
            step.error = so["error"].toString();
            plan.steps.append(step);
        }

        emit self->plan_executed(plan);
    });
    return req_id;
}

// ── System info ──────────────────────────────────────────────────────────────

void AgentService::run_stock_analysis(const QString& symbol, const QJsonObject& config) {
    LOG_INFO("AgentService", QString("Stock analysis: %1").arg(symbol));
    QJsonObject params;
    params["symbol"] = symbol;

    QPointer<AgentService> self = this;
    run_python_stdin("stock_analysis", params, config, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        AgentExecutionResult r;
        r.success = ok && result["success"].toBool(ok);
        r.execution_time_ms = result["execution_time_ms"].toInt();
        r.response = result.contains("response") ? result["response"].toString()
                                                 : QJsonDocument(result).toJson(QJsonDocument::Indented);
        r.error = result["error"].toString();
        emit self->agent_result(r);
        self->publish_agent_result(r, /*final=*/true);
    });
}

void AgentService::run_portfolio_rebalancing(const QJsonObject& portfolio_data) {
    LOG_INFO("AgentService", "Portfolio rebalancing");
    QJsonObject params;
    if (!portfolio_data.isEmpty())
        params["portfolio_data"] = portfolio_data;

    QPointer<AgentService> self = this;
    run_python_stdin("portfolio_rebal", params, {}, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        AgentExecutionResult r;
        r.success = ok && result["success"].toBool(ok);
        r.execution_time_ms = result["execution_time_ms"].toInt();
        r.response = result.contains("response") ? result["response"].toString()
                                                 : QJsonDocument(result).toJson(QJsonDocument::Indented);
        r.error = result["error"].toString();
        emit self->agent_result(r);
        self->publish_agent_result(r, /*final=*/true);
    });
}

void AgentService::run_risk_assessment(const QJsonObject& portfolio_data) {
    LOG_INFO("AgentService", "Risk assessment");
    QJsonObject params;
    if (!portfolio_data.isEmpty())
        params["portfolio_data"] = portfolio_data;

    QPointer<AgentService> self = this;
    run_python_stdin("risk_assessment", params, {}, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        AgentExecutionResult r;
        r.success = ok && result["success"].toBool(ok);
        r.execution_time_ms = result["execution_time_ms"].toInt();
        r.response = result.contains("response") ? result["response"].toString()
                                                 : QJsonDocument(result).toJson(QJsonDocument::Indented);
        r.error = result["error"].toString();
        emit self->agent_result(r);
        self->publish_agent_result(r, /*final=*/true);
    });
}

void AgentService::run_portfolio_analysis(const QString& analysis_type, const QJsonObject& portfolio_summary) {
    LOG_INFO("AgentService", QString("Portfolio analysis: %1").arg(analysis_type));
    QJsonObject params;
    params["analysis_type"] = analysis_type;
    if (!portfolio_summary.isEmpty())
        params["portfolio_summary"] = portfolio_summary;

    // finagent_core's "run" action requires a non-empty `query`. Synthesize
    // one from the analysis type and include the portfolio context so the
    // agent has something to answer even if it ignores `portfolio_summary`.
    QString query;
    if (analysis_type == "risk")
        query = "Analyze the risk of this portfolio — concentration, correlation, and downside scenarios. "
                "Quantify exposures where possible.";
    else if (analysis_type == "rebalance")
        query = "Suggest rebalancing actions for this portfolio with specific weight targets and rationale. "
                "Call out sectors that are over- or under-weight.";
    else if (analysis_type == "opportunities")
        query = "Identify opportunities in this portfolio — undervalued positions, missing sectors, and potential "
                "additions. Prefer concrete candidate tickers.";
    else
        query = "Provide a comprehensive analysis of this portfolio with actionable recommendations. "
                "Organize the output with clear headings.";

    const QString ctx = portfolio_summary.value("context_text").toString();
    if (!ctx.isEmpty())
        query = ctx + "\n\nTask: " + query;
    params["query"] = query;

    QPointer<AgentService> self = this;
    run_python_stdin("run", params, {}, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        AgentExecutionResult r;
        r.success = ok && result["success"].toBool(ok);
        r.execution_time_ms = result["execution_time_ms"].toInt();
        // finagent_core emits the response under one of several keys depending
        // on the underlying agent (same spread handled by run_agent). Mirror
        // that extraction order so callers always get usable text.
        if (result.contains("response") && !result["response"].toString().isEmpty())
            r.response = result["response"].toString();
        else if (result.contains("result") && !result["result"].toString().isEmpty())
            r.response = result["result"].toString();
        else if (result.contains("data"))
            r.response = QJsonDocument(result["data"].toObject()).toJson(QJsonDocument::Indented);
        else
            r.response = QJsonDocument(result).toJson(QJsonDocument::Indented);
        r.error = result["error"].toString();
        emit self->agent_result(r);
        self->publish_agent_result(r, /*final=*/true);
    });
}

// ── Plan variants ────────────────────────────────────────────────────────────

QString AgentService::create_stock_analysis_plan(const QString& symbol, const QJsonObject& /*config*/) {
    LOG_INFO("AgentService", QString("Stock analysis plan: %1").arg(symbol));
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject params;
    params["symbol"] = symbol;

    QPointer<AgentService> self = this;
    run_python_stdin("create_stock_plan", params, {}, [self, req_id](bool ok, QJsonObject result) {
        if (!self)
            return;
        if (!ok) {
            emit self->error_occurred("create_stock_plan", result["error"].toString());
            return;
        }
        ExecutionPlan plan;
        plan.request_id = req_id;
        plan.id = result["id"].toString();
        plan.name = result["name"].toString();
        plan.status = result["status"].toString("pending");
        QJsonArray steps = result["steps"].toArray();
        for (const auto& sv : steps) {
            QJsonObject so = sv.toObject();
            PlanStep step;
            step.id = so["id"].toString();
            step.name = so["name"].toString();
            step.step_type = so["step_type"].toString();
            step.config = so["config"].toObject();
            step.status = "pending";
            plan.steps.append(step);
        }
        emit self->plan_created(plan);
    });
    return req_id;
}

QString AgentService::create_portfolio_plan(const QJsonObject& goals, const QJsonObject& /*config*/) {
    LOG_INFO("AgentService", "Portfolio plan");
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject params;
    if (!goals.isEmpty())
        params["goals"] = goals;

    QPointer<AgentService> self = this;
    run_python_stdin("create_portfolio_plan", params, {}, [self, req_id](bool ok, QJsonObject result) {
        if (!self)
            return;
        if (!ok) {
            emit self->error_occurred("create_portfolio_plan", result["error"].toString());
            return;
        }
        ExecutionPlan plan;
        plan.request_id = req_id;
        plan.id = result["id"].toString();
        plan.name = result["name"].toString();
        plan.status = result["status"].toString("pending");
        QJsonArray steps = result["steps"].toArray();
        for (const auto& sv : steps) {
            QJsonObject so = sv.toObject();
            PlanStep step;
            step.id = so["id"].toString();
            step.name = so["name"].toString();
            step.step_type = so["step_type"].toString();
            step.config = so["config"].toObject();
            step.status = "pending";
            plan.steps.append(step);
        }
        emit self->plan_created(plan);
    });
    return req_id;
}

// ── Memory & Knowledge ───────────────────────────────────────────────────────

} // namespace fincept::services
