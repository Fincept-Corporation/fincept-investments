// src/screens/ai_chat/AiChatScreen_Messaging.cpp
//
// Message lifecycle: typing indicator, on_send, streaming chunk/done,
// provider change, bubble construction, scroll/input helpers, update_stats.
//
// Part of the partial-class split of AiChatScreen.cpp.

#include "core/events/EventBus.h"
#include "core/logging/Logger.h"
#include "core/session/ScreenStateManager.h"
#include "core/symbol/SymbolContext.h"
#include "mcp/McpService.h"
#include "screens/ai_chat/AiChatScreen.h"
#include "screens/ai_chat/ChatBubbleFactory.h"
#include "services/llm/LlmService.h"
#include "storage/repositories/ChatRepository.h"
#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPalette>
#include <QPointer>
#include <QRandomGenerator>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShowEvent>
#include <QSizePolicy>
#include <QTextStream>
#include <QTimer>
#include <QToolButton>
#include <QtConcurrent/QtConcurrent>

#include <cmath>
#include <memory>

namespace fincept::screens {

namespace fnt = fincept::ui::fonts;
namespace col = fincept::ui::colors;

// Bubble visuals live in ChatBubbleFactory — shared with AiChatBubble.
// Only the column max widths are local config: the tab gets more horizontal
// room than the floating bubble.
static constexpr int kUserColMaxWidth = 560;
static constexpr int kAiColMaxWidth = 680;

void AiChatScreen::on_typing_indicator_tick() {
    const QStringList states = {tr("AI is thinking"), tr("AI is thinking·"), tr("AI is thinking··"),
                                tr("AI is thinking···")};
    typing_step_ = (typing_step_ + 1) % states.size();
    typing_dots_lbl_->setText(states[typing_step_]);
}

void AiChatScreen::show_typing(bool show) {
    if (show) {
        typing_step_ = 0;
        typing_dots_lbl_->setText(tr("AI is thinking"));
        typing_indicator_->show();
        typing_timer_->start();
    } else {
        typing_timer_->stop();
        typing_indicator_->hide();
    }
}

// ── Session management ────────────────────────────────────────────────────────

void AiChatScreen::on_send() {
    if (streaming_)
        return;
    const QString raw_text = input_box_->toPlainText().trimmed();
    if (raw_text.isEmpty() && attached_file_path_.isEmpty())
        return;
    if (active_session_id_.isEmpty())
        create_new_session();
    if (active_session_id_.isEmpty()) {
        add_message_bubble("system", tr("Failed to create chat session. Please try again."));
        return;
    }
    if (!ai_chat::LlmService::instance().is_configured()) {
        add_message_bubble("system",
                           tr("No LLM provider configured. Go to Settings > LLM Configuration to set up a provider."));
        return;
    }

    // Build final text: prepend file contents if attached
    QString text = raw_text;
    if (!attached_file_path_.isEmpty()) {
        QFile f(attached_file_path_);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&f);
            in.setEncoding(QStringConverter::Utf8);
            QString file_content = in.read(32000); // cap at 32K chars
            f.close();
            QFileInfo fi(attached_file_path_);
            text = QString("[Attached file: %1]\n\n%2\n\n---\n\n%3").arg(fi.fileName(), file_content, raw_text);
        }
        // Reset attach state
        attached_file_path_.clear();
        if (attach_badge_)
            attach_badge_->setVisible(false);
        if (attach_btn_)
            attach_btn_->setProperty("active", false);
    }

    // Phase 7: prepend the linked-group symbol context so the LLM has the
    // security in scope. Only emitted on the wire — the visible bubble
    // still shows the user's raw_text. The user's bubble below is
    // unchanged, but `text` (the LLM payload) carries the prefix.
    if (linked_symbol_.is_valid()) {
        text = QString("[Context: %1 (%2)]\n\n%3")
                   .arg(linked_symbol_.symbol,
                        linked_symbol_.asset_class.isEmpty() ? QStringLiteral("equity") : linked_symbol_.asset_class,
                        text);
    }

    // Capture request‑time context for correct persistence
    const QString req_session = active_session_id_;
    const QString req_provider = ai_chat::LlmService::instance().active_provider();
    const QString req_model = ai_chat::LlmService::instance().active_model();
    // Store in member variables so on_streaming_done can use them
    pending_req_session_ = req_session;
    pending_req_provider_ = req_provider;
    pending_req_model_ = req_model;
    input_box_->clear();
    input_box_->setFixedHeight(44);
    set_input_enabled(false);
    streaming_ = true;
    show_welcome(false);
    // Display only raw_text in bubble (not full file dump)
    add_message_bubble("user", raw_text.isEmpty() ? tr("[File attached — see context]") : raw_text);
    total_messages_++;
    ChatRepository::instance().add_message(active_session_id_, "user", text,
                                           ai_chat::LlmService::instance().active_provider(),
                                           ai_chat::LlmService::instance().active_model());
    // Snapshot BEFORE appending the new turn: every request builder appends
    // `text` as the trailing user message itself, so including it in the history
    // too sent the same turn twice (and, on the Fincept flat-prompt path, printed
    // "User: …" twice in a row).
    std::vector<ai_chat::ConversationMessage> hist_copy = history_;
    history_.push_back({"user", text});

    // Show typing indicator while waiting for first chunk
    show_typing(true);
    scroll_to_bottom();

    // Fresh Thinking / Tools sections for this message (both cards are created
    // lazily by on_stream_chunk when their channel first produces a chunk).
    reset_thinking_state();
    reset_tools_state();

    const QString provider = ai_chat::LlmService::instance().active_provider();
    if (ai_chat::provider_supports_streaming(provider)) {
        QPointer<AiChatScreen> self = this;
        auto first_chunk = std::make_shared<bool>(true);
        ai_chat::LlmService::instance().chat_streaming(
            text, hist_copy,
            [self, first_chunk](const QString& chunk, bool done) {
                QMetaObject::invokeMethod(
                    qApp,
                    [self, chunk, done, first_chunk]() {
                        if (!self)
                            return;
                        // On first chunk: hide the typing indicator. The answer
                        // bubble and Thinking card are created lazily inside
                        // on_stream_chunk so a leading reasoning chunk doesn't
                        // spawn an empty answer bubble above the card.
                        if (*first_chunk && !chunk.isEmpty()) {
                            *first_chunk = false;
                            self->show_typing(false);
                        }
                        self->on_stream_chunk(chunk, done);
                    },
                    Qt::QueuedConnection);
            },
            ai_chat::LlmService::ToolPolicy::All, active_session_id_);
    } else {
        QPointer<AiChatScreen> self = this;
        (void)QtConcurrent::run([self, text, hist_copy]() {
            auto resp = ai_chat::LlmService::instance().chat(text, hist_copy);
            QMetaObject::invokeMethod(
                qApp,
                [self, resp]() {
                    if (self)
                        self->on_streaming_done(resp);
                },
                Qt::QueuedConnection);
        });
    }
}

