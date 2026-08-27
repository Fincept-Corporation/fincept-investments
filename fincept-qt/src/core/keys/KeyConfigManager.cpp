// src/core/keys/KeyConfigManager.cpp
#include "core/keys/KeyConfigManager.h"

#include "core/logging/Logger.h"
#include "storage/repositories/SettingsRepository.h"

#include <optional>

namespace fincept {

// Storage key format: "key.refresh", "key.focus_mode", etc.
static QString storage_key(KeyAction a) {
    switch (a) {
        case KeyAction::Refresh:
            return "key.refresh";
        case KeyAction::FocusMode:
            return "key.focus_mode";
        case KeyAction::Fullscreen:
            return "key.fullscreen";
        case KeyAction::Screenshot:
            return "key.screenshot";
        case KeyAction::NavNext:
            return "key.nav_next";
        case KeyAction::NavPrev:
            return "key.nav_prev";
        case KeyAction::NavAccept:
            return "key.nav_accept";
        case KeyAction::NavEscape:
            return "key.nav_escape";
        case KeyAction::NewsNext:
            return "key.news_next";
        case KeyAction::NewsPrev:
            return "key.news_prev";
        case KeyAction::NewsOpen:
            return "key.news_open";
        case KeyAction::NewsClose:
            return "key.news_close";
        case KeyAction::RunCell:
            return "key.run_cell";
        case KeyAction::RunAndNext:
            return "key.run_and_next";
        case KeyAction::RenameCell:
            return "key.rename_cell";

        case KeyAction::FocusWindow1:
            return "key.focus_window_1";
        case KeyAction::FocusWindow2:
            return "key.focus_window_2";
        case KeyAction::FocusWindow3:
            return "key.focus_window_3";
        case KeyAction::FocusWindow4:
            return "key.focus_window_4";
        case KeyAction::FocusWindow5:
            return "key.focus_window_5";
        case KeyAction::FocusWindow6:
            return "key.focus_window_6";
        case KeyAction::FocusWindow7:
            return "key.focus_window_7";
        case KeyAction::FocusWindow8:
            return "key.focus_window_8";
        case KeyAction::FocusWindow9:
            return "key.focus_window_9";
        case KeyAction::CyclePanelsForward:
            return "key.cycle_panels_forward";
        case KeyAction::CyclePanelsBack:
            return "key.cycle_panels_back";
        case KeyAction::CycleWindowsForward:
            return "key.cycle_windows_forward";
        case KeyAction::CycleWindowsBack:
            return "key.cycle_windows_back";
        case KeyAction::NewWindowNextMonitor:
            return "key.new_window_next_monitor";
        case KeyAction::MoveWindowToMonitor1:
            return "key.move_window_to_monitor_1";
        case KeyAction::MoveWindowToMonitor2:
            return "key.move_window_to_monitor_2";
        case KeyAction::MoveWindowToMonitor3:
            return "key.move_window_to_monitor_3";
        case KeyAction::MoveWindowToMonitor4:
            return "key.move_window_to_monitor_4";
        case KeyAction::MoveWindowToMonitor5:
            return "key.move_window_to_monitor_5";
        case KeyAction::MoveWindowToMonitor6:
            return "key.move_window_to_monitor_6";
        case KeyAction::MoveWindowToMonitor7:
            return "key.move_window_to_monitor_7";
        case KeyAction::MoveWindowToMonitor8:
            return "key.move_window_to_monitor_8";
        case KeyAction::MoveWindowToMonitor9:
            return "key.move_window_to_monitor_9";

        case KeyAction::BrowseComponents:
            return "key.browse_components";
        case KeyAction::ToggleAlwaysOnTop:
            return "key.toggle_always_on_top";
    }
    return {};
}

KeyConfigManager::KeyConfigManager() {
    // Define defaults
    defaults_[KeyAction::Refresh] = QKeySequence(Qt::Key_F5);
    defaults_[KeyAction::FocusMode] = QKeySequence(Qt::Key_F10);
    defaults_[KeyAction::Fullscreen] = QKeySequence(Qt::Key_F11);
    defaults_[KeyAction::Screenshot] = QKeySequence(Qt::CTRL | Qt::Key_P);
    defaults_[KeyAction::NavNext] = QKeySequence(Qt::Key_Down);
    defaults_[KeyAction::NavPrev] = QKeySequence(Qt::Key_Up);
    defaults_[KeyAction::NavAccept] = QKeySequence(Qt::Key_Tab);
    defaults_[KeyAction::NavEscape] = QKeySequence(Qt::Key_Escape);
    defaults_[KeyAction::NewsNext] = QKeySequence("J");
    defaults_[KeyAction::NewsPrev] = QKeySequence("K");
    defaults_[KeyAction::NewsOpen] = QKeySequence(Qt::Key_Return);
    defaults_[KeyAction::NewsClose] = QKeySequence(Qt::Key_Escape);
    defaults_[KeyAction::RunCell] = QKeySequence(Qt::CTRL | Qt::Key_Return);
    defaults_[KeyAction::RunAndNext] = QKeySequence(Qt::SHIFT | Qt::Key_Return);
    defaults_[KeyAction::RenameCell] = QKeySequence(Qt::Key_F2);

    // Phase 3 — window/panel navigation
    defaults_[KeyAction::FocusWindow1] = QKeySequence(Qt::CTRL | Qt::Key_1);
    defaults_[KeyAction::FocusWindow2] = QKeySequence(Qt::CTRL | Qt::Key_2);
    defaults_[KeyAction::FocusWindow3] = QKeySequence(Qt::CTRL | Qt::Key_3);
    defaults_[KeyAction::FocusWindow4] = QKeySequence(Qt::CTRL | Qt::Key_4);
    defaults_[KeyAction::FocusWindow5] = QKeySequence(Qt::CTRL | Qt::Key_5);
    defaults_[KeyAction::FocusWindow6] = QKeySequence(Qt::CTRL | Qt::Key_6);
    defaults_[KeyAction::FocusWindow7] = QKeySequence(Qt::CTRL | Qt::Key_7);
    defaults_[KeyAction::FocusWindow8] = QKeySequence(Qt::CTRL | Qt::Key_8);
    defaults_[KeyAction::FocusWindow9] = QKeySequence(Qt::CTRL | Qt::Key_9);
    defaults_[KeyAction::CyclePanelsForward] = QKeySequence(Qt::CTRL | Qt::Key_Tab);
    defaults_[KeyAction::CyclePanelsBack] = QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab);
    // Ctrl+Alt+Tab is a window-manager shortcut on several platforms; wrap it
    // with the same gesture plus a non-default modifier so it still feels
    // related but doesn't clash. Users can rebind in Settings.
    defaults_[KeyAction::CycleWindowsForward] = QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Tab);
    defaults_[KeyAction::CycleWindowsBack] = QKeySequence(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_Tab);
    defaults_[KeyAction::NewWindowNextMonitor] = QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N);
    defaults_[KeyAction::MoveWindowToMonitor1] = QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_1);
    defaults_[KeyAction::MoveWindowToMonitor2] = QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_2);
    defaults_[KeyAction::MoveWindowToMonitor3] = QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_3);
    defaults_[KeyAction::MoveWindowToMonitor4] = QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_4);
    defaults_[KeyAction::MoveWindowToMonitor5] = QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_5);
    defaults_[KeyAction::MoveWindowToMonitor6] = QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_6);
    defaults_[KeyAction::MoveWindowToMonitor7] = QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_7);
    defaults_[KeyAction::MoveWindowToMonitor8] = QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_8);
    defaults_[KeyAction::MoveWindowToMonitor9] = QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_9);

    // Phase 6 — Component Browser
    defaults_[KeyAction::BrowseComponents] = QKeySequence(Qt::CTRL | Qt::Key_K);

    // Phase 11 — Always-on-top window toggle. Ctrl+Shift+T is un-used by any
    // existing binding in this project and is a common convention for "top".
    defaults_[KeyAction::ToggleAlwaysOnTop] = QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T);

    // Create QActions with default sequences
    for (auto it = defaults_.begin(); it != defaults_.end(); ++it) {
        auto* act = new QAction(this);
        act->setShortcut(it.value());
        act->setShortcutContext(Qt::ApplicationShortcut);
        actions_[it.key()] = act;
    }

    load_from_storage();
}

