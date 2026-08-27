// src/services/agents/AgentService_Execution.cpp
//
// Hot path for running a single agent / team / multi-agent flow:
// run_agent, run_agent_streaming, route_query (intent routing), run_team,
// run_agent_structured, execute_routed_query, execute_multi_query.
//
// Part of the partial-class split of AgentService.cpp.

#include "core/logging/Logger.h"
#include "datahub/DataHub.h"
#include "datahub/TopicPolicy.h"
#include "mcp/McpProvider.h"
#include "mcp/McpTypes.h"
#include "mcp/TerminalMcpBridge.h"
#include "python/PythonRunner.h"
#include "services/agents/AgentService.h"
#include "services/llm/LlmService.h"
#include "storage/cache/CacheManager.h"
#include "storage/repositories/LlmConfigRepository.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <QUuid>
#include <QVariant>

#include <memory>

namespace fincept::services {

QString AgentService::run_agent(const QString& query, const QJsonObject& config) {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    LOG_INFO("AgentService", QString("Running agent query [%1]: %2").arg(req_id.left(8), query.left(80)));

    QJsonObject params;
    params["query"] = query;

    QPointer<AgentService> self = this;
    run_python_stdin("run", params, config, [self, req_id](bool ok, QJsonObject result) {
        if (!self)
            return;
        AgentExecutionResult r;
        r.request_id = req_id;
        r.success = ok && result["success"].toBool(ok);
        r.execution_time_ms = result["execution_time_ms"].toInt();

        if (r.success) {
            if (result.contains("response"))
                r.response = result["response"].toString();
            else if (result.contains("result"))
                r.response = result["result"].toString();
            else if (result.contains("data"))
                r.response = QJsonDocument(result["data"].toObject()).toJson(QJsonDocument::Indented);
            else
                r.response = QJsonDocument(result).toJson(QJsonDocument::Indented);
        } else {
            r.error = result["error"].toString("Agent execution failed");
        }

        emit self->agent_result(r);
        self->publish_agent_result(r, /*final=*/true);
    });
    return req_id;
}

// ── Streaming agent execution ─────────────────────────────────────────────────

QString AgentService::run_agent_streaming(const QString& query, const QJsonObject& config) {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    LOG_INFO("AgentService", QString("Streaming agent query [%1]: %2").arg(req_id.left(8), query.left(80)));

    auto& py = python::PythonRunner::instance();
    if (!py.is_available()) {
        AgentExecutionResult r;
        r.request_id = req_id;
        r.success = false;
        r.error = "Python not available";
        emit agent_stream_done(r);
        publish_agent_result(r, /*final=*/true);
        return req_id;
    }

    QJsonObject params;
    params["query"] = query;

    QJsonObject payload = build_payload("run", params, config);
    QByteArray payload_bytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);

    QString python_path = py.python_path();
    QString script_path = py.scripts_dir() + "/agents/finagent_core/main.py";

    auto* proc = new QProcess(this);
    // Share the standard Python env + cwd + Windows console suppression with
    // PythonRunner so every finagent spawn sees the same FINCEPT_DATA_DIR,
    // FINAGENT_DATA_DIR, and PYTHONPATH.
    proc->setProcessEnvironment(py.build_python_env());
    proc->setWorkingDirectory(py.scripts_dir());
#ifdef _WIN32
    proc->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* cpa) {
        cpa->flags |= 0x08000000; // CREATE_NO_WINDOW
    });