void AiChatScreen::on_stream_chunk(const QString& chunk, bool done) {
    Q_UNUSED(done)

    // Chain-of-thought chunk → separate, collapsible Thinking section (kept out
    // of the answer bubble). Routed in-band via the think_stream_prefix sentinel.
    const QString think_prefix = ai_chat::think_stream_prefix();
    if (chunk.startsWith(think_prefix)) {
        append_thinking_chunk(chunk.mid(think_prefix.size()));
        scroll_to_bottom();
        return;
    }

    // Tool-progress chunk → its own collapsible Tools card, never the answer
    // bubble. Keeps plumbing (tool_list / tool_describe) out of the reply.
    const QString tool_prefix = ai_chat::tool_stream_prefix();
    if (chunk.startsWith(tool_prefix)) {
        append_tool_chunk(chunk.mid(tool_prefix.size()));
        scroll_to_bottom();
        return;
    }

    // Tool-call clear sentinel: reset bubble content (removes partial XML).
    if (chunk.startsWith("\x01__TOOL_CALL_CLEAR__")) {
        if (!streaming_bubble_)
            streaming_bubble_ = add_streaming_bubble();
        fincept::ai_chat::ChatBubbleFactory::replace_streaming_text(streaming_bubble_, tr("Calling tool..."));
        scroll_to_bottom();
        return;
    }

    if (chunk.isEmpty())
        return;

    // Lazily create the answer bubble on the first real answer chunk so it lands
    // below any Thinking card that arrived first.
    if (!streaming_bubble_)
        streaming_bubble_ = add_streaming_bubble();
    fincept::ai_chat::ChatBubbleFactory::append_streaming_chunk(streaming_bubble_, chunk);
    scroll_to_bottom();
}

