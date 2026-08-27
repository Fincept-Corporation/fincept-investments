// src/services/agents/AgentService_Discovery.cpp
//
// Agent / tool / model discovery + active-config administration. discover_agents
// populates AgentService's cached registry from the Python side; get_system_info,
// list_tools, list_models and the save_/delete_/set_active_config methods round
// out the read/edit surface for AgentConfigManager-backed records.
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

void AgentService::discover_agents() {
    {
        const QVariant cv = fincept::CacheManager::instance().get("agents:list");
        if (!cv.isNull()) {
            const QJsonObject root = QJsonDocument::fromJson(cv.toString().toUtf8()).object();
            if (root["agents"].toArray().isEmpty()) {
                fincept::CacheManager::instance().remove("agents:list");
                LOG_INFO("AgentService", "Discarding empty agents cache — re-discovering");
            } else {
                LOG_DEBUG("AgentService", "agents: serving from cache");
                QVector<AgentInfo> agents;
                for (const auto& v : root["agents"].toArray()) {
                    const QJsonObject o = v.toObject();
                    AgentInfo info;
                    info.id = o["id"].toString();
                    info.name = o["name"].toString();
                    info.description = o["description"].toString();
                    info.category = o["category"].toString();
                    info.provider = o["provider"].toString();
                    info.version = o["version"].toString();
                    info.config = o["config"].toObject();
                    for (const auto& c : o["capabilities"].toArray())
                        info.capabilities.append(c.toString());
                    agents.append(info);
                }
                QVector<AgentCategory> categories;
                for (const auto& v : root["categories"].toArray()) {
                    const QJsonObject o = v.toObject();
                    categories.append({o["name"].toString(), o["count"].toInt()});
                }
                emit agents_discovered(agents, categories);
                return;
            }
        }
    }

    LOG_INFO("AgentService", "Discovering agents from Python");
    QPointer<AgentService> self = this;

    run_python_light("discover_agents", {}, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        if (!ok) {
            emit self->error_occurred("discover_agents", result["error"].toString());
            return;
        }

        QVector<AgentInfo> agents;
        QMap<QString, int> cat_counts;

        QJsonArray arr = result["agents"].toArray();
        if (arr.isEmpty()) {
            // Try alternative response format
            arr = result["data"].toArray();
        }

        for (const auto& v : arr) {
            QJsonObject o = v.toObject();
            AgentInfo info;
            info.id = o["id"].toString();
            info.name = o["name"].toString();
            info.description = o["description"].toString();
            info.category = o["category"].toString("general");
            info.provider = o["provider"].toString();
            info.version = o["version"].toString("1.0.0");
            info.config = o["config"].toObject();

            QJsonArray caps = o["capabilities"].toArray();
            for (const auto& c : caps)
                info.capabilities.append(c.toString());

            agents.append(info);
            cat_counts[info.category]++;
        }

        // Also merge DB-saved custom agents
        auto db_agents = AgentConfigRepository::instance().list_all();
        if (db_agents.is_ok()) {
            for (const auto& dba : db_agents.value()) {
                // Skip if already discovered by Python
                bool found = false;
                for (const auto& a : agents) {
                    if (a.id == dba.id) {
                        found = true;
                        break;
                    }
                }
                if (found)
                    continue;

                AgentInfo info;
                info.id = dba.id;
                info.name = dba.name;
                info.description = dba.description;
                info.category = dba.category;
                info.provider = "custom";
                info.version = "1.0.0";

                QJsonDocument doc = QJsonDocument::fromJson(dba.config_json.toUtf8());
                if (!doc.isNull())
                    info.config = doc.object();

                agents.append(info);
                cat_counts[info.category]++;
            }
        }

        QVector<AgentCategory> categories;
        for (auto it = cat_counts.begin(); it != cat_counts.end(); ++it) {
            categories.append({it.key(), it.value()});
        }

        QJsonArray agents_arr;
        for (const auto& a : agents) {
            QJsonObject o;
            o["id"] = a.id;
            o["name"] = a.name;
            o["description"] = a.description;
            o["category"] = a.category;
            o["provider"] = a.provider;
            o["version"] = a.version;
            o["config"] = a.config;
            QJsonArray caps;
            for (const auto& c : a.capabilities)
                caps.append(c);
            o["capabilities"] = caps;
            agents_arr.append(o);
        }
        QJsonArray cats_arr;
        for (const auto& c : categories) {
            QJsonObject o;
            o["name"] = c.name;
            o["count"] = c.count;
            cats_arr.append(o);
        }
        QJsonObject root;
        root["agents"] = agents_arr;
        root["categories"] = cats_arr;
        fincept::CacheManager::instance().put(
            "agents:list", QVariant(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact))),
            kAgentCacheTtlSec, "agents");

        LOG_INFO("AgentService",
                 QString("Discovered %1 agents in %2 categories").arg(agents.size()).arg(categories.size()));
        emit self->agents_discovered(agents, categories);
    });
}