KeyConfigManager& KeyConfigManager::instance() {
    static KeyConfigManager s;
    return s;
}

void KeyConfigManager::load_from_storage() {
    auto result = SettingsRepository::instance().get_by_category("keybindings");
    if (!result.is_ok())
        return;
    for (const auto& setting : result.value()) {
        for (auto it = defaults_.begin(); it != defaults_.end(); ++it) {
            if (storage_key(it.key()) == setting.key) {
                QKeySequence seq(setting.value);
                if (!seq.isEmpty()) {
                    actions_[it.key()]->setShortcut(seq);
                    LOG_DEBUG("KeyConfig", QString("Loaded %1 = %2").arg(setting.key, setting.value));
                }
                break;
            }
        }
    }
}

void KeyConfigManager::persist(KeyAction a, const QKeySequence& seq) {
    SettingsRepository::instance().set(storage_key(a), seq.toString(), "keybindings");
}

QAction* KeyConfigManager::action(KeyAction a) const {
    return actions_.value(a, nullptr);
}

QKeySequence KeyConfigManager::key(KeyAction a) const {
    if (auto* act = actions_.value(a, nullptr))
        return act->shortcut();
    return {};
}

QKeySequence KeyConfigManager::default_key(KeyAction a) const {
    return defaults_.value(a);
}