void AiChatScreen::on_streaming_done(ai_chat::LlmResponse response) {
    streaming_ = false;
    show_typing(false);

    // Lock in the Thinking and Tools cards (swap their live headers for final
    // labels) and stop tracking them so the next message starts fresh ones.
    finalize_thinking_card();
    finalize_tools_card();

    // Ensure a bubble exists for any terminal state (success + content,
    // failure with an error message, or success-but-empty). Without this,
    // a kimi/openai response that arrives via the non-streaming fallback
    // with no per-chunk emission, or fails before any chunk lands, would
    // be silently dropped — the typing indicator hides and the input is
    // re-enabled but nothing is shown.
    const bool need_bubble = !streaming_bubble_ && ((response.success && !response.content.isEmpty()) ||
                                                    !response.success || response.content.isEmpty());
    if (need_bubble)
        streaming_bubble_ = add_streaming_bubble();

    set_input_enabled(true);

    if (!response.success) {
        if (streaming_bubble_) {
            const QString err =
                response.error.isEmpty() ? tr("Error: request failed") : tr("Error: %1").arg(response.error);
            fincept::ai_chat::ChatBubbleFactory::replace_streaming_text(streaming_bubble_, err);
        }
        LOG_WARN("AiChat", QString("LLM request failed: %1").arg(response.error));
        streaming_bubble_ = nullptr;
        return;
    }

    // Success but empty body — surface a hint instead of silently doing nothing.
    if (response.content.isEmpty()) {
        if (streaming_bubble_) {
            const QString hint = tr("(empty response — model returned no content)");
            fincept::ai_chat::ChatBubbleFactory::replace_streaming_text(streaming_bubble_, hint);
        }
        LOG_WARN("AiChat", "LLM returned success with empty content");
        streaming_bubble_ = nullptr;
        return;
    }

    const QString content = response.content;
    if (streaming_bubble_) {
        // Use accumulated streamed text if present, else fall back to the
        // response body (non-streaming path). finalize_streaming swaps the
        // label to MarkdownText and reveals the copy button.
        const QString acc = streaming_bubble_->property("acc").toString();
        const QString final_text = acc.isEmpty() ? content : acc;
        fincept::ai_chat::ChatBubbleFactory::finalize_streaming(streaming_bubble_, final_text);
        streaming_bubble_ = nullptr;
    }
    if (!content.isEmpty()) {
        history_.push_back({"assistant", content});
        total_messages_++;
        total_tokens_ += response.total_tokens;
        update_stats();
        // Use request‑time captured context to persist the assistant message
        ChatRepository::instance().add_message(pending_req_session_, "assistant", content, pending_req_provider_,
                                               pending_req_model_, response.total_tokens);
        // Clear pending request context to avoid accidental reuse
        pending_req_session_.clear();
        pending_req_provider_.clear();
        pending_req_model_.clear();
    }
    scroll_to_bottom();
    input_box_->setFocus();
}

void AiChatScreen::on_provider_changed() {
    update_stats();
}

// ── Message bubbles ───────────────────────────────────────────────────────────

void AiChatScreen::add_message_bubble(const QString& role, const QString& content, const QString& timestamp) {
    fincept::ai_chat::ChatBubbleFactory::Options opts;
    opts.role = role;
    opts.content = content;
    opts.timestamp_iso = timestamp;
    opts.show_footer = true;
    opts.user_col_max_width = kUserColMaxWidth;
    opts.ai_col_max_width = kAiColMaxWidth;
    auto b = fincept::ai_chat::ChatBubbleFactory::build(opts);
    messages_layout_->insertWidget(messages_layout_->count() - 1, b.row);
}

QLabel* AiChatScreen::add_streaming_bubble() {
    fincept::ai_chat::ChatBubbleFactory::Options opts;
    opts.role = "assistant";
    opts.show_footer = true;
    opts.user_col_max_width = kUserColMaxWidth;
    opts.ai_col_max_width = kAiColMaxWidth;
    auto b = fincept::ai_chat::ChatBubbleFactory::build_streaming(opts);
    messages_layout_->insertWidget(messages_layout_->count() - 1, b.row);
    return b.body;
}

// ── Thinking (chain-of-thought) card ────────────────────────────────────────────
// Reasoning models stream their thoughts on a separate channel (see
// think_stream_prefix()). We collect them into a collapsed dropdown shown ABOVE
// the answer bubble so the answer itself stays clean and unmixed.

