#include "storage/repositories/WorkflowRepository.h"

#include "core/logging/Logger.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace fincept {

WorkflowRepository& WorkflowRepository::instance() {
    static WorkflowRepository s;
    return s;
}

WorkflowRow WorkflowRepository::map_row(QSqlQuery& q) {
    return {
        q.value(0).toString(), q.value(1).toString(), q.value(2).toString(),
        q.value(3).toString(), q.value(4).toString(), q.value(5).toString(),
    };
}

Result<void> WorkflowRepository::save(const workflow::WorkflowDef& wf) {
    // Persist header + nodes + edges atomically. save() DELETEs all existing
    // nodes/edges then re-INSERTs them; without a transaction a mid-sequence
    // failure (constraint / disk-full) or a crash would leave the graph
    // partially wiped and the next load() would return an empty/corrupt
    // workflow. Wrap the whole rewrite in one transaction, roll back on error.
    if (auto tx = db().begin_transaction(); tx.is_err())
        return tx;

    const auto body = [&]() -> Result<void> {
        // Upsert the workflow header
        auto r = exec_write("INSERT INTO workflows (id, name, description, status, static_data, updated_at) "
                        "VALUES (?, ?, ?, ?, ?, datetime('now')) "
                        "ON CONFLICT(id) DO UPDATE SET "
                        "  name = excluded.name,"
                        "  description = excluded.description,"
                        "  status = excluded.status,"
                        "  static_data = excluded.static_data,"
                        "  updated_at = datetime('now')",
                        {wf.id, wf.name, wf.description,
                         wf.status == workflow::WorkflowStatus::Draft       ? "draft"
                         : wf.status == workflow::WorkflowStatus::Idle      ? "idle"
                         : wf.status == workflow::WorkflowStatus::Running   ? "running"
                         : wf.status == workflow::WorkflowStatus::Completed ? "completed"
                                                                            : "error",
                         QString::fromUtf8(QJsonDocument(wf.static_data).toJson(QJsonDocument::Compact))});
    if (r.is_err())
        return r;

    // Delete existing nodes and edges (cascade would handle edges, but be explicit)
    r = exec_write("DELETE FROM workflow_edges WHERE workflow_id = ?", {wf.id});
    if (r.is_err())
        return r;

    r = exec_write("DELETE FROM workflow_nodes WHERE workflow_id = ?", {wf.id});
    if (r.is_err())
        return r;

    // Insert nodes
    for (int i = 0; i < wf.nodes.size(); ++i) {
        const auto& nd = wf.nodes[i];
        r = exec_write("INSERT INTO workflow_nodes "
                       "(id, workflow_id, type, name, type_version, pos_x, pos_y, "
                       " parameters, credentials, disabled, continue_on_fail, retry_on_fail, max_tries, sort_order) "
                       "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                       {nd.id, wf.id, nd.type, nd.name, nd.type_version, nd.x, nd.y,
                        QString::fromUtf8(QJsonDocument(nd.parameters).toJson(QJsonDocument::Compact)),
                        QString::fromUtf8(QJsonDocument(nd.credentials).toJson(QJsonDocument::Compact)),
                        nd.disabled ? 1 : 0, nd.continue_on_fail ? 1 : 0, nd.retry_on_fail ? 1 : 0, nd.max_tries, i});
        if (r.is_err())
            return r;
    }

    // Insert edges
    for (const auto& ed : wf.edges) {
        r = exec_write(
            "INSERT INTO workflow_edges "
            "(id, workflow_id, source_node, target_node, source_port, target_port, animated) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)",
            {ed.id, wf.id, ed.source_node, ed.target_node, ed.source_port, ed.target_port, ed.animated ? 1 : 0});
        if (r.is_err())
            return r;
    }
        return Result<void>::ok();
    }; // body

    if (Result<void> br = body(); br.is_err()) {
        db().rollback();
        return br;
    }
    if (auto cr = db().commit(); cr.is_err())
        return cr;

    LOG_INFO("WorkflowRepo",
             QString("Saved workflow: %1 (%2 nodes, %3 edges)").arg(wf.name).arg(wf.nodes.size()).arg(wf.edges.size()));
    return Result<void>::ok();
}

