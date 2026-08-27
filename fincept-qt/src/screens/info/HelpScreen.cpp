#include "screens/info/HelpScreen.h"

#include "ui/theme/Theme.h"

#include <QDesktopServices>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QUrl>
#include <QVBoxLayout>

namespace fincept::screens {

using namespace fincept::ui;

static const char* MF = "font-family:'Consolas','Courier New',monospace;";

// ── Collapsible FAQ item ──────────────────────────────────────────────────────

static QWidget* make_faq(const QString& question, const QString& answer, const QString& icon = "?") {
    auto* container = new QWidget(nullptr);
    container->setStyleSheet("background: transparent;");
    auto* vl = new QVBoxLayout(container);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->setSpacing(0);

    // The disclosure arrow is part of the label: the previous version computed
    // an arrow into a local and then Q_UNUSED'd it, so the button text never
    // showed whether the answer was open or closed.
    auto btn_text = [question, icon](bool open) {
        const QString arrow = open ? QStringLiteral("▾") : QStringLiteral("▸");
        return icon.isEmpty() ? QString("  %1  %2").arg(arrow, question)
                              : QString("  %1  %2  %3").arg(arrow, icon, question);
    };

    auto* q_btn = new QPushButton(btn_text(false));
    q_btn->setCursor(Qt::PointingHandCursor);
    q_btn->setAccessibleName(question);
    q_btn->setStyleSheet(QString("QPushButton { color: %1; background: %2; border: 1px solid %3;"
                                 " padding: 11px 14px; text-align: left;"
                                 " font-size: 12px; font-weight: 600; %4 }"
                                 "QPushButton:hover { background: %5; border-color: %6; color: %7; }")
                             .arg(colors::TEXT_PRIMARY(), colors::BG_SURFACE(), colors::BORDER_DIM(), MF,
                                  colors::BG_RAISED(), colors::AMBER(), colors::AMBER()));

    auto* a_lbl = new QLabel(answer);
    a_lbl->setWordWrap(true);
    a_lbl->setStyleSheet(
        QString("color: %1; font-size: 12px; background: %2;"
                " border: 1px solid %3; border-top: none;"
                " border-left: 3px solid %4;"
                " padding: 12px 16px 12px 18px; %5")
            .arg(colors::TEXT_SECONDARY(), colors::BG_SURFACE(), colors::BORDER_DIM(), colors::AMBER(), MF));
    a_lbl->setVisible(false);

    QObject::connect(q_btn, &QPushButton::clicked, a_lbl, [a_lbl, q_btn, btn_text]() {
        bool show = !a_lbl->isVisible();
        a_lbl->setVisible(show);
        q_btn->setText(btn_text(show));
        // tint open state
        q_btn->setStyleSheet(
            QString("QPushButton { color: %1; background: %2; border: 1px solid %3;"
                    " border-left: 3px solid %3;"
                    " padding: 11px 14px; text-align: left;"
                    " font-size: 12px; font-weight: 600; font-family:'Consolas','Courier New',monospace; }"
                    "QPushButton:hover { background: %4; }")
                .arg(show ? colors::AMBER() : colors::TEXT_PRIMARY(), show ? colors::BG_RAISED() : colors::BG_SURFACE(),
                     show ? colors::AMBER() : colors::BORDER_DIM(), colors::BG_RAISED()));
    });

    vl->addWidget(q_btn);
    vl->addWidget(a_lbl);
    return container;
}

// ── Section header ────────────────────────────────────────────────────────────

static QWidget* section_header(const QString& title, const QString& subtitle = {}) {
    auto* w = new QWidget(nullptr);
    w->setStyleSheet("background: transparent;");
    auto* vl = new QVBoxLayout(w);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->setSpacing(3);

    auto* t = new QLabel(title);
    t->setStyleSheet(QString("color: %1; font-size: 11px; font-weight: bold; letter-spacing: 1px;"
                             " background: transparent; %2")
                         .arg(colors::AMBER(), MF));
    vl->addWidget(t);

    if (!subtitle.isEmpty()) {
        auto* s = new QLabel(subtitle);
        s->setStyleSheet(
            QString("color: %1; font-size: 11px; background: transparent; %2").arg(colors::TEXT_TERTIARY(), MF));
        vl->addWidget(s);
    }
    return w;
}

// ── Constructor ───────────────────────────────────────────────────────────────

HelpScreen::HelpScreen(QWidget* parent) : QWidget(parent) {
    setStyleSheet(QString("QWidget#HelpRoot { background: %1; }").arg(colors::BG_BASE()));
    setObjectName("HelpRoot");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    scroll_ = new QScrollArea;
    scroll_->setWidgetResizable(true);
    scroll_->setStyleSheet("QScrollArea { border: none; background: transparent; }");
    scroll_->setWidget(build_page());
    root->addWidget(scroll_, 1);

    // ── Theme wiring ──────────────────────────────────────────────────────────
    connect(&ui::ThemeManager::instance(), &ui::ThemeManager::theme_changed, this, [this](const ui::ThemeTokens&) {
        setStyleSheet(QString("QWidget#HelpRoot { background: %1; }").arg(colors::BG_BASE()));
    });
}

// ── Re-translation ────────────────────────────────────────────────────────────
// Static-content screen with no live state — on language change we rebuild
// the page from scratch rather than caching every label/button as a member.
// QScrollArea::setWidget() takes ownership and deletes the previous content.

void HelpScreen::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange && scroll_) {
        scroll_->setWidget(build_page());
    }
    QWidget::changeEvent(event);
}