#endif
    QPointer<AgentService> self = this;
    auto timer = std::make_shared<QElapsedTimer>();
    auto accumulated = std::make_shared<QString>();
    auto final_json_line = std::make_shared<QString>();
    auto done_emitted = std::make_shared<bool>(false);
    timer->start();

    // Read stdout line by line as process writes
    connect(proc, &QProcess::readyReadStandardOutput, this,
            [self, proc, accumulated, final_json_line, done_emitted, req_id]() {
                if (!self)
                    return;
                while (proc->canReadLine()) {
                    QString line = QString::fromUtf8(proc->readLine()).trimmed();
                    if (line.isEmpty())
                        continue;

                    // Python emits "TOKEN: {content}" (stream_print adds a space after colon)
                    auto extract = [](const QString& s, const QString& prefix) -> QString {
                        QString rest = s.mid(prefix.length());
                        // Strip one leading space if present
                        if (rest.startsWith(' '))
                            rest = rest.mid(1);
                        // Unescape \n -> newline
                        rest.replace("\\n", "\n").replace("\\\\", "\\");
                        return rest;
                    };

                    if (line.startsWith('{')) {
                        // Final JSON result line — capture it for the finished handler
                        *final_json_line = line;
                    } else if (line.startsWith("TOKEN:")) {
                        QString token = extract(line, "TOKEN:");
                        *accumulated += token;
                        emit self->agent_stream_token(req_id, token);
                        self->publish_agent_token(req_id, token);
                    } else if (line.startsWith("THINKING:")) {
                        QString status = extract(line, "THINKING:");
                        emit self->agent_stream_thinking(req_id, status);
                        self->publish_agent_status(req_id, status);
                    } else if (line.startsWith("TOOL:")) {
                        QString status = "Tool: " + extract(line, "TOOL:");
                        emit self->agent_stream_thinking(req_id, status);
                        self->publish_agent_status(req_id, status);
                    } else if (line.startsWith("TOOL_RESULT:")) {
                        QString status = "Result: " + extract(line, "TOOL_RESULT:");
                        emit self->agent_stream_thinking(req_id, status);
                        self->publish_agent_status(req_id, status);
                    } else if (line.startsWith("DONE:")) {
                        // DONE carries the full cleaned response — use it if accumulated is empty
                        QString done_content = extract(line, "DONE:");
                        if (accumulated->trimmed().isEmpty() && !done_content.isEmpty())
                            *accumulated = done_content;
                    } else if (line.startsWith("ERROR:")) {
                        if (!*done_emitted) {
                            *done_emitted = true;
                            AgentExecutionResult r;
                            r.request_id = req_id;
                            r.success = false;
                            r.error = extract(line, "ERROR:");
                            emit self->agent_stream_done(r);
                            self->publish_agent_result(r, /*final=*/true);
                        }
                    }
                }
            });

    connect(
        proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
        [self, proc, accumulated, final_json_line, done_emitted, timer, req_id](int exit_code, QProcess::ExitStatus) {
            int elapsed = timer->elapsed();

            // Drain any remaining stdout
            QString remaining = QString::fromUtf8(proc->readAllStandardOutput());
            QString stderr_str = QString::fromUtf8(proc->readAllStandardError());
            proc->deleteLater();

            if (!self || *done_emitted)
                return;
            *done_emitted = true;

            // Scan remaining stdout for final JSON line (may not have been read yet)
            for (const QString& line : remaining.split('\n')) {
                QString t = line.trimmed();
                if (t.startsWith('{'))
                    *final_json_line = t;
            }

            AgentExecutionResult r;
            r.request_id = req_id;
            r.execution_time_ms = elapsed;

            QJsonDocument doc = QJsonDocument::fromJson(final_json_line->toUtf8());
            if (!doc.isNull() && doc.object()["success"].toBool()) {
                QString json_response = doc.object()["response"].toString();
                r.success = true;
                r.response = json_response.isEmpty() ? accumulated->trimmed() : json_response;
            } else if (!accumulated->trimmed().isEmpty()) {
                r.success = true;
                r.response = accumulated->trimmed();
            } else {
                r.success = false;
                r.error = exit_code != 0 ? stderr_str.left(500) : "No response received";
            }

            LOG_INFO("AgentService", QString("Streaming completed in %1ms").arg(elapsed));
            emit self->agent_stream_done(r);
            self->publish_agent_result(r, /*final=*/true);
        });

    connect(proc, &QProcess::errorOccurred, this,
            [self, proc, accumulated, final_json_line, done_emitted, timer, req_id](QProcess::ProcessError) {
                QString err = proc->errorString();
                proc->deleteLater();
                if (!self || *done_emitted)
                    return;
                *done_emitted = true;
                AgentExecutionResult r;
                r.request_id = req_id;
                r.success = false;
                r.error = "Process error: " + err;
                emit self->agent_stream_done(r);
                self->publish_agent_result(r, /*final=*/true);
            });

    LOG_INFO("AgentService", QString("Starting streaming agent (%1 bytes payload)").arg(payload_bytes.size()));
    proc->start(python_path, {script_path, "--stdin", "--stream"});
    proc->write(payload_bytes);
    proc->closeWriteChannel();
    return req_id;
}

