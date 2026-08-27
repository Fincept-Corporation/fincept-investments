// SettingsScreen.cpp — thin shell hosting nav + a QStackedWidget of section
// widgets. All per-section state and rendering lives in *Section.h/.cpp.

#include "screens/settings/SettingsScreen.h"

#include "core/events/EventBus.h"
#include "core/i18n/LanguageManager.h"
#include "core/logging/Logger.h"
#include "core/session/ScreenStateManager.h"
#include "screens/settings/AppearanceSection.h"
#include "screens/settings/CredentialsSection.h"
#include "screens/settings/DataSourcesSection.h"
#include "screens/settings/DeveloperSection.h"
#include "screens/settings/GeneralSection.h"
#include "screens/settings/KeybindingsSection.h"
#include "screens/settings/LlmConfigSection.h"
#include "screens/settings/LoggingSection.h"
#include "screens/settings/McpServersSection.h"
#include "screens/settings/NotificationsSection.h"
#include "screens/settings/ProfilesSection.h"
#include "screens/settings/PythonEnvSection.h"
#include "screens/settings/SecuritySection.h"
#include "screens/settings/SettingsStyles.h"
#include "screens/settings/StorageSection.h"
#include "screens/settings/VoiceConfigSection.h"
#include "services/llm/LlmService.h"
#include "services/stt/SpeechService.h"
#include "services/tts/TtsService.h"
#include "services/voice_trigger/ClapDetectorService.h"
#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