// ── Agent execution ──────────────────────────────────────────────────────────

void AgentService::get_system_info() {
    {
        const QVariant cv = fincept::CacheManager::instance().get("agents:sysinfo");
        if (!cv.isNull()) {
            const QJsonObject r = QJsonDocument::fromJson(cv.toString().toUtf8()).object();
            AgentSystemInfo info;
            info.version = r["version"].toString();
            info.framework = r["framework"].toString();
            info.capabilities = r["capabilities"].toObject();
            for (const auto& f : r["features"].toArray())
                info.features.append(f.toString());
            emit system_info_loaded(info);
            return;
        }
    }

    QPointer<AgentService> self = this;
    run_python_light("system_info", {}, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        if (!ok) {
            emit self->error_occurred("system_info", result["error"].toString());
            return;
        }

        AgentSystemInfo info;
        info.version = result["version"].toString();
        info.framework = result["framework"].toString();
        info.capabilities = result["capabilities"].toObject();
        for (const auto& f : result["features"].toArray())
            info.features.append(f.toString());

        fincept::CacheManager::instance().put(
            "agents:sysinfo", QVariant(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact))),
            kSysInfoCacheTtlSec, "agents");

        emit self->system_info_loaded(info);
    });
}

void AgentService::list_tools() {
    {
        const QVariant cv = fincept::CacheManager::instance().get("agents:tools");
        if (!cv.isNull()) {
            const QJsonObject r = QJsonDocument::fromJson(cv.toString().toUtf8()).object();
            AgentToolsInfo info;
            info.tools = r["tools"].toObject();
            info.total_count = r["total_count"].toInt();
            for (const auto& c : r["categories"].toArray())
                info.categories.append(c.toString());
            emit tools_loaded(info);
            return;
        }
    }

    QPointer<AgentService> self = this;
    run_python_light("list_tools", {}, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        if (!ok) {
            emit self->error_occurred("list_tools", result["error"].toString());
            return;
        }

        AgentToolsInfo info;
        info.tools = result["tools"].toObject();
        info.total_count = result["total_count"].toInt();
        for (const auto& c : result["categories"].toArray())
            info.categories.append(c.toString());

        fincept::CacheManager::instance().put(
            "agents:tools", QVariant(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact))),
            kToolsCacheTtlSec, "agents");

        emit self->tools_loaded(info);
    });
}

void AgentService::list_models() {
    {
        const QVariant cv = fincept::CacheManager::instance().get("agents:models");
        if (!cv.isNull()) {
            const QJsonObject r = QJsonDocument::fromJson(cv.toString().toUtf8()).object();
            AgentModelsInfo info;
            info.count = r["count"].toInt();
            for (const auto& p : r["providers"].toArray())
                info.providers.append(p.toString());
            emit models_loaded(info);
            return;
        }
    }

    QPointer<AgentService> self = this;
    run_python_light("list_models", {}, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        if (!ok) {
            emit self->error_occurred("list_models", result["error"].toString());
            return;
        }

        AgentModelsInfo info;
        info.count = result["count"].toInt();
        for (const auto& p : result["providers"].toArray())
            info.providers.append(p.toString());

        fincept::CacheManager::instance().put(
            "agents:models", QVariant(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact))),
            kModelsCacheTtlSec, "agents");

        emit self->models_loaded(info);
    });
}

// ── Config CRUD ──────────────────────────────────────────────────────────────

void AgentService::save_config(const AgentConfig& config) {
    auto result = AgentConfigRepository::instance().save(config);
    if (result.is_ok()) {
        LOG_INFO("AgentService", QString("Saved agent config: %1").arg(config.name));
        emit config_saved();
    } else {
        emit error_occurred("save_config", QString::fromStdString(result.error()));
    }
}

void AgentService::delete_config(const QString& id) {
    auto result = AgentConfigRepository::instance().remove(id);
    if (result.is_ok()) {
        LOG_INFO("AgentService", QString("Deleted agent config: %1").arg(id));
        emit config_deleted();
    } else {
        emit error_occurred("delete_config", QString::fromStdString(result.error()));
    }
}

void AgentService::set_active_config(const QString& id) {
    auto result = AgentConfigRepository::instance().set_active(id);
    if (result.is_ok()) {
        LOG_INFO("AgentService", QString("Set active agent: %1").arg(id));
    } else {
        emit error_occurred("set_active_config", QString::fromStdString(result.error()));
    }
}

// ── Structured output ────────────────────────────────────────────────────────

} // namespace fincept::services
