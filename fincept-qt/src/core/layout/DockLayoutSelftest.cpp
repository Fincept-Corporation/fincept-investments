#include "core/layout/DockLayoutSelftest.h"

#include <QByteArray>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QWidget>

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>
#include <cstdio>

namespace fincept::layout {

namespace {

QTextStream& out() {
    static QTextStream s(stdout);
    return s;
}

// Count <Area> elements and how many of them hold >1 <Widget> (i.e. a tab
// group). saveState() emits XML by default for our config.
struct LayoutShape {
    int areas = 0;
    int tab_groups = 0;       // areas with >= 2 widgets
    int widget_areas = 0;     // total single-widget areas
    QStringList tabbed_sets;  // human-readable widget lists for each tab group
    QStringList single_names; // widget name of each single-widget area
};

LayoutShape analyze(const QByteArray& state) {
    LayoutShape shape;
    // ADS saveState() qCompress()es the XML by default (XmlCompressionEnabled),
    // exactly as it lands in QSettings. Mirror restoreState's own check.
    const QByteArray raw = state.startsWith("<?xml") ? state : qUncompress(state);
    QString xml = QString::fromUtf8(raw);
    static const QRegularExpression area_re(QStringLiteral("<Area\\b[^>]*>(.*?)</Area>"),
                                            QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression widget_re(QStringLiteral("<Widget\\s+Name=\"([^\"]+)\""));
    auto it = area_re.globalMatch(xml);
    while (it.hasNext()) {
        auto m = it.next();
        const QString body = m.captured(1);
        QStringList names;
        auto wit = widget_re.globalMatch(body);
        while (wit.hasNext())
            names << wit.next().captured(1);
        shape.areas++;
        if (names.size() >= 2) {
            shape.tab_groups++;
            shape.tabbed_sets << names.join('+');
        } else if (names.size() == 1) {
            shape.widget_areas++;
            shape.single_names << names.first();
        }
    }
    return shape;
}

ads::CDockWidget* make_widget(ads::CDockManager* mgr, const QString& id) {
    auto* dw = new ads::CDockWidget(mgr, id);
    dw->setObjectName(id);
    dw->setWidget(new QWidget);
    return dw;
}

// All screen ids the real app registers via ensure_all_registered(). The bug
// is sensitive to the parking-area population, so we mirror a realistic set.
const QStringList& all_ids() {
    static const QStringList ids = {"dashboard",      "markets",     "portfolio",    "news",        "report_builder",
                                    "settings",       "about",       "fno",
                                    "watchlist",      "economics",   "ai_chat",     "crypto_trading",
                                    "equity_trading", "backtesting", "algo_trading", "node_editor", "code_editor"};
    return ids;
}

} // namespace

int run_dock_layout_selftest() {
    // Unbuffered stdout so progress survives an abnormal exit mid-case.
    setvbuf(stdout, nullptr, _IONBF, 0);
    int failures = 0;
    auto check = [&](bool cond, const QString& msg) {
        out() << (cond ? "  PASS  " : "  FAIL  ") << msg << "\n";
        out().flush();
        if (!cond)
            failures++;
    };

    out() << "=== Dock layout persistence self-test ===\n\n";

    // ── Case 1: raw ADS — can a tab group even be saved as a tab group? ───────
    QByteArray tabbed_blob;
    {
        QWidget host;
        auto* mgr = new ads::CDockManager(&host);
        auto* area = mgr->addDockWidget(ads::CenterDockWidgetArea, make_widget(mgr, "settings"));
        mgr->addDockWidget(ads::CenterDockWidgetArea, make_widget(mgr, "fno"), area);
        tabbed_blob = mgr->saveState();
        LayoutShape s = analyze(tabbed_blob);
        out() << "[1] raw tab group save: areas=" << s.areas << " tab_groups=" << s.tab_groups << " sets=["
              << s.tabbed_sets.join(", ") << "]\n";
        check(s.tab_groups == 1 && s.tabbed_sets.contains("settings+fno"), "settings+fno saves as ONE tabbed area");
    }

    // ── Case 2: parking-area + restore (the ensure_all_registered path) ───────
    // Reproduces the real restore sequence: pre-create EVERY screen tabbed into
    // one parking area (all hidden), then restoreState() the tabbed blob, then
    // saveState() again. Asserts the tab group survives and no phantom closed
    // single-widget areas leak into the re-saved blob.
    {
        QWidget host;
        auto* mgr = new ads::CDockManager(&host);
        ads::CDockAreaWidget* park = nullptr;
        for (const QString& id : all_ids()) {
            auto* dw = make_widget(mgr, id);
            if (park)
                mgr->addDockWidget(ads::CenterDockWidgetArea, dw, park);
            else
                park = mgr->addDockWidget(ads::CenterDockWidgetArea, dw);
            dw->toggleView(false);
        }
        const bool restored = mgr->restoreState(tabbed_blob);
        QByteArray re_saved = mgr->saveState();
        LayoutShape s = analyze(re_saved);
        out() << "\n[2] parking-area + restore + re-save: restored=" << restored << " areas=" << s.areas
              << " tab_groups=" << s.tab_groups << " single_areas=" << s.widget_areas << " sets=["
              << s.tabbed_sets.join(", ") << "]\n";
        check(restored, "restoreState() succeeds");
        check(s.tab_groups == 1 && s.tabbed_sets.contains("settings+fno"),
              "settings+fno SURVIVES round-trip as a tab group");
        check(s.widget_areas == 0, "no phantom closed single-widget areas leak into re-saved blob");
    }

    // ── Case 3: the real bug — closed panels accumulate as phantom areas, and ─
    //           prune_hidden_panels() restores a clean, faithfully-saved grid.
    // This mirrors the live state the app builds: a visible left|right grid
    // (fno | equity_trading) plus a pile of closed single-widget areas left
    // behind by earlier navigate/close cycles. We assert the bug exists before
    // pruning, then that the inline prune (identical to
    // DockScreenRouter::prune_hidden_panels — removeDockWidget on every closed
    // widget) yields a blob containing ONLY the visible grid.
    {
        QWidget host;
        auto* mgr = new ads::CDockManager(&host);
        // Visible grid: fno (left) | equity_trading (right) — retile n==2 shape.
        auto* left = mgr->addDockWidget(ads::CenterDockWidgetArea, make_widget(mgr, "fno"));
        mgr->addDockWidget(ads::RightDockWidgetArea, make_widget(mgr, "equity_trading"), left);
        // Phantom build-up: every other screen opened-then-closed over time,
        // each stranded in its own area.
        for (const QString& id : all_ids()) {
            if (id == "fno" || id == "equity_trading")
                continue;
            auto* dw = make_widget(mgr, id);
            mgr->addDockWidget(ads::BottomDockWidgetArea, dw);
            dw->toggleView(false);
        }

        LayoutShape before = analyze(mgr->saveState());
        out() << "\n[3] accumulated state: areas=" << before.areas
              << " visible_grid=2 phantom_closed=" << (before.widget_areas - 2) << "\n";
        check(before.widget_areas > 2, "BUG reproduced: phantom closed areas pollute the save");

        // --- the fix: prune every closed dock widget (mirror of the method) ---
        QList<ads::CDockWidget*> closed;
        for (auto* dw : mgr->dockWidgetsMap())
            if (dw && dw->isClosed())
                closed.append(dw);
        for (auto* dw : closed)
            mgr->removeDockWidget(dw);

        LayoutShape after = analyze(mgr->saveState());
        out() << "    after prune: areas=" << after.areas << " single=[" << after.single_names.join(", ") << "]\n";
        check(after.areas == 2 && after.widget_areas == 2, "after prune: saved blob holds ONLY the 2 visible panels");
        check(after.single_names.contains("fno") && after.single_names.contains("equity_trading"),
              "after prune: the fno | equity_trading grid is preserved");
        check(!after.single_names.contains("dashboard") && !after.single_names.contains("markets"),
              "after prune: no phantom closed panels remain in the blob");
    }

    // Build the parking area (ensure_all_registered) then restore `blob` and
    // re-save — the exact app restore path. Returns the re-saved blob.
    // Stack host → the manager is a child and is destroyed at scope exit while
    // QApplication is still alive (a static/deleteLater'd manager would be torn
    // down at program exit after QApplication is gone → segfault).
    auto parking_restore_resave = [](const QByteArray& blob) -> QByteArray {
        QWidget host;
        auto* mgr = new ads::CDockManager(&host);
        ads::CDockAreaWidget* park = nullptr;
        for (const QString& id : all_ids()) {
            auto* dw = make_widget(mgr, id);
            if (park)
                mgr->addDockWidget(ads::CenterDockWidgetArea, dw, park);
            else
                park = mgr->addDockWidget(ads::CenterDockWidgetArea, dw);
            dw->toggleView(false);
        }
        mgr->restoreState(blob);
        return mgr->saveState(); // host (and its child mgr) destroyed after the copy
    };

    // ── Case 4: the 2x2 quad grid round-trips with no phantom leakage ─────────
    {
        QWidget host;
        auto* mgr = new ads::CDockManager(&host);
        // Mirror retile_grid's n==4 sequence exactly.
        auto* tl = mgr->addDockWidget(ads::CenterDockWidgetArea, make_widget(mgr, "fno"));
        mgr->addDockWidget(ads::RightDockWidgetArea, make_widget(mgr, "equity_trading"), tl);
        auto* bl = mgr->addDockWidget(ads::BottomDockWidgetArea, make_widget(mgr, "markets"), tl);
        mgr->addDockWidget(ads::RightDockWidgetArea, make_widget(mgr, "news"), bl);

        LayoutShape rt = analyze(parking_restore_resave(mgr->saveState()));
        out() << "\n[4] 2x2 grid round-trip: areas=" << rt.areas << " single=[" << rt.single_names.join(", ") << "]\n";
        const QStringList want = {"fno", "equity_trading", "markets", "news"};
        bool all_present = true;
        for (const QString& w : want)
            all_present = all_present && rt.single_names.contains(w);
        check(rt.areas == 4 && rt.widget_areas == 4, "4-panel grid restores as exactly 4 areas");
        check(all_present, "all four panels survive the round-trip");
    }

    // ── Case 5: a floating (torn-off) panel survives the round-trip ───────────
    {
        QWidget host;
        auto* mgr = new ads::CDockManager(&host);
        mgr->addDockWidget(ads::CenterDockWidgetArea, make_widget(mgr, "fno"));
        mgr->addDockWidgetFloating(make_widget(mgr, "equity_trading"));

        QByteArray rt = parking_restore_resave(mgr->saveState());
        LayoutShape s = analyze(rt);
        const QString xml = QString::fromUtf8(rt.startsWith("<?xml") ? rt : qUncompress(rt));
        const int containers = static_cast<int>(xml.count("<Container "));
        out() << "\n[5] floating panel round-trip: containers=" << containers << " areas=" << s.areas << " single=["
              << s.single_names.join(", ") << "]\n";
        check(containers >= 2, "floating panel restores as its own (2nd) container");
        check(s.single_names.contains("fno") && s.single_names.contains("equity_trading"),
              "both docked and floating panels survive");
    }

    out() << "\n=== " << (failures == 0 ? "ALL PASS" : QString("%1 FAILURE(S)").arg(failures)) << " ===\n";
    out().flush();
    return failures == 0 ? 0 : 1;
}

} // namespace fincept::layout