// ── Query routing ────────────────────────────────────────────────────────────

QString AgentService::route_query(const QString& query) {
    LOG_INFO("AgentService", QString("Routing query: %1").arg(query.left(80)));

    QJsonObject params;
    params["query"] = query;

    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QPointer<AgentService> self = this;
    run_python_stdin("route_query", params, {}, [self, req_id](bool ok, QJsonObject result) {
        if (!self)
            return;
        RoutingResult r;
        r.request_id = req_id;
        r.success = ok && result["success"].toBool(ok);
        r.agent_id = result["agent_id"].toString();
        r.intent = result["intent"].toString();
        r.confidence = result["confidence"].toDouble();
        r.config = result["config"].toObject();

        QJsonArray kw = result["matched_keywords"].toArray();
        for (const auto& k : kw)
            r.matched_keywords.append(k.toString());

        emit self->routing_result(r);
        self->publish_routing_result(r);
    });
    return req_id;
}

// ── Team execution ───────────────────────────────────────────────────────────

QString AgentService::run_team(const QString& query, const QJsonObject& team_config) {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    LOG_INFO("AgentService", QString("Running team query [%1]: %2").arg(req_id.left(8), query.left(80)));

    auto& py = python::PythonRunner::instance();
    if (!py.is_available()) {
        AgentExecutionResult r;
        r.request_id = req_id;
        r.success = false;
        r.error = "Python not available";
        emit agent_stream_done(r);
        publish_agent_result(r, /*final=*/true);
        return req_id;
    }

    QJsonObject params;
    params["query"] = query;
    params["team_config"] = team_config;

    // Promote coordinator model to active_llm so Python resolves it correctly
    QJsonObject coord_config;
    if (team_config.contains("model"))
        coord_config["model"] = team_config["model"];

    QJsonObject payload = build_payload("run_team", params, coord_config);
    QByteArray payload_bytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);

    QString python_path = py.python_path();
    QString script_path = py.scripts_dir() + "/agents/finagent_core/main.py";

    auto* proc = new QProcess(this);
    // Share the standard Python env + cwd + Windows console suppression with
    // PythonRunner so every finagent spawn sees the same FINCEPT_DATA_DIR,
    // FINAGENT_DATA_DIR, and PYTHONPATH.
    proc->setProcessEnvironment(py.build_python_env());
    proc->setWorkingDirectory(py.scripts_dir());
#ifdef _WIN32
    proc->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* cpa) {
        cpa->flags |= 0x08000000; // CREATE_NO_WINDOW
    });