Result<workflow::WorkflowDef> WorkflowRepository::load(const QString& id) {
    // Load header
    auto hr = db().execute("SELECT id, name, description, status, static_data, created_at, updated_at "
                           "FROM workflows WHERE id = ?",
                           {id});
    if (hr.is_err())
        return Result<workflow::WorkflowDef>::err(hr.error());

    auto& hq = hr.value();
    if (!hq.next())
        return Result<workflow::WorkflowDef>::err("Workflow not found");

    workflow::WorkflowDef wf;
    wf.id = hq.value(0).toString();
    wf.name = hq.value(1).toString();
    wf.description = hq.value(2).toString();

    QString status_str = hq.value(3).toString();
    if (status_str == "idle")
        wf.status = workflow::WorkflowStatus::Idle;
    else if (status_str == "running")
        wf.status = workflow::WorkflowStatus::Running;
    else if (status_str == "completed")
        wf.status = workflow::WorkflowStatus::Completed;
    else if (status_str == "error")
        wf.status = workflow::WorkflowStatus::Error;
    else
        wf.status = workflow::WorkflowStatus::Draft;

    wf.static_data = QJsonDocument::fromJson(hq.value(4).toString().toUtf8()).object();
    wf.created_at = hq.value(5).toString();
    wf.updated_at = hq.value(6).toString();

    // Load nodes
    auto nr = db().execute("SELECT id, type, name, type_version, pos_x, pos_y, parameters, credentials, "
                           "disabled, continue_on_fail, retry_on_fail, max_tries "
                           "FROM workflow_nodes WHERE workflow_id = ? ORDER BY sort_order",
                           {id});
    if (nr.is_err())
        return Result<workflow::WorkflowDef>::err(nr.error());

    auto& nq = nr.value();
    while (nq.next()) {
        workflow::NodeDef nd;
        nd.id = nq.value(0).toString();
        nd.type = nq.value(1).toString();
        nd.name = nq.value(2).toString();
        nd.type_version = nq.value(3).toInt();
        nd.x = nq.value(4).toDouble();
        nd.y = nq.value(5).toDouble();
        nd.parameters = QJsonDocument::fromJson(nq.value(6).toString().toUtf8()).object();
        nd.credentials = QJsonDocument::fromJson(nq.value(7).toString().toUtf8()).object();
        nd.disabled = nq.value(8).toBool();
        nd.continue_on_fail = nq.value(9).toBool();
        nd.retry_on_fail = nq.value(10).toBool();
        nd.max_tries = nq.value(11).toInt();
        wf.nodes.append(nd);
    }

    // Load edges
    auto er = db().execute("SELECT id, source_node, target_node, source_port, target_port, animated "
                           "FROM workflow_edges WHERE workflow_id = ?",
                           {id});
    if (er.is_err())
        return Result<workflow::WorkflowDef>::err(er.error());

    auto& eq = er.value();
    while (eq.next()) {
        workflow::EdgeDef ed;
        ed.id = eq.value(0).toString();
        ed.source_node = eq.value(1).toString();
        ed.target_node = eq.value(2).toString();
        ed.source_port = eq.value(3).toString();
        ed.target_port = eq.value(4).toString();
        ed.animated = eq.value(5).toBool();
        wf.edges.append(ed);
    }

    LOG_INFO(
        "WorkflowRepo",
        QString("Loaded workflow: %1 (%2 nodes, %3 edges)").arg(wf.name).arg(wf.nodes.size()).arg(wf.edges.size()));
    return Result<workflow::WorkflowDef>::ok(std::move(wf));
}

Result<QVector<WorkflowRow>> WorkflowRepository::list_all() {
    return query_list("SELECT id, name, description, status, created_at, updated_at "
                      "FROM workflows ORDER BY updated_at DESC",
                      {}, map_row);
}

Result<void> WorkflowRepository::remove(const QString& id) {
    // Foreign key cascade handles nodes and edges
    auto r = exec_write("DELETE FROM workflows WHERE id = ?", {id});
    return r;
}

} // namespace fincept
