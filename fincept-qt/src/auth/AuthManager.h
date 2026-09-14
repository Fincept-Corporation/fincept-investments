#pragma once
#include "auth/AuthTypes.h"

#include <QObject>

#include <functional>

namespace fincept::auth {

/// Central auth state machine.
/// Manages login/signup/MFA flows, session persistence, and validation.
class AuthManager : public QObject {
    Q_OBJECT
  public:
    static AuthManager& instance();

    const SessionData& session() const { return session_; }
    bool is_authenticated() const { return session_.authenticated; }
    bool is_loading() const { return is_loading_; }

    /// Resolve the Fincept api_key for LLM/service callers WITHOUT touching the
    /// plaintext SQLite settings table. Prefers the live in-memory session;
    /// falls back to the encrypted SecureStorage copy (which load_session also
    /// restores into the session at startup). Returns empty if no key is known.
    /// This is the single supported resolver — callers must NOT read the legacy
    /// plaintext "fincept_api_key" settings row.
    QString fincept_api_key() const;

    // Auth flows
    void login(const QString& email, const QString& password, bool force_login = false);

    /// Desktop Google sign-in via the website loopback handoff. Opens the
    /// browser, redeems the returned one-time code, then drives the same
    /// post-login flow as login() (emits login_succeeded / login_failed).
    void login_with_google();

    void signup(const QString& username, const QString& email, const QString& password, const QString& phone,
                const QString& country = {}, const QString& country_code = {});
    void verify_otp(const QString& email, const QString& otp);
    void verify_mfa(const QString& email, const QString& otp);
    void forgot_password(const QString& email);
    void reset_password(const QString& email, const QString& otp, const QString& new_password);
    void logout();

    // Session management
    void initialize();
    void refresh_user_data();
    void attempt_session_recovery(std::function<void(bool)> cb);

    /// True if user needs to set up a PIN (authenticated but no PIN configured).
    bool needs_pin_setup() const;

  signals:
    void auth_state_changed();
    void login_succeeded();
    void login_failed(const QString& error);
    void login_mfa_required();
    void login_active_session(const QString& message);
    void signup_succeeded();
    void signup_failed(const QString& error);
    void otp_verified();
    void otp_failed(const QString& error);
    void mfa_verified();
    void mfa_failed(const QString& error);
    void forgot_password_sent();
    void forgot_password_failed(const QString& error);
    void password_reset_succeeded();
    void password_reset_failed(const QString& error);
    void logged_out();
    void session_expired();
    void loading_changed(bool loading);
    void subscription_fetched();

    /// Emitted after successful login when PIN setup is needed.
    void pin_setup_required();

    /// Emitted after login + PIN verify (or setup) — terminal is fully unlocked.
    void terminal_unlocked();

  private:
    AuthManager();

    void set_loading(bool v);
    void save_session();
    void load_session();
    void clear_session();
    // One-shot: move secrets out of the legacy plaintext settings rows into
    // SecureStorage, then purge the cleartext copies (CR-08). Idempotent.
    void migrate_legacy_plaintext_credentials();
    void fetch_user_profile(std::function<void()> on_done = {});
    void fetch_user_subscription(std::function<void()> on_done = {});
    void complete_auth_flow(std::function<void()> on_done);
    /// Inject a redeemed desktop-handoff session (api_key + session_token) and
    /// run the shared post-login flow. Shared by login_with_google().
    void complete_desktop_login(const QString& api_key, const QString& session_token);
    void auto_configure_fincept_llm();
    QString generate_device_id() const;
    QJsonObject unwrap_data(const QJsonObject& raw) const;

    SessionData session_;
    bool is_loading_ = true;
    bool is_logging_out_ = false;
};

} // namespace fincept::auth