namespace fincept::screens {

SettingsScreen::SettingsScreen(QWidget* parent) : QWidget(parent) {
    using namespace settings_styles;

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Left nav panel
    nav_ = new QWidget(this);
    nav_->setFixedWidth(200);
    auto* nvl = new QVBoxLayout(nav_);
    nvl->setContentsMargins(8, 16, 8, 8);
    nvl->setSpacing(2);

    nav_title_ = new QLabel;
    nav_title_->setStyleSheet(section_title_ss());
    nvl->addWidget(nav_title_);
    nvl->addSpacing(6);

    nav_filter_ = new QLineEdit;
    nav_filter_->setFixedHeight(26);
    nav_filter_->setClearButtonEnabled(true);
    nav_filter_->setStyleSheet(input_ss());
    nav_filter_->setAccessibleName(tr("Search settings"));
    connect(nav_filter_, &QLineEdit::textChanged, this, &SettingsScreen::apply_nav_filter);
    nvl->addWidget(nav_filter_);
    nvl->addSpacing(6);

    // ── Section factories ────────────────────────────────────────────────────
    // One factory per stack index. Used at construction AND by the language-
    // change rebuild path so we don't hardcode the type list twice.
    section_factories_.clear();
    section_factories_.resize(15);
    section_factories_[0] = [] { return new CredentialsSection; };
    section_factories_[1] = [] { return new AppearanceSection; };
    section_factories_[2] = [] { return new NotificationsSection; };
    section_factories_[3] = [] { return new StorageSection; };
    section_factories_[4] = [] { return new DataSourcesSection; };
    section_factories_[5] = [] { return new LlmConfigSection; };
    section_factories_[6] = [] { return new McpServersSection; };
    section_factories_[7] = [] { return new LoggingSection; };
    section_factories_[8] = [] { return new SecuritySection; };
    section_factories_[9] = [] { return new ProfilesSection; };
    section_factories_[10] = [] { return new KeybindingsSection; };
    section_factories_[11] = [] { return new PythonEnvSection; };
    section_factories_[12] = [] { return new DeveloperSection; };
    section_factories_[13] = [] { return new VoiceConfigSection; };
    section_factories_[14] = [] { return new GeneralSection; };

    sections_ = new QStackedWidget;
    for (const auto& factory : section_factories_)
        sections_->addWidget(factory());

    auto make_btn = [&](const QString& source_key, int idx, const QString& keywords = {}) {
        auto* btn = new QPushButton;
        btn->setFixedHeight(32);
        btn->setCheckable(true);
        btn->setStyleSheet(nav_btn_ss());
        btn->setProperty("settingsSectionIndex", idx);
        connect(btn, &QPushButton::clicked, this, [this, btn, idx]() {
            if (auto* parent = btn->parentWidget()) {
                for (auto* sibling : parent->findChildren<QPushButton*>())
                    sibling->setChecked(false);
            }
            btn->setChecked(true);
            sections_->setCurrentIndex(idx);
            ScreenStateManager::instance().notify_changed(this);
        });
        nvl->addWidget(btn);
        nav_buttons_.append({btn, source_key, keywords});
        return btn;
    };

    auto add_scope_header = [&](const QString& key) {
        auto* h = new QLabel;
        h->setStyleSheet(QString("color:%1;font-size:10px;font-weight:700;"
                                 "letter-spacing:1.2px;padding:8px 6px 4px 6px;")
                             .arg(ui::colors::TEXT_DIM()));
        nvl->addWidget(h);
        scope_headers_.append(h);
        scope_header_keys_.append(key);
    };

    // The keyword strings are search aliases only — never displayed, so they
    // stay in English (the filter also matches the translated label text).
    add_scope_header(QStringLiteral("SHELL"));
    auto* first = make_btn(QStringLiteral("General"), 14,
                           QStringLiteral("language locale currency window close launchpad quit"));
    make_btn(QStringLiteral("Appearance"), 1, QStringLiteral("theme font size family density ticker chat bubble "
                                                             "animation typography interface"));
    make_btn(QStringLiteral("Notifications"), 2,
             QStringLiteral("telegram discord slack email smtp whatsapp twilio pushover ntfy pushbullet gotify "
                            "mattermost teams webhook pagerduty opsgenie sms alerts price news order fill"));
    make_btn(QStringLiteral("Keybindings"), 10, QStringLiteral("shortcut hotkey keyboard rebind keys"));
    make_btn(QStringLiteral("Voice"), 13,
             QStringLiteral("speech stt tts deepgram microphone mic clap wake trigger aura pyttsx3"));
    make_btn(QStringLiteral("Logging"), 7, QStringLiteral("log level debug trace json log file tags diagnostics"));
    make_btn(QStringLiteral("Developer"), 12, QStringLiteral("datahub inspector agentic experimental devtools"));

    add_scope_header(QStringLiteral("PROFILE"));
    make_btn(QStringLiteral("Profiles"), 9, QStringLiteral("account switch multi profile workspace isolation"));
    make_btn(QStringLiteral("Credentials"), 0,
             QStringLiteral("api key secret token keychain alpha vantage polygon fred binance kraken finnhub "
                            "polymarket tiingo quandl databento newsapi iex"));
    make_btn(QStringLiteral("Security"), 8,
             QStringLiteral("pin lock auto-lock timeout inactivity audit log attempts lockout minimize"));
    make_btn(QStringLiteral("Data Sources"), 4, QStringLiteral("connections connectors websocket rest sql providers"));
    make_btn(QStringLiteral("LLM Config"), 5,
             QStringLiteral("ai model openai anthropic ollama groq provider profile temperature tokens system "
                            "prompt tool rounds mcp tools"));
    make_btn(QStringLiteral("MCP Servers"), 6, QStringLiteral("model context protocol tools external server"));
    make_btn(QStringLiteral("Python Env"), 11, QStringLiteral("packages venv pip uv numpy libraries install upgrade"));
    make_btn(QStringLiteral("Storage & Cache"), 3,
             QStringLiteral("disk database sqlite sql console cache clear delete data danger zone workspaces"));

    first->setChecked(true);
    sections_->setCurrentIndex(14);

    nvl->addStretch();
    root->addWidget(nav_);
    root->addWidget(sections_, 1);

    retranslateUi();
    wire_section_signals();

    connect(&ui::ThemeManager::instance(), &ui::ThemeManager::theme_changed, this,
            [this](const ui::ThemeTokens&) { refresh_theme(); });
    refresh_theme();

    // Language changes mean every section needs fresh tr() lookups. We rebuild
    // each section by constructing a new instance via its factory — simpler
    // and more reliable than threading retranslateUi() through every section.
    connect(&i18n::LanguageManager::instance(), &i18n::LanguageManager::language_changed, this,
            [this](const QString&) { rebuild_sections_for_language_change(); });
}

void SettingsScreen::changeEvent(QEvent* e) {
    if (e->type() == QEvent::LanguageChange)
        retranslateUi();
    QWidget::changeEvent(e);
}

void SettingsScreen::apply_nav_filter(const QString& text) {
    const QString needle = text.trimmed();
    const bool filtering = !needle.isEmpty();

    for (const auto& nb : nav_buttons_) {
        if (!nb.btn)
            continue;
        const bool match = !filtering || nb.btn->text().contains(needle, Qt::CaseInsensitive) ||
                           nb.source_key.contains(needle, Qt::CaseInsensitive) ||
                           nb.keywords.contains(needle, Qt::CaseInsensitive);
        nb.btn->setVisible(match);
    }
    for (auto* h : scope_headers_) {
        if (h)
            h->setVisible(!filtering);
    }
}

void SettingsScreen::retranslateUi() {
    if (nav_title_)
        nav_title_->setText(tr("SETTINGS"));
    if (nav_filter_)
        nav_filter_->setPlaceholderText(tr("Search settings…"));
    for (const auto& nb : nav_buttons_) {
        if (nb.btn)
            nb.btn->setText(tr(nb.source_key.toUtf8().constData()));
    }
    for (int i = 0; i < scope_headers_.size() && i < scope_header_keys_.size(); ++i) {
        if (scope_headers_[i])
            scope_headers_[i]->setText(tr(scope_header_keys_[i].toUtf8().constData()));
    }
}

void SettingsScreen::rebuild_sections_for_language_change() {
    if (!sections_)
        return;
    const int current = sections_->currentIndex();
    // Replace each widget in-place. We insert at the same index first, then
    // remove the old widget — preserves index ordering for nav buttons.
    for (int i = 0; i < section_factories_.size(); ++i) {
        if (!section_factories_[i])
            continue;
        QWidget* old = sections_->widget(i);
        QWidget* fresh = section_factories_[i]();
        sections_->insertWidget(i, fresh);
        sections_->removeWidget(old);
        if (old)
            old->deleteLater();
    }
    sections_->setCurrentIndex(current);
    // External signals must be re-wired against the fresh section instances.
    wire_section_signals();
}

void SettingsScreen::wire_section_signals() {
    if (!sections_)
        return;
    // LLM config changes → reload AI chat service.
    if (auto* llm = qobject_cast<LlmConfigSection*>(sections_->widget(5))) {
        connect(llm, &LlmConfigSection::config_changed, this,
                []() { ai_chat::LlmService::instance().reload_config(); });
    }
    // Voice config changes → reload BOTH STT and TTS services and restart
    // the clap detector so the user's new provider / key / voice / wake-trigger
    // picks take effect on the next session.
    if (auto* voice = qobject_cast<VoiceConfigSection*>(sections_->widget(13))) {
        connect(voice, &VoiceConfigSection::config_changed, this, []() {
            fincept::services::SpeechService::instance().reload_config();
            fincept::services::TtsService::instance().reload_config();

            auto& clap = fincept::services::ClapDetectorService::instance();
            clap.stop();
            if (fincept::services::ClapDetectorService::is_enabled_in_config())
                clap.start();
        });
    }
}

void SettingsScreen::refresh_theme() {
    using namespace settings_styles;
    setStyleSheet(QString("background:%1;").arg(ui::colors::BG_BASE()));
    if (nav_) {
        nav_->setStyleSheet(QString("background:%1;border-right:1px solid %2;")
                                .arg(ui::colors::BG_SURFACE(), ui::colors::BORDER_DIM()));
        // Walk our own list rather than every QPushButton descendant, so the
        // nav style is never applied to embedded controls (e.g. the filter
        // box's clear button).
        for (const auto& nb : nav_buttons_) {
            if (nb.btn)
                nb.btn->setStyleSheet(nav_btn_ss());
        }
        if (nav_filter_)
            nav_filter_->setStyleSheet(input_ss());
        // Re-apply scope-header colour. Walk our explicit list rather than
        // text-matching, so the headers' style survives translation to
        // languages where the source label no longer literally reads "SHELL".
        for (auto* lbl : scope_headers_) {
            if (!lbl)
                continue;
            lbl->setStyleSheet(QString("color:%1;font-size:10px;font-weight:700;"
                                       "letter-spacing:1.2px;padding:8px 6px 4px 6px;")
                                   .arg(ui::colors::TEXT_DIM()));
        }
    }
}

void SettingsScreen::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    subscribe_mcp_events();
}

