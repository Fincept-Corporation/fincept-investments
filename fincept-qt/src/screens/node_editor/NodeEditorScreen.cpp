#include "screens/node_editor/NodeEditorScreen.h"

#include "core/logging/Logger.h"
#include "core/session/ScreenStateManager.h"
#include "screens/node_editor/NodeEditorCommands.h"
#include "screens/node_editor/canvas/EdgeItem.h"
#include "screens/node_editor/canvas/MiniMap.h"
#include "screens/node_editor/canvas/NodeCanvas.h"
#include "screens/node_editor/canvas/NodeItem.h"
#include "screens/node_editor/canvas/NodeScene.h"
#include "screens/node_editor/palette/NodePalette.h"
#include "screens/node_editor/properties/ExecutionResultsPanel.h"
#include "screens/node_editor/properties/NodePropertiesPanel.h"
#include "screens/node_editor/toolbar/DeployDialog.h"
#include "screens/node_editor/toolbar/NodeEditorToolbar.h"
#include "services/workflow/NodeRegistry.h"
#include "services/workflow/WorkflowService.h"
#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include <QApplication>
#include <QFileDialog>
#include <QFrame>
#include <QInputDialog>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QMessageBox>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QSplitter>
#include <QUuid>
#include <QVBoxLayout>

namespace fincept::workflow {

NodeEditorScreen::NodeEditorScreen(QWidget* parent)
    : QWidget(parent), undo_stack_(new QUndoStack(this)), auto_save_timer_(new QTimer(this)) {
    build_ui();
    wire_signals();

    // Auto-save every 30 seconds (P3: started/stopped in show/hideEvent)
    auto_save_timer_->setInterval(30000);
    connect(auto_save_timer_, &QTimer::timeout, this, &NodeEditorScreen::on_auto_save);

    LOG_INFO("NodeEditor", "Node editor screen created");
}

void NodeEditorScreen::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);

    // Force the global stylesheet to re-polish this widget tree.
    // Without this, ADS internal container widgets created after qApp->setStyleSheet()
    // don't get the theme applied and fall back to Windows system grey.
    QApplication::style()->polish(this);

    auto_save_timer_->start();
    if (minimap_)
        minimap_->start_tracking();
    // Resume edge animations if execution is in progress
    if (scene_ && scene_->has_animated_edges())
        scene_->resume_edge_animations();

    // Restore workspace state
    QSettings settings;
    settings.beginGroup("NodeEditor");
    if (settings.contains("zoom")) {
        qreal zoom = settings.value("zoom", 1.0).toDouble();
        canvas_->resetTransform();
        canvas_->scale(zoom, zoom);
    }
    if (settings.contains("scroll_x")) {
        canvas_->horizontalScrollBar()->setValue(settings.value("scroll_x", 0).toInt());
        canvas_->verticalScrollBar()->setValue(settings.value("scroll_y", 0).toInt());
    }
    if (settings.contains("last_workflow_id")) {
        QString last_id = settings.value("last_workflow_id").toString();
        if (!last_id.isEmpty() && current_workflow_id_.isEmpty())
            WorkflowService::instance().load_workflow(last_id);
    }
    settings.endGroup();

    LOG_INFO("NodeEditor", "Node editor shown");
}

void NodeEditorScreen::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    auto_save_timer_->stop();
    if (minimap_)
        minimap_->stop_tracking();
    // Pause edge animations while hidden — no point rendering offscreen
    if (scene_)
        scene_->pause_edge_animations();

    // Save workspace state
    QSettings settings;
    settings.beginGroup("NodeEditor");
    settings.setValue("zoom", canvas_->transform().m11());
    settings.setValue("scroll_x", canvas_->horizontalScrollBar()->value());
    settings.setValue("scroll_y", canvas_->verticalScrollBar()->value());
    if (!current_workflow_id_.isEmpty())
        settings.setValue("last_workflow_id", current_workflow_id_);
    settings.endGroup();

    LOG_INFO("NodeEditor", "Node editor hidden");
}

void NodeEditorScreen::keyPressEvent(QKeyEvent* event) {
    if (event->matches(QKeySequence::Undo)) {
        undo_stack_->undo();
        event->accept();
    } else if (event->matches(QKeySequence::Redo)) {
        undo_stack_->redo();
        event->accept();
    } else if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        // Delete selected nodes. Now undoable (RemoveNodesCommand restores the
        // nodes AND every edge that touched them), so no confirmation prompt —
        // Ctrl+Z is the recovery path.
        QStringList ids;
        for (auto* item : scene_->selectedItems()) {
            if (auto* node = dynamic_cast<NodeItem*>(item))
                ids.append(node->node_def().id);
        }
        if (!ids.isEmpty()) {
            push_remove_nodes(ids, ids.size() > 1 ? tr("Delete %1 nodes").arg(ids.size()) : tr("Delete node"));
            event->accept();
            return;
        }
        // Nothing but edges selected → delete those instead.
        QStringList edge_ids;
        for (auto* item : scene_->selectedItems()) {
            if (auto* edge = dynamic_cast<EdgeItem*>(item))
                edge_ids.append(edge->edge_id());
        }
        for (const auto& eid : edge_ids) {
            const EdgeDef ed = scene_->edge_def(eid);
            if (!ed.id.isEmpty())
                undo_stack_->push(new commands::RemoveEdgeCommand(scene_, ed));
        }
        event->accept();
    } else if (event->matches(QKeySequence::Save)) {
        on_save_workflow();
        event->accept();
    } else if (event->key() == Qt::Key_D && (event->modifiers() & Qt::ControlModifier)) {
        // Ctrl+D — duplicate the selection (same offset/rename as the context menu).
        QVector<NodeDef> copies;
        for (auto* item : scene_->selectedItems()) {
            auto* node = dynamic_cast<NodeItem*>(item);
            if (!node)
                continue;
            NodeDef copy = node->node_def();
            copy.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            copy.x += 40;
            copy.y += 40;
            copy.name += " (copy)";
            copies.append(copy);
        }
        push_add_nodes(copies, {}, tr("Duplicate %1 node(s)").arg(copies.size()));
        event->accept();
    } else if (event->matches(QKeySequence::SelectAll)) {
        // Ctrl+A — select all nodes
        for (auto* item : scene_->items())
            item->setSelected(true);
        event->accept();
    } else if (event->matches(QKeySequence::Copy)) {
        // Ctrl+C — copy selected nodes to clipboard buffer
        clipboard_nodes_.clear();
        clipboard_edges_.clear();
        QSet<QString> selected_ids;
        for (auto* item : scene_->selectedItems()) {
            if (auto* node = dynamic_cast<NodeItem*>(item)) {
                clipboard_nodes_.append(node->node_def());
                selected_ids.insert(node->node_def().id);
            }
        }
        // Copy edges that connect two selected nodes
        WorkflowDef wf = scene_->serialize();
        for (const auto& ed : wf.edges) {
            if (selected_ids.contains(ed.source_node) && selected_ids.contains(ed.target_node))
                clipboard_edges_.append(ed);
        }
        LOG_INFO("NodeEditor", QString("Copied %1 nodes").arg(clipboard_nodes_.size()));
        event->accept();
    } else if (event->matches(QKeySequence::Paste)) {
        // Ctrl+V — paste nodes with offset and new IDs
        if (clipboard_nodes_.isEmpty()) {
            event->accept();
            return;
        }

        QMap<QString, QString> id_remap;
        QVector<NodeDef> pasted_nodes;
        QVector<EdgeDef> pasted_edges;
        for (const auto& nd : clipboard_nodes_) {
            NodeDef copy = nd;
            QString new_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            id_remap.insert(nd.id, new_id);
            copy.id = new_id;
            copy.x += 40;
            copy.y += 40;
            copy.name = nd.name + " (copy)";
            pasted_nodes.append(copy);
        }
        for (const auto& ed : clipboard_edges_) {
            EdgeDef copy = ed;
            copy.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            copy.source_node = id_remap.value(ed.source_node);
            copy.target_node = id_remap.value(ed.target_node);
            if (!copy.source_node.isEmpty() && !copy.target_node.isEmpty())
                pasted_edges.append(copy);
        }
        // Single undoable step for the whole paste, wiring included.
        push_add_nodes(pasted_nodes, pasted_edges, tr("Paste %1 node(s)").arg(pasted_nodes.size()));
        LOG_INFO("NodeEditor", QString("Pasted %1 nodes").arg(pasted_nodes.size()));
        event->accept();
    } else {
        QWidget::keyPressEvent(event);
    }
}

