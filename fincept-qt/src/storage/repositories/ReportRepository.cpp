#include "storage/repositories/ReportRepository.h"


namespace fincept {

ReportRepository& ReportRepository::instance() {
    static ReportRepository s;
    return s;
}

Report ReportRepository::map_report(QSqlQuery& q) {
    return {q.value(0).toInt(), q.value(1).toString(), q.value(2).toString(), q.value(3).toString(),
            q.value(4).toString()};
}

ReportTemplate ReportRepository::map_template(QSqlQuery& q) {
    return {q.value(0).toString(), q.value(1).toString(), q.value(2).toString(),
            q.value(3).toString(), q.value(4).toString(), q.value(5).toString()};
}

Result<qint64> ReportRepository::create(const QString& title, const QString& content_json) {
    auto r = exec_insert("INSERT INTO reports (title, content_json) VALUES (?, ?)", {title, content_json});
    return r;
}

Result<Report> ReportRepository::get(int id) {
    return query_one("SELECT id, title, content_json, created_at, updated_at FROM reports WHERE id = ?", {id},
                     map_report);
}

Result<QVector<Report>> ReportRepository::list_all() {
    return query_list("SELECT id, title, content_json, created_at, updated_at FROM reports ORDER BY updated_at DESC",
                      {}, map_report);
}

Result<void> ReportRepository::update(int id, const QString& title, const QString& content_json) {
    auto r = exec_write("UPDATE reports SET title = ?, content_json = ?, updated_at = datetime('now') WHERE id = ?",
                        {title, content_json, id});
    return r;
}

Result<void> ReportRepository::remove(int id) {
    auto r = exec_write("DELETE FROM reports WHERE id = ?", {id});
    return r;
}

Result<QVector<ReportTemplate>> ReportRepository::list_templates() {
    auto r = db().execute("SELECT id, name, description, template_data, created_at, updated_at "
                          "FROM report_templates ORDER BY name");
    if (r.is_err())
        return Result<QVector<ReportTemplate>>::err(r.error());
    QVector<ReportTemplate> result;
    auto& q = r.value();
    while (q.next())
        result.append(map_template(q));
    return Result<QVector<ReportTemplate>>::ok(std::move(result));
}

Result<void> ReportRepository::save_template(const ReportTemplate& t) {
    return exec_write("INSERT OR REPLACE INTO report_templates (id, name, description, template_data, updated_at) "
                      "VALUES (?, ?, ?, ?, datetime('now'))",
                      {t.id, t.name, t.description, t.template_data});
}

Result<void> ReportRepository::remove_template(const QString& id) {
    return exec_write("DELETE FROM report_templates WHERE id = ?", {id});
}

} // namespace fincept
