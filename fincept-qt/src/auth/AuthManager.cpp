#include "auth/AuthManager.h"

#include "auth/AuthApi.h"
#include "auth/GoogleDesktopLogin.h"
#include "auth/PinManager.h"
#include "auth/UserApi.h"
#include "core/logging/Logger.h"
#include "network/http/HttpClient.h"
#include "storage/repositories/LlmConfigRepository.h"
#include "storage/repositories/SettingsRepository.h"
#include "storage/secure/SecureStorage.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSysInfo>

namespace fincept::auth {

AuthManager& AuthManager::instance() {
    static AuthManager s;
    return s;
}

AuthManager::AuthManager() {}

void AuthManager::set_loading(bool v) {
    if (is_loading_ != v) {
        is_loading_ = v;
        emit loading_changed(v);
    }
}

QJsonObject AuthManager::unwrap_data(const QJsonObject& raw) const {
    if (raw.contains("data") && raw["data"].isObject()) {
        const auto inner = raw["data"].toObject();
        if (inner.contains("data") && inner["data"].isObject()) {
            return inner["data"].toObject();
        }
        return inner;
    }
    return raw;
}

QString AuthManager::generate_device_id() const {
    QString info =
        QSysInfo::machineHostName() + "|" + QSysInfo::productType() + "|" + QSysInfo::currentCpuArchitecture();
    QByteArray hash = QCryptographicHash::hash(info.toUtf8(), QCryptographicHash::Sha256).toHex();
    QString timestamp = QString::number(QDateTime::currentMSecsSinceEpoch(), 36);
    quint32 random = QRandomGenerator::global()->generate();
    return QString("fincept_desktop_%1_%2_%3")
        .arg(QString::fromUtf8(hash.left(16)), timestamp, QString::number(random, 36));
}

// ── Token helper — single place to set/clear tokens on HttpClient ────────────

static void apply_tokens(const QString& api_key, const QString& session_token) {
    auto& http = fincept::HttpClient::instance();
    http.set_auth_header(api_key);
    http.set_session_token(session_token);
}

static void clear_tokens() {
    apply_tokens({}, {});
}

// ── Session persistence (SQLite via SettingsRepository) ──────────────────────

void AuthManager::save_session() {
    // CR-08: persist ONLY non-secret session fields to the unencrypted settings
    // table. The api_key and session_token are stored exclusively in
    // SecureStorage (AES-256-GCM) so a stolen fincept.db can't yield credentials.
    QJsonDocument doc(session_.to_persisted_json());
    QString json = QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
    auto r = fincept::SettingsRepository::instance().set("fincept_session", json, "auth");
    if (r.is_err()) {
        LOG_ERROR("Auth", "Failed to save session: " + QString::fromStdString(r.error()));
    }

    // Secrets → OS-native encrypted storage (DPAPI / Keychain) only.
    if (!session_.api_key.isEmpty()) {
        auto sr = fincept::SecureStorage::instance().store("api_key", session_.api_key);
        if (sr.is_err())
            LOG_ERROR("Auth", "SecureStorage: failed to persist api_key — credential not saved");
    } else {
        fincept::SecureStorage::instance().remove("api_key");
    }

    if (!session_.session_token.isEmpty()) {
        auto st = fincept::SecureStorage::instance().store("session_token", session_.session_token);
        if (st.is_err())
            LOG_ERROR("Auth", "SecureStorage: failed to persist session_token");
    } else {
        fincept::SecureStorage::instance().remove("session_token");
    }
}

void AuthManager::load_session() {
    auto r = fincept::SettingsRepository::instance().get("fincept_session");
    if (r.is_ok() && !r.value().isEmpty()) {
        auto doc = QJsonDocument::fromJson(r.value().toUtf8());
        if (!doc.isNull())
            session_ = SessionData::from_json(doc.object());
    }

    // CR-08 one-shot migration: existing installs have api_key + session_token
    // written in clear inside the legacy "fincept_session" blob (and a separate
    // plaintext "fincept_api_key" row). If the loaded session still carries
    // secrets, move them into SecureStorage and purge the plaintext copies.
    migrate_legacy_plaintext_credentials();

    // Recover secrets from SecureStorage (DPAPI / Keychain) — the only durable,
    // encrypted source. Survives SQLite corruption and DB migrations.
    auto secure_key = fincept::SecureStorage::instance().retrieve("api_key");
    if (secure_key.is_ok() && !secure_key.value().isEmpty()) {
        if (session_.api_key.isEmpty() || session_.api_key != secure_key.value()) {
            LOG_INFO("Auth", "Restored api_key from SecureStorage");
            session_.api_key = secure_key.value();
        }
    }

    auto secure_token = fincept::SecureStorage::instance().retrieve("session_token");
    if (secure_token.is_ok() && !secure_token.value().isEmpty()) {
        session_.session_token = secure_token.value();
    }

    // Never trust saved authenticated flag — must be re-validated
    session_.authenticated = false;
}

void AuthManager::migrate_legacy_plaintext_credentials() {
    auto& settings = fincept::SettingsRepository::instance();
    auto& secure = fincept::SecureStorage::instance();

    // Two independent sources, tracked separately: each one only authorises the
    // purge of the row it actually read. Collapsing them into a single
    // `migrated` flag let a secret found in the session blob authorise deleting
    // the *plaintext key row we never managed to read* — one transient DB error
    // then destroyed the user's only copy of the API key, irrecoverably.
    bool blob_secrets_migrated = false;  // source 1: "fincept_session"
    bool legacy_row_migrated = false;    // source 2: "fincept_api_key"

    // 1. Secrets that came in via the legacy plaintext "fincept_session" blob.
    if (!session_.api_key.isEmpty()) {
        secure.store("api_key", session_.api_key);
        blob_secrets_migrated = true;
    }
    if (!session_.session_token.isEmpty()) {
        secure.store("session_token", session_.session_token);
        blob_secrets_migrated = true;
    }

    // 2. The standalone plaintext "fincept_api_key" row written by older builds.
    //    A read error is NOT "the row is absent" — the row may still hold the
    //    only copy of the key, so the removal below stays gated on a successful
    //    read.
    auto legacy_key = settings.get("fincept_api_key");
    if (legacy_key.is_err()) {
        LOG_ERROR("Auth",
                  QString("settings read failed for 'fincept_api_key' — leaving the legacy plaintext row untouched "
                          "(it may hold the only copy of the key): %1")
                      .arg(QString::fromStdString(legacy_key.error())));
    } else if (!legacy_key.value().isEmpty()) {
        if (session_.api_key.isEmpty())
            session_.api_key = legacy_key.value();
        secure.store("api_key", legacy_key.value());
        legacy_row_migrated = true;
    }

    // Rewrite fincept_session without secrets only when the blob actually
    // carried some — a source-2-only migration has nothing to scrub there, and
    // writing a session we may have failed to load would drop its non-secret
    // fields too.
    if (blob_secrets_migrated) {
        QJsonDocument doc(session_.to_persisted_json());
        settings.set("fincept_session", QString::fromUtf8(doc.toJson(QJsonDocument::Compact)), "auth");
    }
    if (legacy_row_migrated)
        settings.remove("fincept_api_key");

    if (blob_secrets_migrated || legacy_row_migrated)
        LOG_INFO("Auth", "Migrated legacy plaintext credentials into SecureStorage and purged settings rows");
}

QString AuthManager::fincept_api_key() const {
    // Single supported resolver. Prefer the live in-memory session; fall back to
    // the encrypted SecureStorage copy. Never reads the legacy plaintext row.
    if (!session_.api_key.isEmpty())
        return session_.api_key;
    auto secure_key = fincept::SecureStorage::instance().retrieve("api_key");
    if (secure_key.is_ok())
        return secure_key.value();
    return {};
}

void AuthManager::clear_session() {
    session_ = SessionData{};
    clear_tokens();
    fincept::SettingsRepository::instance().remove("fincept_session");
    fincept::SettingsRepository::instance().remove("fincept_api_key");
    fincept::SecureStorage::instance().remove("api_key");
    fincept::SecureStorage::instance().remove("session_token");

    // PIN intentionally NOT cleared here. The PIN is a local-device credential,
    // independent of the backend session. Wiping it on every logout means a
    // transient session expiry (SessionGuard 401 path, explicit Logout) trains
    // the user to keep choosing fresh PINs and destroys the audit trail across
    // sessions. The only path that should reset the PIN is the max-attempts
    // re-auth flow (LockScreen → reauth_requested), and that path should call
    // PinManager::clear_pin() explicitly before invoking logout().

    // Clear auto-configured fincept LLM provider and reset LlmService
    LlmConfigRepository::instance().delete_provider("fincept");
}

bool AuthManager::needs_pin_setup() const {
    // Sign-in requirement disabled for this build: never require PIN setup so
    // launch always goes straight to the dashboard (see initialize()).
    return false;
}

// ── Initialize ───────────────────────────────────────────────────────────────

void AuthManager::initialize() {
    set_loading(true);
    load_session();

    // Sign-in requirement disabled for this build: skip server-side session
    // validation and the login/pricing gate entirely, and treat every launch
    // as an authenticated, fully-paid session so WindowFrame::on_auth_state_changed
    // routes straight to the dashboard. Any api_key the user still has saved
    // (e.g. from before this change) is kept applied so LLM/cloud features that
    // resolve it via fincept_api_key() keep working; it is not required.
    if (!session_.api_key.isEmpty()) {
        auto& http = fincept::HttpClient::instance();
        http.set_auth_header(session_.api_key);
        http.clear_session_token();
    }

    session_.authenticated = true;
    session_.subscription.account_type = "enterprise";
    session_.user_info.account_type = "enterprise";
    session_.has_subscription = true;

    set_loading(false);
    emit auth_state_changed();
}

void AuthManager::fetch_user_profile(std::function<void()> on_done) {
    AuthApi::instance().get_user_profile([this, on_done = std::move(on_done)](ApiResponse r) mutable {
        if (!r.success && (r.status_code == 401 || r.status_code == 403)) {
            // API key is revoked or invalid — force re-login
            LOG_WARN("Auth", "Profile fetch returned 401/403 — API key invalid, clearing session");
            clear_session();
            set_loading(false);
            emit auth_state_changed();
            return;
        }
        if (r.success) {
            const auto data = unwrap_data(r.data);
            session_.user_info = UserProfile::from_json(data);

            // api_key confirmed valid — now restore session_token on HttpClient
            // so subsequent authenticated requests include it. If the old
            // session_token turns out to be stale during usage, SessionGuard
            // will handle recovery without clearing the api_key.
            if (!session_.session_token.isEmpty()) {
                fincept::HttpClient::instance().set_session_token(session_.session_token);
            }
        } else {
            // Network/server error — keep going with cached session data
            LOG_WARN("Auth", "Profile fetch failed (non-auth error) — using cached data");
            session_.authenticated = !session_.api_key.isEmpty();
            // Restore session_token for subsequent requests even in offline mode
            if (!session_.session_token.isEmpty()) {
                fincept::HttpClient::instance().set_session_token(session_.session_token);
            }
        }
        fetch_user_subscription(std::move(on_done));
    });
}

// ── Shared post-auth completion ───────────────────────────────────────────────
// Fetches subscription, syncs state, saves session, then calls on_done().
// Used by login / verify_otp / verify_mfa / session restore.

void AuthManager::complete_auth_flow(std::function<void()> on_done) {
    UserApi::instance().get_user_subscription([this, on_done = std::move(on_done)](ApiResponse r) {
        if (r.success) {
            const auto sub_data = unwrap_data(r.data);
            session_.subscription = UserSubscription::from_json(sub_data);
            session_.has_subscription = !session_.subscription.account_type.isEmpty();
        }
        // Fallback: promote profile account_type into subscription so account_type() is consistent
        if (session_.subscription.account_type.isEmpty() && !session_.user_info.account_type.isEmpty()) {
            session_.subscription.account_type = session_.user_info.account_type;
            session_.subscription.credit_balance = session_.user_info.credit_balance;
            session_.has_subscription = true;
        }
        // Sync user_info from subscription so legacy readers stay consistent
        if (!session_.subscription.account_type.isEmpty())
            session_.user_info.account_type = session_.subscription.account_type;
        if (session_.subscription.credit_balance > 0)
            session_.user_info.credit_balance = session_.subscription.credit_balance;

        if (!session_.api_key.isEmpty())
            session_.authenticated = true;
        save_session();
        auto_configure_fincept_llm();
        set_loading(false);
        if (on_done)
            on_done();
        emit auth_state_changed();
    });
}

void AuthManager::fetch_user_subscription(std::function<void()> on_done) {
    complete_auth_flow(std::move(on_done));
}

// ── Login ────────────────────────────────────────────────────────────────────

void AuthManager::login(const QString& email, const QString& password, bool force_login) {
    set_loading(true);

    LoginRequest req;
    req.email = sanitize_input(email).toLower();
    req.password = password;
    req.force_login = force_login;

    AuthApi::instance().login(req, [this](ApiResponse r) {
        if (!r.success) {
            set_loading(false);
            emit login_failed(r.error.isEmpty() ? "Login failed" : r.error);
            return;
        }

        const auto data = unwrap_data(r.data);

        if (data["active_session"].toBool()) {
            set_loading(false);
            emit login_active_session(data["message"].toString("You are already logged in on another device."));
            return;
        }

        if (data["mfa_required"].toBool()) {
            set_loading(false);
            emit login_mfa_required();
            return;
        }

        const QString api_key = data["api_key"].toString();
        if (api_key.isEmpty()) {
            set_loading(false);
            emit login_failed("No API key returned from server");
            return;
        }

        const QString session_token = data["session_token"].toString();

        // Set tokens on HttpClient — all subsequent API calls will use them
        apply_tokens(api_key, session_token);

        session_.authenticated = true;
        session_.api_key = api_key;
        session_.session_token = session_token;
        session_.device_id = generate_device_id();

        // Fetch profile then subscription; on_done emits login_succeeded
        fetch_user_profile([this] { emit login_succeeded(); });
    });
}

// ── Google desktop login (website loopback handoff) ──────────────────────────

void AuthManager::login_with_google() {
    set_loading(true);

    // Owned by AuthManager; self-deletes after emitting code_received/failed.
    auto* flow = new GoogleDesktopLogin(this);

    connect(flow, &GoogleDesktopLogin::code_received, this, [this](const QString& code) {
        AuthApi::instance().redeem_desktop_handoff(code, [this](ApiResponse r) {
            if (!r.success) {
                set_loading(false);
                emit login_failed(r.error.isEmpty() ? "Google login failed" : r.error);
                return;
            }
            const auto data = unwrap_data(r.data);
            complete_desktop_login(data["api_key"].toString(), data["session_token"].toString());
        });
    });

    connect(flow, &GoogleDesktopLogin::failed, this, [this](const QString& message) {
        set_loading(false);
        emit login_failed(message);
    });

    flow->start();
}

void AuthManager::complete_desktop_login(const QString& api_key, const QString& session_token) {
    if (api_key.isEmpty()) {
        set_loading(false);
        emit login_failed("No API key returned from server");
        return;
    }

    // Mirror the login() success tail exactly so the desktop handoff lands in
    // the same post-login machinery (profile + subscription fetch, save_session,
    // fincept LLM auto-config, login_succeeded).
    apply_tokens(api_key, session_token);

    session_.authenticated = true;
    session_.api_key = api_key;
    session_.session_token = session_token;
    session_.device_id = generate_device_id();

    LOG_INFO("Auth", "Desktop (Google) login successful");
    fetch_user_profile([this] { emit login_succeeded(); });
}

// ── Signup ───────────────────────────────────────────────────────────────────

void AuthManager::signup(const QString& username, const QString& email, const QString& password, const QString& phone,
                         const QString& country, const QString& country_code) {
    set_loading(true);

    RegisterRequest req;
    req.username = sanitize_input(username).toLower();
    req.email = sanitize_input(email).toLower();
    req.password = password;
    req.phone = phone;
    req.country = country;
    req.country_code = country_code;

    AuthApi::instance().register_user(req, [this](ApiResponse r) {
        set_loading(false);
        if (r.success)
            emit signup_succeeded();
        else
            emit signup_failed(r.error.isEmpty() ? "Registration failed" : r.error);
    });
}

// ── OTP verification ─────────────────────────────────────────────────────────

void AuthManager::verify_otp(const QString& email, const QString& otp) {
    set_loading(true);

    VerifyOtpRequest req;
    req.email = sanitize_input(email).toLower();
    req.otp = sanitize_input(otp);

    AuthApi::instance().verify_otp(req, [this](ApiResponse r) {
        if (!r.success) {
            set_loading(false);
            emit otp_failed(r.error.isEmpty() ? "Verification failed" : r.error);
            return;
        }

        const auto data = unwrap_data(r.data);
        const QString api_key = data["api_key"].toString();
        if (api_key.isEmpty()) {
            set_loading(false);
            emit otp_failed("No API key returned");
            return;
        }

        const QString session_token = data["session_token"].toString();
        apply_tokens(api_key, session_token);

        session_.authenticated = true;
        session_.api_key = api_key;
        session_.session_token = session_token;
        session_.device_id = generate_device_id();

        LOG_INFO("Auth", "OTP verified successfully");
        fetch_user_profile([this] { emit otp_verified(); });
    });
}

// ── MFA verification ─────────────────────────────────────────────────────────

void AuthManager::verify_mfa(const QString& email, const QString& otp) {
    set_loading(true);

    AuthApi::instance().verify_mfa(sanitize_input(email).toLower(), sanitize_input(otp), [this](ApiResponse r) {
        if (!r.success) {
            set_loading(false);
            emit mfa_failed(r.error.isEmpty() ? "MFA verification failed" : r.error);
            return;
        }

        const auto data = unwrap_data(r.data);
        const QString api_key = data["api_key"].toString();
        if (api_key.isEmpty()) {
            set_loading(false);
            emit mfa_failed("No API key returned");
            return;
        }

        const QString session_token = data["session_token"].toString();
        apply_tokens(api_key, session_token);

        session_.authenticated = true;
        session_.api_key = api_key;
        session_.session_token = session_token;
        session_.device_id = generate_device_id();

        LOG_INFO("Auth", "MFA verified successfully");
        fetch_user_profile([this] { emit mfa_verified(); });
    });
}

// ── Forgot password ──────────────────────────────────────────────────────────

void AuthManager::forgot_password(const QString& email) {
    set_loading(true);

    ForgotPasswordRequest req;
    req.email = sanitize_input(email).toLower();

    AuthApi::instance().forgot_password(req, [this](ApiResponse r) {
        set_loading(false);
        if (r.success)
            emit forgot_password_sent();
        else
            emit forgot_password_failed(r.error.isEmpty() ? "Failed to send reset code" : r.error);
    });
}

// ── Reset password ───────────────────────────────────────────────────────────

void AuthManager::reset_password(const QString& email, const QString& otp, const QString& new_password) {
    set_loading(true);

    ResetPasswordRequest req;
    req.email = sanitize_input(email).toLower();
    req.otp = sanitize_input(otp);
    req.new_password = new_password;

    AuthApi::instance().reset_password(req, [this](ApiResponse r) {
        set_loading(false);
        if (r.success)
            emit password_reset_succeeded();
        else
            emit password_reset_failed(r.error.isEmpty() ? "Password reset failed" : r.error);
    });
}

// ── Logout ───────────────────────────────────────────────────────────────────

void AuthManager::logout() {
    if (is_logging_out_)
        return;
    is_logging_out_ = true;

    if (!session_.api_key.isEmpty()) {
        AuthApi::instance().logout([](ApiResponse) {});
    }

    clear_session();
    is_logging_out_ = false;

    emit logged_out();
    emit auth_state_changed();
}

// ── Session recovery ─────────────────────────────────────────────────────────
// Called by SessionGuard when it gets 401. Instead of immediately logging out,
// we strip the stale session_token and re-validate with api_key only.
// If the api_key is still valid → session recovered, no disruption.
// If api_key is also invalid → truly expired, must re-login.

void AuthManager::attempt_session_recovery(std::function<void(bool)> cb) {
    if (session_.api_key.isEmpty()) {
        if (cb)
            cb(false);
        return;
    }

    LOG_INFO("Auth", "Attempting session recovery with api_key only");

    // Temporarily strip session_token so the validation request doesn't 401
    fincept::HttpClient::instance().clear_session_token();

    AuthApi::instance().get_user_profile([this, cb = std::move(cb)](ApiResponse r) mutable {
        if (r.success) {
            LOG_INFO("Auth", "Session recovery succeeded — api_key is valid");
            const auto data = unwrap_data(r.data);
            session_.user_info = UserProfile::from_json(data);
            session_.authenticated = true;

            // The old session_token was stale. Clear it from our saved state.
            // The app continues with api_key-only auth. A fresh session_token
            // will be obtained on the next explicit login.
            session_.session_token.clear();
            save_session();

            // Don't restore session_token — operate with api_key only
            if (cb)
                cb(true);
        } else if (r.status_code == 401 || r.status_code == 403) {
            LOG_WARN("Auth", "Session recovery failed — api_key is invalid");
            if (cb)
                cb(false);
        } else {
            // Network error — don't force logout, assume transient
            LOG_WARN("Auth", "Session recovery: network error — keeping session alive");
            // Restore session_token since we can't determine if it's stale
            if (!session_.session_token.isEmpty())
                fincept::HttpClient::instance().set_session_token(session_.session_token);
            if (cb)
                cb(true);
        }
    });
}

// ── Refresh user data ────────────────────────────────────────────────────────

void AuthManager::refresh_user_data() {
    if (!session_.authenticated || session_.api_key.isEmpty())
        return;
    // fetch_user_profile chains into fetch_user_subscription automatically
    fetch_user_profile([this] { emit subscription_fetched(); });
}

// ── Auto-configure Fincept LLM provider ──────────────────────────────────────

void AuthManager::auto_configure_fincept_llm() {
    if (session_.api_key.isEmpty())
        return;

    // CR-08: do NOT persist the api_key in the plaintext settings table. The
    // key already lives in the in-memory session and in SecureStorage (written
    // by save_session). LlmService and friends resolve it via
    // AuthManager::fincept_api_key(). Defensively purge any stale plaintext row.
    fincept::SettingsRepository::instance().remove("fincept_api_key");

    // Only create the fincept provider row if it doesn't already exist.
    // This prevents overwriting the user's model/settings choice on every
    // session revalidation (~30s interval).
    auto providers = LlmConfigRepository::instance().list_providers();
    bool fincept_exists = false;
    if (providers.is_ok()) {
        for (const auto& p : providers.value()) {
            if (p.provider.toLower() == "fincept") {
                fincept_exists = true;
                break;
            }
        }
    }

    if (!fincept_exists) {
        LlmConfig fincept_llm;
        fincept_llm.provider = "fincept";
        fincept_llm.model = "MiniMax-M2.7";
        fincept_llm.base_url = {};
        LlmConfigRepository::instance().save_provider(fincept_llm);
        LOG_INFO("Auth", "Created fincept LLM provider config");
    }

    // Set as active if no other provider is currently active
    auto active = LlmConfigRepository::instance().get_active_provider();
    bool has_active = active.is_ok() && !active.value().provider.isEmpty();
    if (!has_active)
        LlmConfigRepository::instance().set_active("fincept");
}

} // namespace fincept::auth
