#pragma once
#include "ui/navigation/CommandBar.h"

#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

namespace fincept::ui {

class PushpinBar;

/// Combined toolbar: File/Navigate/View/Help menus + branding + clock.
/// Replaces both ToolBar and NavigationBar in a single row.
///
/// Internationalised: all menus and labels flow through tr(). Menus are
/// rebuilt on QEvent::LanguageChange so action labels pick up the new
/// translator; static labels and buttons retranslate via setText().
class ToolBar : public QWidget {
    Q_OBJECT
  public:
    explicit ToolBar(QWidget* parent = nullptr);

    PushpinBar* pushpin_bar() const { return pushpin_bar_; }

  signals:
    void navigate_to(const QString& tab_id);
    void dock_command(const QString& action, const QString& primary, const QString& secondary);
    void action_triggered(const QString& action);
    void upgrade_clicked();

  protected:
    void resizeEvent(QResizeEvent* e) override;
    void changeEvent(QEvent* e) override;

  private slots:
    void update_clock();

  private:
    QMenuBar* menu_bar_ = nullptr;
    QLabel* clock_label_ = nullptr;
    QLabel* subtitle_label_ = nullptr;
    QLabel* branding_label_ = nullptr;
    QLabel* live_dot_ = nullptr;
    QLabel* live_label_ = nullptr;
    /// Always-visible CTA for Fincept Terminal Enterprise (the private edition).
    QPushButton* upgrade_btn_ = nullptr;
    QTimer* clock_timer_ = nullptr;
    QVector<QLabel*> separators_;
    QLabel* fincept_label_ = nullptr;

    CommandBar* command_bar_ = nullptr;
    PushpinBar* pushpin_bar_ = nullptr;

    void refresh_theme();
    void retranslateUi();
    /// Tear down and recreate every menu on the menu bar so action labels
    /// pick up the active QTranslator. Called from retranslateUi() so a
    /// LanguageChange event rebuilds menus alongside static labels.
    void rebuild_menus();
    void apply_responsive_layout(int width);

    QMenu* build_file_menu();
    QMenu* build_navigate_menu();
    QMenu* build_view_menu();
    QMenu* build_help_menu();
};

} // namespace fincept::ui
