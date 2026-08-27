// src/mcp/tools/WorkspaceTools_internal.h
//
// Private header — included only by WorkspaceTools*.cpp TUs. Provides the
// JSON converters, screen/frame lookups, group/action helpers, and the
// register_* section entry points used by the dispatcher in WorkspaceTools.cpp.
// All helpers are `inline`/template so the header is safe to include from
// multiple TUs.

#pragma once

#include "app/DockScreenRouter.h"
#include "app/WindowFrame.h"
#include "core/actions/ActionDef.h"
#include "core/layout/LayoutCatalog.h"
#include "core/panel/PanelHandle.h"
#include "core/symbol/SymbolContext.h"
#include "core/symbol/SymbolGroup.h"
#include "core/symbol/SymbolGroupRegistry.h"
#include "core/symbol/SymbolRef.h"
#include "core/window/WindowRegistry.h"
#include "mcp/AsyncDispatch.h"
#include "mcp/McpTypes.h"
#include "storage/workspace/WorkspaceSnapshotRing.h"

#include <QApplication>
#include <QChar>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QPromise>
#include <QRect>
#include <QScreen>
#include <QString>

#include <memory>
#include <utility>
#include <vector>

namespace fincept::mcp::tools {

namespace workspace_internal {

inline constexpr const char* TAG = "WorkspaceTools";

inline constexpr int kDefaultTimeoutMs = 15000;

inline QJsonObject rect_to_json(const QRect& r) {
    return QJsonObject{{"x", r.x()}, {"y", r.y()}, {"width", r.width()}, {"height", r.height()}};
}

inline QJsonObject screen_to_json(QScreen* s, int index) {
    if (!s)
        return {};
    return QJsonObject{
        {"index", index},
        {"name", s->name()},
        {"manufacturer", s->manufacturer()},
        {"model", s->model()},
        {"serial_number", s->serialNumber()},
        {"primary", s == QGuiApplication::primaryScreen()},
        {"device_pixel_ratio", s->devicePixelRatio()},
        {"refresh_rate", s->refreshRate()},
        {"geometry", rect_to_json(s->geometry())},
        {"available_geometry", rect_to_json(s->availableGeometry())},
        {"logical_dpi", s->logicalDotsPerInch()},
        {"physical_dpi", s->physicalDotsPerInch()},
    };
}

inline int screen_index_for(QScreen* s) {
    const auto screens = QGuiApplication::screens();
    return screens.indexOf(s);
}

inline QScreen* screen_by_index(int index) {
    const auto screens = QGuiApplication::screens();
    if (index < 0 || index >= screens.size())
        return nullptr;
    return screens.at(index);
}

inline QJsonObject window_to_json(WindowFrame* w) {
    if (!w)
        return {};
    return QJsonObject{
        {"window_id", w->window_id()},
        {"frame_uuid", w->frame_uuid().to_string()},
        {"title", w->windowTitle()},
        {"monitor_index", screen_index_for(w->screen())},
        {"geometry", rect_to_json(w->geometry())},
        {"is_minimized", w->isMinimized()},
        {"is_maximized", w->isMaximized()},
        {"is_fullscreen", w->isFullScreen()},
        {"is_visible", w->isVisible()},
        {"is_active_for_work", w->is_active_for_work()},
        {"is_always_on_top", w->is_always_on_top()},
        {"current_screen_id", w->dock_router() ? w->dock_router()->current_screen_id() : QString()},
    };
}

inline WindowFrame* frame_by_id(int window_id) {
    for (auto* w : WindowRegistry::instance().frames()) {
        if (w && w->window_id() == window_id)
            return w;
    }
    return nullptr;
}

inline WindowFrame* frame_by_uuid(const QString& uuid_str) {
    for (auto* w : WindowRegistry::instance().frames()) {
        if (w && w->frame_uuid().to_string() == uuid_str)
            return w;
    }
    return nullptr;
}

inline WindowFrame* resolve_window(const QJsonObject& args) {
    // Prefer explicit window_id; fall back to uuid; fall back to focused.
    if (args.contains("window_id") && !args["window_id"].isNull()) {
        return frame_by_id(args["window_id"].toInt());
    }
    if (args.contains("frame_uuid") && !args["frame_uuid"].toString().isEmpty()) {
        return frame_by_uuid(args["frame_uuid"].toString());
    }
    auto* aw = qApp->activeWindow();
    if (auto* wf = qobject_cast<WindowFrame*>(aw))
        return wf;
    auto frames = WindowRegistry::instance().frames();
    return frames.isEmpty() ? nullptr : frames.first();
}

inline QJsonObject panel_to_json(PanelHandle* p) {
    if (!p)
        return {};
    return QJsonObject{
        {"instance_id", p->instance_id().to_string()},
        {"type_id", p->type_id()},
        {"title", p->title()},
        {"frame_id", p->frame_id().to_string()},
        {"link_group", p->link_group()},
        {"state", static_cast<int>(p->state())},
        {"has_live_widget", p->dock_widget() != nullptr},
    };
}

inline const char* state_to_str(PanelHandle::State s) {
    switch (s) {
        case PanelHandle::State::Spawning:
            return "spawning";
        case PanelHandle::State::Materialising:
            return "materialising";
        case PanelHandle::State::Active:
            return "active";
        case PanelHandle::State::Suspended:
            return "suspended";
        case PanelHandle::State::Closing:
            return "closing";
        case PanelHandle::State::Closed:
            return "closed";
    }
    return "unknown";
}

inline QJsonObject panel_to_json_v2(PanelHandle* p) {
    QJsonObject j = panel_to_json(p);
    j["state"] = state_to_str(p->state());
    return j;
}

inline QJsonObject layout_entry_to_json(const LayoutCatalog::Entry& e) {
    return QJsonObject{
        {"id", e.id.to_string()},
        {"name", e.name},
        {"kind", e.kind},
        {"created_at_unix", e.created_at_unix},
        {"updated_at_unix", e.updated_at_unix},
        {"description", e.description},
        {"thumbnail_path", e.thumbnail_path},
    };
}

inline QJsonObject snapshot_entry_to_json(const WorkspaceSnapshotRing::Entry& e) {
    return QJsonObject{
        {"snapshot_id", e.snapshot_id},
        {"created_at_ms", e.created_at_ms},
        {"kind", e.kind},
        {"name", e.name},
    };
}

inline QChar parse_group_letter(const QJsonObject& args) {
    const QString s = args["group"].toString().toUpper();
    return s.isEmpty() ? QChar() : QChar(s[0]);
}

inline SymbolGroup parse_group(const QJsonObject& args) {
    const QChar c = parse_group_letter(args);
    return c.isNull() ? SymbolGroup::None : symbol_group_from_char(c);
}

inline QJsonObject group_to_json(SymbolGroup g) {
    auto& reg = SymbolGroupRegistry::instance();
    auto& ctx = SymbolContext::instance();
    const SymbolRef ref = ctx.group_symbol(g);
    return QJsonObject{
        {"group", QString(symbol_group_letter(g))},
        {"name", reg.name(g)},
        {"color", reg.color(g).name(QColor::HexArgb)},
        {"enabled", reg.enabled(g)},
        {"current_symbol", ref.symbol},
        {"current_exchange", ref.exchange},
    };
}

inline QJsonObject action_to_json(const ActionDef& a) {
    QJsonArray aliases;
    for (const auto& al : a.aliases)
        aliases.append(al);
    QJsonArray param_slots;
    for (const auto& s : a.parameter_slots) {
        param_slots.append(QJsonObject{
            {"name", s.name},
            {"display", s.display},
            {"type", s.type},
            {"required", s.required},
            {"suggestion_source", s.suggestion_source},
        });
    }
    return QJsonObject{
        {"id", a.id},
        {"display", a.display},
        {"category", a.category},
        {"aliases", aliases},
        {"default_hotkey", a.default_hotkey.toString()},
        {"parameter_slots", param_slots},
    };
}

// Marshal a synchronous body onto the UI thread, resolving the promise
// with its return value. Body must be `(auto resolve) -> void` and call
// resolve(ToolResult) exactly once.
template <typename BodyFn>
inline void run_on_ui(ToolContext ctx, std::shared_ptr<QPromise<ToolResult>> promise, BodyFn&& body) {
    AsyncDispatch::callback_to_promise(qApp, std::move(ctx), promise, std::forward<BodyFn>(body));
}

// Per-section registration entry points. Each appends tool definitions to
// the passed-in vector. Implementations live in the matching .cpp files.
void register_monitor_window_tools(std::vector<ToolDef>& tools);
void register_panel_tools(std::vector<ToolDef>& tools);
void register_layout_snapshot_tools(std::vector<ToolDef>& tools);
void register_symbol_action_tools(std::vector<ToolDef>& tools);

} // namespace workspace_internal

} // namespace fincept::mcp::tools