#endif
    QPointer<AgentService> self = this;
    auto timer = std::make_shared<QElapsedTimer>();
    auto accumulated = std::make_shared<QString>();
    auto final_json = std::make_shared<QString>();
    auto done_emitted = std::make_shared<bool>(false);
    timer->start();

    connect(proc, &QProcess::readyReadStandardOutput, this,
            [self, proc, accumulated, final_json, done_emitted, req_id]() {
                if (!self)
                    return;
                while (proc->canReadLine()) {
                    QString line = QString::fromUtf8(proc->readLine()).trimmed();
                    if (line.isEmpty())
                        continue;

                    auto extract = [](const QString& s, const QString& prefix) -> QString {
                        QString rest = s.mid(prefix.length());
                        if (rest.startsWith(' '))
                            rest = rest.mid(1);
                        rest.replace("\\n", "\n").replace("\\\\", "\\");
                        return rest;
                    };

                    if (line.startsWith('{')) {
                        *final_json = line;
                    } else if (line.startsWith("TOKEN:")) {
                        QString token = extract(line, "TOKEN:");
                        *accumulated += token;
                        emit self->agent_stream_token(req_id, token);
                        self->publish_agent_token(req_id, token);
                    } else if (line.startsWith("THINKING:")) {
                        QString status = extract(line, "THINKING:");
                        emit self->agent_stream_thinking(req_id, status);
                        self->publish_agent_status(req_id, status);
                    } else if (line.startsWith("TOOL:")) {
                        QString status = "Tool: " + extract(line, "TOOL:");
                        emit self->agent_stream_thinking(req_id, status);
                        self->publish_agent_status(req_id, status);
                    } else if (line.startsWith("DONE:")) {
                        QString done_content = extract(line, "DONE:");
                        if (accumulated->trimmed().isEmpty() && !done_content.isEmpty())
                            *accumulated = done_content;
                    } else if (line.startsWith("ERROR:")) {
                        if (!*done_emitted) {
                            *done_emitted = true;
                            AgentExecutionResult r;
                            r.request_id = req_id;
                            r.success = false;
                            r.error = extract(line, "ERROR:");
                            emit self->agent_stream_done(r);
                            self->publish_agent_result(r, /*final=*/true);
                        }
                    }
                }
            });

    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [self, proc, accumulated, final_json, done_emitted, timer, req_id](int exit_code, QProcess::ExitStatus) {
                int elapsed = timer->elapsed();
                QString remaining = QString::fromUtf8(proc->readAllStandardOutput());
                QString stderr_str = QString::fromUtf8(proc->readAllStandardError());
                proc->deleteLater();

                if (!self || *done_emitted)
                    return;
                *done_emitted = true;

                for (const QString& line : remaining.split('\n')) {
                    QString t = line.trimmed();
                    if (t.startsWith('{'))
                        *final_json = t;
                }

                AgentExecutionResult r;
                r.request_id = req_id;
                r.execution_time_ms = elapsed;

                QJsonDocument doc = QJsonDocument::fromJson(final_json->toUtf8());
                if (!doc.isNull() && doc.object()["success"].toBool()) {
                    QString json_response = doc.object()["response"].toString();
                    r.success = true;
                    r.response = json_response.isEmpty() ? accumulated->trimmed() : json_response;
                } else if (!accumulated->trimmed().isEmpty()) {
                    r.success = true;
                    r.response = accumulated->trimmed();
                } else {
                    r.success = false;
                    r.error = exit_code != 0 ? stderr_str.left(500) : "No response from team";
                }

                LOG_INFO("AgentService", QString("Team completed in %1ms").arg(elapsed));
                emit self->agent_stream_done(r);
                self->publish_agent_result(r, /*final=*/true);
            });

    connect(proc, &QProcess::errorOccurred, this, [self, proc, done_emitted, timer, req_id](QProcess::ProcessError) {
        QString err = proc->errorString();
        proc->deleteLater();
        if (!self || *done_emitted)
            return;
        *done_emitted = true;
        AgentExecutionResult r;
        r.request_id = req_id;
        r.success = false;
        r.error = "Process error: " + err;
        emit self->agent_stream_done(r);
        self->publish_agent_result(r, /*final=*/true);
    });

    LOG_INFO("AgentService", QString("Starting team stream (%1 bytes payload, %2 members)")
                                 .arg(payload_bytes.size())
                                 .arg(team_config["members"].toArray().size()));
    proc->start(python_path, {script_path, "--stdin", "--stream"});
    proc->write(payload_bytes);
    proc->closeWriteChannel();
    return req_id;
}

// ── Workflow execution ───────────────────────────────────────────────────────