void NodeEditorScreen::build_ui() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── Toolbar ────────────────────────────────────────────────────
    toolbar_ = new NodeEditorToolbar(this);
    root->addWidget(toolbar_);

    // ── Separator ──────────────────────────────────────────────────
    auto* sep = new QFrame(this);
    sep->setFixedHeight(1);
    sep->setStyleSheet(QString("background: %1; border: none;").arg(ui::colors::BORDER_MED()));
    root->addWidget(sep);

    // ── 3-panel splitter ───────────────────────────────────────────
    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->setHandleWidth(1);
    splitter->setStyleSheet(QString("QSplitter::handle { background: %1; }"
                                    "QSplitter::handle:hover { background: %2; }")
                                .arg(ui::colors::BORDER_MED(), ui::colors::TEXT_DIM()));

    // Left: Node palette
    palette_ = new NodePalette(this);
    splitter->addWidget(palette_);

    // Center: Canvas
    scene_ = new NodeScene(this);
    canvas_ = new NodeCanvas(scene_);
    splitter->addWidget(canvas_);

    // Right: Properties panel
    properties_ = new NodePropertiesPanel(this);
    splitter->addWidget(properties_);

    splitter->setSizes({240, 700, 280});
    splitter->setStretchFactor(0, 0); // palette fixed
    splitter->setStretchFactor(1, 1); // canvas stretches
    splitter->setStretchFactor(2, 0); // properties fixed

    root->addWidget(splitter, 1);

    // ── Execution results panel (bottom drawer) ────────────────────
    results_panel_ = new ExecutionResultsPanel(this);
    root->addWidget(results_panel_);

    // ── MiniMap (bottom-right corner of canvas) ────────────────────
    minimap_ = new MiniMap(scene_, canvas_, canvas_);
    minimap_->move(canvas_->width() - 210, canvas_->height() - 160);
    minimap_->show();
}

void NodeEditorScreen::wire_signals() {
    // ── Canvas: drop from palette ──────────────────────────────────
    connect(canvas_, &NodeCanvas::node_drop_requested, this, &NodeEditorScreen::on_node_drop);

    // ── Scene: selection changed ───────────────────────────────────
    connect(scene_, &NodeScene::node_selection_changed, this, &NodeEditorScreen::on_node_selected);

    // ── Scene: duplicate and execute-from ──────────────────────────
    connect(scene_, &NodeScene::node_duplicate_requested, this, [this](const QString& node_id) {
        auto* item = scene_->find_node(node_id);
        if (!item)
            return;
        NodeDef copy = item->node_def();
        copy.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        copy.x += 40;
        copy.y += 40;
        copy.name += " (copy)";
        push_add_nodes({copy}, {}, tr("Duplicate %1").arg(item->node_def().name));
    });

    // ── Undoable-intent signals from the canvas ────────────────────────────
    // NodeScene announces user-initiated deletes/connects instead of applying
    // them, so every structural mutation lands on the QUndoStack.
    connect(scene_, &NodeScene::node_delete_requested, this, &NodeEditorScreen::on_delete_node);
    connect(scene_, &NodeScene::edge_delete_requested, this, [this](const QString& edge_id) {
        const EdgeDef ed = scene_->edge_def(edge_id);
        if (ed.id.isEmpty()) {
            scene_->remove_edge(edge_id); // dangling edge — drop it without history
            return;
        }
        undo_stack_->push(new commands::RemoveEdgeCommand(scene_, ed));
    });
    connect(scene_, &NodeScene::edge_create_requested, this,
            [this](const EdgeDef& ed) { undo_stack_->push(new commands::AddEdgeCommand(scene_, ed)); });
    connect(scene_, &NodeScene::node_move_started, this, &NodeEditorScreen::begin_move_snapshot);
    connect(scene_, &NodeScene::node_move_finished, this, &NodeEditorScreen::commit_move_snapshot);

    // ── Node execution flags (SETTINGS block in the properties panel) ──────
    connect(properties_, &NodePropertiesPanel::node_flag_changed, this,
            [this](const QString& node_id, const QString& flag, bool value) {
                auto* item = scene_->find_node(node_id);
                if (!item)
                    return;
                if (flag == QLatin1String("disabled"))
                    item->set_disabled(value);
                else if (flag == QLatin1String("continue_on_fail"))
                    item->set_continue_on_fail(value);
            });
    connect(scene_, &NodeScene::node_execute_from_requested, this, [this](const QString& node_id) {
        WorkflowDef wf = scene_->serialize();
        if (wf.nodes.isEmpty())
            return;
        wf.name = toolbar_->workflow_name();

        // Reset all node states
        for (auto* item : scene_->node_items())
            item->set_execution_state("idle");

        WorkflowService::instance().execute_from_node(wf, node_id);
    });

    // ── Properties: parameter changes ──────────────────────────────
    connect(properties_, &NodePropertiesPanel::param_changed, this, &NodeEditorScreen::on_param_changed);
    connect(properties_, &NodePropertiesPanel::name_changed, this, &NodeEditorScreen::on_name_changed);
    connect(properties_, &NodePropertiesPanel::delete_requested, this, &NodeEditorScreen::on_delete_node);

    // ── Toolbar actions ────────────────────────────────────────────
    connect(toolbar_, &NodeEditorToolbar::undo_clicked, undo_stack_, &QUndoStack::undo);
    connect(toolbar_, &NodeEditorToolbar::redo_clicked, undo_stack_, &QUndoStack::redo);
    connect(toolbar_, &NodeEditorToolbar::save_clicked, this, &NodeEditorScreen::on_save_workflow);
    connect(toolbar_, &NodeEditorToolbar::load_clicked, this, &NodeEditorScreen::on_load_workflow);
    connect(toolbar_, &NodeEditorToolbar::clear_clicked, this, &NodeEditorScreen::on_clear_workflow);
    connect(toolbar_, &NodeEditorToolbar::import_clicked, this, &NodeEditorScreen::on_import_workflow);
    connect(toolbar_, &NodeEditorToolbar::export_clicked, this, &NodeEditorScreen::on_export_workflow);
    connect(toolbar_, &NodeEditorToolbar::execute_clicked, this, &NodeEditorScreen::on_execute);
    connect(toolbar_, &NodeEditorToolbar::templates_clicked, this, &NodeEditorScreen::on_show_templates);
    connect(toolbar_, &NodeEditorToolbar::deploy_clicked, this, &NodeEditorScreen::on_deploy);

    // ── Undo stack state ───────────────────────────────────────────
    connect(undo_stack_, &QUndoStack::canUndoChanged, toolbar_, &NodeEditorToolbar::set_can_undo);
    connect(undo_stack_, &QUndoStack::canRedoChanged, toolbar_, &NodeEditorToolbar::set_can_redo);

    // ── Scene: deselection → clear properties ──────────────────────
    connect(scene_, &QGraphicsScene::selectionChanged, this, [this]() {
        auto selected = scene_->selectedItems();
        if (selected.isEmpty()) {
            properties_->clear();
        }
    });

    // ── WorkflowService signals ──────────────────────────────────
    auto& svc = WorkflowService::instance();
    connect(&svc, &WorkflowService::workflow_saved, this, [this](const QString& id) {
        current_workflow_id_ = id;
        LOG_INFO("NodeEditor", QString("Workflow saved: %1").arg(id));
    });
    connect(&svc, &WorkflowService::workflow_loaded, this, [this](const WorkflowDef& wf) {
        scene_->deserialize(wf);
        toolbar_->set_workflow_name(wf.name);
        current_workflow_id_ = wf.id;
        properties_->clear();
        // The stack's commands reference nodes from the previous graph — undoing
        // into them after a load would resurrect foreign nodes / drop live ones.
        reset_undo_history();
        LOG_INFO("NodeEditor", QString("Workflow loaded: %1").arg(wf.name));
    });
    connect(&svc, &WorkflowService::workflow_load_failed, this,
            [](const QString& err) { LOG_ERROR("NodeEditor", QString("Load failed: %1").arg(err)); });

    // ── Execution signals ────────────────────────────────────────────
    connect(&svc, &WorkflowService::execution_started, this, [this](const QString& wf_id) {
        results_panel_->set_started(wf_id);
        toolbar_->set_executing(true);
    });
    connect(&svc, &WorkflowService::node_execution_started, this, [this](const QString& node_id) {
        auto* item = scene_->find_node(node_id);
        if (item)
            item->set_execution_state("running");
        scene_->set_edges_animated(node_id, true);
    });
    connect(&svc, &WorkflowService::node_execution_completed, this,
            [this](const QString& node_id, const NodeExecutionResult& result) {
                auto* item = scene_->find_node(node_id);
                if (item)
                    item->set_execution_state(result.success ? "completed" : "error");
                scene_->set_edges_animated(node_id, false);
                results_panel_->add_node_result(result);
            });
    connect(&svc, &WorkflowService::execution_finished, this, [this](const WorkflowExecutionResult& result) {
        scene_->stop_all_edge_animations();
        results_panel_->set_finished(result);
        toolbar_->set_executing(false);
        QString msg = result.success ? QString("Execution completed in %1ms").arg(result.total_duration_ms)
                                     : QString("Execution failed: %1").arg(result.error);
        LOG_INFO("NodeEditor", msg);
    });
}

