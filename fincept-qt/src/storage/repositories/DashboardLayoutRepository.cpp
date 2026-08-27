#include "storage/repositories/DashboardLayoutRepository.h"

#include "core/logging/Logger.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlQuery>
#include <QUuid>

namespace fincept {

DashboardLayoutRepository& DashboardLayoutRepository::instance() {
    static DashboardLayoutRepository inst;
    return inst;
}

Result<screens::GridLayout> DashboardLayoutRepository::load_layout(const QString& profile_name) {
    // Load or create the layout profile row
    auto r = db().execute("SELECT id, cols, row_height, margin FROM dashboard_layouts WHERE profile_name = ?",
                          {profile_name});
    if (r.is_err())
        return Result<screens::GridLayout>::err(r.error());

    screens::GridLayout layout;
    qint64 layout_id = -1;

    auto& q = r.value();
    if (q.next()) {
        layout_id = q.value(0).toLongLong();
        layout.cols = q.value(1).toInt();
        layout.row_h = q.value(2).toInt();
        layout.margin = q.value(3).toInt();
    } else {
        // No saved layout yet — return empty layout (caller applies default template)
        return Result<screens::GridLayout>::ok(layout);
    }

    // Load widget instances
    auto r2 = db().execute("SELECT instance_id, widget_type, grid_x, grid_y, grid_w, grid_h, min_w, min_h, config_json "
                           "FROM dashboard_widget_instances WHERE layout_id = ? ORDER BY sort_order ASC",
                           {layout_id});
    if (r2.is_err())
        return Result<screens::GridLayout>::err(r2.error());

    auto& q2 = r2.value();
    while (q2.next()) {
        screens::GridItem item;
        item.instance_id = q2.value(0).toString();
        item.id = q2.value(1).toString();
        item.cell.x = q2.value(2).toInt();
        item.cell.y = q2.value(3).toInt();
        item.cell.w = q2.value(4).toInt();
        item.cell.h = q2.value(5).toInt();
        item.cell.min_w = q2.value(6).toInt();
        item.cell.min_h = q2.value(7).toInt();
        const QString cfg_text = q2.value(8).toString();
        if (!cfg_text.isEmpty()) {
            const auto doc = QJsonDocument::fromJson(cfg_text.toUtf8());
            if (doc.isObject())
                item.config = doc.object();
        }
        layout.items.append(item);
    }

    LOG_INFO("DashboardRepo", QString("Loaded %1 widgets for profile '%2'").arg(layout.items.size()).arg(profile_name));
    return Result<screens::GridLayout>::ok(std::move(layout));
}

Result<void> DashboardLayoutRepository::save_layout(const screens::GridLayout& layout, const QString& profile_name) {
    // Persist the layout header + widget instances atomically. save_layout()
    // DELETEs all instances then re-INSERTs them; without a transaction a
    // mid-loop failure or crash reloads an empty/corrupt dashboard. Wrap in one
    // transaction, roll back on error.
    if (auto tx = db().begin_transaction(); tx.is_err())
        return tx;

    const auto body = [&]() -> Result<void> {
        // Upsert the layout profile
        auto r = exec_write("INSERT INTO dashboard_layouts (profile_name, cols, row_height, margin, is_active, updated_at) "
                        "VALUES (?, ?, ?, ?, 1, datetime('now')) "
                        "ON CONFLICT(profile_name) DO UPDATE SET "
                        "cols=excluded.cols, row_height=excluded.row_height, margin=excluded.margin, "
                        "is_active=1, updated_at=excluded.updated_at",
                        {profile_name, layout.cols, layout.row_h, layout.margin});
    if (r.is_err())
        return r;

    // Get layout id
    auto r2 = db().execute("SELECT id FROM dashboard_layouts WHERE profile_name = ?", {profile_name});
    if (r2.is_err())
        return Result<void>::err(r2.error());
    if (!r2.value().next())
        return Result<void>::err("Layout not found after upsert");
    qint64 layout_id = r2.value().value(0).toLongLong();

    // Delete old instances
    auto r3 = exec_write("DELETE FROM dashboard_widget_instances WHERE layout_id = ?", {layout_id});
    if (r3.is_err())
        return r3;

    // Insert new instances
    int sort = 0;
    for (const auto& item : layout.items) {
        const QString cfg_json = QString::fromUtf8(QJsonDocument(item.config).toJson(QJsonDocument::Compact));
        auto ri = exec_write("INSERT INTO dashboard_widget_instances "
                             "(instance_id, layout_id, widget_type, grid_x, grid_y, grid_w, grid_h, min_w, min_h, "
                             "sort_order, config_json) "
                             "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                             {item.instance_id, layout_id, item.id, item.cell.x, item.cell.y, item.cell.w, item.cell.h,
                              item.cell.min_w, item.cell.min_h, sort++, cfg_json});
        if (ri.is_err())
            return ri;
    }
        return Result<void>::ok();
    }; // body

    if (Result<void> br = body(); br.is_err()) {
        db().rollback();
        return br;
    }
    if (auto cr = db().commit(); cr.is_err())
        return cr;

    LOG_INFO("DashboardRepo", QString("Saved %1 widgets for profile '%2'").arg(layout.items.size()).arg(profile_name));
    return Result<void>::ok();
}

Result<void> DashboardLayoutRepository::clear_layout(const QString& profile_name) {
    auto r = db().execute("SELECT id FROM dashboard_layouts WHERE profile_name = ?", {profile_name});
    if (r.is_err())
        return Result<void>::ok(); // nothing to clear
    if (!r.value().next())
        return Result<void>::ok();
    qint64 layout_id = r.value().value(0).toLongLong();
    return exec_write("DELETE FROM dashboard_widget_instances WHERE layout_id = ?", {layout_id});
}

} // namespace fincept
