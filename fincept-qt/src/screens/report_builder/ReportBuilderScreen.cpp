// ReportBuilderScreen.cpp — view onto ReportBuilderService.
//
// Core lifecycle: ctor, build_ui via build_toolbar, show/hide events,
// save_state/restore_state. Other concerns:
//   - ReportBuilderScreen_Components.cpp — panel toggles + component CRUD + service events
//   - ReportBuilderScreen_Dialogs.cpp    — Recent/Template/Theme/Metadata + file actions
//
// All document state (components, metadata, theme, undo, autosave, recent
// files) lives in the service. The screen renders that state, forwards user
// gestures to the service, and observes service signals so LLM/MCP-driven
// mutations appear live on the canvas.

#include "screens/report_builder/ReportBuilderScreen.h"

#include "core/session/ScreenStateManager.h"
#include "datahub/DataHub.h"
#include "datahub/DataHubMetaTypes.h"
#include "services/file_manager/FileManagerService.h"
#include "services/markets/MarketDataService.h"
#include "services/report_builder/ReportBuilderService.h"
#include "ui/theme/Theme.h"

#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPointer>
#include <QPrintPreviewDialog>
#include <QPrinter>
#include <QPushButton>
#include <QShortcut>
#include <QTextDocument>
#include <QTextFrame>
#include <QVBoxLayout>

