#pragma once

namespace fincept {

enum class KeyAction {
    // Global
    Refresh,
    FocusMode,
    Fullscreen,
    Screenshot,

    // CommandBar navigation
    NavNext,
    NavPrev,
    NavAccept,
    NavEscape,

    // News
    NewsNext,
    NewsPrev,
    NewsOpen,
    NewsClose,

    // Code editor
    RunCell,
    RunAndNext,
    RenameCell,

    // Focus WindowFrame N (Ctrl+1..9; Ctrl+1 = window_id 0).
    FocusWindow1,
    FocusWindow2,
    FocusWindow3,
    FocusWindow4,
    FocusWindow5,
    FocusWindow6,
    FocusWindow7,
    FocusWindow8,
    FocusWindow9,

    CyclePanelsForward,
    CyclePanelsBack,

    CycleWindowsForward,
    CycleWindowsBack,

    NewWindowNextMonitor,

    // Move WindowFrame to monitor N (Ctrl+Shift+1..9).
    MoveWindowToMonitor1,
    MoveWindowToMonitor2,
    MoveWindowToMonitor3,
    MoveWindowToMonitor4,
    MoveWindowToMonitor5,
    MoveWindowToMonitor6,
    MoveWindowToMonitor7,
    MoveWindowToMonitor8,
    MoveWindowToMonitor9,

    BrowseComponents,
    ToggleAlwaysOnTop,
};

} // namespace fincept