void SettingsScreen::hideEvent(QHideEvent* e) {
    QWidget::hideEvent(e);
    unsubscribe_mcp_events();
}

void SettingsScreen::reload_visible_section() {
    if (!sections_)
        return;
    auto* w = sections_->currentWidget();
    if (!w)
        return;
    w->hide();
    w->show();
}

// ── MCP-driven UI sync ──────────────────────────────────────────────────────

void SettingsScreen::subscribe_mcp_events() {
    if (!mcp_event_subs_.isEmpty())
        return; // idempotent

    QPointer<SettingsScreen> self = this;
    auto on_settings_changed = [self](const QVariantMap&) {
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self]() {
                if (!self)
                    return;
                self->reload_visible_section();
            },
            Qt::QueuedConnection);
    };
    auto on_provider_changed = [self](const QVariantMap&) {
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self]() {
                if (!self)
                    return;
                self->reload_visible_section();
                ai_chat::LlmService::instance().reload_config();
            },
            Qt::QueuedConnection);
    };

    auto& bus = EventBus::instance();
    mcp_event_subs_.append(bus.subscribe(this, "settings.changed", on_settings_changed));
    mcp_event_subs_.append(bus.subscribe(this, "llm.provider_changed", on_provider_changed));
}

void SettingsScreen::unsubscribe_mcp_events() {
    auto& bus = EventBus::instance();
    for (auto id : mcp_event_subs_)
        bus.unsubscribe(id);
    mcp_event_subs_.clear();
}

QVariantMap SettingsScreen::save_state() const {
    return {{"section", sections_ ? sections_->currentIndex() : 0}};
}

void SettingsScreen::restore_state(const QVariantMap& state) {
    if (!sections_)
        return;
    const int idx = state.value("section", 14).toInt();
    if (idx < 0 || idx >= sections_->count())
        return;
    sections_->setCurrentIndex(idx);

    if (!nav_)
        return;
    for (auto* btn : nav_->findChildren<QPushButton*>()) {
        const QVariant tag = btn->property("settingsSectionIndex");
        btn->setChecked(tag.isValid() && tag.toInt() == idx);
    }
}

} // namespace fincept::screens
