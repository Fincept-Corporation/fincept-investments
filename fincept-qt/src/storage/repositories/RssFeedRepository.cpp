#include "storage/repositories/RssFeedRepository.h"


namespace fincept {

RssFeedRepository& RssFeedRepository::instance() {
    static RssFeedRepository s;
    return s;
}

RssFeedRow RssFeedRepository::map_row(QSqlQuery& q) {
    RssFeedRow r;
    r.id = q.value(0).toString();
    r.name = q.value(1).toString();
    r.url = q.value(2).toString();
    r.category = q.value(3).toString();
    r.region = q.value(4).toString();
    r.source = q.value(5).toString();
    r.tier = q.value(6).toInt();
    r.is_builtin = q.value(7).toBool();
    r.enabled = q.value(8).toBool();
    return r;
}

Result<QVector<RssFeedRow>> RssFeedRepository::list_all() const {
    return query_list("SELECT id, name, url, category, region, source, tier, is_builtin, enabled "
                      "FROM rss_feeds ORDER BY tier ASC, source ASC",
                      {}, map_row);
}

Result<void> RssFeedRepository::upsert(const RssFeedRow& r) const {
    auto res = exec_write(
        "INSERT INTO rss_feeds "
        "(id, name, url, category, region, source, tier, is_builtin, enabled, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, datetime('now')) "
        "ON CONFLICT(id) DO UPDATE SET "
        "  name=excluded.name, url=excluded.url, category=excluded.category, "
        "  region=excluded.region, source=excluded.source, tier=excluded.tier, "
        "  is_builtin=excluded.is_builtin, enabled=excluded.enabled, "
        "  updated_at=datetime('now')",
        {r.id, r.name, r.url, r.category, r.region, r.source, r.tier, r.is_builtin ? 1 : 0, r.enabled ? 1 : 0});
    return res;
}

Result<void> RssFeedRepository::remove(const QString& id) const {
    auto r = exec_write("DELETE FROM rss_feeds WHERE id = ?", {id});
    return r;
}

Result<void> RssFeedRepository::set_enabled(const QString& id, bool enabled) const {
    auto r = exec_write("UPDATE rss_feeds SET enabled = ?, updated_at = datetime('now') WHERE id = ?",
                        {enabled ? 1 : 0, id});
    return r;
}

} // namespace fincept
