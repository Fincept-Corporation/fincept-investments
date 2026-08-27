#include "storage/repositories/NotebookRepository.h"


namespace fincept {

NotebookRepository& NotebookRepository::instance() {
    static NotebookRepository s;
    return s;
}

Notebook NotebookRepository::map_row(QSqlQuery& q) {
    return {
        q.value(0).toString(), q.value(1).toString(), q.value(2).toString(), q.value(3).toString(),
        q.value(4).toString(), q.value(5).toInt(),    q.value(6).toString(), q.value(7).toString(),
    };
}

static const char* kNotebookCols = "id, name, description, cells, metadata, execution_counter, created_at, updated_at";

Result<QVector<Notebook>> NotebookRepository::list_all() {
    return query_list(QString("SELECT %1 FROM notebooks ORDER BY updated_at DESC").arg(kNotebookCols), {}, map_row);
}

Result<Notebook> NotebookRepository::get(const QString& id) {
    return query_one(QString("SELECT %1 FROM notebooks WHERE id = ?").arg(kNotebookCols), {id}, map_row);
}

Result<void> NotebookRepository::save(const Notebook& n) {
    auto r = exec_write("INSERT OR REPLACE INTO notebooks "
                        "(id, name, description, cells, metadata, execution_counter, updated_at) "
                        "VALUES (?, ?, ?, ?, ?, ?, datetime('now'))",
                        {n.id, n.name, n.description, n.cells, n.metadata, n.execution_counter});
    return r;
}

Result<void> NotebookRepository::remove(const QString& id) {
    auto r = exec_write("DELETE FROM notebooks WHERE id = ?", {id});
    return r;
}

} // namespace fincept