QString AgentService::run_agent_structured(const QString& query, const QJsonObject& config,
                                           const QString& output_model) {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    LOG_INFO("AgentService",
             QString("Running structured agent [%1] (%2): %3").arg(req_id.left(8), output_model, query.left(60)));
    QJsonObject params;
    params["query"] = query;
    params["output_model"] = output_model;

    QPointer<AgentService> self = this;
    run_python_stdin("run_structured", params, config, [self, req_id](bool ok, QJsonObject result) {
        if (!self)
            return;
        AgentExecutionResult r;
        r.request_id = req_id;
        r.success = ok && result["success"].toBool(ok);
        r.execution_time_ms = result["execution_time_ms"].toInt();
        r.response = result.contains("response") ? result["response"].toString()
                                                 : QJsonDocument(result).toJson(QJsonDocument::Indented);
        r.error = result["error"].toString();
        emit self->agent_result(r);
        self->publish_agent_result(r, /*final=*/true);
    });
    return req_id;
}

// ── Routed query execution ───────────────────────────────────────────────────

void AgentService::execute_routed_query(const QString& query, const QJsonObject& config, const QString& session_id) {
    LOG_INFO("AgentService", QString("Execute routed query: %1").arg(query.left(60)));
    QJsonObject params;
    params["query"] = query;
    if (!session_id.isEmpty())
        params["session_id"] = session_id;

    QPointer<AgentService> self = this;
    run_python_stdin("execute_query", params, config, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        AgentExecutionResult r;
        r.success = ok && result["success"].toBool(ok);
        r.execution_time_ms = result["execution_time_ms"].toInt();
        r.response = result.contains("response") ? result["response"].toString()
                                                 : QJsonDocument(result).toJson(QJsonDocument::Indented);
        r.error = result["error"].toString();
        emit self->agent_result(r);
        self->publish_agent_result(r, /*final=*/true);
    });
}

// ── Multi-query ──────────────────────────────────────────────────────────────

void AgentService::execute_multi_query(const QString& query, bool aggregate, const QJsonObject& config) {
    LOG_INFO("AgentService", QString("Multi-query: %1").arg(query.left(60)));
    QJsonObject params;
    params["query"] = query;
    params["aggregate"] = aggregate;

    QPointer<AgentService> self = this;
    run_python_stdin("execute_multi_query", params, config, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        if (ok)
            emit self->multi_query_result(result);
        else
            emit self->error_occurred("multi_query", result["error"].toString());
    });
}

// ── Financial workflows (specialized) ────────────────────────────────────────

// ── Agentic Mode ─────────────────────────────────────────────────────────────
// Spawns a streaming Python subprocess that runs an AgenticRunner. Events
// arrive as AGENTIC_EVENT: <json> lines on stdout; each is forwarded to the
// `task_event` signal and DataHub topic `task:event:<task_id>`.

QString AgentService::run_agentic_streaming(const QString& action, const QJsonObject& params, const QJsonObject& config,
                                            const QString& known_task_id) {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    LOG_INFO("AgentService",
             QString("Agentic streaming [%1] action=%2 task=%3").arg(req_id.left(8), action, known_task_id.left(8)));

    auto& py = python::PythonRunner::instance();
    if (!py.is_available()) {
        QJsonObject evt{{"kind", "error"}, {"task_id", known_task_id}, {"error", "Python not available"}};
        publish_task_event(known_task_id, evt);
        return req_id;
    }

    QJsonObject payload = build_payload(action, params, config);
    QByteArray payload_bytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);

    QString python_path = py.python_path();
    QString script_path = py.scripts_dir() + "/agents/finagent_core/main.py";

    auto* proc = new QProcess(this);
    proc->setProcessEnvironment(py.build_python_env());
    proc->setWorkingDirectory(py.scripts_dir());
#ifdef _WIN32
    proc->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* cpa) {
        cpa->flags |= 0x08000000; // CREATE_NO_WINDOW
    });
