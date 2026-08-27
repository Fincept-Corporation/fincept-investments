#include "storage/repositories/WatchlistRepository.h"


namespace fincept {

WatchlistRepository& WatchlistRepository::instance() {
    static WatchlistRepository s;
    return s;
}

Watchlist WatchlistRepository::map_watchlist(QSqlQuery& q) {
    return {
        q.value(0).toString(), q.value(1).toString(), q.value(2).toString(), q.value(3).toString(),
        q.value(4).toInt(),    q.value(5).toBool(),   q.value(6).toString(), q.value(7).toString(),
    };
}

WatchlistStock WatchlistRepository::map_stock(QSqlQuery& q) {
    return {
        q.value(0).toInt(),    q.value(1).toString(), q.value(2).toString(), q.value(3).toString(),
        q.value(4).toString(), q.value(5).toString(), q.value(6).toInt(),    q.value(7).toString(),
    };
}

Result<Watchlist> WatchlistRepository::create(const QString& name, const QString& color) {
    QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto r = exec_write("INSERT INTO watchlists (id, name, color) VALUES (?, ?, ?)", {id, name, color});
    if (r.is_err())
        return Result<Watchlist>::err(r.error());
    return get(id);
}

Result<QVector<Watchlist>> WatchlistRepository::list_all() {
    return query_list("SELECT id, name, description, color, sort_order, is_default, created_at, updated_at "
                      "FROM watchlists ORDER BY sort_order, name",
                      {}, map_watchlist);
}

Result<Watchlist> WatchlistRepository::get(const QString& id) {
    return query_one("SELECT id, name, description, color, sort_order, is_default, created_at, updated_at "
                     "FROM watchlists WHERE id = ?",
                     {id}, map_watchlist);
}

Result<void> WatchlistRepository::update(const Watchlist& w) {
    auto r = exec_write("UPDATE watchlists SET name = ?, description = ?, color = ?, sort_order = ?, "
                        "is_default = ?, updated_at = datetime('now') WHERE id = ?",
                        {w.name, w.description, w.color, w.sort_order, w.is_default ? 1 : 0, w.id});
    return r;
}

Result<void> WatchlistRepository::remove(const QString& id) {
    auto r = exec_write("DELETE FROM watchlists WHERE id = ?", {id});
    return r;
}

Result<void> WatchlistRepository::add_stock(const QString& watchlist_id, const QString& symbol, const QString& name,
                                            const QString& exchange) {
    auto r = exec_write("INSERT OR IGNORE INTO watchlist_stocks (watchlist_id, symbol, name, exchange) "
                        "VALUES (?, ?, ?, ?)",
                        {watchlist_id, symbol.toUpper(), name, exchange});
    return r;
}

Result<void> WatchlistRepository::remove_stock(const QString& watchlist_id, const QString& symbol) {
    auto r = exec_write("DELETE FROM watchlist_stocks WHERE watchlist_id = ? AND symbol = ?",
                        {watchlist_id, symbol.toUpper()});
    return r;
}

Result<QVector<WatchlistStock>> WatchlistRepository::get_stocks(const QString& watchlist_id) {
    // Use a temporary BaseRepository<WatchlistStock> adapter for the different entity type
    auto r = db().execute("SELECT id, watchlist_id, symbol, name, exchange, notes, sort_order, added_at "
                          "FROM watchlist_stocks WHERE watchlist_id = ? ORDER BY sort_order, symbol",
                          {watchlist_id});
    if (r.is_err())
        return Result<QVector<WatchlistStock>>::err(r.error());

    QVector<WatchlistStock> result;
    auto& q = r.value();
    while (q.next()) {
        result.append(map_stock(q));
    }
    return Result<QVector<WatchlistStock>>::ok(std::move(result));
}

} // namespace fincept