QString KeyConfigManager::display_name(KeyAction a) const {
    switch (a) {
        case KeyAction::Refresh:
            return "Refresh Screen";
        case KeyAction::FocusMode:
            return "Focus Mode";
        case KeyAction::Fullscreen:
            return "Fullscreen";
        case KeyAction::Screenshot:
            return "Screenshot";
        case KeyAction::NavNext:
            return "Next Suggestion";
        case KeyAction::NavPrev:
            return "Previous Suggestion";
        case KeyAction::NavAccept:
            return "Autocomplete";
        case KeyAction::NavEscape:
            return "Dismiss";
        case KeyAction::NewsNext:
            return "Next Article";
        case KeyAction::NewsPrev:
            return "Previous Article";
        case KeyAction::NewsOpen:
            return "Open Article";
        case KeyAction::NewsClose:
            return "Close Panel";
        case KeyAction::RunCell:
            return "Run Cell";
        case KeyAction::RunAndNext:
            return "Run & Move Next";
        case KeyAction::RenameCell:
            return "Rename Cell";

        case KeyAction::FocusWindow1:
            return "Focus Window 1";
        case KeyAction::FocusWindow2:
            return "Focus Window 2";
        case KeyAction::FocusWindow3:
            return "Focus Window 3";
        case KeyAction::FocusWindow4:
            return "Focus Window 4";
        case KeyAction::FocusWindow5:
            return "Focus Window 5";
        case KeyAction::FocusWindow6:
            return "Focus Window 6";
        case KeyAction::FocusWindow7:
            return "Focus Window 7";
        case KeyAction::FocusWindow8:
            return "Focus Window 8";
        case KeyAction::FocusWindow9:
            return "Focus Window 9";
        case KeyAction::CyclePanelsForward:
            return "Next Panel";
        case KeyAction::CyclePanelsBack:
            return "Previous Panel";
        case KeyAction::CycleWindowsForward:
            return "Next Window";
        case KeyAction::CycleWindowsBack:
            return "Previous Window";
        case KeyAction::NewWindowNextMonitor:
            return "New Window (Next Monitor)";
        case KeyAction::MoveWindowToMonitor1:
            return "Move Window to Monitor 1";
        case KeyAction::MoveWindowToMonitor2:
            return "Move Window to Monitor 2";
        case KeyAction::MoveWindowToMonitor3:
            return "Move Window to Monitor 3";
        case KeyAction::MoveWindowToMonitor4:
            return "Move Window to Monitor 4";
        case KeyAction::MoveWindowToMonitor5:
            return "Move Window to Monitor 5";
        case KeyAction::MoveWindowToMonitor6:
            return "Move Window to Monitor 6";
        case KeyAction::MoveWindowToMonitor7:
            return "Move Window to Monitor 7";
        case KeyAction::MoveWindowToMonitor8:
            return "Move Window to Monitor 8";
        case KeyAction::MoveWindowToMonitor9:
            return "Move Window to Monitor 9";

        case KeyAction::BrowseComponents:
            return "Browse Components";
        case KeyAction::ToggleAlwaysOnTop:
            return "Toggle Always-on-Top";
    }
    return {};
}

