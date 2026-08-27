#pragma once
// SecuritySection.h — AI tool permissions.
//
// This used to also own PIN status, the change-PIN form, auto-lock policy and
// the security audit log. Those went with user login; what remains is the
// `mcp/allow_destructive_tools` grant, which has nothing to do with accounts
// and has no other UI anywhere in the app.

#include <QCheckBox>
#include <QEvent>
#include <QLabel>
#include <QShowEvent>
#include <QWidget>

namespace fincept::screens {

class SecuritySection : public QWidget {
    Q_OBJECT
  public:
    explicit SecuritySection(QWidget* parent = nullptr);

    /// Re-read the destructive-tools grant from its owner.
    void reload();

  protected:
    void showEvent(QShowEvent* e) override;
    void changeEvent(QEvent* event) override;

  private:
    void build_ui();

    /// Re-apply tr() lookups to every widget whose text we keep a handle to.
    /// Called from changeEvent() on QEvent::LanguageChange.
    void retranslateUi();

    QLabel* title_ai_tools_ = nullptr;

    /// Grants destructive MCP tools (`mcp/allow_destructive_tools`). Applies
    /// immediately on toggle — it is a capability grant, not a form field, so
    /// it deliberately does not wait for a Save button.
    QCheckBox* sec_allow_destructive_ = nullptr;

    QLabel* row_destructive_lbl_ = nullptr;
    QLabel* row_destructive_desc_ = nullptr;
};

} // namespace fincept::screens