namespace fincept::screens {

namespace rep = ::fincept::report;
using Service = ::fincept::services::ReportBuilderService;

// Tracks the most recently constructed Report Builder screen so MCP tools
// can find a live instance without walking the widget tree. Cleared in the
// destructor. QPointer auto-nulls if Qt deletes the widget another way.
static QPointer<ReportBuilderScreen> s_current_screen;

ReportBuilderScreen* ReportBuilderScreen::current() {
    return s_current_screen.data();
}

ReportBuilderScreen::ReportBuilderScreen(QWidget* parent) : QWidget(parent) {
    s_current_screen = this;
    setStyleSheet(QString("background: %1;").arg(ui::colors::DARK()));

    auto* vl = new QVBoxLayout(this);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->setSpacing(0);

    vl->addWidget(build_toolbar());

    auto* sep = new QFrame;
    sep->setFixedHeight(1);
    sep->setStyleSheet(QString("background: %1; border: none;").arg(ui::colors::BORDER()));
    vl->addWidget(sep);

    splitter_ = new QSplitter(Qt::Horizontal);
    splitter_->setHandleWidth(1);
    splitter_->setStyleSheet(QString("QSplitter::handle { background: %1; }"
                                     "QSplitter::handle:hover { background: %2; }")
                                 .arg(ui::colors::BORDER_DIM(), ui::colors::BORDER_BRIGHT()));

    comp_toolbar_ = new ComponentToolbar;
    canvas_ = new DocumentCanvas;
    properties_ = new PropertiesPanel;

    splitter_->addWidget(comp_toolbar_);
    splitter_->addWidget(canvas_);
    splitter_->addWidget(properties_);
    splitter_->setSizes({240, 700, 280});

    vl->addWidget(splitter_, 1);

    // ── Component toolbar wiring ─────────────────────────────────────────
    connect(comp_toolbar_, &ComponentToolbar::new_report_requested, this, &ReportBuilderScreen::on_new);
    connect(comp_toolbar_, &ComponentToolbar::open_report_requested, this, &ReportBuilderScreen::on_open);
    connect(comp_toolbar_, &ComponentToolbar::recent_reports_requested, this, &ReportBuilderScreen::show_recent_dialog);
    connect(comp_toolbar_, &ComponentToolbar::templates_requested, this, &ReportBuilderScreen::show_template_dialog);
    connect(comp_toolbar_, &ComponentToolbar::theme_requested, this, &ReportBuilderScreen::show_theme_dialog);
    connect(comp_toolbar_, &ComponentToolbar::metadata_requested, this, &ReportBuilderScreen::show_metadata_dialog);

    connect(comp_toolbar_, &ComponentToolbar::font_changed, this,
            [this](const QString& family, int size, bool bold, bool italic) {
                QTextCharFormat fmt;
                fmt.setFontFamilies({family});
                fmt.setFontPointSize(size);
                fmt.setFontWeight(bold ? QFont::Bold : QFont::Normal);
                fmt.setFontItalic(italic);
                if (canvas_->text_edit())
                    canvas_->text_edit()->mergeCurrentCharFormat(fmt);
            });

    connect(comp_toolbar_, &ComponentToolbar::add_component, this, &ReportBuilderScreen::add_component);
    connect(comp_toolbar_, &ComponentToolbar::structure_selected, this, &ReportBuilderScreen::select_component);
    connect(comp_toolbar_, &ComponentToolbar::delete_item, this, &ReportBuilderScreen::remove_component_at);
    connect(comp_toolbar_, &ComponentToolbar::duplicate, this, &ReportBuilderScreen::duplicate_at);
    connect(comp_toolbar_, &ComponentToolbar::move_up, this, &ReportBuilderScreen::move_up_at);
    connect(comp_toolbar_, &ComponentToolbar::move_down, this, &ReportBuilderScreen::move_down_at);

    // ── Properties panel wiring ───────────────────────────────────────────
    connect(properties_, &PropertiesPanel::content_changed, this, [](int idx, const QString& content) {
        const auto comps = Service::instance().components();
        if (idx < 0 || idx >= comps.size())
            return;
        Service::instance().update_component(comps[idx].id, content, comps[idx].config);
    });

    connect(properties_, &PropertiesPanel::config_changed, this,
            [this](int idx, const QString& key, const QString& val) {
                auto& svc = Service::instance();
                auto comps = svc.components();
                if (idx < 0 || idx >= comps.size())
                    return;
                const int comp_id = comps[idx].id;
                const QString comp_type = comps[idx].type;

                // ── stats_block: fetch real fundamentals via get_info ─────
                if (comp_type == "stats_block" && key == "fetch_ticker") {
                    QString sym = val;
                    auto loading_cfg = comps[idx].config;
                    loading_cfg["fetch_ticker"] = "";
                    loading_cfg["data"] = "(Loading " + sym + "...)";
                    svc.update_component(comp_id, comps[idx].content, loading_cfg);

                    QPointer<ReportBuilderScreen> self = this;
                    fincept::services::MarketDataService::instance().fetch_info(
                        sym, [self, comp_id, sym](bool ok, fincept::services::InfoData info) {
                            if (!self)
                                return;
                            QMetaObject::invokeMethod(
                                self,
                                [self, comp_id, sym, ok, info]() {
                                    if (!self)
                                        return;
                                    auto& s = Service::instance();
                                    int idx2 = s.index_of(comp_id);
                                    if (idx2 < 0)
                                        return;
                                    auto comps2 = s.components();
                                    QStringList lines;
                                    auto fmt_dbl = [](double v, int dec = 2) -> QString {
                                        return v != 0 ? QString::number(v, 'f', dec) : "—";
                                    };
                                    auto fmt_pct = [](double v) -> QString {
                                        return v != 0 ? QString::number(v * 100, 'f', 2) + "%" : "—";
                                    };
                                    auto fmt_mcap = [](double v) -> QString {
                                        if (v <= 0)
                                            return QString("—");
                                        if (v >= 1e12)
                                            return QString::number(v / 1e12, 'f', 2) + "T";
                                        if (v >= 1e9)
                                            return QString::number(v / 1e9, 'f', 2) + "B";
                                        if (v >= 1e6)
                                            return QString::number(v / 1e6, 'f', 2) + "M";
                                        return QString::number(v, 'f', 0);
                                    };
                                    auto fmt_vol = [](double v) -> QString {
                                        if (v <= 0)
                                            return QString("—");
                                        if (v >= 1e9)
                                            return QString::number(v / 1e9, 'f', 2) + "B";
                                        if (v >= 1e6)
                                            return QString::number(v / 1e6, 'f', 2) + "M";
                                        if (v >= 1e3)
                                            return QString::number(v / 1e3, 'f', 1) + "K";
                                        return QString::number(v, 'f', 0);
                                    };

                                    if (!ok) {
                                        lines << "Error: Could not fetch data for " + sym;
                                    } else {
                                        if (!info.name.isEmpty())
                                            lines << "Company: " + info.name;
                                        if (!info.sector.isEmpty())
                                            lines << "Sector: " + info.sector;
                                        if (!info.industry.isEmpty())
                                            lines << "Industry: " + info.industry;
                                        if (!info.country.isEmpty())
                                            lines << "Country: " + info.country;
                                        lines << "Market Cap: " + fmt_mcap(info.market_cap);
                                        lines << "P/E Ratio: " + fmt_dbl(info.pe_ratio);
                                        lines << "Forward P/E: " + fmt_dbl(info.forward_pe);
                                        lines << "Price/Book: " + fmt_dbl(info.price_to_book);
                                        lines << "Dividend Yield: " + fmt_pct(info.dividend_yield);
                                        lines << "Beta: " + fmt_dbl(info.beta);
                                        lines << "52W High: " + fmt_dbl(info.week52_high);
                                        lines << "52W Low: " + fmt_dbl(info.week52_low);
                                        lines << "Avg Volume: " + fmt_vol(info.avg_volume);
                                        lines << "ROE: " + fmt_pct(info.roe);
                                        lines << "Profit Margin: " + fmt_pct(info.profit_margin);
                                        if (info.debt_to_equity != 0)
                                            lines << "Debt/Equity: " + fmt_dbl(info.debt_to_equity);
                                        if (info.current_ratio != 0)
                                            lines << "Current Ratio: " + fmt_dbl(info.current_ratio);
                                        if (info.eps != 0)
                                            lines << "Rev/Share: " + fmt_dbl(info.eps);
                                    }
                                    auto cfg = comps2[idx2].config;
                                    cfg["data"] = lines.join("\n");
                                    cfg["fetch_ticker"] = "";
                                    s.update_component(comp_id, comps2[idx2].content, cfg);
                                },
                                Qt::QueuedConnection);
                        });
                    return;
                }

                // ── chart: hub-driven 6mo/1d history fetch ─────────────────
                if (comp_type == "chart" && key == "fetch_history") {
                    QString sym = val;
                    auto cfg = comps[idx].config;
                    cfg["fetch_history"] = "";
                    cfg["data"] = "";
                    svc.update_component(comp_id, comps[idx].content, cfg);

                    const QString topic = QStringLiteral("market:history:") + sym + QStringLiteral(":6mo:1d");
                    auto& hub = fincept::datahub::DataHub::instance();
                    QPointer<ReportBuilderScreen> self = this;

                    auto apply = [self, comp_id, sym](const QVector<fincept::services::HistoryPoint>& history) {
                        if (!self)
                            return;
                        auto& s = Service::instance();
                        int idx2 = s.index_of(comp_id);
                        if (idx2 < 0)
                            return;
                        auto comps2 = s.components();
                        if (history.isEmpty()) {
                            auto cfg2 = comps2[idx2].config;
                            cfg2["data"] = "";
                            s.update_component(comp_id, comps2[idx2].content, cfg2);
                            return;
                        }
                        QStringList pts, lbls;
                        int step = qMax(1, history.size() / 20);
                        for (int i = 0; i < history.size(); i += step) {
                            const auto& pt = history[i];
                            pts << QString::number(pt.close, 'f', 2);
                            QDateTime dt = QDateTime::fromSecsSinceEpoch(pt.timestamp);
                            lbls << dt.toString("MMM d");
                        }
                        if ((history.size() - 1) % step != 0) {
                            pts << QString::number(history.last().close, 'f', 2);
                            lbls << QDateTime::fromSecsSinceEpoch(history.last().timestamp).toString("MMM d");
                        }
                        auto cfg2 = comps2[idx2].config;
                        cfg2["data"] = pts.join(",");
                        cfg2["labels"] = lbls.join(",");
                        if (cfg2.value("title").isEmpty() || cfg2.value("title") == "Chart")
                            cfg2["title"] = sym + " (6mo)";
                        s.update_component(comp_id, comps2[idx2].content, cfg2);
                    };

                    QVariant cached = hub.peek(topic);
                    if (cached.isValid() && cached.canConvert<QVector<fincept::services::HistoryPoint>>()) {
                        apply(cached.value<QVector<fincept::services::HistoryPoint>>());
                        return;
                    }

                    hub.subscribe<QVector<fincept::services::HistoryPoint>>(
                        this, topic, [self, topic, apply](const QVector<fincept::services::HistoryPoint>& history) {
                            if (!self)
                                return;
                            apply(history);
                            fincept::datahub::DataHub::instance().unsubscribe(self, topic);
                        });
                    hub.request(topic, /*force=*/true);
                    return;
                }

                // ── market_data: hub-driven quote fetch ───────────────────
                if (comp_type == "market_data" && key == "status" && val == "loading") {
                    auto cfg = comps[idx].config;
                    QString sym = cfg.value("symbol", "");
                    if (sym.isEmpty())
                        return;
                    cfg["status"] = "loading";
                    svc.update_component(comp_id, comps[idx].content, cfg);

                    const QString topic = QStringLiteral("market:quote:") + sym;
                    auto& hub = fincept::datahub::DataHub::instance();
                    QPointer<ReportBuilderScreen> self = this;

                    auto apply = [self, comp_id](const fincept::services::QuoteData& q) {
                        if (!self)
                            return;
                        auto& s = Service::instance();
                        int idx2 = s.index_of(comp_id);
                        if (idx2 < 0)
                            return;
                        auto comps2 = s.components();
                        auto cfg2 = comps2[idx2].config;
                        cfg2["price"] = QString::number(q.price, 'f', 2);
                        cfg2["change"] = QString::number(q.change, 'f', 2);
                        cfg2["change_pct"] = QString::number(q.change_pct, 'f', 2);
                        cfg2["name"] = q.name;
                        cfg2["high"] = q.high > 0 ? QString::number(q.high, 'f', 2) : "";
                        cfg2["low"] = q.low > 0 ? QString::number(q.low, 'f', 2) : "";
                        cfg2["volume"] = q.volume > 0 ? QString::number(q.volume, 'f', 0) : "";
                        cfg2["status"] = "ok";
                        s.update_component(comp_id, comps2[idx2].content, cfg2);
                    };

                    QVariant cached = hub.peek(topic);
                    if (cached.isValid() && cached.canConvert<fincept::services::QuoteData>()) {
                        apply(cached.value<fincept::services::QuoteData>());
                        return;
                    }
                    hub.subscribe<fincept::services::QuoteData>(
                        this, topic, [self, topic, apply](const fincept::services::QuoteData& q) {
                            if (!self)
                                return;
                            apply(q);
                            fincept::datahub::DataHub::instance().unsubscribe(self, topic);
                        });
                    hub.request(topic, /*force=*/true);
                    return;
                }

                // Generic single-key config patch.
                QMap<QString, QString> patch;
                patch[key] = val;
                svc.patch_component(comp_id, nullptr, patch);
            });

    connect(properties_, &PropertiesPanel::duplicate_requested, this, &ReportBuilderScreen::duplicate_at);
    connect(properties_, &PropertiesPanel::delete_requested, this, &ReportBuilderScreen::remove_component_at);

    // ── Canvas drag-and-drop ──────────────────────────────────────────────
    connect(canvas_, &DocumentCanvas::image_dropped, this, [this](const QString& path) {
        auto& svc = Service::instance();
        auto comps = svc.components();
        // If selected component is an image, update its path.
        int sel_idx = (selected_id_ > 0) ? svc.index_of(selected_id_) : -1;
        if (sel_idx >= 0 && comps[sel_idx].type == "image") {
            auto cfg = comps[sel_idx].config;
            cfg["path"] = path;
            cfg["clipboard"] = "";
            svc.update_component(comps[sel_idx].id, comps[sel_idx].content, cfg);
        } else {
            rep::ReportComponent comp;
            comp.type = "image";
            comp.config["path"] = path;
            comp.config["width"] = "400";
            comp.config["align"] = "left";
            int at = (sel_idx >= 0) ? sel_idx + 1 : -1;
            int new_id = svc.add_component(comp, at);
            selected_id_ = new_id;
            select_component(svc.index_of(new_id));
        }
    });

    // ── Service signals → screen ──────────────────────────────────────────
    connect(&Service::instance(), &Service::component_added, this, &ReportBuilderScreen::on_component_added);
    connect(&Service::instance(), &Service::component_updated, this, &ReportBuilderScreen::on_component_updated);
    connect(&Service::instance(), &Service::component_removed, this, &ReportBuilderScreen::on_component_removed);
    connect(&Service::instance(), &Service::component_moved, this, &ReportBuilderScreen::on_component_moved);
    connect(&Service::instance(), &Service::metadata_changed, this, &ReportBuilderScreen::on_metadata_changed);
    connect(&Service::instance(), &Service::theme_changed, this, &ReportBuilderScreen::on_theme_changed);
    connect(&Service::instance(), &Service::document_changed, this, &ReportBuilderScreen::on_document_changed);
    connect(&Service::instance(), &Service::document_loaded, this, &ReportBuilderScreen::on_document_loaded);

    // First render — pull whatever the service already holds (autosave restore,
    // earlier LLM mutations while the screen wasn't open, etc.).
    rebind_from_service();
}

ReportBuilderScreen::~ReportBuilderScreen() {
    if (s_current_screen == this)
        s_current_screen.clear();
}

// ── Top toolbar ──────────────────────────────────────────────────────────────

QWidget* ReportBuilderScreen::build_toolbar() {
    auto* bar = new QWidget(this);
    bar->setFixedHeight(36);
    bar->setStyleSheet(QString("background: %1;").arg(ui::colors::HEADER()));

    auto* hl = new QHBoxLayout(bar);
    hl->setContentsMargins(8, 0, 8, 0);
    hl->setSpacing(6);

    auto make_panel_toggle = [&](const char* glyph, const QString& tooltip, const char* shortcut) {
        auto* b = new QPushButton(glyph);
        b->setFixedSize(26, 26);
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(tooltip);
        if (shortcut)
            b->setShortcut(QKeySequence(shortcut));
        b->setStyleSheet(QString("QPushButton{background:transparent;color:%1;border:1px solid %2;"
                                 "border-radius:0px;font-size:14px;font-weight:700;padding:0;}"
                                 "QPushButton:hover{background:%3;color:%4;border-color:%4;}")
                             .arg(ui::colors::TEXT_SECONDARY(), ui::colors::BORDER_DIM(), ui::colors::BG_HOVER(),
                                  ui::colors::AMBER()));
        hl->addWidget(b);
        return b;
    };

    left_toggle_btn_ = make_panel_toggle("‹", tr("Collapse components panel  (Ctrl+B)"), "Ctrl+B");
    connect(left_toggle_btn_, &QPushButton::clicked, this, &ReportBuilderScreen::on_toggle_left);

    toolbar_title_ = new QLabel(tr("REPORT BUILDER"));
    toolbar_title_->setStyleSheet(
        QString("color: %1; font-size: 14px; font-weight: bold; background: transparent;").arg(ui::colors::AMBER()));
    hl->addWidget(toolbar_title_);

    hl->addStretch();

    auto make_btn = [&](const QString& text) {
        auto* b = new QPushButton(text);
        b->setFixedHeight(26);
        hl->addWidget(b);
        return b;
    };

    auto* undo_stack = Service::instance().undo_stack();

    undo_btn_ = make_btn(tr("Undo"));
    undo_btn_->setEnabled(undo_stack->canUndo());
    connect(undo_stack, &QUndoStack::canUndoChanged, undo_btn_, &QPushButton::setEnabled);
    connect(undo_btn_, &QPushButton::clicked, undo_stack, &QUndoStack::undo);

    redo_btn_ = make_btn(tr("Redo"));
    redo_btn_->setEnabled(undo_stack->canRedo());
    connect(undo_stack, &QUndoStack::canRedoChanged, redo_btn_, &QPushButton::setEnabled);
    connect(redo_btn_, &QPushButton::clicked, undo_stack, &QUndoStack::redo);

    make_btn("|")->setEnabled(false);

    open_btn_ = make_btn(tr("Open"));
    connect(open_btn_, &QPushButton::clicked, this, &ReportBuilderScreen::on_open);

    save_btn_ = make_btn(tr("Save"));
    connect(save_btn_, &QPushButton::clicked, this, &ReportBuilderScreen::on_save);

    pdf_btn_ = make_btn(tr("Export PDF"));
    pdf_btn_->setStyleSheet(QString("QPushButton { color: %1; font-weight: bold; }").arg(ui::colors::AMBER()));
    connect(pdf_btn_, &QPushButton::clicked, this, &ReportBuilderScreen::on_export_pdf);

    preview_btn_ = make_btn(tr("Preview"));
    connect(preview_btn_, &QPushButton::clicked, this, &ReportBuilderScreen::on_preview);

    right_toggle_btn_ = make_panel_toggle("›", tr("Collapse properties panel  (Ctrl+Shift+B)"), "Ctrl+Shift+B");
    connect(right_toggle_btn_, &QPushButton::clicked, this, &ReportBuilderScreen::on_toggle_right);

    // ── Standard document shortcuts ───────────────────────────────────────────
    // Undo/Redo were toolbar-only; Save/Open/New/Print had no key bindings at
    // all, which is surprising for a document editor.
    auto add_sc = [this](QKeySequence::StandardKey key, auto fn) {
        auto* sc = new QShortcut(QKeySequence(key), this);
        sc->setContext(Qt::WidgetWithChildrenShortcut);
        connect(sc, &QShortcut::activated, this, fn);
    };
    add_sc(QKeySequence::Save, [this]() { on_save(); });
    add_sc(QKeySequence::Open, [this]() { on_open(); });
    add_sc(QKeySequence::New, [this]() { on_new(); });
    add_sc(QKeySequence::Print, [this]() { on_preview(); });
    {
        auto* undo_sc = new QShortcut(QKeySequence(QKeySequence::Undo), this);
        undo_sc->setContext(Qt::WidgetWithChildrenShortcut);
        connect(undo_sc, &QShortcut::activated, undo_stack, &QUndoStack::undo);
        auto* redo_sc = new QShortcut(QKeySequence(QKeySequence::Redo), this);
        redo_sc->setContext(Qt::WidgetWithChildrenShortcut);
        connect(redo_sc, &QShortcut::activated, undo_stack, &QUndoStack::redo);
    }

    // ── Accessibility ─────────────────────────────────────────────────────────
    undo_btn_->setAccessibleName(tr("Undo last change"));
    redo_btn_->setAccessibleName(tr("Redo last change"));
    open_btn_->setAccessibleName(tr("Open a saved report"));
    save_btn_->setAccessibleName(tr("Save this report"));
    pdf_btn_->setAccessibleName(tr("Export this report as PDF"));
    preview_btn_->setAccessibleName(tr("Print preview"));
    save_btn_->setToolTip(tr("Save  (Ctrl+S)"));
    open_btn_->setToolTip(tr("Open  (Ctrl+O)"));
    preview_btn_->setToolTip(tr("Preview  (Ctrl+P)"));

    return bar;
}

// ── Side-panel collapse ──────────────────────────────────────────────────────

void ReportBuilderScreen::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    // The service runs autosave continuously. We just rebind from current
    // service state in case mutations happened while the screen was hidden.
    rebind_from_service();
}

