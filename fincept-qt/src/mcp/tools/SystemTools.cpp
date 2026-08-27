// SystemTools.cpp — Auth status, cache, app info (Qt port)

#include "mcp/tools/SystemTools.h"

#include "core/HealthMonitor.h"
#include "core/logging/Logger.h"
#include "mcp/McpProvider.h"
#include "python/PythonRunner.h"
#include "storage/cache/CacheManager.h"

// FINCEPT_VERSION_STRING is injected by CMake from CMAKE_PROJECT_VERSION.
// Fallback mirrors main.cpp so dev builds without the compile-definition
// still produce something parseable instead of failing to compile.
#ifndef FINCEPT_VERSION_STRING
#    define FINCEPT_VERSION_STRING "0.0.0-dev"
#endif

namespace fincept::mcp::tools {

std::vector<ToolDef> get_system_tools() {
    std::vector<ToolDef> tools;

    // ── get_cache_stats ────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "get_cache_stats";
        t.description = "Get cache statistics: total number of cached entries.";
        t.category = "system";
        t.handler = [](const QJsonObject&) -> ToolResult {
            return ToolResult::ok_data(QJsonObject{{"total_entries", CacheManager::instance().entry_count()}});
        };
        tools.push_back(std::move(t));
    }

    // ── clear_cache ────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "clear_cache";
        t.description = "Clear all cache entries.";
        t.category = "system";
        t.is_destructive = true; // mutation tool — penalise on read-style queries
        t.handler = [](const QJsonObject&) -> ToolResult {
            CacheManager::instance().clear();
            return ToolResult::ok("Cache cleared");
        };
        tools.push_back(std::move(t));
    }

    // ── get_app_info ───────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "get_app_info";
        t.description = "Get application version, platform, number of MCP tools, and Python availability.";
        t.category = "system";
        t.handler = [](const QJsonObject&) -> ToolResult {
            return ToolResult::ok_data(
                QJsonObject{{"version", QString::fromUtf8(FINCEPT_VERSION_STRING)},
                            {"platform",
#ifdef _WIN32
                             "windows"
#elif defined(__APPLE__)
                             "macos"
#else
                             "linux"
#endif
                            },
                            {"internal_tools", static_cast<int>(McpProvider::instance().tool_count())},
                            {"python_available", python::PythonRunner::instance().is_available()}});
        };
        tools.push_back(std::move(t));
    }

    // ── system_health_check ────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "system_health_check";
        t.description = "Run local subsystem health checks (broker connections, "
                        "WebSocket streams, Python pool, DataHub) and return a "
                        "snapshot with an overall all_ok flag and per-check details.";
        t.category = "system";
        t.handler = [](const QJsonObject&) -> ToolResult {
            return ToolResult::ok_data(HealthMonitor::instance().check().to_json());
        };
        tools.push_back(std::move(t));
    }

    return tools;
}

} // namespace fincept::mcp::tools
