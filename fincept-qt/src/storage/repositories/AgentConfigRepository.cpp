#include "storage/repositories/AgentConfigRepository.h"


namespace fincept {

AgentConfigRepository& AgentConfigRepository::instance() {
    static AgentConfigRepository s;
    return s;
}

AgentConfig AgentConfigRepository::map_row(QSqlQuery& q) {
    return {q.value(0).toString(), q.value(1).toString(), q.value(2).toString(),
            q.value(3).toString(), q.value(4).toString(), q.value(5).toBool(),
            q.value(6).toBool(),   q.value(7).toString(), q.value(8).toString()};
}

static const char* kCols =
    "id, name, description, config_json, category, is_default, is_active, created_at, updated_at";

Result<QVector<AgentConfig>> AgentConfigRepository::list_all() {
    return query_list(QString("SELECT %1 FROM agent_configs ORDER BY name").arg(kCols), {}, map_row);
}

Result<QVector<AgentConfig>> AgentConfigRepository::list_by_category(const QString& category) {
    return query_list(QString("SELECT %1 FROM agent_configs WHERE category = ? ORDER BY name").arg(kCols), {category},
                      map_row);
}

Result<AgentConfig> AgentConfigRepository::get(const QString& id) {
    return query_one(QString("SELECT %1 FROM agent_configs WHERE id = ?").arg(kCols), {id}, map_row);
}

Result<AgentConfig> AgentConfigRepository::get_active() {
    return query_one(QString("SELECT %1 FROM agent_configs WHERE is_active = 1 LIMIT 1").arg(kCols), {}, map_row);
}

Result<void> AgentConfigRepository::save(const AgentConfig& c) {
    auto r =
        exec_write("INSERT OR REPLACE INTO agent_configs "
                   "(id, name, description, config_json, category, is_default, is_active, updated_at) "
                   "VALUES (?, ?, ?, ?, ?, ?, ?, datetime('now'))",
                   {c.id, c.name, c.description, c.config_json, c.category, c.is_default ? 1 : 0, c.is_active ? 1 : 0});
    return r;
}

Result<void> AgentConfigRepository::remove(const QString& id) {
    auto r = exec_write("DELETE FROM agent_configs WHERE id = ?", {id});
    return r;
}

Result<void> AgentConfigRepository::set_active(const QString& id) {
    exec_write("UPDATE agent_configs SET is_active = 0", {});
    auto r = exec_write("UPDATE agent_configs SET is_active = 1, updated_at = datetime('now') WHERE id = ?", {id});
    return r;
}

} // namespace fincept
