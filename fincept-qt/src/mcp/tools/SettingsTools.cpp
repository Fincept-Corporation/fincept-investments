// SettingsTools.cpp — Settings and LLM config management (Qt port)

#include "mcp/tools/SettingsTools.h"

#include "core/events/EventBus.h"
#include "core/logging/Logger.h"
#include "mcp/ToolSchemaBuilder.h"
#include "storage/repositories/LlmConfigRepository.h"
#include "storage/repositories/SettingsRepository.h"

#include <QVariantMap>

namespace fincept::mcp::tools {

static constexpr const char* TAG = "SettingsTools";

std::vector<ToolDef> get_settings_tools() {
    std::vector<ToolDef> tools;

    // ── get_setting ─────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "get_setting";
        t.description = "Get an application setting by key.";
        t.category = "settings";
        t.input_schema.properties =
            QJsonObject{{"key", QJsonObject{{"type", "string"}, {"description", "Setting key to retrieve"}}}};
        t.input_schema.required = {"key"};
        t.handler = [](const QJsonObject& args) -> ToolResult {
            QString key = args["key"].toString();
            if (key.isEmpty())
                return ToolResult::fail("Missing 'key'");

            auto result = SettingsRepository::instance().get(key);
            if (result.is_err())
                return ToolResult::fail("Setting not found: " + key);

            return ToolResult::ok_data(QJsonObject{{"key", key}, {"value", result.value()}});
        };
        tools.push_back(std::move(t));
    }

    // ── set_setting ─────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "set_setting";
        t.description = "Set an application setting.";
        t.category = "settings";
        // Phase 6.3: mutates persisted state (could touch API keys / billing).
        t.is_destructive = true;
        t.input_schema.properties = QJsonObject{
            {"key", QJsonObject{{"type", "string"}, {"description", "Setting key"}}},
            {"value", QJsonObject{{"type", "string"}, {"description", "Setting value"}}},
            {"category", QJsonObject{{"type", "string"}, {"description", "Setting category (default: general)"}}}};
        t.input_schema.required = {"key", "value"};
        t.handler = [](const QJsonObject& args) -> ToolResult {
            QString key = args["key"].toString();
            QString value = args["value"].toString();
            QString cat = args["category"].toString("general");

            if (key.isEmpty())
                return ToolResult::fail("Missing 'key'");

            auto r = SettingsRepository::instance().set(key, value, cat);
            if (r.is_err())
                return ToolResult::fail("Failed to save setting: " + QString::fromStdString(r.error()));

            EventBus::instance().publish("settings.changed", QVariantMap{{"key", key}, {"value", value}});

            LOG_INFO(TAG, "Setting saved: " + key + " = " + value);
            return ToolResult::ok("Setting saved: " + key);
        };
        tools.push_back(std::move(t));
    }

    // ── get_all_settings ────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "get_all_settings";
        t.description = "Get all application settings.";
        t.category = "settings";
        t.handler = [](const QJsonObject&) -> ToolResult {
            // Retrieve all settings by getting each common category
            QJsonArray result;
            const QStringList categories = {"general", "ui", "trading", "market", "network"};
            for (const auto& cat : categories) {
                auto items = SettingsRepository::instance().get_by_category(cat);
                if (items.is_ok()) {
                    for (const auto& s : items.value()) {
                        result.append(QJsonObject{{"key", s.key},
                                                  {"value", s.value},
                                                  {"category", s.category},
                                                  {"updated_at", s.updated_at}});
                    }
                }
            }
            return ToolResult::ok_data(result);
        };
        tools.push_back(std::move(t));
    }

    // ── get_llm_configs ─────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "get_llm_configs";
        t.description = "Get all configured LLM providers and their settings (API keys are not exposed).";
        t.category = "settings";
        t.handler = [](const QJsonObject&) -> ToolResult {
            auto configs = LlmConfigRepository::instance().list_providers();
            if (configs.is_err())
                return ToolResult::fail("Failed to load LLM configs: " + QString::fromStdString(configs.error()));

            QJsonArray result;
            for (const auto& c : configs.value()) {
                result.append(QJsonObject{{"provider", c.provider},
                                          {"model", c.model},
                                          {"is_active", c.is_active},
                                          {"has_api_key", !c.api_key.isEmpty()},
                                          {"base_url", c.base_url}});
            }
            return ToolResult::ok_data(result);
        };
        tools.push_back(std::move(t));
    }

    // ── set_active_llm ──────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "set_active_llm";
        t.description = "Set the active LLM provider. Must be one of the supported providers.";
        t.category = "settings";
        // Phase 6.3: switching LLM provider mid-conversation is surprising;
        // requires explicit confirmation.
        t.is_destructive = true;
        t.input_schema = ToolSchemaBuilder()
                             .string("provider", "Provider id (openai, anthropic, ollama, groq, google)")
                             .required()
                             .enums({"openai", "anthropic", "ollama", "groq", "google"})
                             .build();
        t.handler = [](const QJsonObject& args) -> ToolResult {
            QString provider = args["provider"].toString();
            if (provider.isEmpty())
                return ToolResult::fail("Missing 'provider'");

            auto r = LlmConfigRepository::instance().set_active(provider);
            if (r.is_err())
                return ToolResult::fail("Failed to set active LLM: " + QString::fromStdString(r.error()));

            EventBus::instance().publish("llm.provider_changed", QVariantMap{{"provider", provider}});

            return ToolResult::ok("Active LLM set to: " + provider);
        };
        tools.push_back(std::move(t));
    }

    return tools;
}

} // namespace fincept::mcp::tools