void ReportBuilderScreen::hideEvent(QHideEvent* e) {
    QWidget::hideEvent(e);
}

// ── Live language switch ─────────────────────────────────────────────────────

void ReportBuilderScreen::changeEvent(QEvent* e) {
    if (e->type() == QEvent::LanguageChange)
        retranslateUi();
    QWidget::changeEvent(e);
}

void ReportBuilderScreen::retranslateUi() {
    if (toolbar_title_)
        toolbar_title_->setText(tr("REPORT BUILDER"));
    if (undo_btn_)
        undo_btn_->setText(tr("Undo"));
    if (redo_btn_)
        redo_btn_->setText(tr("Redo"));
    if (open_btn_)
        open_btn_->setText(tr("Open"));
    if (save_btn_)
        save_btn_->setText(tr("Save"));
    if (pdf_btn_)
        pdf_btn_->setText(tr("Export PDF"));
    if (preview_btn_)
        preview_btn_->setText(tr("Preview"));
    // Re-apply the panel-toggle tooltips for the current collapse state.
    if (left_toggle_btn_)
        left_toggle_btn_->setToolTip(left_collapsed_ ? tr("Expand components panel  (Ctrl+B)")
                                                     : tr("Collapse components panel  (Ctrl+B)"));
    if (right_toggle_btn_)
        right_toggle_btn_->setToolTip(right_collapsed_ ? tr("Expand properties panel  (Ctrl+Shift+B)")
                                                       : tr("Collapse properties panel  (Ctrl+Shift+B)"));
}

// ── IStatefulScreen ──────────────────────────────────────────────────────────

QVariantMap ReportBuilderScreen::save_state() const {
    return {
        {"selected_id", selected_id_},
        {"left_collapsed", left_collapsed_},
        {"right_collapsed", right_collapsed_},
    };
}

void ReportBuilderScreen::restore_state(const QVariantMap& state) {
    if (state.contains("selected_id"))
        selected_id_ = state.value("selected_id").toInt(0);
    if (state.contains("left_collapsed"))
        apply_left_collapsed(state.value("left_collapsed").toBool(), /*animate=*/false);
    if (state.contains("right_collapsed"))
        apply_right_collapsed(state.value("right_collapsed").toBool(), /*animate=*/false);
    rebind_from_service();
}

} // namespace fincept::screens
