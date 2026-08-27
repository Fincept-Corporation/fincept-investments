// src/services/agents/AgentService_Repositories.cpp
//
// Persistence-facing methods: memory + session repositories, paper-trading
// portfolio/positions, and the trade-decision log. Thin wrappers over the
// Python-side stores reached through the standard run_python_* path.
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

void AgentService::store_memory(const QString& content, const QString& memory_type, const QJsonObject& metadata,
                                const QString& agent_id) {
    QJsonObject params;
    params["content"] = content;
    params["memory_type"] = memory_type;
    if (!metadata.isEmpty())
        params["metadata"] = metadata;
    if (!agent_id.isEmpty())
        params["agent_id"] = agent_id;

    QPointer<AgentService> self = this;
    run_python_stdin("store_memory", params, {}, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        emit self->memory_stored(ok, ok ? "Memory stored" : result["error"].toString());
    });
}

void AgentService::recall_memories(const QString& query, const QString& memory_type, int limit,
                                   const QString& agent_id) {
    QJsonObject params;
    params["query"] = query;
    if (!memory_type.isEmpty())
        params["memory_type"] = memory_type;
    params["limit"] = limit;
    if (!agent_id.isEmpty())
        params["agent_id"] = agent_id;

    QPointer<AgentService> self = this;
    run_python_stdin("recall_memories", params, {}, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        if (ok)
            emit self->memories_recalled(result["memories"].toArray());
        else
            emit self->error_occurred("recall_memories", result["error"].toString());
    });
}

void AgentService::search_knowledge(const QString& query, int limit) {
    QJsonObject params;
    params["query"] = query;
    params["limit"] = limit;

    QPointer<AgentService> self = this;
    run_python_stdin("search_knowledge", params, {}, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        if (ok)
            emit self->knowledge_results(result["results"].toArray());
        else
            emit self->error_occurred("search_knowledge", result["error"].toString());
    });
}

void AgentService::save_memory_repo(const QString& content, const QString& agent_id, const QJsonObject& options) {
    QJsonObject params;
    params["content"] = content;
    if (!agent_id.isEmpty())
        params["agent_id"] = agent_id;
    if (!options.isEmpty())
        params["options"] = options;

    QPointer<AgentService> self = this;
    run_python_stdin("save_memory", params, {}, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        emit self->memory_stored(ok, ok ? "Memory saved to repository" : result["error"].toString());
    });
}

void AgentService::search_memories_repo(const QString& query, const QString& agent_id, int limit) {
    QJsonObject params;
    params["query"] = query;
    if (!agent_id.isEmpty())
        params["agent_id"] = agent_id;
    params["limit"] = limit;

    QPointer<AgentService> self = this;
    run_python_stdin("search_memories", params, {}, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        if (ok)
            emit self->memories_recalled(result["memories"].toArray());
        else
            emit self->error_occurred("search_memories", result["error"].toString());
    });
}

// ── Session management ───────────────────────────────────────────────────────

void AgentService::save_session(const QJsonObject& session_data) {
    QJsonObject params = session_data;
    QPointer<AgentService> self = this;
    run_python_stdin("save_session", params, {}, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        emit self->session_saved(ok && result["success"].toBool(ok));
    });
}

void AgentService::get_session(const QString& session_id) {
    QJsonObject params;
    params["session_id"] = session_id;

    QPointer<AgentService> self = this;
    run_python_stdin("get_session", params, {}, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        if (ok)
            emit self->session_loaded(result);
        else
            emit self->error_occurred("get_session", result["error"].toString());
    });
}

void AgentService::add_session_message(const QString& session_id, const QString& role, const QString& content) {
    QJsonObject params;
    params["session_id"] = session_id;
    params["role"] = role;
    params["content"] = content;

    QPointer<AgentService> self = this;
    run_python_stdin("add_message", params, {}, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        emit self->session_saved(ok && result["success"].toBool(ok));
    });
}

// ── Paper trading ────────────────────────────────────────────────────────────

void AgentService::paper_execute_trade(const QString& portfolio_id, const QString& symbol, const QString& action,
                                       double quantity, double price) {
    LOG_INFO("AgentService", QString("Paper trade: %1 %2 %3").arg(action, symbol).arg(quantity));
    QJsonObject params;
    params["portfolio_id"] = portfolio_id;
    params["symbol"] = symbol;
    params["action"] = action;
    params["quantity"] = quantity;
    params["price"] = price;

    QPointer<AgentService> self = this;
    run_python_stdin("paper_execute_trade", params, {}, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        if (ok)
            emit self->trade_executed(result);
        else
            emit self->error_occurred("paper_trade", result["error"].toString());
    });
}

void AgentService::paper_get_portfolio(const QString& portfolio_id) {
    QJsonObject params;
    params["portfolio_id"] = portfolio_id;

    QPointer<AgentService> self = this;
    run_python_stdin("paper_get_portfolio", params, {}, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        if (ok)
            emit self->trade_executed(result);
        else
            emit self->error_occurred("paper_portfolio", result["error"].toString());
    });
}

void AgentService::paper_get_positions(const QString& portfolio_id) {
    QJsonObject params;
    params["portfolio_id"] = portfolio_id;

    QPointer<AgentService> self = this;
    run_python_stdin("paper_get_positions", params, {}, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        if (ok)
            emit self->trade_executed(result);
        else
            emit self->error_occurred("paper_positions", result["error"].toString());
    });
}

// ── Trade decisions ──────────────────────────────────────────────────────────

void AgentService::save_trade_decision(const QJsonObject& decision) {
    QPointer<AgentService> self = this;
    run_python_stdin("save_trade_decision", decision, {}, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        if (!ok)
            emit self->error_occurred("save_trade_decision", result["error"].toString());
    });
}

void AgentService::get_trade_decisions(const QString& competition_id, const QString& model_name) {
    QJsonObject params;
    if (!competition_id.isEmpty())
        params["competition_id"] = competition_id;
    if (!model_name.isEmpty())
        params["model_name"] = model_name;

    QPointer<AgentService> self = this;
    run_python_stdin("get_decisions", params, {}, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        if (ok)
            emit self->trade_decisions_loaded(result["decisions"].toArray());
        else
            emit self->error_occurred("get_decisions", result["error"].toString());
    });
}

// ── LRU Response Cache ───────────────────────────────────────────────────────

} // namespace fincept::services