// ── Undo plumbing ──────────────────────────────────────────────────────

void NodeEditorScreen::push_add_nodes(const QVector<NodeDef>& nodes, const QVector<EdgeDef>& edges,
                                      const QString& label) {
    if (nodes.isEmpty())
        return;
    undo_stack_->push(new commands::AddNodesCommand(scene_, nodes, edges, label));
}

void NodeEditorScreen::push_remove_nodes(const QStringList& ids, const QString& label) {
    if (ids.isEmpty())
        return;
    QVector<NodeDef> defs;
    QSet<QString> id_set;
    defs.reserve(ids.size());
    for (const auto& id : ids) {
        if (auto* item = scene_->find_node(id)) {
            defs.append(item->node_def()); // carries the live x/y (itemChange keeps it current)
            id_set.insert(id);
        }
    }
    if (defs.isEmpty())
        return;
    const QVector<EdgeDef> edges = commands::edges_touching(scene_, id_set);
    undo_stack_->push(new commands::RemoveNodesCommand(scene_, defs, edges, label));
    properties_->clear();
}

void NodeEditorScreen::begin_move_snapshot() {
    // A QGraphicsScene delivers the press to exactly one item, so a press always
    // starts a fresh gesture — no nesting to reconcile. Snapshotting every node
    // (not just the pressed one) is what makes a multi-select drag undoable as a
    // single step.
    move_snapshot_.clear();
    for (auto* item : scene_->node_items())
        move_snapshot_.insert(item->node_def().id, item->pos());
}

void NodeEditorScreen::commit_move_snapshot() {
    if (move_snapshot_.isEmpty())
        return;

    QHash<QString, QPointF> before;
    QHash<QString, QPointF> after;
    for (auto* item : scene_->node_items()) {
        const QString id = item->node_def().id;
        auto it = move_snapshot_.constFind(id);
        if (it == move_snapshot_.constEnd())
            continue;
        if (it.value() != item->pos()) {
            before.insert(id, it.value());
            after.insert(id, item->pos());
        }
    }
    move_snapshot_.clear();
    if (after.isEmpty())
        return; // a click, not a drag
    undo_stack_->push(new commands::MoveNodesCommand(scene_, before, after));
}

void NodeEditorScreen::reset_undo_history() {
    if (undo_stack_)
        undo_stack_->clear();
    move_snapshot_.clear();
}

// ── Action handlers ────────────────────────────────────────────────────

void NodeEditorScreen::on_node_drop(const QString& type_id, const QPointF& scene_pos) {
    auto& registry = NodeRegistry::instance();
    const auto* type_def = registry.find(type_id);
    if (!type_def) {
        LOG_WARN("NodeEditor", QString("Unknown node type for drop: %1").arg(type_id));
        return;
    }

    NodeDef def;
    def.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    def.type = type_id;
    def.name = type_def->display_name;
    def.type_version = type_def->version;
    def.x = scene_pos.x();
    def.y = scene_pos.y();

    // Set default parameter values
    for (const auto& param : type_def->parameters) {
        if (!param.default_value.isUndefined() && !param.default_value.isNull())
            def.parameters[param.key] = param.default_value;
    }

    push_add_nodes({def}, {}, tr("Add %1").arg(type_def->display_name));
}

void NodeEditorScreen::on_node_selected(const QString& node_id) {
    auto* node_item = scene_->find_node(node_id);
    if (!node_item) {
        properties_->clear();
        return;
    }

    auto& registry = NodeRegistry::instance();
    const auto* type_def = registry.find(node_item->node_def().type);

    properties_->show_properties(&node_item->node_def(), type_def);
}

void NodeEditorScreen::on_param_changed(const QString& node_id, const QString& key, QJsonValue value) {
    auto* node_item = scene_->find_node(node_id);
    if (node_item) {
        node_item->set_parameter(key, value);
    }
}

void NodeEditorScreen::on_name_changed(const QString& node_id, const QString& new_name) {
    auto* node_item = scene_->find_node(node_id);
    if (node_item) {
        node_item->set_name(new_name);
    }
}

void NodeEditorScreen::on_delete_node(const QString& node_id) {
    auto* item = scene_->find_node(node_id);
    push_remove_nodes({node_id}, item ? tr("Delete %1").arg(item->node_def().name) : tr("Delete node"));
}