#endif

    QPointer<AgentService> self = this;
    auto resolved_task_id = std::make_shared<QString>(known_task_id);
    auto done_emitted = std::make_shared<bool>(false);

    // Streaming line reader: parse only AGENTIC_EVENT: <json>; ignore other
    // lines (the Python wrapper still emits "thinking ..." / "done" via
    // stream_print which we don't need to surface here).
    connect(proc, &QProcess::readyReadStandardOutput, this, [self, proc, resolved_task_id, done_emitted, req_id]() {
        if (!self)
            return;
        static const QString kPrefix = QStringLiteral("AGENTIC_EVENT:");
        while (proc->canReadLine()) {
            QString line = QString::fromUtf8(proc->readLine()).trimmed();
            if (!line.startsWith(kPrefix))
                continue;
            QString rest = line.mid(kPrefix.length()).trimmed();
            QJsonDocument doc = QJsonDocument::fromJson(rest.toUtf8());
            if (!doc.isObject())
                continue;
            QJsonObject evt = doc.object();
            // Adopt the task_id from the first event that carries one
            // (start_task scenario; resume_task already has it).
            QString tid = evt.value(QStringLiteral("task_id")).toString();
            if (tid.isEmpty())
                tid = *resolved_task_id;
            else
                *resolved_task_id = tid;
            // Tag with the C++ correlator so UI panels can match.
            evt["request_id"] = req_id;
            self->publish_task_event(tid, evt);
            const QString kind = evt.value(QStringLiteral("kind")).toString();
            if (kind == QLatin1String("done") || kind == QLatin1String("error") || kind == QLatin1String("cancelled")) {
                *done_emitted = true;
            }
        }
    });

    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [self, proc, resolved_task_id, done_emitted, req_id](int exit_code, QProcess::ExitStatus) {
                QString stderr_str = QString::fromUtf8(proc->readAllStandardError());
                // Drain any remaining stdout in case the last events arrived
                // in the same chunk as exit.
                QString remaining = QString::fromUtf8(proc->readAllStandardOutput());
                static const QString kPrefix = QStringLiteral("AGENTIC_EVENT:");
                for (const QString& raw : remaining.split('\n')) {
                    QString line = raw.trimmed();
                    if (!line.startsWith(kPrefix))
                        continue;
                    QString rest = line.mid(kPrefix.length()).trimmed();
                    QJsonDocument doc = QJsonDocument::fromJson(rest.toUtf8());
                    if (!doc.isObject())
                        continue;
                    QJsonObject evt = doc.object();
                    QString tid = evt.value(QStringLiteral("task_id")).toString();
                    if (tid.isEmpty())
                        tid = *resolved_task_id;
                    else
                        *resolved_task_id = tid;
                    evt["request_id"] = req_id;
                    if (self)
                        self->publish_task_event(tid, evt);
                    const QString kind = evt.value(QStringLiteral("kind")).toString();
                    if (kind == QLatin1String("done") || kind == QLatin1String("error") ||
                        kind == QLatin1String("cancelled")) {
                        *done_emitted = true;
                    }
                }
                proc->deleteLater();
                if (!self)
                    return;
                // If the subprocess died without emitting a terminal event,
                // synthesise an error event so UI can clean up.
                if (!*done_emitted) {
                    QJsonObject evt{{"kind", "error"},
                                    {"task_id", *resolved_task_id},
                                    {"request_id", req_id},
                                    {"error", exit_code != 0
                                                  ? stderr_str.left(500)
                                                  : QStringLiteral("Subprocess exited without terminal event")}};
                    self->publish_task_event(*resolved_task_id, evt);
                }
            });

    connect(proc, &QProcess::errorOccurred, this,
            [self, proc, resolved_task_id, done_emitted, req_id](QProcess::ProcessError) {
                QString err = proc->errorString();
                proc->deleteLater();
                if (!self || *done_emitted)
                    return;
                *done_emitted = true;
                QJsonObject evt{{"kind", "error"},
                                {"task_id", *resolved_task_id},
                                {"request_id", req_id},
                                {"error", QStringLiteral("Process error: ") + err}};
                self->publish_task_event(*resolved_task_id, evt);
            });

    proc->start(python_path, {script_path, "--stdin", "--stream"});
    proc->write(payload_bytes);
    proc->closeWriteChannel();
    return req_id;
}

QString AgentService::start_task(const QString& query, const QJsonObject& config) {
    QJsonObject params;
    params["query"] = query;
    return run_agentic_streaming(QStringLiteral("agentic_start_task"), params, config);
}