void AiChatScreen::create_thinking_card() {
    if (thinking_card_)
        return;

    auto* card = new QWidget;
    card->setMaximumWidth(kAiColMaxWidth);
    card->setStyleSheet(QString("background:%1;border:1px solid %2;").arg(col::BG_SURFACE(), col::BORDER_DIM()));
    auto* vl = new QVBoxLayout(card);
    vl->setContentsMargins(10, 6, 10, 6);
    vl->setSpacing(4);

    // Collapsed by default — header toggles the body. The leading chevron (▸/▾)
    // reflects expanded state; the body holds the dimmed, selectable reasoning.
    auto* header = new QPushButton(
        QChar(0x25B8) + (QStringLiteral("  ") + QString::fromUtf8("\xF0\x9F\x92\xAD") + QStringLiteral(" ")) +
        tr("Thinking…"));
    header->setCursor(Qt::PointingHandCursor);
    header->setStyleSheet(QString("QPushButton{background:transparent;color:%1;border:none;"
                                  "font-size:%2px;font-weight:600;text-align:left;padding:0;}"
                                  "QPushButton:hover{color:%3;}")
                              .arg(col::TEXT_TERTIARY())
                              .arg(fnt::SMALL)
                              .arg(col::TEXT_SECONDARY()));
    vl->addWidget(header);

    auto* body = new QLabel;
    body->setWordWrap(true);
    body->setTextInteractionFlags(Qt::TextSelectableByMouse);
    body->setVisible(false);
    body->setStyleSheet(QString("QLabel{color:%1;font-size:%2px;font-style:italic;background:transparent;}")
                            .arg(col::TEXT_DIM())
                            .arg(fnt::SMALL));
    vl->addWidget(body);

    // Capture raw pointers (not the QPointer members, which get nulled when the
    // stream finishes) so the finished card stays toggleable.
    QObject::connect(header, &QPushButton::clicked, card, [header, body]() {
        const bool show = !body->isVisible();
        body->setVisible(show);
        QString t = header->text();
        if (!t.isEmpty())
            t.replace(0, 1, show ? QChar(0x25BE) : QChar(0x25B8));
        header->setText(t);
    });

    // Insert just before the trailing stretch so it sits above the answer bubble
    // (which is created lazily on the first answer chunk and lands below it).
    messages_layout_->insertWidget(messages_layout_->count() - 1, card, 0, Qt::AlignLeft);
    thinking_card_ = card;
    thinking_header_ = header;
    thinking_body_ = body;
}

void AiChatScreen::append_thinking_chunk(const QString& text) {
    if (text.isEmpty())
        return;
    show_typing(false); // the card now signals activity
    if (!thinking_card_)
        create_thinking_card();
    thinking_text_ += text;
    if (thinking_body_)
        thinking_body_->setText(thinking_text_);
}

void AiChatScreen::finalize_thinking_card() {
    if (thinking_header_) {
        const bool expanded = thinking_body_ && thinking_body_->isVisible();
        const QChar chev = expanded ? QChar(0x25BE) : QChar(0x25B8);
        thinking_header_->setText(chev +
                                  (QStringLiteral("  ") + QString::fromUtf8("\xF0\x9F\x92\xAD") + QStringLiteral(" ")) +
                                  tr("Thoughts"));
    }
    reset_thinking_state();
}

void AiChatScreen::reset_thinking_state() {
    // Stops tracking the current card (the widget itself remains in the
    // transcript); the next message creates a fresh one.
    thinking_card_ = nullptr;
    thinking_header_ = nullptr;
    thinking_body_ = nullptr;
    thinking_text_.clear();
}

// ── Tools card ──────────────────────────────────────────────────────────────────
// Same shape as the Thinking card, different channel (tool_stream_prefix()).
// Starts EXPANDED — unlike chain-of-thought, tool progress is the only signal the
// user has that a long multi-round turn is advancing — and collapses on finish
// into a one-line "N tools used" summary.

