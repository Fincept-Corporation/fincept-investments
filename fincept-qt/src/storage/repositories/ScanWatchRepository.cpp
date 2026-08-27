// src/storage/repositories/ScanWatchRepository.cpp
#include "storage/repositories/ScanWatchRepository.h"


#include <QDateTime>
#include <QJsonDocument>
#include <QUuid>

namespace fincept {

namespace {
const char* kCols = "id, name, conditions_json, logic, symbols, timeframe, lookback_days,"
                    " data_source, broker_id, account_id, mode, interval_sec, cooldown_min,"
                    " actions_json, active, status, last_eval_at, last_fired_at, universe";

QString json_array_to_str(const QJsonArray& a) {
    return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact));
}
QString json_obj_to_str(const QJsonObject& o) {
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}
// Coerce a possibly-null QString to a non-null empty string so it binds as ''
// (not SQL NULL) into NOT NULL TEXT columns.
QString nn(const QString& s) {
    return s.isNull() ? QString::fromLatin1("") : s;
}
} // namespace

ScanWatchRepository& ScanWatchRepository::instance() {
    static ScanWatchRepository s;
    return s;
}

ScanWatch ScanWatchRepository::map_row(QSqlQuery& q) {
    ScanWatch w;
    w.id = q.value(0).toString();
    w.name = q.value(1).toString();
    w.conditions = QJsonDocument::fromJson(q.value(2).toString().toUtf8()).array();
    w.logic = q.value(3).toString();
    w.symbols = q.value(4).toString().split('\n', Qt::SkipEmptyParts);
    w.timeframe = q.value(5).toString();
    w.lookback_days = q.value(6).toInt();
    w.data_source = q.value(7).toString();
    w.broker_id = q.value(8).toString();
    w.account_id = q.value(9).toString();
    w.mode = q.value(10).toString();
    w.interval_sec = q.value(11).toInt();
    w.cooldown_min = q.value(12).toInt();
    w.actions = QJsonDocument::fromJson(q.value(13).toString().toUtf8()).object();
    w.active = q.value(14).toInt() != 0;
    w.status = q.value(15).toString();
    w.last_eval_at = q.value(16).toLongLong();
    w.last_fired_at = q.value(17).toLongLong();
    w.universe = q.value(18).toString();
    return w;
}

Result<ScanWatch> ScanWatchRepository::create(const ScanWatch& in) {
    ScanWatch w = in;
    if (w.id.isEmpty())
        w.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto r = exec_write("INSERT INTO scan_watches (id, name, conditions_json, logic, symbols, timeframe,"
                        " lookback_days, data_source, broker_id, account_id, mode, interval_sec, cooldown_min,"
                        " actions_json, active, status, last_eval_at, last_fired_at, universe, created_at, updated_at)"
                        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                        {nn(w.id),
                         nn(w.name),
                         nn(json_array_to_str(w.conditions)),
                         nn(w.logic),
                         nn(w.symbols.join('\n')),
                         nn(w.timeframe),
                         w.lookback_days,
                         nn(w.data_source),
                         nn(w.broker_id),
                         nn(w.account_id),
                         nn(w.mode),
                         w.interval_sec,
                         w.cooldown_min,
                         nn(json_obj_to_str(w.actions)),
                         w.active ? 1 : 0,
                         nn(w.status),
                         w.last_eval_at,
                         w.last_fired_at,
                         nn(w.universe),
                         now,
                         now});
    if (r.is_err())
        return Result<ScanWatch>::err(r.error());
    return get(w.id);
}

Result<void> ScanWatchRepository::update(const ScanWatch& w) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto r = exec_write("UPDATE scan_watches SET name=?, conditions_json=?, logic=?, symbols=?, timeframe=?,"
                        " lookback_days=?, data_source=?, broker_id=?, account_id=?, mode=?, interval_sec=?,"
                        " cooldown_min=?, actions_json=?, active=?, universe=?, updated_at=? WHERE id=?",
                        {nn(w.name), nn(json_array_to_str(w.conditions)), nn(w.logic), nn(w.symbols.join('\n')),
                         nn(w.timeframe), w.lookback_days, nn(w.data_source), nn(w.broker_id), nn(w.account_id),
                         nn(w.mode), w.interval_sec, w.cooldown_min, nn(json_obj_to_str(w.actions)), w.active ? 1 : 0,
                         nn(w.universe), now, nn(w.id)});
    return r;
}

Result<void> ScanWatchRepository::remove(const QString& id) {
    auto r = exec_write("DELETE FROM scan_watches WHERE id=?", {id});
    return r;
}

Result<void> ScanWatchRepository::set_active(const QString& id, bool active) {
    auto r = exec_write("UPDATE scan_watches SET active=?, updated_at=? WHERE id=?",
                        {active ? 1 : 0, QDateTime::currentMSecsSinceEpoch(), id});
    return r;
}

Result<void> ScanWatchRepository::touch_status(const QString& id, const QString& status, qint64 last_eval_at) {
    return exec_write("UPDATE scan_watches SET status=?, last_eval_at=? WHERE id=?", {status, last_eval_at, id});
}

Result<void> ScanWatchRepository::touch_fired(const QString& id, qint64 last_fired_at) {
    return exec_write("UPDATE scan_watches SET last_fired_at=? WHERE id=?", {last_fired_at, id});
}

Result<QVector<ScanWatch>> ScanWatchRepository::list_all() {
    return query_list(QString("SELECT %1 FROM scan_watches ORDER BY created_at DESC").arg(kCols), {}, map_row);
}

Result<QVector<ScanWatch>> ScanWatchRepository::list_active() {
    return query_list(QString("SELECT %1 FROM scan_watches WHERE active=1").arg(kCols), {}, map_row);
}

Result<ScanWatch> ScanWatchRepository::get(const QString& id) {
    return query_one(QString("SELECT %1 FROM scan_watches WHERE id=?").arg(kCols), {id}, map_row);
}

} // namespace fincept
