#include "app/ScreenSmokeTest.h"

#include "app/DockScreenRouter.h"
#include "core/logging/Logger.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QString>
#include <QStringList>

#include <cstdio>

namespace fincept {

namespace {
// Pump the event loop for `ms` so showEvent, deferred singleShot(100) init, and
// lazy web-view / chart construction actually run before we judge the screen.
// We do NOT wait for data fetches to finish — only for the widget tree to build.
void spin(int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
}

// Upper bound on how long one screen may take to materialise before we call it
// a failure. Generous on purpose: this is a "did it build at all" check, not a
// performance budget, and it gates releases on shared CI runners.
constexpr int kConstructDeadlineMs = 8000;

// Settle time after the widget appears, so showEvent-driven child construction
// runs before we move on. Short because the deadline loop already did the wait.
constexpr int kSettleMs = 120;

// Wait until `id`'s widget exists, or the deadline expires. Returns the elapsed
// milliseconds so a slow-but-passing screen can still be reported.
//
// A fixed spin() budget was the previous approach and it made this test lie: on
// a machine under memory pressure, 8 unrelated screens were reported as "did not
// construct" purely because deferred materialisation had not been serviced yet,
// and the same binary passed on the next run. Polling costs nothing when things
// are healthy — it returns on the first iteration — and stops the test failing
// a release for being busy rather than broken.
int wait_for_construction(DockScreenRouter* router, const QString& id) {
    QElapsedTimer t;
    t.start();
    while (!router->screen_widget(id) && t.elapsed() < kConstructDeadlineMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    return static_cast<int>(t.elapsed());
}
} // namespace

int run_screen_smoke_test(DockScreenRouter* router) {
    if (!router) {
        std::fprintf(stderr, "[Smoke] FAIL: no router\n");
        std::fflush(stderr);
        return 2;
    }

    const QStringList ids = router->all_screen_ids();
    std::fprintf(stderr, "[Smoke] walking %d screens\n", static_cast<int>(ids.size()));
    std::fflush(stderr);
    LOG_INFO("Smoke", QString("Screen smoke test: %1 screens").arg(ids.size()));

    QStringList failures;
    for (const QString& id : ids) {
        // Breadcrumb BEFORE construction so a hard process abort (e.g. a missing
        // QtWebEngineProcess.exe) names the culprit as the last line in the log.
        std::fprintf(stderr, "[Smoke] >>> constructing %s\n", qUtf8Printable(id));
        std::fflush(stderr);
        LOG_INFO("Smoke", QString("constructing %1").arg(id));

        // exclusive=true closes the previously-opened screen before opening this
        // one, so panels don't accumulate (non-exclusive navigation re-layouts an
        // ever-growing set → O(n^2) and a multi-minute walk). We only need each
        // screen open long enough to construct + fire showEvent.
        router->navigate(id, /*exclusive=*/true);
        const int waited_ms = wait_for_construction(router, id);

        if (!router->screen_widget(id)) {
            failures << id;
            std::fprintf(stderr, "[Smoke] !!! %s did not construct within %d ms\n", qUtf8Printable(id),
                         kConstructDeadlineMs);
            std::fflush(stderr);
            continue;
        }

        // Let showEvent-driven child construction run before moving on.
        spin(kSettleMs);

        // Surface slow screens without failing them — a screen that needs seconds
        // to build is a real finding, just not this test's verdict to make.
        if (waited_ms > 1000) {
            std::fprintf(stderr, "[Smoke] ... %s took %d ms to construct\n", qUtf8Printable(id), waited_ms);
            std::fflush(stderr);
            LOG_WARN("Smoke", QString("%1 took %2 ms to construct").arg(id).arg(waited_ms));
        }
    }

    if (failures.isEmpty()) {
        std::fprintf(stderr, "[Smoke] OK: all %d screens constructed\n", static_cast<int>(ids.size()));
        std::fflush(stderr);
        LOG_INFO("Smoke", "All screens constructed OK");
        return 0;
    }

    std::fprintf(stderr, "[Smoke] FAIL: %d screen(s) did not construct: %s\n", static_cast<int>(failures.size()),
                 qUtf8Printable(failures.join(", ")));
    std::fflush(stderr);
    LOG_ERROR("Smoke", QString("%1 screen(s) failed: %2").arg(failures.size()).arg(failures.join(", ")));
    return 1;
}

} // namespace fincept