void AiChatScreen::create_tools_card() {
    if (tools_card_)
        return;

    auto* card = new QWidget;
    card->setMaximumWidth(kAiColMaxWidth);
    card->setStyleSheet(QString("background:%1;border:1px solid %2;").arg(col::BG_SURFACE(), col::BORDER_DIM()));
    auto* vl = new QVBoxLayout(card);
    vl->setContentsMargins(10, 6, 10, 6);
    vl->setSpacing(4);

    auto* header = new QPushButton(QChar(0x25BE) + (QStringLiteral("  ") + QString::fromUtf8("\xF0\x9F\x94\xA7") +
                                                    QStringLiteral(" ")) +
                                   tr("Using tools…"));
    header->setCursor(Qt::PointingHandCursor);
    header->setStyleSheet(QString("QPushButton{background:transparent;color:%1;border:none;"
                                  "font-size:%2px;font-weight:600;text-align:left;padding:0;}"
                                  "QPushButton:hover{color:%3;}")
                              .arg(col::TEXT_TERTIARY())
                              .arg(fnt::SMALL)
                              .arg(col::TEXT_SECONDARY()));
    vl->addWidget(header);

    auto* body = new QLabel;
    body->setWordWrap(true);
    body->setTextInteractionFlags(Qt::TextSelectableByMouse);
    body->setVisible(true); // expanded while the turn runs
    body->setStyleSheet(QString("QLabel{color:%1;font-size:%2px;background:transparent;}")
                            .arg(col::TEXT_DIM())
                            .arg(fnt::SMALL));
    vl->addWidget(body);

    // Raw pointers, not the QPointer members — those are nulled when the stream
    // finishes, and the finished card must stay toggleable.
    QObject::connect(header, &QPushButton::clicked, card, [header, body]() {
        const bool show = !body->isVisible();
        body->setVisible(show);
        QString t = header->text();
        if (!t.isEmpty())
            t.replace(0, 1, show ? QChar(0x25BE) : QChar(0x25B8));
        header->setText(t);
    });

    messages_layout_->insertWidget(messages_layout_->count() - 1, card, 0, Qt::AlignLeft);
    tools_card_ = card;
    tools_header_ = header;
    tools_body_ = body;
}

void AiChatScreen::append_tool_chunk(const QString& text) {
    if (text.isEmpty())
        return;
    show_typing(false); // the card now signals activity
    if (!tools_card_)
        create_tools_card();

    for (const QString& raw : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QString line = raw.trimmed();
        if (line.isEmpty())
            continue;
        ++tools_count_;

        // Collapse a consecutive run of the same tool into "name ×N". Writing a
        // spreadsheet is ~45 back-to-back set_excel_cell calls; listing each is a
        // wall of noise that buries the steps that actually differ.
        const QString base = line.section(QStringLiteral(" · "), 0, 0);
        if (base == tools_run_base_ && !tools_lines_.isEmpty()) {
            ++tools_run_count_;
            tools_lines_.last() = base + QStringLiteral(" ×") + QString::number(tools_run_count_);
        } else {
            tools_run_base_ = base;
            tools_run_count_ = 1;
            tools_lines_.append(line);
        }
    }

    tools_text_ = QStringLiteral("• ") + tools_lines_.join(QStringLiteral("\n• "));
    if (tools_body_)
        tools_body_->setText(tools_text_);
}

void AiChatScreen::finalize_tools_card() {
    if (tools_header_) {
        // Collapse once the answer is in — the detail stays one click away.
        if (tools_body_)
            tools_body_->setVisible(false);
        tools_header_->setText(QChar(0x25B8) +
                               (QStringLiteral("  ") + QString::fromUtf8("\xF0\x9F\x94\xA7") + QStringLiteral(" ")) +
                               tr("%n tool step(s)", "", tools_count_));
    }
    reset_tools_state();
}

void AiChatScreen::reset_tools_state() {
    tools_card_ = nullptr;
    tools_header_ = nullptr;
    tools_body_ = nullptr;
    tools_text_.clear();
    tools_lines_.clear();
    tools_run_base_.clear();
    tools_run_count_ = 0;
    tools_count_ = 0;
}

// ── Helpers ───────────────────────────────────────────────────────────────────

void AiChatScreen::clear_messages() {
    streaming_bubble_.clear();
    reset_thinking_state(); // card widgets are deleted by the loop below
    reset_tools_state();
    // Skip the welcome panel — it is a permanent fixture reused across sessions
    // (it is only shown/hidden). Everything else is a message row.
    for (int i = messages_layout_->count() - 1; i >= 0; --i) {
        QLayoutItem* item = messages_layout_->itemAt(i);
        QWidget* w = item ? item->widget() : nullptr;
        if (!w || w == welcome_panel_)
            continue; // trailing stretch has no widget
        QLayoutItem* taken = messages_layout_->takeAt(i);
        w->deleteLater();
        delete taken;
    }
}