QString KeyConfigManager::group_name(KeyAction a) const {
    switch (a) {
        case KeyAction::Refresh:
        case KeyAction::FocusMode:
        case KeyAction::Fullscreen:
        case KeyAction::Screenshot:
            return "Global";
        case KeyAction::NavNext:
        case KeyAction::NavPrev:
        case KeyAction::NavAccept:
        case KeyAction::NavEscape:
            return "Navigation";
        case KeyAction::NewsNext:
        case KeyAction::NewsPrev:
        case KeyAction::NewsOpen:
        case KeyAction::NewsClose:
            return "News";
        case KeyAction::RunCell:
        case KeyAction::RunAndNext:
        case KeyAction::RenameCell:
            return "Code Editor";

        case KeyAction::FocusWindow1:
        case KeyAction::FocusWindow2:
        case KeyAction::FocusWindow3:
        case KeyAction::FocusWindow4:
        case KeyAction::FocusWindow5:
        case KeyAction::FocusWindow6:
        case KeyAction::FocusWindow7:
        case KeyAction::FocusWindow8:
        case KeyAction::FocusWindow9:
        case KeyAction::CyclePanelsForward:
        case KeyAction::CyclePanelsBack:
        case KeyAction::CycleWindowsForward:
        case KeyAction::CycleWindowsBack:
        case KeyAction::NewWindowNextMonitor:
        case KeyAction::MoveWindowToMonitor1:
        case KeyAction::MoveWindowToMonitor2:
        case KeyAction::MoveWindowToMonitor3:
        case KeyAction::MoveWindowToMonitor4:
        case KeyAction::MoveWindowToMonitor5:
        case KeyAction::MoveWindowToMonitor6:
        case KeyAction::MoveWindowToMonitor7:
        case KeyAction::MoveWindowToMonitor8:
        case KeyAction::MoveWindowToMonitor9:
            return "Windows";

        case KeyAction::BrowseComponents:
        case KeyAction::ToggleAlwaysOnTop:
            return "Global";
    }
    return {};
}

QList<KeyAction> KeyConfigManager::all_actions() const {
    return {
        KeyAction::Refresh,
        KeyAction::FocusMode,
        KeyAction::Fullscreen,
        KeyAction::Screenshot,
        KeyAction::NavNext,
        KeyAction::NavPrev,
        KeyAction::NavAccept,
        KeyAction::NavEscape,
        KeyAction::NewsNext,
        KeyAction::NewsPrev,
        KeyAction::NewsOpen,
        KeyAction::NewsClose,
        KeyAction::RunCell,
        KeyAction::RunAndNext,
        KeyAction::RenameCell,
        KeyAction::FocusWindow1,
        KeyAction::FocusWindow2,
        KeyAction::FocusWindow3,
        KeyAction::FocusWindow4,
        KeyAction::FocusWindow5,
        KeyAction::FocusWindow6,
        KeyAction::FocusWindow7,
        KeyAction::FocusWindow8,
        KeyAction::FocusWindow9,
        KeyAction::CyclePanelsForward,
        KeyAction::CyclePanelsBack,
        KeyAction::CycleWindowsForward,
        KeyAction::CycleWindowsBack,
        KeyAction::NewWindowNextMonitor,
        KeyAction::MoveWindowToMonitor1,
        KeyAction::MoveWindowToMonitor2,
        KeyAction::MoveWindowToMonitor3,
        KeyAction::MoveWindowToMonitor4,
        KeyAction::MoveWindowToMonitor5,
        KeyAction::MoveWindowToMonitor6,
        KeyAction::MoveWindowToMonitor7,
        KeyAction::MoveWindowToMonitor8,
        KeyAction::MoveWindowToMonitor9,
        KeyAction::BrowseComponents,
        KeyAction::ToggleAlwaysOnTop,
    };
}

void KeyConfigManager::set_key(KeyAction a, const QKeySequence& seq) {
    if (auto* act = actions_.value(a, nullptr)) {
        act->setShortcut(seq);
        persist(a, seq);
        emit key_changed(a, seq);
        LOG_INFO("KeyConfig", QString("Rebound %1 to %2").arg(display_name(a), seq.toString()));
    }
}

void KeyConfigManager::reset_key(KeyAction a) {
    const QKeySequence def = defaults_.value(a);
    if (auto* act = actions_.value(a, nullptr)) {
        act->setShortcut(def);
        SettingsRepository::instance().remove(storage_key(a));
        emit key_changed(a, def);
    }
}

void KeyConfigManager::reset_all() {
    SettingsRepository::instance().clear_category("keybindings");
    for (auto it = defaults_.begin(); it != defaults_.end(); ++it) {
        if (auto* act = actions_.value(it.key(), nullptr)) {
            act->setShortcut(it.value());
            emit key_changed(it.key(), it.value());
        }
    }
    LOG_INFO("KeyConfig", "All keybindings reset to defaults");
}

std::optional<KeyAction> KeyConfigManager::find_conflict(const QKeySequence& seq, KeyAction exclude) const {
    for (auto it = actions_.begin(); it != actions_.end(); ++it) {
        if (it.key() == exclude)
            continue;
        if (it.value()->shortcut() == seq)
            return it.key();
    }
    return std::nullopt;
}

} // namespace fincept