// ── Page builder ──────────────────────────────────────────────────────────────

QWidget* HelpScreen::build_page() {
    auto* page = new QWidget;
    page->setStyleSheet(QString("background: %1;").arg(colors::BG_BASE()));
    auto* vl = new QVBoxLayout(page);
    vl->setContentsMargins(28, 24, 28, 32);
    vl->setSpacing(0);

    // ── Hero banner ───────────────────────────────────────────────────────────
    {
        auto* hero = new QWidget(page);
        hero->setStyleSheet(QString("background: %1; border: 1px solid %2; border-left: 4px solid %3;")
                                .arg(colors::BG_SURFACE(), colors::BORDER_DIM(), colors::AMBER()));
        auto* hl = new QHBoxLayout(hero);
        hl->setContentsMargins(20, 18, 20, 18);
        hl->setSpacing(16);

        auto* text_vl = new QVBoxLayout;
        text_vl->setSpacing(5);

        auto* title = new QLabel(tr("HELP CENTER"));
        title->setStyleSheet(QString("color: %1; font-size: 22px; font-weight: 700; letter-spacing: 2px;"
                                     " background: transparent; %2")
                                 .arg(colors::AMBER(), MF));
        text_vl->addWidget(title);

        auto* sub = new QLabel(tr("Find answers, get support, and connect with the Fincept community."));
        sub->setStyleSheet(
            QString("color: %1; font-size: 12px; background: transparent; %2").arg(colors::TEXT_SECONDARY(), MF));
        text_vl->addWidget(sub);

        hl->addLayout(text_vl, 1);

        // Contact chips on the right
        auto* chips_vl = new QVBoxLayout;
        chips_vl->setSpacing(5);

        auto make_chip = [](const QString& icon, const QString& text, const QString& color,
                            const QString& url = {}) -> QWidget* {
            if (url.isEmpty()) {
                auto* chip = new QLabel(icon.isEmpty() ? text : QString("%1  %2").arg(icon, text));
                chip->setStyleSheet(QString("color: %1; font-size: 11px; background: transparent;"
                                            " font-family:'Consolas','Courier New',monospace;")
                                        .arg(color));
                return chip;
            }
            auto* chip = new QPushButton(icon.isEmpty() ? text : QString("%1  %2").arg(icon, text));
            chip->setFlat(true);
            chip->setCursor(Qt::PointingHandCursor);
            chip->setStyleSheet(QString("QPushButton { color: %1; font-size: 11px; background: transparent;"
                                        " border: none; text-align: left; padding: 0;"
                                        " font-family:'Consolas','Courier New',monospace; }"
                                        "QPushButton:hover { color: %2; }")
                                    .arg(color, colors::AMBER()));
            QObject::connect(chip, &QPushButton::clicked, chip, [url]() { QDesktopServices::openUrl(QUrl(url)); });
            return chip;
        };
        // Email + Discord chip text is shown verbatim (URL/handle) — not translated.
        // Business-hours label IS translated.
        chips_vl->addWidget(make_chip("✉", "support@fincept.in", colors::CYAN, "mailto:support@fincept.in"));
        chips_vl->addWidget(make_chip("", "discord.gg/ae87a8ygbN", colors::POSITIVE, "https://discord.gg/ae87a8ygbN"));
        // No staffed-hours claim: the project has no published support roster,
        // and the previous "Mon-Fri 9AM-6PM EST" line implied one.
        chips_vl->addWidget(
            make_chip("", "github.com/Fincept-Corporation/FinceptTerminal", colors::TEXT_TERTIARY,
                      "https://github.com/Fincept-Corporation/FinceptTerminal/issues"));
        hl->addLayout(chips_vl);

        vl->addWidget(hero);
    }

    vl->addSpacing(20);

    // ── Quick Actions ─────────────────────────────────────────────────────────
    {
        vl->addWidget(section_header(tr("QUICK ACTIONS"), tr("Common tasks you can do right now")));
        vl->addSpacing(8);

        auto* grid = new QGridLayout;
        grid->setSpacing(8);

        // Each Action carries a stable English `key` (used to wire signals)
        // separate from the translated label/desc strings.
        struct Action {
            const char* icon;
            const char* key;
            QString label;
            QString desc;
        };
        const Action actions[] = {
            {"", "documentation", tr("Documentation"), tr("Guides, tutorials & API ref")},
            {"", "report_bug", tr("Report a Bug"), tr("Open a GitHub issue")},
            {"", "join_discord", tr("Join Discord"), tr("Community & live support")},
            {"", "support_tickets", tr("Email Support"), tr("Write to support@fincept.in")},
        };

        int col = 0, row = 0;
        for (const auto& a : actions) {
            auto* btn = new QPushButton;
            btn->setCursor(Qt::PointingHandCursor);
            btn->setFixedHeight(60);
            btn->setStyleSheet(QString("QPushButton { background: %1; color: %2; border: 1px solid %3;"
                                       " text-align: left; padding: 0; }"
                                       "QPushButton:hover { background: %4; border-color: %5; }")
                                   .arg(colors::BG_SURFACE(), colors::TEXT_PRIMARY(), colors::BORDER_DIM(),
                                        colors::BG_RAISED(), colors::AMBER()));

            auto* bl = new QVBoxLayout(btn);
            bl->setContentsMargins(12, 8, 12, 8);
            bl->setSpacing(3);

            auto* top = new QHBoxLayout;
            top->setSpacing(7);
            if (a.icon[0]) {
                auto* icon_lbl = new QLabel(QString::fromUtf8(a.icon));
                icon_lbl->setStyleSheet("background: transparent; font-size: 14px;");
                top->addWidget(icon_lbl);
            }
            auto* name_lbl = new QLabel(a.label);
            name_lbl->setStyleSheet(QString("background: transparent; color: %1; font-size: 12px;"
                                            " font-weight: bold; %2")
                                        .arg(colors::TEXT_PRIMARY(), MF));
            top->addWidget(name_lbl);
            top->addStretch();
            bl->addLayout(top);

            auto* desc_lbl = new QLabel(a.desc);
            desc_lbl->setStyleSheet(
                QString("background: transparent; color: %1; font-size: 10px; %2").arg(colors::TEXT_TERTIARY(), MF));
            bl->addWidget(desc_lbl);

            if (col == 3) {
                col = 0;
                ++row;
            }
            grid->addWidget(btn, row, col++);
            btn->setAccessibleName(a.label);
            btn->setToolTip(a.desc);

            // Wire known actions by stable English key (label is localized).
            // Four of these six buttons had no connect() at all — clicking
            // Documentation / Report a Bug / Join Discord / Support Tickets did
            // nothing. The three that map to a public URL now open it; the
            // in-app ticket view is reachable from the Support tab, which the
            // description now says.
            const QString key = QString::fromLatin1(a.key);
            auto open = [btn](const QString& url) {
                QObject::connect(btn, &QPushButton::clicked, btn, [url]() { QDesktopServices::openUrl(QUrl(url)); });
            };
            if (key == "documentation")
                open(QStringLiteral("https://github.com/Fincept-Corporation/FinceptTerminal/tree/main/docs"));
            else if (key == "report_bug")
                open(QStringLiteral("https://github.com/Fincept-Corporation/FinceptTerminal/issues/new"));
            else if (key == "join_discord")
                open(QStringLiteral("https://discord.gg/ae87a8ygbN"));
            else if (key == "support_tickets")
                open(QStringLiteral("mailto:support@fincept.in"));
        }

        vl->addLayout(grid);
    }

    vl->addSpacing(24);

    // ── FAQ ───────────────────────────────────────────────────────────────────
    {
        vl->addWidget(section_header(tr("FREQUENTLY ASKED QUESTIONS"), tr("Click a question to expand the answer")));
        vl->addSpacing(8);

        struct FAQ {
            const char* icon;
            QString q;
            QString a;
        };
        const FAQ faqs[] = {
            {"", tr("How do I reset my password?"),
             tr("Click \"Forgot Password\" on the login screen. Enter your email address and we'll "
                "send you a reset link. The link expires in 24 hours.")},

            {"", tr("What is Guest Access?"),
             tr("Guest access lets you explore the terminal without creating an account. "
                "Features like trading, portfolio management, and AI analytics require a "
                "registered account.")},

            {"", tr("What is a Credit?"),
             tr("Credits are the in-app currency used for premium features such as AI analysis, "
                "advanced data feeds, and quantitative analytics. Free accounts receive a limited "
                "number of credits on signup. Additional credits can be purchased in Settings → Billing.")},

            {"", tr("How do I connect a broker?"),
             tr("Navigate to Settings → Brokers, select your broker from the list, and enter your "
                "API key and secret. Fincept supports 16 brokers including Zerodha, Angel One, "
                "Upstox, Interactive Brokers, and more.")},

            {"", tr("Why does Python install at first launch?"),
             tr("Fincept embeds Python 3.11 for its analytics scripts covering equity, "
                "portfolio, derivatives, and quant analysis. The one-time install happens "
                "automatically in the background.")},

            {"", tr("What are the system requirements?"),
             tr("Windows 10+ (x64), macOS 12+, or Linux (glibc 2.31+). 8 GB RAM recommended. "
                "Active internet required for data feeds. Python 3.11 is installed automatically "
                "during first-time setup.")},

            {"", tr("Is my data secure?"),
             tr("Credentials are stored encrypted via SecureStorage (OS keychain on each platform). "
                "API keys are never logged or sent to Fincept servers — they are used only for "
                "direct broker connections from your machine.")},

            {"", tr("How do I report a bug?"),
             tr("Open a ticket in the Support tab with category \"Bug Report\", or file a GitHub "
                "issue. Include your OS, version, steps to reproduce, and any error messages you "
                "see. If the app crashed, attach the dump from About → Diagnostics.")},
        };

        for (const auto& f : faqs)
            vl->addWidget(make_faq(f.q, f.a, QString::fromUtf8(f.icon)));

        vl->addSpacing(24);
    }

    // ── Getting Started ────────────────────────────────────────────────────────
    {
        vl->addWidget(section_header(tr("GETTING STARTED"), tr("New to Fincept? Start here")));
        vl->addSpacing(8);

        struct Step {
            const char* num;
            QString title;
            QString detail;
        };
        const Step steps[] = {
            {"1", tr("Complete setup"), tr("The setup wizard installs Python and configures your paths.")},
            {"2", tr("Connect a data source"), tr("Add a broker or enable free data feeds in Data Sources.")},
            {"3", tr("Configure an AI provider"), tr("Add your own API key in Settings → LLM Config.")},
            {"4", tr("Explore the terminal"), tr("Browse Markets, Research, AI Chat, and the Dashboard.")},
        };

        auto* steps_widget = new QWidget(page);
        steps_widget->setStyleSheet(
            QString("background: %1; border: 1px solid %2;").arg(colors::BG_SURFACE(), colors::BORDER_DIM()));
        auto* swl = new QVBoxLayout(steps_widget);
        swl->setContentsMargins(0, 0, 0, 0);
        swl->setSpacing(0);

        for (int i = 0; i < 4; ++i) {
            const auto& s = steps[i];
            auto* row = new QWidget(steps_widget);
            bool last = (i == 3);
            row->setStyleSheet(QString("background: transparent; border-bottom: %1;")
                                   .arg(last ? "none" : QString("1px solid %1;").arg(colors::BORDER_DIM())));
            auto* rl = new QHBoxLayout(row);
            rl->setContentsMargins(16, 12, 16, 12);
            rl->setSpacing(14);

            auto* num = new QLabel(s.num);
            num->setFixedSize(26, 26);
            num->setAlignment(Qt::AlignCenter);
            num->setStyleSheet(QString("background: %1; color: %2; font-size: 12px; font-weight: bold;"
                                       " border-radius: 13px; %3")
                                   .arg(colors::AMBER(), colors::BG_BASE(), MF));
            rl->addWidget(num);

            auto* txt_vl = new QVBoxLayout;
            txt_vl->setSpacing(2);
            auto* ttl = new QLabel(s.title);
            ttl->setStyleSheet(QString("color: %1; font-size: 12px; font-weight: bold;"
                                       " background: transparent; %2")
                                   .arg(colors::TEXT_PRIMARY(), MF));
            auto* det = new QLabel(s.detail);
            det->setStyleSheet(
                QString("color: %1; font-size: 11px; background: transparent; %2").arg(colors::TEXT_SECONDARY(), MF));
            det->setWordWrap(true);
            txt_vl->addWidget(ttl);
            txt_vl->addWidget(det);
            rl->addLayout(txt_vl, 1);

            swl->addWidget(row);
        }

        vl->addWidget(steps_widget);
        vl->addSpacing(24);
    }

    // ── Contact & Resources ───────────────────────────────────────────────────
    {
        vl->addWidget(section_header(tr("CONTACT & RESOURCES")));
        vl->addSpacing(8);

        auto* grid2 = new QGridLayout;
        grid2->setSpacing(8);

        struct Contact {
            const char* icon;
            QString label;
            const char* value; // brand string — URL / handle, not translated
            const char* url;
        };
        const Contact contacts[] = {
            {"✉", tr("Email Support"), "support@fincept.in", "mailto:support@fincept.in"},
            {"", tr("Discord Server"), "discord.gg/ae87a8ygbN", "https://discord.gg/ae87a8ygbN"},
            {"", tr("Website"), "fincept.in", "https://fincept.in"},
            {"", tr("GitHub"), "github.com/Fincept-Corporation/FinceptTerminal",
             "https://github.com/Fincept-Corporation/FinceptTerminal"},
        };

        int ci = 0;
        for (const auto& c : contacts) {
            auto* card = new QWidget(page);
            card->setStyleSheet(
                QString("background: %1; border: 1px solid %2;").arg(colors::BG_SURFACE(), colors::BORDER_DIM()));
            auto* cl = new QHBoxLayout(card);
            cl->setContentsMargins(14, 12, 14, 12);
            cl->setSpacing(10);

            if (c.icon[0]) {
                auto* ico = new QLabel(QString::fromUtf8(c.icon));
                ico->setStyleSheet("background: transparent; font-size: 18px;");
                cl->addWidget(ico);
            }

            auto* tvl = new QVBoxLayout;
            tvl->setSpacing(2);
            auto* lbl = new QLabel(c.label);
            lbl->setStyleSheet(QString("color: %1; font-size: 10px; font-weight: bold; background: transparent; %2")
                                   .arg(colors::TEXT_TERTIARY(), MF));

            auto* val = new QPushButton(c.value);
            val->setFlat(true);
            val->setCursor(Qt::PointingHandCursor);
            val->setStyleSheet(QString("QPushButton { color: %1; font-size: 11px; background: transparent;"
                                       " border: none; text-align: left; padding: 0; %2 }"
                                       "QPushButton:hover { color: %3; text-decoration: underline; }")
                                   .arg(colors::CYAN(), MF, colors::AMBER()));
            const QString link(c.url);
            QObject::connect(val, &QPushButton::clicked, val, [link]() { QDesktopServices::openUrl(QUrl(link)); });

            tvl->addWidget(lbl);
            tvl->addWidget(val);
            cl->addLayout(tvl, 1);

            grid2->addWidget(card, ci / 2, ci % 2);
            ++ci;
        }

        vl->addLayout(grid2);
    }

    vl->addStretch();
    return page;
}

} // namespace fincept::screens