QString AgentService::resume_task(const QString& task_id) {
    QJsonObject params;
    params["task_id"] = task_id;
    return run_agentic_streaming(QStringLiteral("agentic_resume_task"), params, {}, task_id);
}

QString AgentService::reply_to_question(const QString& task_id, const QString& answer) {
    QJsonObject params;
    params["task_id"] = task_id;
    params["answer"] = answer;
    return run_agentic_streaming(QStringLiteral("agentic_reply_question"), params, {}, task_id);
}

// ── Scheduled recurring tasks ────────────────────────────────────────────────

QString AgentService::schedule_create_task(const QString& name, const QString& query, const QString& schedule_expr,
                                           const QJsonObject& config, bool start_now) {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject params;
    params["name"] = name;
    params["query"] = query;
    params["schedule"] = schedule_expr;
    params["schedule_config"] = config;
    params["start_now"] = start_now;
    QPointer<AgentService> self = this;
    run_python_stdin(QStringLiteral("agentic_schedule_create"), params, {}, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        if (ok)
            emit self->schedule_created(result.value("schedule_id").toString());
        else
            emit self->error_occurred("schedule_create", result["error"].toString());
    });
    // Make sure the polling timer is on now that there might be schedules.
    ensure_schedule_timer();
    return req_id;
}

QString AgentService::schedule_list() {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QPointer<AgentService> self = this;
    run_python_stdin(QStringLiteral("agentic_schedule_list"), {}, {}, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        if (ok)
            emit self->schedules_listed(result.value("schedules").toArray());
        else
            emit self->error_occurred("schedule_list", result["error"].toString());
    });
    return req_id;
}

QString AgentService::delete_task(const QString& task_id) {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject params;
    params["task_id"] = task_id;
    // "delete_task" removes the record via the state manager; "agentic_cancel_task"
    // only sets a cooperative stop signal and leaves the row behind.
    run_python_stdin(QStringLiteral("delete_task"), params, {}, [](bool, QJsonObject) {});
    return req_id;
}

QString AgentService::schedule_delete(const QString& schedule_id) {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject params;
    params["schedule_id"] = schedule_id;
    QPointer<AgentService> self = this;
    run_python_stdin(QStringLiteral("agentic_schedule_delete"), params, {}, [self, schedule_id](bool ok, QJsonObject) {
        if (!self || !ok)
            return;
        emit self->schedule_deleted(schedule_id);
    });
    return req_id;
}

QString AgentService::schedule_set_enabled(const QString& schedule_id, bool enabled) {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject params;
    params["schedule_id"] = schedule_id;
    params["enabled"] = enabled;
    run_python_stdin(QStringLiteral("agentic_schedule_set_enabled"), params, {}, [](bool, QJsonObject) {});
    return req_id;
}

void AgentService::tick_schedules() {
    QPointer<AgentService> self = this;
    run_python_stdin(QStringLiteral("agentic_schedule_tick"), {}, {}, [self](bool ok, QJsonObject result) {
        if (!self || !ok)
            return;
        const QJsonArray due = result.value("due").toArray();
        for (const auto& v : due) {
            QJsonObject row = v.toObject();
            const QString sid = row.value("id").toString();
            const QString name = row.value("name").toString();
            const QString query = row.value("query").toString();
            const QJsonObject cfg = row.value("config").toObject();
            // Fire as a normal agentic task — streaming
            // events route through task:event:* like manual
            // starts. The schedule's id/name is forwarded so
            // UI can correlate the task with its trigger.
            const QString task_req = self->start_task(query, cfg);
            emit self->scheduled_task_fired(sid, name, task_req);
        }
    });
}

// ── Library inspection (Phase 3 management UI) ───────────────────────────────

QString AgentService::skills_list() {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QPointer<AgentService> self = this;
    run_python_stdin(QStringLiteral("agentic_skills_list"), {}, {}, [self](bool ok, QJsonObject result) {
        if (!self || !ok)
            return;
        emit self->skills_listed(result.value("skills").toArray());
    });
    return req_id;
}