void AiChatScreen::scroll_to_bottom() {
    // Re-arm following. The rangeChanged handler installed in build_chat_area()
    // does the real work — it fires once the layout has settled and therefore
    // knows the true maximum. This deferred nudge covers the case where content
    // changed without altering the scroll range.
    stick_to_bottom_ = true;

    // Debounce: a single scroll lands at the end of the current event-loop batch.
    // Without this, a 100-chunk stream queued 100 independent 50ms timers, each
    // racing to setValue() and starving paint events.
    if (scroll_pending_)
        return;
    scroll_pending_ = true;
    QTimer::singleShot(0, this, [this]() {
        scroll_pending_ = false;
        if (!scroll_area_ || !scroll_area_->verticalScrollBar())
            return;
        auto* sb = scroll_area_->verticalScrollBar();
        programmatic_scroll_ = true;
        sb->setValue(sb->maximum());
        programmatic_scroll_ = false;
    });
}

void AiChatScreen::set_input_enabled(bool enabled) {
    input_box_->setEnabled(enabled);
    send_btn_->setEnabled(enabled);
    new_btn_->setEnabled(enabled);
    search_edit_->setEnabled(enabled);
    session_list_->setEnabled(enabled);
    delete_btn_->setEnabled(enabled && !active_session_id_.isEmpty());
    rename_btn_->setEnabled(enabled && !active_session_id_.isEmpty());
    send_btn_->setText(enabled ? tr("Send") : "···");

    // Status dot + label
    const QString status_color = enabled ? col::POSITIVE() : col::AMBER();
    hdr_status_dot_->setStyleSheet(QString("background:%1;border-radius:0px;").arg(status_color));
    hdr_status_lbl_->setText(enabled ? tr("Ready") : tr("Streaming"));
    hdr_status_lbl_->setStyleSheet(
        QString("color:%1;font-size:%2px;font-weight:600;").arg(status_color).arg(fnt::SMALL));
}

void AiChatScreen::update_stats() {
    // Header session name
    if (!active_session_title_.isEmpty())
        hdr_session_lbl_->setText(active_session_title_);
    else if (!active_session_id_.isEmpty())
        hdr_session_lbl_->setText(active_session_id_.left(8));
    else
        hdr_session_lbl_->setText(tr("New Conversation"));

    // Token count in header
    if (total_tokens_ > 0) {
        hdr_tokens_lbl_->setText(total_tokens_ < 1000 ? tr("%1 tokens").arg(total_tokens_)
                                                      : tr("%1k tokens").arg(total_tokens_ / 1000.0, 0, 'f', 1));
    } else {
        hdr_tokens_lbl_->clear();
    }

    auto& llm = ai_chat::LlmService::instance();
    if (llm.is_configured()) {
        const QString provider_raw = llm.active_provider();
        const QString prov_display = provider_raw.toUpper();
        const QString model_raw = llm.active_model();
        QString model_short = model_raw;
        if (model_short.length() > 24)
            model_short = model_short.left(22) + "..";

        // Sidebar footer
        provider_lbl_->setText(prov_display);
        provider_lbl_->setStyleSheet(
            QString("color:%1;font-size:%2px;font-weight:600;").arg(col::AMBER()).arg(fnt::SMALL));
        model_lbl_->setText(model_short);
        model_lbl_->setToolTip(model_raw);
        model_lbl_->setStyleSheet(QString("color:%1;font-size:%2px;").arg(col::TEXT_SECONDARY()).arg(fnt::TINY));

        // Header model pill — show "Provider / Model" for clarity
        hdr_model_lbl_->setText(provider_raw.left(1).toUpper() + provider_raw.mid(1) + " / " + model_short);
        hdr_model_lbl_->setToolTip(
            tr("Provider: %1\nModel: %2\n\nChange in Settings > LLM Configuration").arg(prov_display, model_raw));
    } else {
        provider_lbl_->setText(tr("No provider"));
        provider_lbl_->setStyleSheet(
            QString("color:%1;font-size:%2px;font-weight:600;").arg(col::NEGATIVE()).arg(fnt::SMALL));
        model_lbl_->setText(tr("Configure in Settings"));
        model_lbl_->setStyleSheet(QString("color:%1;font-size:%2px;").arg(col::TEXT_DIM()).arg(fnt::TINY));
        hdr_model_lbl_->setText(tr("No model"));
    }
}

} // namespace fincept::screens
