#include "services/data_normalization/DataNormalizationService.h"

#include "core/logging/Logger.h"
#include "network/http/HttpClient.h"
#include "storage/sqlite/Database.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QPointer>
#include <QTimeZone>
#include <QUrl>
#include <QUuid>

namespace fincept::services {

static const QString TAG = "DataNormalization";

// ── Schema definitions ─────────────────────────────────────────────────────
// Required fields per schema — used for validation.
static const QHash<QString, QStringList> kRequiredFields = {
    {"OHLCV", {"symbol", "timestamp", "open", "high", "low", "close", "volume"}},
    {"QUOTE", {"symbol", "timestamp", "price"}},
    {"TICK", {"symbol", "timestamp", "price", "quantity"}},
    {"ORDER", {"orderId", "symbol", "side", "type", "quantity", "status"}},
    {"POSITION", {"symbol", "quantity", "averagePrice"}},
    {"PORTFOLIO", {"totalValue", "timestamp"}},
    {"INSTRUMENT", {"symbol", "name", "exchange"}},
};

// ── Singleton ─────────────────────────────────────────────────────────────

DataNormalizationService& DataNormalizationService::instance() {
    static DataNormalizationService s;
    return s;
}

DataNormalizationService::DataNormalizationService() = default;

// ── Public API ─────────────────────────────────────────────────────────────

void DataNormalizationService::fetch_and_normalize(const DataMapping& mapping, NormalizeCallback cb) {
    apply_auth(mapping);

    const QString url = build_url(mapping);
    // Log the URL WITHOUT its query string: the shipped mapping templates put
    // `&apikey=`/`&token=` there, and this is LOG_INFO — the default level — so
    // the raw URL would write a live third-party credential into fincept.log.
    LOG_INFO(TAG, QString("Fetching mapping '%1' from %2")
                      .arg(mapping.name, QUrl(url).adjusted(QUrl::RemoveQuery).toString()));

    QPointer<DataNormalizationService> self = this;

    auto on_reply = [self, mapping, cb](Result<QJsonDocument> result) {
        if (!self)
            return;

        if (result.is_err()) {
            const QString err = QString::fromStdString(result.error());
            LOG_ERROR(TAG, QString("Fetch failed for mapping '%1': %2").arg(mapping.name, err));
            emit self->normalization_failed(mapping.id, err);
            cb(false, {});
            return;
        }

        NormalizedRecord record = self->normalize_raw(mapping, result.value());
        persist(mapping, record);

        emit self->normalization_complete(record);
        cb(record.errors.isEmpty(), record);
    };

    if (mapping.method == "POST") {
        QJsonObject body;
        if (!mapping.body.isEmpty()) {
            body = QJsonDocument::fromJson(mapping.body.toUtf8()).object();
        }
        HttpClient::instance().post(url, body, on_reply);
    } else {
        HttpClient::instance().get(url, on_reply);
    }
}

NormalizedRecord DataNormalizationService::normalize_raw(const DataMapping& mapping, const QJsonDocument& raw) {
    NormalizedRecord record;
    record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    record.mapping_id = mapping.id;
    record.source_id = mapping.source_id;
    record.schema_name = mapping.schema_name;
    record.raw = raw.object();
    record.extracted_at = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    // Parse field_mappings_json
    const QJsonArray field_mappings = QJsonDocument::fromJson(mapping.field_mappings_json.toUtf8()).array();

    const QJsonValue root = raw.isArray() ? QJsonValue(raw.array()) : QJsonValue(raw.object());

    QJsonObject normalized;
    for (const QJsonValue& fv : field_mappings) {
        const QJsonObject fm = fv.toObject();
        const QString target = fm["target"].toString();
        const QString expression = fm["expression"].toString();
        const QString transform = fm["transform"].toString();
        const QString default_v = fm["default_val"].toString();

        if (target.isEmpty())
            continue;

        QJsonValue extracted;
        if (!expression.isEmpty()) {
            extracted = extract_jsonpath(root, expression);
        }

        // Fall back to default if extraction yielded null/undefined
        if (extracted.isNull() || extracted.isUndefined()) {
            if (!default_v.isEmpty()) {
                extracted = QJsonValue(default_v);
            } else {
                extracted = QJsonValue::Null;
            }
        }

        // Apply transform
        if (!transform.isEmpty() && !extracted.isNull()) {
            extracted = apply_transform(extracted, transform);
        }

        normalized[target] = extracted;
    }

    record.normalized = normalized;
    record.errors = validate_schema(normalized, mapping.schema_name);

    if (!record.errors.isEmpty()) {
        LOG_WARN(TAG,
                 QString("Mapping '%1' produced %2 validation error(s)").arg(mapping.name).arg(record.errors.size()));
    }

    return record;
}

std::optional<NormalizedRecord> DataNormalizationService::latest_for_mapping(const QString& mapping_id) {
    auto r = Database::instance().execute("SELECT id, mapping_id, source_id, schema_name, normalized_json, raw_json, "
                                          "       validation_errors, extracted_at "
                                          "FROM normalized_data WHERE mapping_id = ? "
                                          "ORDER BY extracted_at DESC LIMIT 1",
                                          {mapping_id});

    if (r.is_err() || !r.value().next())
        return std::nullopt;

    auto& q = r.value();
    NormalizedRecord rec;
    rec.id = q.value(0).toString();
    rec.mapping_id = q.value(1).toString();
    rec.source_id = q.value(2).toString();
    rec.schema_name = q.value(3).toString();
    rec.normalized = QJsonDocument::fromJson(q.value(4).toString().toUtf8()).object();
    rec.raw = QJsonDocument::fromJson(q.value(5).toString().toUtf8()).object();
    const QJsonArray errs = QJsonDocument::fromJson(q.value(6).toString().toUtf8()).array();
    for (const auto& e : errs)
        rec.errors << e.toString();
    rec.extracted_at = q.value(7).toString();
    return rec;
}

QVector<NormalizedRecord> DataNormalizationService::records_for_schema(const QString& schema_name) {
    auto r = Database::instance().execute("SELECT id, mapping_id, source_id, schema_name, normalized_json, raw_json, "
                                          "       validation_errors, extracted_at "
                                          "FROM normalized_data WHERE schema_name = ? "
                                          "ORDER BY extracted_at DESC LIMIT 500",
                                          {schema_name});

    QVector<NormalizedRecord> results;
    if (r.is_err())
        return results;

    auto& q = r.value();
    while (q.next()) {
        NormalizedRecord rec;
        rec.id = q.value(0).toString();
        rec.mapping_id = q.value(1).toString();
        rec.source_id = q.value(2).toString();
        rec.schema_name = q.value(3).toString();
        rec.normalized = QJsonDocument::fromJson(q.value(4).toString().toUtf8()).object();
        rec.raw = QJsonDocument::fromJson(q.value(5).toString().toUtf8()).object();
        const QJsonArray errs = QJsonDocument::fromJson(q.value(6).toString().toUtf8()).array();
        for (const auto& e : errs)
            rec.errors << e.toString();
        rec.extracted_at = q.value(7).toString();
        results.append(rec);
    }
    return results;
}

// ── JSONPath extraction ────────────────────────────────────────────────────
// Supports: $.key  $.a.b.c  $[0]  $[*][2]  $[*].key (returns array of values)

QJsonValue DataNormalizationService::extract_jsonpath(const QJsonValue& root, const QString& path) {
    if (path.isEmpty() || path == "$")
        return root;

    // Strip leading "$" and split on "." and "[]"
    QString p = path;
    if (p.startsWith("$"))
        p = p.mid(1);

    // Tokenise: split on '.' but also parse [N] and [*] inline
    QStringList tokens;
    for (const QString& part : p.split('.', Qt::SkipEmptyParts)) {
        if (part.contains('[')) {
            // e.g. "key[0]" or "[*][2]" — split on '['
            QStringList sub = part.split('[', Qt::SkipEmptyParts);
            for (const QString& s : sub) {
                if (s.endsWith(']')) {
                    tokens << "[" + s.chopped(1) + "]";
                } else {
                    tokens << s;
                }
            }
        } else {
            tokens << part;
        }
    }

    QJsonValue current = root;
    for (const QString& token : tokens) {
        if (token.startsWith('[') && token.endsWith(']')) {
            const QString inner = token.mid(1, token.size() - 2);
            if (inner == "*") {
                // Wildcard — return array of child values
                if (!current.isArray())
                    return QJsonValue::Undefined;
                return current; // caller handles the array
            }
            bool ok = false;
            int idx = inner.toInt(&ok);
            if (!ok)
                return QJsonValue::Undefined;
            if (!current.isArray())
                return QJsonValue::Undefined;
            const QJsonArray arr = current.toArray();
            if (idx < 0 || idx >= arr.size())
                return QJsonValue::Undefined;
            current = arr[idx];
        } else {
            if (!current.isObject())
                return QJsonValue::Undefined;
            current = current.toObject().value(token);
        }
    }
    return current;
}

// ── Transform functions ────────────────────────────────────────────────────

QJsonValue DataNormalizationService::apply_transform(const QJsonValue& val, const QString& transform) {
    if (transform == "to_number") {
        if (val.isDouble())
            return val;
        bool ok = false;
        double d = val.toString().toDouble(&ok);
        return ok ? QJsonValue(d) : QJsonValue(0.0);
    }

    if (transform == "to_string") {
        if (val.isString())
            return val;
        return QJsonValue(QString::fromUtf8(QJsonDocument(val.toObject()).toJson(QJsonDocument::Compact)));
    }

    if (transform == "unix_ms_to_iso") {
        qint64 ms = val.isDouble() ? static_cast<qint64>(val.toDouble()) : val.toString().toLongLong();
        return QJsonValue(QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(Qt::ISODate));
    }

    if (transform == "unix_s_to_iso") {
        qint64 s = val.isDouble() ? static_cast<qint64>(val.toDouble()) : val.toString().toLongLong();
        return QJsonValue(QDateTime::fromSecsSinceEpoch(s, QTimeZone::UTC).toString(Qt::ISODate));
    }

    if (transform == "upper") {
        return QJsonValue(val.toString().toUpper());
    }

    if (transform == "lower") {
        return QJsonValue(val.toString().toLower());
    }

    if (transform == "abs_value") {
        double d = val.isDouble() ? val.toDouble() : val.toString().toDouble();
        return QJsonValue(d < 0 ? -d : d);
    }

    return val; // unknown transform — pass through unchanged
}

// ── Schema validation ──────────────────────────────────────────────────────

QStringList DataNormalizationService::validate_schema(const QJsonObject& data, const QString& schema_name) {
    QStringList errors;
    const auto it = kRequiredFields.find(schema_name);
    if (it == kRequiredFields.end())
        return errors; // unknown schema, skip

    for (const QString& field : it.value()) {
        if (!data.contains(field) || data[field].isNull() || data[field].isUndefined()) {
            errors << QString("Missing required field: %1").arg(field);
        }
    }
    return errors;
}

// ── Persistence ────────────────────────────────────────────────────────────

void DataNormalizationService::persist(const DataMapping& mapping, const NormalizedRecord& record) {
    const QString norm_json = QJsonDocument(record.normalized).toJson(QJsonDocument::Compact);
    const QString raw_json = QJsonDocument(record.raw).toJson(QJsonDocument::Compact);

    QJsonArray err_array;
    for (const QString& e : record.errors)
        err_array.append(e);
    const QString err_json = QJsonDocument(err_array).toJson(QJsonDocument::Compact);

    const QString hash = QCryptographicHash::hash(norm_json.toUtf8(), QCryptographicHash::Sha256).toHex();

    auto r = Database::instance().execute(
        "INSERT INTO normalized_data "
        "(id, mapping_id, source_id, schema_name, normalized_json, raw_json, "
        " validation_errors, data_hash) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        {record.id, mapping.id, mapping.source_id, mapping.schema_name, norm_json, raw_json, err_json, hash});

    if (r.is_err()) {
        LOG_ERROR(TAG, QString("Failed to persist normalized record: %1").arg(QString::fromStdString(r.error())));
    }
}

// ── HTTP helpers ───────────────────────────────────────────────────────────

QString DataNormalizationService::build_url(const DataMapping& mapping) {
    QString url = mapping.base_url;
    if (!url.endsWith('/') && !mapping.endpoint.startsWith('/'))
        url += '/';
    url += mapping.endpoint;
    return url;
}

void DataNormalizationService::apply_auth(const DataMapping& mapping) {
    // ── SECURITY: do NOT mutate the shared HttpClient singleton here ─────────
    // This used to stash the saved mapping's third-party token on the shared
    // HttpClient and never restore it, which leaked that credential onto every
    // subsequent request the singleton made.
    //
    // HttpClient now carries no credential state at all; the supported way to
    // authenticate one call is the per-request `Headers` argument. Wiring
    // mapping auth through it is unfinished work — mapping auth is inert, which
    // is what it already was, but it corrupts nothing.
    if (mapping.auth_type == "API Key" || mapping.auth_type == "Bearer Token") {
        if (!mapping.auth_token.isEmpty()) {
            LOG_WARN(TAG, QString("Mapping '%1' declares %2 auth, but per-request auth headers are not "
                                  "supported yet — the request is being sent unauthenticated")
                              .arg(mapping.name, mapping.auth_type));
        }
    }
    // Basic Auth and None require no setup on HttpClient
}

} // namespace fincept::services