QString AgentService::skill_delete(const QString& skill_id) {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject p;
    p["skill_id"] = skill_id;
    run_python_stdin(QStringLiteral("agentic_skill_delete"), p, {}, [](bool, QJsonObject) {});
    return req_id;
}

QString AgentService::archival_list(const QString& user_id, const QString& type) {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject p;
    if (!user_id.isEmpty())
        p["user_id"] = user_id;
    if (!type.isEmpty())
        p["type"] = type;
    QPointer<AgentService> self = this;
    run_python_stdin(QStringLiteral("agentic_memory_list"), p, {}, [self](bool ok, QJsonObject result) {
        if (!self || !ok)
            return;
        emit self->archival_listed(result.value("memories").toArray());
    });
    return req_id;
}

QString AgentService::archival_delete(const QString& memory_id) {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject p;
    p["memory_id"] = memory_id;
    run_python_stdin(QStringLiteral("agentic_memory_delete"), p, {}, [](bool, QJsonObject) {});
    return req_id;
}

QString AgentService::reflexion_list() {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QPointer<AgentService> self = this;
    run_python_stdin(QStringLiteral("agentic_reflexion_list"), {}, {}, [self](bool ok, QJsonObject result) {
        if (!self || !ok)
            return;
        emit self->reflexion_listed(result.value("reflections").toArray());
    });
    return req_id;
}

void AgentService::ensure_schedule_timer() {
    if (schedule_timer_)
        return;
    auto* t = new QTimer(this);
    t->setInterval(30 * 1000); // 30s — finer than this is overkill at the minute granularity DSL
    QPointer<AgentService> self = this;
    QObject::connect(t, &QTimer::timeout, this, [self]() {
        if (self)
            self->tick_schedules();
    });
    t->start();
    schedule_timer_ = t;
    // Fire one immediate tick so a freshly-created `start_now=true` schedule
    // doesn't wait the full 30s.
    QMetaObject::invokeMethod(
        this,
        [self]() {
            if (self)
                self->tick_schedules();
        },
        Qt::QueuedConnection);
}

QString AgentService::pause_task(const QString& task_id) {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject params;
    params["task_id"] = task_id;
    QPointer<AgentService> self = this;
    run_python_stdin(QStringLiteral("agentic_pause_task"), params, {},
                     [self, task_id, req_id](bool ok, QJsonObject result) {
                         if (!self || !ok)
                             return;
                         QJsonObject evt{{"kind", "control_ack"},
                                         {"task_id", task_id},
                                         {"request_id", req_id},
                                         {"signal", result.value("signal").toString("pause_requested")}};
                         emit self->task_event(task_id, evt);
                     });
    return req_id;
}

QString AgentService::cancel_task(const QString& task_id) {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject params;
    params["task_id"] = task_id;
    QPointer<AgentService> self = this;
    run_python_stdin(QStringLiteral("agentic_cancel_task"), params, {},
                     [self, task_id, req_id](bool ok, QJsonObject result) {
                         if (!self || !ok)
                             return;
                         QJsonObject evt{{"kind", "control_ack"},
                                         {"task_id", task_id},
                                         {"request_id", req_id},
                                         {"signal", result.value("signal").toString("cancel_requested")}};
                         emit self->task_event(task_id, evt);
                     });
    return req_id;
}

QString AgentService::list_tasks(const QString& status_filter, int limit) {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject params;
    if (!status_filter.isEmpty())
        params["status"] = status_filter;
    params["limit"] = limit;
    QPointer<AgentService> self = this;
    run_python_stdin(QStringLiteral("list_tasks"), params, {}, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        if (ok)
            emit self->tasks_listed(result.value("tasks").toArray());
        else
            emit self->error_occurred("list_tasks", result["error"].toString());
    });
    return req_id;
}

QString AgentService::get_task(const QString& task_id) {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject params;
    params["task_id"] = task_id;
    QPointer<AgentService> self = this;
    run_python_stdin(QStringLiteral("get_task"), params, {}, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        if (ok)
            emit self->task_loaded(result.value("task").toObject());
        else
            emit self->error_occurred("get_task", result["error"].toString());
    });
    return req_id;
}

} // namespace fincept::services
