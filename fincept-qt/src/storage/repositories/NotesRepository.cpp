#include "storage/repositories/NotesRepository.h"


namespace fincept {

NotesRepository& NotesRepository::instance() {
    static NotesRepository s;
    return s;
}

FinancialNote NotesRepository::map_note(QSqlQuery& q) {
    FinancialNote n;
    n.id = q.value(0).toInt();
    n.title = q.value(1).toString();
    n.content = q.value(2).toString();
    n.category = q.value(3).toString();
    n.priority = q.value(4).toString();
    n.tags = q.value(5).toString();
    n.tickers = q.value(6).toString();
    n.sentiment = q.value(7).toString();
    n.is_favorite = q.value(8).toBool();
    n.is_archived = q.value(9).toBool();
    n.color_code = q.value(10).toString();
    n.attachments = q.value(11).toString();
    n.created_at = q.value(12).toString();
    n.updated_at = q.value(13).toString();
    n.reminder_date = q.value(14).toString();
    n.word_count = q.value(15).toInt();
    return n;
}

NoteTemplate NotesRepository::map_template(QSqlQuery& q) {
    return {
        q.value(0).toInt(),    q.value(1).toString(), q.value(2).toString(),
        q.value(3).toString(), q.value(4).toString(), q.value(5).toString(),
    };
}

static const char* kNoteColumns = "id, title, content, category, priority, tags, tickers, sentiment, "
                                  "is_favorite, is_archived, color_code, attachments, created_at, updated_at, "
                                  "reminder_date, word_count";

Result<qint64> NotesRepository::create(const FinancialNote& n) {
    auto r = exec_insert("INSERT INTO financial_notes "
                         "(title, content, category, priority, tags, tickers, sentiment, "
                         " is_favorite, is_archived, color_code, reminder_date, word_count) "
                         "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                         {n.title, n.content, n.category, n.priority, n.tags, n.tickers, n.sentiment,
                          n.is_favorite ? 1 : 0, n.is_archived ? 1 : 0, n.color_code, n.reminder_date, n.word_count});
    return r;
}

Result<FinancialNote> NotesRepository::get(int id) {
    return query_one(QString("SELECT %1 FROM financial_notes WHERE id = ?").arg(kNoteColumns), {id}, map_note);
}

Result<QVector<FinancialNote>> NotesRepository::list_all(bool include_archived) {
    QString where = include_archived ? "" : " WHERE is_archived = 0";
    return query_list(QString("SELECT %1 FROM financial_notes%2 ORDER BY updated_at DESC").arg(kNoteColumns, where), {},
                      map_note);
}

Result<QVector<FinancialNote>> NotesRepository::list_by_category(const QString& category) {
    return query_list(QString("SELECT %1 FROM financial_notes WHERE category = ? AND is_archived = 0 "
                              "ORDER BY updated_at DESC")
                          .arg(kNoteColumns),
                      {category}, map_note);
}

Result<QVector<FinancialNote>> NotesRepository::search(const QString& query) {
    QString like = "%" + query + "%";
    return query_list(QString("SELECT %1 FROM financial_notes "
                              "WHERE (title LIKE ? OR content LIKE ? OR tags LIKE ? OR tickers LIKE ?) "
                              "AND is_archived = 0 ORDER BY updated_at DESC")
                          .arg(kNoteColumns),
                      {like, like, like, like}, map_note);
}

Result<void> NotesRepository::update(const FinancialNote& n) {
    auto r =
        exec_write("UPDATE financial_notes SET title = ?, content = ?, category = ?, priority = ?, "
                   "tags = ?, tickers = ?, sentiment = ?, is_favorite = ?, is_archived = ?, "
                   "color_code = ?, reminder_date = ?, word_count = ?, updated_at = datetime('now') "
                   "WHERE id = ?",
                   {n.title, n.content, n.category, n.priority, n.tags, n.tickers, n.sentiment, n.is_favorite ? 1 : 0,
                    n.is_archived ? 1 : 0, n.color_code, n.reminder_date, n.word_count, n.id});
    return r;
}

Result<void> NotesRepository::remove(int id) {
    auto r = exec_write("DELETE FROM financial_notes WHERE id = ?", {id});
    return r;
}

Result<void> NotesRepository::toggle_favorite(int id) {
    auto r = exec_write("UPDATE financial_notes SET is_favorite = NOT is_favorite, "
                        "updated_at = datetime('now') WHERE id = ?",
                        {id});
    return r;
}

Result<void> NotesRepository::toggle_archive(int id) {
    auto r = exec_write("UPDATE financial_notes SET is_archived = NOT is_archived, "
                        "updated_at = datetime('now') WHERE id = ?",
                        {id});
    return r;
}

Result<QVector<NoteTemplate>> NotesRepository::list_templates() {
    auto r = db().execute("SELECT id, name, description, content, category, created_at "
                          "FROM note_templates ORDER BY name");
    if (r.is_err())
        return Result<QVector<NoteTemplate>>::err(r.error());
    QVector<NoteTemplate> result;
    auto& q = r.value();
    while (q.next())
        result.append(map_template(q));
    return Result<QVector<NoteTemplate>>::ok(std::move(result));
}

Result<qint64> NotesRepository::create_template(const NoteTemplate& t) {
    return exec_insert("INSERT INTO note_templates (name, description, content, category) VALUES (?, ?, ?, ?)",
                       {t.name, t.description, t.content, t.category});
}

} // namespace fincept