void NodeEditorScreen::on_clear_workflow() {
    if (scene_->is_empty())
        return;
    auto result = QMessageBox::question(
        this, tr("Clear Workflow"),
        tr("Clear all %1 node(s) and their connections?\n\nThis can be undone with Ctrl+Z.")
            .arg(scene_->node_items().size()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (result == QMessageBox::Yes) {
        undo_stack_->push(
            new commands::ReplaceGraphCommand(scene_, scene_->serialize(), WorkflowDef{}, tr("Clear workflow")));
        properties_->clear();
        LOG_INFO("NodeEditor", "Workflow cleared");
    }
}

void NodeEditorScreen::on_import_workflow() {
    // Importing replaces the whole canvas — guard the work already on it.
    if (!scene_->is_empty()) {
        const auto ans = QMessageBox::question(
            this, tr("Import Workflow"),
            tr("Importing replaces the %1 node(s) currently on the canvas.\n\nContinue?")
                .arg(scene_->node_items().size()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ans != QMessageBox::Yes)
            return;
    }

    QString path =
        QFileDialog::getOpenFileName(this, tr("Import Workflow"), {}, tr("JSON Files (*.json);;All Files (*)"));
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        LOG_ERROR("NodeEditor", QString("Failed to open file: %1").arg(path));
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (doc.isNull() || !doc.isObject()) {
        LOG_ERROR("NodeEditor", "Invalid JSON workflow file");
        return;
    }

    // Parse workflow from JSON
    QJsonObject obj = doc.object();
    WorkflowDef wf;
    wf.id = obj.value("id").toString(QUuid::createUuid().toString(QUuid::WithoutBraces));
    wf.name = obj.value("name").toString(tr("Imported Workflow"));
    wf.description = obj.value("description").toString();

    for (const auto& nv : obj.value("nodes").toArray()) {
        QJsonObject no = nv.toObject();
        NodeDef nd;
        nd.id = no.value("id").toString();
        nd.type = no.value("type").toString();
        nd.name = no.value("name").toString();
        nd.type_version = no.value("typeVersion").toInt(1);
        nd.x = no.value("position").toObject().value("x").toDouble();
        nd.y = no.value("position").toObject().value("y").toDouble();
        nd.parameters = no.value("parameters").toObject();
        nd.disabled = no.value("disabled").toBool();
        nd.continue_on_fail = no.value("continueOnFail").toBool();
        wf.nodes.append(nd);
    }

    for (const auto& ev : obj.value("edges").toArray()) {
        QJsonObject eo = ev.toObject();
        EdgeDef ed;
        ed.id = eo.value("id").toString();
        ed.source_node = eo.value("source").toString();
        ed.target_node = eo.value("target").toString();
        ed.source_port = eo.value("sourceHandle").toString();
        ed.target_port = eo.value("targetHandle").toString();
        wf.edges.append(ed);
    }

    scene_->deserialize(wf);
    toolbar_->set_workflow_name(wf.name);
    properties_->clear();
    reset_undo_history(); // the previous graph's commands no longer apply
    LOG_INFO("NodeEditor", QString("Imported workflow: %1").arg(wf.name));
}

void NodeEditorScreen::on_export_workflow() {
    QString path = QFileDialog::getSaveFileName(this, tr("Export Workflow"), "workflow.json",
                                                tr("JSON Files (*.json);;All Files (*)"));
    if (path.isEmpty())
        return;

    WorkflowDef wf = scene_->serialize();
    wf.name = toolbar_->workflow_name();

    QJsonObject obj;
    obj["id"] = wf.id;
    obj["name"] = wf.name;
    obj["description"] = wf.description;

    QJsonArray nodes_arr;
    for (const auto& nd : wf.nodes) {
        QJsonObject no;
        no["id"] = nd.id;
        no["type"] = nd.type;
        no["name"] = nd.name;
        no["typeVersion"] = nd.type_version;
        no["position"] = QJsonObject{{"x", nd.x}, {"y", nd.y}};
        no["parameters"] = nd.parameters;
        no["disabled"] = nd.disabled;
        no["continueOnFail"] = nd.continue_on_fail;
        nodes_arr.append(no);
    }
    obj["nodes"] = nodes_arr;

    QJsonArray edges_arr;
    for (const auto& ed : wf.edges) {
        QJsonObject eo;
        eo["id"] = ed.id;
        eo["source"] = ed.source_node;
        eo["target"] = ed.target_node;
        eo["sourceHandle"] = ed.source_port;
        eo["targetHandle"] = ed.target_port;
        edges_arr.append(eo);
    }
    obj["edges"] = edges_arr;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        LOG_ERROR("NodeEditor", QString("Failed to write file: %1").arg(path));
        return;
    }

    file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    LOG_INFO("NodeEditor", QString("Exported workflow to: %1").arg(path));
}

void NodeEditorScreen::on_save_workflow() {
    WorkflowDef wf = scene_->serialize();
    if (current_workflow_id_.isEmpty())
        current_workflow_id_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    wf.id = current_workflow_id_;
    wf.name = toolbar_->workflow_name();
    WorkflowService::instance().save_workflow(wf);
    ScreenStateManager::instance().notify_changed(this);
}

void NodeEditorScreen::on_load_workflow() {
    // Request workflow list, then show picker when signal arrives
    auto& svc = WorkflowService::instance();

    // One-shot connection for the list response
    auto* conn = new QMetaObject::Connection;
    *conn =
        connect(&svc, &WorkflowService::workflows_listed, this, [this, conn](const QVector<WorkflowDef>& workflows) {
            disconnect(*conn);
            delete conn;

            if (workflows.isEmpty()) {
                QMessageBox::information(this, tr("Load Workflow"), tr("No saved workflows found."));
                return;
            }

            // Build a simple picker dialog
            QStringList items;
            for (const auto& wf : workflows)
                items << QString("%1  (%2)").arg(wf.name, wf.updated_at);

            bool ok = false;
            QString chosen =
                QInputDialog::getItem(this, tr("Load Workflow"), tr("Select a workflow:"), items, 0, false, &ok);
            if (!ok)
                return;

            int idx = items.indexOf(chosen);
            if (idx >= 0 && idx < workflows.size())
                WorkflowService::instance().load_workflow(workflows[idx].id);
        });

    svc.list_workflows();
}

void NodeEditorScreen::on_auto_save() {
    if (scene_->node_items().isEmpty())
        return; // nothing to save

    WorkflowDef wf = scene_->serialize();
    if (current_workflow_id_.isEmpty())
        current_workflow_id_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    wf.id = current_workflow_id_;
    wf.name = toolbar_->workflow_name();
    WorkflowService::instance().save_workflow(wf);
    LOG_DEBUG("NodeEditor", "Auto-saved workflow");
}

void NodeEditorScreen::on_execute() {
    if (WorkflowService::instance().is_executing()) {
        WorkflowService::instance().stop_execution();
        LOG_INFO("NodeEditor", "Execution stopped by user");
        return;
    }

    WorkflowDef wf = scene_->serialize();
    if (wf.nodes.isEmpty()) {
        LOG_WARN("NodeEditor", "No nodes to execute");
        return;
    }

    wf.name = toolbar_->workflow_name();

    // A workflow is not just a computation — trading.* nodes place, modify and
    // cancel REAL orders. The algo tab gates live deployment behind an explicit
    // confirmation; this path had none, so pressing EXECUTE on a graph
    // containing a live order node sent it straight to the broker. Several
    // shipped templates (Price Alert Trading, Mean Reversion, Portfolio
    // Rebalancer, Pre-Trade Compliance) contain order nodes, so this is
    // reachable by loading a template and pressing one button.
    if (!confirm_live_order_nodes(wf, tr("Execute Workflow")))
        return;

    // Reset all node states to idle before execution
    for (auto* item : scene_->node_items())
        item->set_execution_state("idle");

    WorkflowService::instance().execute_workflow(wf);
}

bool NodeEditorScreen::confirm_live_order_nodes(const WorkflowDef& wf, const QString& action) {
    // Only MUTATING trading nodes matter. Read-only ones (get_quote,
    // get_positions, get_balance, …) are safe and must not nag, or the warning
    // becomes noise people click through — which is worse than no warning.
    static const QSet<QString> kMutatingTradingTypes = {
        QStringLiteral("trading.place_order"),   QStringLiteral("trading.cancel_order"),
        QStringLiteral("trading.modify_order"),  QStringLiteral("trading.close_position"),
        QStringLiteral("trading.bracket_order"), QStringLiteral("trading.trailing_stop"),
        QStringLiteral("trading.scale_in"),
    };

    QStringList live_rows;
    for (const auto& nd : wf.nodes) {
        if (nd.disabled || !kMutatingTradingTypes.contains(nd.type))
            continue;
        // `mode` is declared on the order nodes as select{paper,live}, default
        // "paper". Treat anything that is not explicitly "paper" as live: a
        // missing or unexpected value must fail toward asking, not toward
        // sending an order silently.
        const QString mode = nd.parameters.value(QStringLiteral("mode")).toString(QStringLiteral("paper"));
        if (mode.compare(QLatin1String("paper"), Qt::CaseInsensitive) == 0)
            continue;

        const QString broker = nd.parameters.value(QStringLiteral("broker")).toString();
        const QString symbol = nd.parameters.value(QStringLiteral("symbol")).toString();
        const QString side = nd.parameters.value(QStringLiteral("action")).toString();
        const QString qty = nd.parameters.value(QStringLiteral("quantity")).toVariant().toString();

        QString row = QStringLiteral("  • %1  [%2]").arg(nd.name.isEmpty() ? nd.type : nd.name, nd.type);
        if (!symbol.isEmpty())
            row += QStringLiteral("  %1").arg(symbol);
        if (!side.isEmpty())
            row += QStringLiteral("  %1").arg(side.toUpper());
        if (!qty.isEmpty())
            row += QStringLiteral(" x%1").arg(qty);
        if (!broker.isEmpty())
            row += QStringLiteral("  → %1").arg(broker);
        live_rows << row;
    }

    if (live_rows.isEmpty())
        return true; // nothing live — proceed silently

    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(action);
    box.setText(tr("This workflow contains %n LIVE order node(s).", "", live_rows.size()));
    box.setInformativeText(tr("Running it will place REAL orders with REAL money at your broker:\n\n%1\n\n"
                              "Set each node's Mode to \"paper\" if you only meant to simulate.")
                               .arg(live_rows.join(QLatin1Char('\n'))));
    auto* proceed = box.addButton(tr("Run with LIVE orders"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel); // Enter must not fire live orders
    box.exec();

    const bool go = (box.clickedButton() == proceed);
    LOG_WARN("NodeEditor", QString("%1: %2 live order node(s) — user %3")
                               .arg(action)
                               .arg(live_rows.size())
                               .arg(go ? "CONFIRMED" : "cancelled"));
    return go;
}

void NodeEditorScreen::on_show_templates() {
    QStringList templates = {
        // Beginner
        tr("1. Hello World — Trigger → Set Variable → Display"),
        tr("2. Stock Quote Lookup — Fetch quote for multiple tickers"),
        // Market Analysis
        tr("3. Multi-Indicator Scanner — RSI + MACD + Bollinger on a stock"),
        tr("4. Sector Correlation — Compare correlations across sector ETFs"),
        tr("5. Economic Dashboard — Fetch GDP, CPI, unemployment data"),
        // Trading
        tr("6. Price Alert Trading — Auto-trade when price crosses threshold"),
        tr("7. Mean Reversion Strategy — Buy oversold / sell overbought with risk checks"),
        tr("8. Portfolio Rebalancer — Check drift → optimize → place orders"),
        // Risk & Safety
        tr("9. Daily Risk Monitor — Positions → VaR → loss limit → alert"),
        tr("10. Pre-Trade Compliance — Full validation before order execution"),
        // Data & AI
        tr("11. News Sentiment Pipeline — Fetch news → NLP → filter → alert"),
        tr("12. AI Research Agent — Multi-agent analysis with mediator"),
        // Operations
        tr("13. Scheduled Report — Daily P&L report to email"),
        tr("14. Data Export Pipeline — Fetch → transform → CSV export"),
        tr("15. Webhook Automation — External trigger → process → notify"),
    };

    bool ok = false;
    QString chosen =
        QInputDialog::getItem(this, tr("Workflow Templates"), tr("Select a template:"), templates, 0, false, &ok);
    if (!ok)
        return;

    // Loading a template wipes the canvas. Confirm, and capture the current graph
    // so the whole swap is a single Ctrl+Z.
    const WorkflowDef before = scene_->serialize();
    if (!before.nodes.isEmpty()) {
        const auto ans = QMessageBox::question(
            this, tr("Workflow Templates"),
            tr("Loading a template replaces the %1 node(s) on the canvas.\n\nThis can be undone with Ctrl+Z. Continue?")
                .arg(before.nodes.size()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ans != QMessageBox::Yes)
            return;
    }

    int idx = templates.indexOf(chosen);
    scene_->clear_all();
    properties_->clear();
    auto& reg = NodeRegistry::instance();

    auto make_node = [&](const QString& type, const QString& name, double x, double y) -> NodeDef {
        NodeDef nd;
        nd.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        nd.type = type;
        nd.name = name;
        nd.x = x;
        nd.y = y;
        const auto* td = reg.find(type);
        if (td) {
            nd.type_version = td->version;
            for (const auto& p : td->parameters) {
                if (!p.default_value.isUndefined() && !p.default_value.isNull())
                    nd.parameters[p.key] = p.default_value;
            }
            scene_->add_node(nd, *td);
        }
        return nd;
    };

    auto make_edge = [&](const QString& src_id, const QString& src_port, const QString& tgt_id,
                         const QString& tgt_port) {
        EdgeDef ed;
        ed.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        ed.source_node = src_id;
        ed.target_node = tgt_id;
        ed.source_port = src_port;
        ed.target_port = tgt_port;
        scene_->add_edge(ed);
    };

    // Helper to set params on a node after creation
    auto set_param = [&](NodeDef& nd, const QString& key, const QJsonValue& val) {
        nd.parameters[key] = val;
        auto* item = scene_->find_node(nd.id);
        if (item)
            item->set_parameter(key, val);
    };

    if (idx == 0) {
        // ── 1. Hello World ─────────────────────────────────────────
        auto n1 = make_node("trigger.manual", "Start", 100, 200);
        auto n2 = make_node("core.set", "Set Greeting", 400, 200);
        set_param(n2, "key", "message");
        set_param(n2, "value", "Hello, World!");
        auto n3 = make_node("output.results_display", "Show Result", 700, 200);
        make_edge(n1.id, "output_main", n2.id, "input_0");
        make_edge(n2.id, "output_main", n3.id, "input_0");
        toolbar_->set_workflow_name("Hello World");
    } else if (idx == 1) {
        // ── 2. Stock Quote Lookup ──────────────────────────────────
        auto n1 = make_node("trigger.manual", "Start", 100, 200);
        auto n2 = make_node("market.get_quote", "AAPL Quote", 400, 100);
        set_param(n2, "symbol", "AAPL");
        auto n3 = make_node("market.get_quote", "MSFT Quote", 400, 250);
        set_param(n3, "symbol", "MSFT");
        auto n4 = make_node("market.get_quote", "GOOGL Quote", 400, 400);
        set_param(n4, "symbol", "GOOGL");
        auto n5 = make_node("control.merge", "Merge Quotes", 700, 250);
        auto n6 = make_node("output.results_display", "All Quotes", 1000, 250);
        make_edge(n1.id, "output_main", n2.id, "input_0");
        make_edge(n1.id, "output_main", n3.id, "input_0");
        make_edge(n1.id, "output_main", n4.id, "input_0");
        make_edge(n2.id, "output_main", n5.id, "input_0");
        make_edge(n3.id, "output_main", n5.id, "input_1");
        make_edge(n4.id, "output_main", n5.id, "input_2");
        make_edge(n5.id, "output_main", n6.id, "input_0");
        toolbar_->set_workflow_name("Stock Quote Lookup");
    } else if (idx == 2) {
        // ── 3. Multi-Indicator Scanner ─────────────────────────────
        auto n1 = make_node("trigger.manual", "Start", 100, 250);
        auto n2 = make_node("market.get_historical", "Get AAPL History", 400, 250);
        set_param(n2, "symbol", "AAPL");
        set_param(n2, "period", "6mo");
        auto n3 = make_node("analytics.technical_indicators", "RSI (14)", 750, 100);
        set_param(n3, "indicator", "RSI");
        set_param(n3, "period", 14);
        auto n4 = make_node("analytics.technical_indicators", "MACD", 750, 250);
        set_param(n4, "indicator", "MACD");
        auto n5 = make_node("analytics.technical_indicators", "Bollinger", 750, 400);
        set_param(n5, "indicator", "BBANDS");
        set_param(n5, "period", 20);
        auto n6 = make_node("control.merge", "Combine Signals", 1050, 250);
        auto n7 = make_node("output.results_display", "Indicator Report", 1350, 250);
        make_edge(n1.id, "output_main", n2.id, "input_0");
        make_edge(n2.id, "output_main", n3.id, "input_0");
        make_edge(n2.id, "output_main", n4.id, "input_0");
        make_edge(n2.id, "output_main", n5.id, "input_0");
        make_edge(n3.id, "output_main", n6.id, "input_0");
        make_edge(n4.id, "output_main", n6.id, "input_1");
        make_edge(n5.id, "output_main", n6.id, "input_2");
        make_edge(n6.id, "output_main", n7.id, "input_0");
        toolbar_->set_workflow_name("Multi-Indicator Scanner");
    } else if (idx == 3) {
        // ── 4. Sector Correlation ──────────────────────────────────
        auto n1 = make_node("trigger.manual", "Start", 100, 200);
        auto n2 = make_node("market.get_historical", "SPY", 400, 100);
        set_param(n2, "symbol", "SPY");
        set_param(n2, "period", "1y");
        auto n3 = make_node("market.get_historical", "XLK (Tech)", 400, 250);
        set_param(n3, "symbol", "XLK");
        set_param(n3, "period", "1y");
        auto n4 = make_node("market.get_historical", "XLF (Finance)", 400, 400);
        set_param(n4, "symbol", "XLF");
        set_param(n4, "period", "1y");
        auto n5 = make_node("control.merge", "Merge", 700, 250);
        auto n6 = make_node("analytics.correlation_matrix", "Correlation", 1000, 250);
        set_param(n6, "method", "pearson");
        auto n7 = make_node("output.results_display", "Correlation Matrix", 1300, 250);
        make_edge(n1.id, "output_main", n2.id, "input_0");
        make_edge(n1.id, "output_main", n3.id, "input_0");
        make_edge(n1.id, "output_main", n4.id, "input_0");
        make_edge(n2.id, "output_main", n5.id, "input_0");
        make_edge(n3.id, "output_main", n5.id, "input_1");
        make_edge(n4.id, "output_main", n5.id, "input_2");
        make_edge(n5.id, "output_main", n6.id, "input_0");
        make_edge(n6.id, "output_main", n7.id, "input_0");
        toolbar_->set_workflow_name("Sector Correlation Analysis");
    } else if (idx == 4) {
        // ── 5. Economic Dashboard ──────────────────────────────────
        auto n1 = make_node("trigger.manual", "Start", 100, 250);
        auto n2 = make_node("market.get_economics", "GDP", 400, 100);
        set_param(n2, "indicator", "GDP");
        auto n3 = make_node("market.get_economics", "CPI", 400, 250);
        set_param(n3, "indicator", "CPI");
        auto n4 = make_node("market.get_economics", "Unemployment", 400, 400);
        set_param(n4, "indicator", "UNEMPLOYMENT");
        auto n5 = make_node("control.merge", "Merge Indicators", 700, 250);
        auto n6 = make_node("output.results_display", "Econ Dashboard", 1000, 250);
        make_edge(n1.id, "output_main", n2.id, "input_0");
        make_edge(n1.id, "output_main", n3.id, "input_0");
        make_edge(n1.id, "output_main", n4.id, "input_0");
        make_edge(n2.id, "output_main", n5.id, "input_0");
        make_edge(n3.id, "output_main", n5.id, "input_1");
        make_edge(n4.id, "output_main", n5.id, "input_2");
        make_edge(n5.id, "output_main", n6.id, "input_0");
        toolbar_->set_workflow_name("Economic Dashboard");
    } else if (idx == 5) {
        // ── 6. Price Alert Trading ─────────────────────────────────
        auto n1 = make_node("trigger.price_alert", "AAPL > $200", 100, 200);
        set_param(n1, "symbol", "AAPL");
        set_param(n1, "condition", "above");
        set_param(n1, "price", 200.0);
        auto n2 = make_node("market.get_quote", "Latest Quote", 400, 200);
        set_param(n2, "symbol", "AAPL");
        auto n3 = make_node("safety.risk_check", "Risk Check", 700, 200);
        auto n4 = make_node("trading.place_order", "Buy AAPL", 1000, 100);
        set_param(n4, "symbol", "AAPL");
        set_param(n4, "side", "buy");
        set_param(n4, "quantity", 10);
        set_param(n4, "broker", "paper");
        auto n5 = make_node("notify.telegram", "Alert: Blocked", 1000, 350);
        auto n6 = make_node("output.results_display", "Order Result", 1300, 100);
        auto n7 = make_node("output.results_display", "Risk Alert", 1300, 350);
        make_edge(n1.id, "output_main", n2.id, "input_0");
        make_edge(n2.id, "output_main", n3.id, "input_0");
        make_edge(n3.id, "output_pass", n4.id, "input_0");
        make_edge(n3.id, "output_fail", n5.id, "input_0");
        make_edge(n4.id, "output_main", n6.id, "input_0");
        make_edge(n5.id, "output_main", n7.id, "input_0");
        toolbar_->set_workflow_name("Price Alert Auto-Trade");
    } else if (idx == 6) {
        // ── 7. Mean Reversion Strategy ─────────────────────────────
        auto n1 = make_node("trigger.manual", "Start", 100, 250);
        auto n2 = make_node("market.get_historical", "Get TSLA 3mo", 400, 250);
        set_param(n2, "symbol", "TSLA");
        set_param(n2, "period", "3mo");
        auto n3 = make_node("analytics.technical_indicators", "RSI (14)", 700, 250);
        set_param(n3, "indicator", "RSI");
        set_param(n3, "period", 14);
        auto n4 = make_node("control.if_else", "RSI < 30?", 1000, 250);
        auto n5 = make_node("safety.position_size_limit", "Size Check", 1300, 100);
        auto n6 = make_node("trading.place_order", "Buy TSLA", 1600, 100);
        set_param(n6, "symbol", "TSLA");
        set_param(n6, "side", "buy");
        set_param(n6, "quantity", 5);
        set_param(n6, "broker", "paper");
        auto n7 = make_node("core.set", "Skip: Not Oversold", 1300, 400);
        set_param(n7, "key", "action");
        set_param(n7, "value", "no_trade");
        auto n8 = make_node("output.results_display", "Trade Result", 1900, 100);
        auto n9 = make_node("output.results_display", "Skip Result", 1600, 400);
        make_edge(n1.id, "output_main", n2.id, "input_0");
        make_edge(n2.id, "output_main", n3.id, "input_0");
        make_edge(n3.id, "output_main", n4.id, "input_0");
        make_edge(n4.id, "output_true", n5.id, "input_0");
        make_edge(n4.id, "output_false", n7.id, "input_0");
        make_edge(n5.id, "output_pass", n6.id, "input_0");
        make_edge(n6.id, "output_main", n8.id, "input_0");
        make_edge(n7.id, "output_main", n9.id, "input_0");
        toolbar_->set_workflow_name("Mean Reversion Strategy");
    } else if (idx == 7) {
        // ── 8. Portfolio Rebalancer ─────────────────────────────────
        auto n1 = make_node("trigger.manual", "Start", 100, 200);
        auto n2 = make_node("trading.get_holdings", "Current Holdings", 400, 200);
        auto n3 = make_node("analytics.portfolio_optimization", "Optimize", 700, 200);
        set_param(n3, "method", "mean_variance");
        auto n4 = make_node("analytics.risk_analysis", "Risk Check", 1000, 200);
        set_param(n4, "method", "parametric_var");
        auto n5 = make_node("safety.loss_limit", "Loss Limit", 1300, 200);
        auto n6 = make_node("trading.place_order", "Rebalance Order", 1600, 100);
        set_param(n6, "broker", "paper");
        auto n7 = make_node("notify.email", "Limit Breached", 1600, 350);
        auto n8 = make_node("output.results_display", "Order Confirmation", 1900, 100);
        auto n9 = make_node("output.results_display", "Alert", 1900, 350);
        make_edge(n1.id, "output_main", n2.id, "input_0");
        make_edge(n2.id, "output_main", n3.id, "input_0");
        make_edge(n3.id, "output_main", n4.id, "input_0");
        make_edge(n4.id, "output_main", n5.id, "input_0");
        make_edge(n5.id, "output_pass", n6.id, "input_0");
        make_edge(n5.id, "output_fail", n7.id, "input_0");
        make_edge(n6.id, "output_main", n8.id, "input_0");
        make_edge(n7.id, "output_main", n9.id, "input_0");
        toolbar_->set_workflow_name("Portfolio Rebalancer");
    } else if (idx == 8) {
        // ── 9. Daily Risk Monitor ───────────────────────────────────
        auto n1 = make_node("trigger.schedule", "Daily 9:30 AM", 100, 200);
        set_param(n1, "cron", "30 9 * * 1-5");
        auto n2 = make_node("trading.get_positions", "Open Positions", 400, 200);
        auto n3 = make_node("analytics.risk_analysis", "VaR Analysis", 700, 200);
        set_param(n3, "method", "historical_var");
        set_param(n3, "confidence", 0.99);
        auto n4 = make_node("safety.loss_limit", "Check Daily Limit", 1000, 200);
        auto n5 = make_node("notify.slack", "All Clear", 1300, 100);
        set_param(n5, "channel", "#risk-alerts");
        auto n6 = make_node("notify.discord", "RISK ALERT!", 1300, 350);
        auto n7 = make_node("output.results_display", "Risk Report", 1600, 200);
        make_edge(n1.id, "output_main", n2.id, "input_0");
        make_edge(n2.id, "output_main", n3.id, "input_0");
        make_edge(n3.id, "output_main", n4.id, "input_0");
        make_edge(n4.id, "output_pass", n5.id, "input_0");
        make_edge(n4.id, "output_fail", n6.id, "input_0");
        make_edge(n5.id, "output_main", n7.id, "input_0");
        toolbar_->set_workflow_name("Daily Risk Monitor");
    } else if (idx == 9) {
        // ── 10. Pre-Trade Compliance ────────────────────────────────
        auto n1 = make_node("trigger.manual", "New Order", 100, 250);
        auto n2 = make_node("safety.trading_hours", "Market Open?", 400, 250);
        set_param(n2, "exchange", "NYSE");
        auto n3 = make_node("safety.risk_check", "Position Limits", 700, 150);
        auto n4 = make_node("safety.position_size_limit", "Size Limits", 1000, 150);
        auto n5 = make_node("safety.loss_limit", "PnL Limits", 1300, 150);
        auto n6 = make_node("trading.place_order", "Execute Order", 1600, 150);
        set_param(n6, "broker", "paper");
        auto n7 = make_node("core.set", "Market Closed", 700, 400);
        set_param(n7, "key", "reason");
        set_param(n7, "value", "Market is closed");
        auto n8 = make_node("output.results_display", "Execution Result", 1900, 150);
        auto n9 = make_node("output.results_display", "Rejected", 1000, 400);
        make_edge(n1.id, "output_main", n2.id, "input_0");
        make_edge(n2.id, "output_open", n3.id, "input_0");
        make_edge(n2.id, "output_closed", n7.id, "input_0");
        make_edge(n3.id, "output_pass", n4.id, "input_0");
        make_edge(n4.id, "output_pass", n5.id, "input_0");
        make_edge(n5.id, "output_pass", n6.id, "input_0");
        make_edge(n6.id, "output_main", n8.id, "input_0");
        make_edge(n7.id, "output_main", n9.id, "input_0");
        toolbar_->set_workflow_name("Pre-Trade Compliance");
    } else if (idx == 10) {
        // ── 11. News Sentiment Pipeline ─────────────────────────────
        auto n1 = make_node("trigger.news_event", "Tech News", 100, 200);
        set_param(n1, "keywords", "AAPL,earnings,revenue,guidance");
        auto n2 = make_node("market.get_news", "Fetch News", 400, 200);
        set_param(n2, "symbol", "AAPL");
        set_param(n2, "limit", 20);
        auto n3 = make_node("transform.filter", "Filter Relevant", 700, 200);
        set_param(n3, "field", "relevance");
        set_param(n3, "operator", "greater_than");
        set_param(n3, "value", "0.7");
        auto n4 = make_node("agent.single", "Sentiment Agent", 1000, 200);
        set_param(n4, "agent_type", "economic");
        set_param(n4, "prompt", "Analyze sentiment of these news articles. Rate bullish/bearish 1-10.");
        auto n5 = make_node("control.if_else", "Bullish?", 1300, 200);
        auto n6 = make_node("notify.telegram", "Buy Signal", 1600, 100);
        auto n7 = make_node("core.set", "Hold", 1600, 350);
        set_param(n7, "key", "action");
        set_param(n7, "value", "hold_position");
        auto n8 = make_node("output.results_display", "Signal", 1900, 100);
        auto n9 = make_node("output.results_display", "No Action", 1900, 350);
        make_edge(n1.id, "output_main", n2.id, "input_0");
        make_edge(n2.id, "output_main", n3.id, "input_0");
        make_edge(n3.id, "output_main", n4.id, "input_0");
        make_edge(n4.id, "output_main", n5.id, "input_0");
        make_edge(n5.id, "output_true", n6.id, "input_0");
        make_edge(n5.id, "output_false", n7.id, "input_0");
        make_edge(n6.id, "output_main", n8.id, "input_0");
        make_edge(n7.id, "output_main", n9.id, "input_0");
        toolbar_->set_workflow_name("News Sentiment Pipeline");
    } else if (idx == 11) {
        // ── 12. AI Research Agent ───────────────────────────────────
        auto n1 = make_node("trigger.manual", "Start Research", 100, 250);
        auto n2 = make_node("market.get_quote", "Current Price", 400, 100);
        set_param(n2, "symbol", "NVDA");
        auto n3 = make_node("market.get_fundamentals", "Fundamentals", 400, 300);
        set_param(n3, "symbol", "NVDA");
        set_param(n3, "type", "overview");
        auto n4 = make_node("market.get_news", "Recent News", 400, 500);
        set_param(n4, "symbol", "NVDA");
        auto n5 = make_node("control.merge", "Combine Data", 700, 250);
        auto n6 = make_node("agent.single", "Bull Case Agent", 1000, 100);
        set_param(n6, "agent_type", "investor");
        set_param(n6, "prompt", "Make the bull case for this stock based on the data.");
        auto n7 = make_node("agent.single", "Bear Case Agent", 1000, 400);
        set_param(n7, "agent_type", "hedge_fund");
        set_param(n7, "prompt", "Make the bear case for this stock. Identify risks.");
        auto n8 = make_node("agent.mediator", "Synthesize", 1300, 250);
        set_param(n8, "mode", "debate");
        set_param(n8, "mediator_prompt", "Synthesize bull and bear cases into a balanced investment thesis.");
        auto n9 = make_node("output.results_display", "Research Report", 1600, 250);
        make_edge(n1.id, "output_main", n2.id, "input_0");
        make_edge(n1.id, "output_main", n3.id, "input_0");
        make_edge(n1.id, "output_main", n4.id, "input_0");
        make_edge(n2.id, "output_main", n5.id, "input_0");
        make_edge(n3.id, "output_main", n5.id, "input_1");
        make_edge(n4.id, "output_main", n5.id, "input_2");
        make_edge(n5.id, "output_main", n6.id, "input_0");
        make_edge(n5.id, "output_main", n7.id, "input_0");
        make_edge(n6.id, "output_main", n8.id, "input_0");
        make_edge(n7.id, "output_main", n8.id, "input_1");
        make_edge(n8.id, "output_main", n9.id, "input_0");
        toolbar_->set_workflow_name("AI Research Agent");
    } else if (idx == 12) {
        // ── 13. Scheduled Report ────────────────────────────────────
        auto n1 = make_node("trigger.schedule", "Daily 5 PM", 100, 200);
        set_param(n1, "cron", "0 17 * * 1-5");
        auto n2 = make_node("trading.get_positions", "Positions", 400, 100);
        auto n3 = make_node("trading.get_balance", "Balance", 400, 300);
        auto n4 = make_node("control.merge", "Merge", 700, 200);
        auto n5 = make_node("analytics.performance_metrics", "Daily P&L", 1000, 200);
        set_param(n5, "benchmark", "SPY");
        auto n6 = make_node("notify.email", "Send Report", 1300, 200);
        set_param(n6, "subject", "Daily Portfolio Report");
        auto n7 = make_node("output.results_display", "Report", 1600, 200);
        make_edge(n1.id, "output_main", n2.id, "input_0");
        make_edge(n1.id, "output_main", n3.id, "input_0");
        make_edge(n2.id, "output_main", n4.id, "input_0");
        make_edge(n3.id, "output_main", n4.id, "input_1");
        make_edge(n4.id, "output_main", n5.id, "input_0");
        make_edge(n5.id, "output_main", n6.id, "input_0");
        make_edge(n6.id, "output_main", n7.id, "input_0");
        toolbar_->set_workflow_name("Scheduled Daily Report");
    } else if (idx == 13) {
        // ── 14. Data Export Pipeline ────────────────────────────────
        auto n1 = make_node("trigger.manual", "Start", 100, 200);
        auto n2 = make_node("utility.http_request", "Fetch API Data", 400, 200);
        set_param(n2, "url", "https://api.example.com/data");
        set_param(n2, "method", "GET");
        auto n3 = make_node("transform.filter", "Filter Valid", 700, 200);
        set_param(n3, "field", "status");
        set_param(n3, "operator", "equals");
        set_param(n3, "value", "active");
        auto n4 = make_node("transform.sort", "Sort by Date", 1000, 200);
        set_param(n4, "field", "date");
        set_param(n4, "direction", "desc");
        auto n5 = make_node("transform.deduplicate", "Remove Dupes", 1300, 200);
        set_param(n5, "field", "id");
        auto n6 = make_node("file.spreadsheet", "Export CSV", 1600, 200);
        set_param(n6, "operation", "write");
        set_param(n6, "format", "csv");
        set_param(n6, "path", "output/export.csv");
        auto n7 = make_node("output.results_display", "Export Done", 1900, 200);
        make_edge(n1.id, "output_main", n2.id, "input_0");
        make_edge(n2.id, "output_main", n3.id, "input_0");
        make_edge(n3.id, "output_main", n4.id, "input_0");
        make_edge(n4.id, "output_main", n5.id, "input_0");
        make_edge(n5.id, "output_main", n6.id, "input_0");
        make_edge(n6.id, "output_main", n7.id, "input_0");
        toolbar_->set_workflow_name("Data Export Pipeline");
    } else if (idx == 14) {
        // ── 15. Webhook Automation ──────────────────────────────────
        auto n1 = make_node("trigger.webhook", "Incoming Webhook", 100, 200);
        set_param(n1, "path", "/trading-signal");
        set_param(n1, "method", "POST");
        auto n2 = make_node("format.json", "Parse Payload", 400, 200);
        set_param(n2, "operation", "parse");
        auto n3 = make_node("control.if_else", "Valid Signal?", 700, 200);
        auto n4 = make_node("safety.risk_check", "Risk Check", 1000, 100);
        auto n5 = make_node("trading.place_order", "Execute Trade", 1300, 100);
        set_param(n5, "broker", "paper");
        auto n6 = make_node("notify.discord", "Invalid Signal", 1000, 350);
        auto n7 = make_node("output.results_display", "Trade Executed", 1600, 100);
        auto n8 = make_node("output.results_display", "Rejected", 1300, 350);
        make_edge(n1.id, "output_main", n2.id, "input_0");
        make_edge(n2.id, "output_main", n3.id, "input_0");
        make_edge(n3.id, "output_true", n4.id, "input_0");
        make_edge(n3.id, "output_false", n6.id, "input_0");
        make_edge(n4.id, "output_pass", n5.id, "input_0");
        make_edge(n5.id, "output_main", n7.id, "input_0");
        make_edge(n6.id, "output_main", n8.id, "input_0");
        toolbar_->set_workflow_name("Webhook Automation");
    }

    current_workflow_id_.clear();
    // The scene already holds the template; record the swap so Ctrl+Z restores
    // whatever was on the canvas before. (push() re-applies `after`, which
    // rebuilds an identical graph.)
    undo_stack_->push(new commands::ReplaceGraphCommand(scene_, before, scene_->serialize(), tr("Load template")));
    LOG_INFO("NodeEditor", QString("Loaded template: %1").arg(chosen));
}

void NodeEditorScreen::on_deploy() {
    DeployDialog dlg(toolbar_->workflow_name(), this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    WorkflowDef wf = scene_->serialize();
    if (current_workflow_id_.isEmpty())
        current_workflow_id_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    wf.id = current_workflow_id_;
    wf.name = dlg.workflow_name();
    wf.description = dlg.workflow_description();
    // There is no scheduler or webhook runtime, so a saved workflow is never a
    // live deployment. Persist it as a Draft rather than an "idle" (active,
    // waiting-for-trigger) deployment so listings don't imply it will run again.
    wf.status = WorkflowStatus::Draft;

    toolbar_->set_workflow_name(wf.name);
    WorkflowService::instance().save_workflow(wf);

    if (dlg.is_deploy()) {
        LOG_INFO("NodeEditor", QString("Saved and ran workflow once: %1").arg(wf.name));
        // "Deploy" is save + run once. There is no scheduler/webhook listener to
        // fire trigger.* / schedule nodes again, so the workflow does not stay active.
        on_execute();
    } else {
        LOG_INFO("NodeEditor", QString("Saved draft: %1").arg(wf.name));
    }
}

// ── IStatefulScreen ───────────────────────────────────────────────────────────

QVariantMap NodeEditorScreen::save_state() const {
    return {{"workflow_id", current_workflow_id_}};
}

void NodeEditorScreen::restore_state(const QVariantMap& state) {
    const QString wf_id = state.value("workflow_id").toString();
    if (wf_id.isEmpty())
        return;

    // wire_signals() already connected workflow_loaded → scene_->deserialize().
    // Just trigger the load — the existing handler in wire_signals() does the rest.
    current_workflow_id_ = wf_id;
    WorkflowService::instance().load_workflow(wf_id);
}

} // namespace fincept::workflow
