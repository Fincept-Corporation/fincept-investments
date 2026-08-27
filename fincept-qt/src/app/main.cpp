#include "algo_engine/AlgoEngineProducer.h"
#include "algo_engine/ScanMonitor.h"
#include "algo_engine/UniverseScanSelftest.h"
#include "algo_engine/fno/FnoAlgoSelftest.h"
#include "app/InstanceLock.h"
#include "app/MonitorPickerDialog.h"
#include "app/ScreenSmokeTest.h"
#include "app/TerminalShell.h"
#include "app/WindowFrame.h"
#include "core/components/ComponentCatalog.h"
#include "core/layout/WorkspaceShell.h"
#include "core/config/AppConfig.h"
#include "core/config/AppPaths.h"
#include "core/config/ProfileManager.h"
#include "core/crash/CrashHandler.h"
#include "core/currency/CurrencyManager.h"
#include "core/i18n/LanguageManager.h"
#include "core/keys/KeyConfigManager.h"
#include "core/layout/DockLayoutSelftest.h"
#include "core/logging/Logger.h"
#include "core/session/ScreenStateManager.h"
#include "core/session/SessionManager.h"
#include "core/symbol/SymbolGroup.h"
#include "core/symbol/SymbolRef.h"
#include "core/window/WindowRegistry.h"
#include "datahub/DataHub.h"
#include "datahub/DataHubMetaTypes.h"
#include "datahub/TopicPolicy.h"
#include "mcp/McpInit.h"
#include "mcp/ProviderToolFormatSelfTest.h"
#include "mcp/ToolSelfTest.h"
#include "network/http/HttpClient.h"
#include "python/OptionGreeksWorker.h"
#include "python/PythonSetupManager.h"
#include "python/PythonWorker.h"
#include "screens/launchpad/LaunchpadScreen.h"
#include "screens/recovery/CrashRecoveryDialog.h"
#include "screens/setup/SetupScreen.h"
#include "services/agents/AgentService.h"
#include "services/alpha_arena/ArenaEngine.h"
#include "services/alpha_arena/ArenaSelftest.h"
#include "services/billing/FeeDiscountService.h"
#include "services/billing/TierService.h"
#include "services/dbnomics/DBnomicsService.h"
#include "services/economics/EconomicsService.h"
#include "services/economics/MacroCalendarService.h"
#include "services/feeds/FeedSelfTest.h"
#include "services/geopolitics/GeopoliticsService.h"
#include "services/gov_data/GovDataService.h"
#include "services/llm/LlmService.h"
#include "services/ma_analytics/MAAnalyticsService.h"
#include "services/maritime/MaritimeService.h"
#include "services/maritime/PortsCatalog.h"
#include "services/markets/MarketDataService.h"
#include "services/news/NewsService.h"
#include "services/notebooks/NotebookLibraryService.h"
#include "services/options/FiiDiiService.h"
#include "services/options/OISnapshotter.h"
#include "services/options/OptionChainService.h"
#include "services/polymarket/PolymarketWebSocket.h"
#include "services/prediction/PredictionCredentialStore.h"
#include "services/prediction/PredictionExchangeRegistry.h"
#include "services/prediction/fincept_internal/FinceptInternalAdapter.h"
#include "services/prediction/kalshi/KalshiAdapter.h"
#include "services/prediction/polymarket/PolymarketAdapter.h"
#include "services/relationship_map/RelationshipMapService.h"
#include "services/report_builder/ReportBuilderService.h"
#include "services/updater/UpdateService.h"
#include "services/wallet/BuybackBurnService.h"
#include "services/wallet/RealYieldService.h"
#include "services/wallet/StakingService.h"
#include "services/wallet/TokenMetadataService.h"
#include "services/wallet/TreasuryService.h"
#include "services/wallet/WalletService.h"
#include "storage/HistoricalDataStore.h"
#include "storage/StorageManager.h"
#include "storage/repositories/NewsArticleRepository.h"
#include "storage/repositories/SettingsRepository.h"
#include "storage/sqlite/CacheDatabase.h"
#include "storage/sqlite/Database.h"
#include "storage/sqlite/migrations/MigrationRunner.h"
#include "storage/workspace/CrashRecovery.h"
#include "storage/workspace/WorkspaceSnapshotRing.h"
#include "trading/AccountManager.h"
#include "trading/DataStreamManager.h"
#include "trading/ExchangeService.h"
#include "trading/ExchangeSessionManager.h"
#include "trading/PaperMarkService.h"
#include "trading/PaperTradingSelftest.h"
#include "trading/UnifiedPortfolioService.h"
#include "trading/instruments/InstrumentService.h"
#include "trading/instruments/SymbolResolver.h"
#include "trading/replication/PortfolioReplicationSelftest.h"
#include "ui/notifications/DesktopNotifier.h"
#include "ui/tables/LiveTableSelftest.h"
#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"
#include "ui/widgets/EnterprisePromo.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QLibrary>
#include <QMessageBox>
#include <QPointer>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSslSocket>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#ifdef Q_OS_WIN
#    include <Windows.h>
#endif

// Run `steps` one per event-loop turn, in order. A single QTimer::singleShot(0)
// that does everything is still one uninterruptible main-thread block — the
// window is frozen for its full duration whether it lands just before or just
// after the first paint, and that freeze is what users read as "slow startup".
// Chaining lets the event loop breathe (repaint, input, queued hub deliveries)
// between groups. Steps must be independent of each other's completion within a
// turn; only their relative ORDER is guaranteed.
static void post_chain(std::vector<std::function<void()>> steps, std::size_t i = 0) {
    if (i >= steps.size())
        return;
    QTimer::singleShot(0, qApp, [steps = std::move(steps), i]() mutable {
        steps[i]();
        post_chain(std::move(steps), i + 1);
    });
}

// Work that used to run inside WindowFrame's auth callbacks (on_auth_state_changed
// once a paid plan was confirmed, and again after the PIN unlock). With no auth
// gate left there is no callback to hang it on, so it runs here — right after the
// primary frame is shown, the same position in the boot sequence it always held.
//
// It lives in main() rather than the WindowFrame constructor because
// WorkspaceShell::load_last_or_default() is process-global: it spawns and applies
// frames, so calling it per-frame would recurse. `recovered` skips the restore when
// CrashRecoveryDialog has already rebuilt the frames from a snapshot.
static void start_post_boot_work(fincept::WindowFrame* primary, bool recovered) {
    if (!primary)
        return;
    if (!recovered)
        fincept::layout::WorkspaceShell::load_last_or_default();

    // Silent update check, delayed so the UI settles first. UpdateService
    // de-dupes silent checks across the session.
    QPointer<fincept::WindowFrame> target = primary;
    QTimer::singleShot(3000, primary, [target]() {
        fincept::services::UpdateService::instance().set_dialog_parent(target.data());
        fincept::services::UpdateService::instance().check_for_updates(true);
    });

    // Warm the instrument cache in the background — only loads what isn't
    // already cached. Every registered broker is warmed so unified cross-broker
    // search sees any master that was already downloaded. InstrumentService::
    // instance() also registers the broker sources, so registered_brokers() is
    // populated by the time the loop runs.
    auto& isvc = fincept::trading::InstrumentService::instance();
    for (const QString& bid : fincept::trading::SymbolResolver::instance().registered_brokers())
        isvc.load_from_db_async(bid);
}

// Wire the two app-level lifecycle handlers that fire after the primary
// window exists: InstanceLock::message_received (a re-launch of the exe
// asks us to bring the running instance to the front — args ignored, the
// request itself is the trigger) and QApplication::lastWindowClosed (surface the Launchpad
// instead of quitting; the Launchpad's own close handler quits explicitly).
// Called from both the post-setup-screen path and the no-setup path so the
// two branches stay in sync.
static void wire_app_lifecycle(QApplication& app, fincept::InstanceLock& lock) {
    QObject::connect(&lock, &fincept::InstanceLock::message_received, [](const QStringList& /*args*/) {
        // Re-launching the exe while an instance is already
        // running means "bring the running instance forward" —
        // the standard single-instance behaviour — NOT "open a
        // new window". Opening a new window (and the monitor
        // picker that goes with it) stays an EXPLICIT action:
        // the toolbar "New Window", Ctrl+Shift+N, the Launchpad
        // button, and tear-off. Routing relaunches through the
        // picker surprised users by prompting for a monitor on
        // every open even when they never asked for a new window.
        const auto frames = fincept::WindowRegistry::instance().frames();
        if (!frames.isEmpty()) {
            // Lowest window_id (the primary) is the predictable
            // target. Activating one window pulls the whole app
            // forward on every platform we support.
            fincept::WindowFrame* target = frames.first();
            if (target->isMinimized())
                target->showNormal();
            target->raise();
            target->activateWindow();
            LOG_INFO("App", "Secondary instance request — raised existing window");
        } else {
            // No live frames (e.g. the user closed to the
            // Launchpad). Surface it instead of silently no-op'ing.
            fincept::screens::LaunchpadScreen::instance()->surface();
            LOG_INFO("App", "Secondary instance request — surfaced Launchpad");
        }
    });
    QObject::connect(&app, &QApplication::lastWindowClosed, &app, []() {
        // Settings → General → "On last window close" controls behaviour.
        // Default = "quit" so closing the last window quits the app like
        // every normal desktop app. Power users opt in to the Launchpad.
        const auto r = fincept::SettingsRepository::instance().get(QStringLiteral("general.on_last_window_close"),
                                                                   QStringLiteral("quit"));
        const QString choice = r.is_ok() ? r.value() : QStringLiteral("quit");

        if (choice == QStringLiteral("show_launchpad")) {
            fincept::screens::LaunchpadScreen::instance()->surface();
        } else {
            // "quit" or any unknown value → quit (the safe default).
            QCoreApplication::quit();
        }
    });
}

int main(int argc, char* argv[]) {
    // ── TLS backend selection (must happen before any Qt plugin loading) ────
    // Force QtNetwork to use the OpenSSL TLS backend across platforms.
    //   - macOS: Apple SecureTransport (qtls_st.cpp) double-frees inside
    //     SSLWrite when QWebSocket sends a close frame after a protocol
    //     error. Reproducible by opening the crypto tab — Kraken/Polymarket
    //     WS reconnect path crashes the main thread. SecureTransport was
    //     deprecated by Apple in 10.15.
    //   - Windows: Schannel has had similar instability around WS close
    //     and certificate revocation paths in Qt 6.8. OpenSSL is the
    //     consistent backend on every platform Qt supports.
    // QT_TLS_BACKEND is read by QTlsBackendFactory at plugin-loader time, so
    // it has to be set BEFORE QCoreApplication/QApplication is constructed —
    // setActiveBackend() after the fact does not always take effect because
    // singleton factories may already be bound.
    qputenv("QT_TLS_BACKEND", "openssl");

#ifdef Q_OS_MACOS
    // Pre-load OpenSSL from Homebrew so the openssl plugin's runtime dlopen
    // succeeds. Qt's QLibrary search defaults don't include /opt/homebrew/...
    // Order matters: libcrypto first (libssl depends on it).
    {
        const QStringList crypto_candidates = {
            QStringLiteral("/opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib"),
            QStringLiteral("/usr/local/opt/openssl@3/lib/libcrypto.3.dylib"),
        };
        const QStringList ssl_candidates = {
            QStringLiteral("/opt/homebrew/opt/openssl@3/lib/libssl.3.dylib"),
            QStringLiteral("/usr/local/opt/openssl@3/lib/libssl.3.dylib"),
        };
        for (const auto& p : crypto_candidates) {
            if (QFile::exists(p)) {
                QLibrary(p).load();
                break;
            }
        }
        for (const auto& p : ssl_candidates) {
            if (QFile::exists(p)) {
                QLibrary(p).load();
                break;
            }
        }
    }
#endif

    // ── Parse --profile <name> from argv before Qt initialises ───────────────
    // This must happen first so that:
    //   1. AppPaths returns the correct per-profile directories
    //   2. InstanceLock uses a profile-scoped IPC key so two different
    //      profiles can run simultaneously as independent primary instances
    {
        for (int i = 1; i < argc - 1; ++i) {
            if (qstrcmp(argv[i], "--profile") == 0) {
                fincept::ProfileManager::instance().set_active(QString::fromUtf8(argv[i + 1]));
                break;
            }
        }
        // AppPaths::root() must exist before ensure_all() so ProfileManager can
        // write the manifest. Create root now (single mkdir, idempotent).
        QDir().mkpath(fincept::AppPaths::root());
    }

    // Install the unhandled-exception filter BEFORE any Qt object is
    // constructed. On Windows this writes a minidump to AppPaths::crashdumps()
    // when the process dies from an access violation, stack overflow, or GS
    // cookie check failure (STATUS_STACK_BUFFER_OVERRUN — see issue #215).
    // Do it early so a crash in Qt's own startup still produces a dump.
    fincept::crash::install();

    // Required before QApplication when any dock panel contains an OpenGL widget
    // (Qt Charts, QOpenGLWidget) — prevents black rendering in floating windows.
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    // QApplication first — InstanceLock's QLocalServer needs an event loop
    // owner. Then the lock probes for an existing primary; if found, it
    // ships our argv to the primary (which opens a new WindowFrame in
    // response) and we exit cleanly.
    //
    // Why not SingleApplication? Its QSharedMemory + QSystemSemaphore lock
    // leaks on macOS Qt 6.6+ (QTBUG-111855), causing silent exits on Finder
    // launch. See InstanceLock.h for the fuller story (issues #234, #252).
    //
    // The instance key is scoped to the active profile name, so
    // "FinceptTerminal --profile work" and "FinceptTerminal --profile personal"
    // run as two independent primaries.
    QApplication app(argc, argv);
    app.setApplicationName("FinceptTerminal");
    app.setOrganizationName("Fincept");
#ifndef FINCEPT_VERSION_STRING
#    define FINCEPT_VERSION_STRING "0.0.0-dev"
#endif
    app.setApplicationVersion(QStringLiteral(FINCEPT_VERSION_STRING));

    // Quit only when LaunchpadScreen::closeEvent calls QCoreApplication::quit().
    // Default Qt behaviour fires lastWindowClosed AND schedules an auto-quit;
    // we connect a slot to surface() the Launchpad on lastWindowClosed, and
    // the auto-quit would race that slot — sometimes killing the app while
    // the launchpad is mid-show. With this off, the only quit path is the
    // explicit one in LaunchpadScreen::closeEvent.
    app.setQuitOnLastWindowClosed(false);

    // Belt-and-braces: if QT_TLS_BACKEND wasn't honoured for some reason
    // (e.g. plugin load order on a particular platform), retry the switch
    // explicitly. This is a no-op if openssl is already active.
    {
        const auto backends = QSslSocket::availableBackends();
        if (QSslSocket::activeBackend() != QStringLiteral("openssl") && backends.contains(QStringLiteral("openssl"))) {
            QSslSocket::setActiveBackend(QStringLiteral("openssl"));
        }
    }

    // ── Single-instance lock + new-window IPC ────────────────────────────────
    const QString profile_key = QString("FinceptTerminal-%1").arg(fincept::ProfileManager::instance().active());
    fincept::InstanceLock instance_lock;
    const auto lock_status = instance_lock.acquire(profile_key, QCoreApplication::arguments());

    // ── Secondary instance: argv was already shipped to the primary. Exit. ──
    if (lock_status == fincept::InstanceLock::Status::Secondary) {
#ifdef Q_OS_WIN
        // Grant the primary process permission to bring its new window to
        // the foreground — Windows blocks focus-steal without this. Pre-
        // SingleApplication this used app.primaryPid(); without that we
        // call AllowSetForegroundWindow(ASFW_ANY) which whitelists the
        // whole foreground request from this process.
        AllowSetForegroundWindow(ASFW_ANY);
#endif
        return 0;
    }

    // ── Primary instance from here on ────────────────────────────────────────

    // Bring up the TerminalShell. This is the multi-window refactor's
    // process-level coordinator. Phase 1 ships a skeleton: it bootstraps
    // ProfilePaths, resolves the active ProfileId, and warms the registries
    // (WindowRegistry, ActionRegistry) so WindowFrame constructors don't race
    // their singleton init.
    //
    // Must run BEFORE any service init so future phases that lift services
    // into the shell can rely on it being present.
    fincept::TerminalShell::instance().initialise();
    QObject::connect(&app, &QCoreApplication::aboutToQuit, []() {
        // MCP first, shell second — deliberate ordering.
        //
        // shutdown_mcp() stops TerminalMcpBridge and then every external MCP
        // server child process (npx/uvx/python). It had ZERO call sites, so
        // those children were orphaned on every exit and kept running as the
        // user. It is idempotent, so calling it here is safe even if some other
        // teardown path reaches it too.
        //
        // It must run BEFORE TerminalShell::shutdown() because the shell
        // teardown closes the workspace DB and deletes CrashRecovery /
        // WorkspaceSnapshotRing, while MCP's workspace/dashboard tool handlers
        // read exactly those. Draining MCP first guarantees no tool call is
        // in flight against a half-torn-down shell.
        fincept::mcp::shutdown_mcp();

        // Stop the Python daemons before the shell tears anything down.
        //
        // Both own a QProcess plus repeating QTimers (PythonWorker gained a
        // deadline-sweep timer that can call proc_->kill()). Neither had a
        // shutdown call site: they are singletons, so their destructors run at
        // STATIC destruction — after QApplication is gone and after the crash
        // handler is unregistered. A timer or process callback firing in that
        // window crashes with no minidump and no log line, which is exactly the
        // kind of exit failure that is near-impossible to diagnose after the
        // fact. stop() is idempotent on both.
        fincept::python::PythonWorker::instance().stop();
        fincept::python::OptionGreeksWorker::instance().stop();

        fincept::TerminalShell::instance().shutdown();
    });

    // Register DataHub payload meta-types (QuoteData, HistoryPoint, InfoData,
    // NewsArticle, EconomicsResult) so they can flow through QVariant-keyed
    // topics and cross-thread queued signals. Phase 0 — see
    // fincept-qt/DATAHUB_ARCHITECTURE.md.
    // Phase 2: register MarketDataService as the `market:quote:*` producer.
    fincept::datahub::register_metatypes();
    // SymbolContext payload types — signals cross threads when a producer
    // service (not just UI) publishes a group change.
    qRegisterMetaType<fincept::SymbolRef>("fincept::SymbolRef");
    qRegisterMetaType<fincept::SymbolGroup>("fincept::SymbolGroup");
    // Phase 6: load the Component Browser catalogue. Try the build-side copy
    // first (present after cmake configure copies resources) and fall back to
    // the source-tree path for local dev runs without install step.
    fincept::ComponentCatalog::instance().load_with_fallbacks({
        QCoreApplication::applicationDirPath() + "/resources/component_catalog.json",
        QCoreApplication::applicationDirPath() + "/component_catalog.json",
        "resources/component_catalog.json",
    });
    fincept::services::MarketDataService::instance().ensure_registered_with_hub();

    // ── Sync services needed by the default dashboard ─────────────────────────
    // Anything a default dashboard widget subscribes to during its first show
    // must be registered with the hub before the window paints. Everything
    // else is deferred to a single QTimer::singleShot(0) below — the event
    // loop runs that batch immediately after the first paint, so cold-start
    // perceived latency drops without changing functional behavior.
    fincept::services::NewsService::instance().ensure_registered_with_hub();
    fincept::services::EconomicsService::instance().ensure_registered_with_hub();
    fincept::services::MacroCalendarService::instance().ensure_registered_with_hub();
    fincept::trading::DataStreamManager::instance().ensure_registered_with_hub();
    fincept::services::geo::GeopoliticsService::instance().ensure_registered_with_hub();
    fincept::services::maritime::MaritimeService::instance().ensure_registered_with_hub();
    fincept::services::maritime::PortsCatalog::instance().ensure_registered_with_hub();
    fincept::services::RelationshipMapService::instance().ensure_registered_with_hub();
    fincept::services::ma::MAAnalyticsService::instance().ensure_registered_with_hub();
    fincept::wallet::TokenMetadataService::instance().load_from_storage();

    // ── Pre-warm the dashboard topics ────────────────────────────────────────
    // The setup / crash-recovery flow, and the frame construction itself, run
    // before the dashboard ever paints. Kick the hub now so producers fetch
    // immediately; by the time the dashboard widgets subscribe in showEvent,
    // peek() returns a fresh value and deliver_initial_value() paints it on
    // the first frame instead of showing the loading overlay.
    //
    // Set is the union of topics used by every widget in the default
    // `portfolio_manager` template plus the global indices/forex/crypto/
    // commodities universes (so any user-customised default still hits the
    // warm cache). Late-registered producers (the QTimer::singleShot(0)
    // batch below) warm themselves on their own next scheduler pass once
    // subscribers exist; pre-warming them here would orphan-log because
    // ensure_registered_with_hub() hasn't run for them yet.
    QTimer::singleShot(0, qApp, []() {
        auto& hub = fincept::datahub::DataHub::instance();
        QStringList topics;

        auto add_quotes = [&](const QStringList& syms) {
            topics.reserve(topics.size() + syms.size());
            for (const auto& s : syms)
                topics.append(QStringLiteral("market:quote:") + s);
        };

        // Default-template widget symbol sets (kept aligned with the widget
        // source — if a widget's hardcoded list changes, update here too).
        add_quotes(fincept::services::MarketDataService::indices_symbols());
        add_quotes(fincept::services::MarketDataService::forex_symbols());
        add_quotes(fincept::services::MarketDataService::crypto_symbols());
        add_quotes(fincept::services::MarketDataService::commodity_symbols());
        add_quotes({"^GSPC", "^IXIC", "^DJI", "^RUT", "^VIX", "GC=F"});                                  // performance
        add_quotes({"^VIX", "SPY", "QQQ", "IWM", "TLT", "NVDA", "TSLA", "AMD", "META", "PLTR", "COIN"}); // risk_metrics
        add_quotes({"AAPL", "MSFT", "GOOGL", "AMZN", "NVDA", "TSLA", "META", "JPM"}); // watchlist default

        // Non-quote topics used by the default template.
        topics.append(QStringLiteral("news:general"));
        topics.append(QStringLiteral("econ:fincept:upcoming_events"));

        // De-duplicate (several add_quotes calls overlap on common symbols).
        topics.removeDuplicates();

        // force=true bypasses min_interval_ms so the very first cold-start
        // fetch isn't gated by an unrelated test refresh; producer rate
        // limits still apply at dispatch (DataHub::flush_coalesced_requests).
        hub.request(topics, /*force=*/true);
        LOG_INFO("App", QString("Pre-warmed %1 dashboard topics").arg(topics.size()));
    });

    // ── Deferred service init — fires after first window paint ───────────────
    // These services back tab-specific screens (F&O, prediction markets,
    // alpha arena, agents, wallet/treasury/staking, etc.) — none of them is
    // needed for the dashboard's first paint, so registering them here would
    // only add latency to the user-visible cold start. Late registration is
    // safe: the hub's scheduler tick picks up matching subscriptions on the
    // next pass once the producer is registered.
    //
    // Split into groups run one per event-loop turn (post_chain above).
    // As a single lambda this was ~180 lines of uninterruptible main-thread
    // work — 20 hub registrations, prediction-adapter construction with
    // SecureStorage credential loads, 15 policy patterns, wallet restore, and
    // a live broker ping sweep — and the window stayed frozen for all of it.
    // Order WITHIN a group is preserved; the groups only touch their own
    // singletons.

    // ── Group 1: DataHub producer registrations ─────────────────────────────
    auto init_hub_producers = []() {
        // F&O / Options chain — `option:chain:*`, `option:tick:*`,
        // `option:atm_iv:*`, `fno:pcr:*`, `fno:max_pain:*`.
        fincept::services::options::OptionChainService::instance().ensure_registered_with_hub();
        // F&O OI snapshotter — subscribes to option:chain:* and persists
        // minute-aligned OI/LTP/Vol/IV rows to SQLite. Producer for
        // oi:history:* (window queries).
        fincept::services::options::OISnapshotter::instance().ensure_registered_with_hub();
        // F&O FII/DII flows — daily NSE cash-market institutional buy/sell.
        fincept::services::options::FiiDiiService::instance().ensure_registered_with_hub();
        // Multi-broker session manager — `ws:kraken:*` / `ws:hyperliquid:*`.
        fincept::trading::ExchangeSessionManager::instance().ensure_registered_with_hub();
        // Prediction Markets — `prediction:polymarket:*`.
        fincept::services::polymarket::PolymarketWebSocket::instance().ensure_registered_with_hub();
        // Alpha Arena engine — init() is idempotent and only scans for
        // crashed competitions (no-op with none). Not a DataHub Producer
        // (callback-style by design).
        fincept::arena::ArenaEngine::instance().init();
        {
            auto& reg = fincept::services::prediction::PredictionExchangeRegistry::instance();
            reg.register_adapter(std::make_unique<fincept::services::prediction::polymarket_ns::PolymarketAdapter>());
            reg.register_adapter(std::make_unique<fincept::services::prediction::kalshi_ns::KalshiAdapter>());
            // Fincept internal prediction-market adapter (demo mode until
            // `fincept.markets_endpoint` is configured).
            reg.register_adapter(
                std::make_unique<fincept::services::prediction::fincept_internal::FinceptInternalAdapter>());

            // Hydrate credentials from SecureStorage if previously saved.
            if (auto* pm = dynamic_cast<fincept::services::prediction::polymarket_ns::PolymarketAdapter*>(
                    reg.adapter(QStringLiteral("polymarket")))) {
                pm->reload_credentials();
            }
            if (auto* ks = dynamic_cast<fincept::services::prediction::kalshi_ns::KalshiAdapter*>(
                    reg.adapter(QStringLiteral("kalshi")))) {
                if (auto creds = fincept::services::prediction::PredictionCredentialStore::load_kalshi()) {
                    ks->set_credentials(*creds);
                }
                // Register the Kalshi WS producer + push-only topic policies
                // (prediction:kalshi:price:*, prediction:kalshi:orderbook:*) with the hub.
                ks->ensure_registered_with_hub();
            }
            if (auto* fi = reg.adapter(QStringLiteral("fincept"))) {
                fi->ensure_registered_with_hub();
            }
        }
        // Specialized data sources.
        fincept::services::DBnomicsService::instance().ensure_registered_with_hub();
        fincept::services::GovDataService::instance().ensure_registered_with_hub();
        // Agents — `agent:*` push-only producer.
        fincept::services::AgentService::instance().ensure_registered_with_hub();

        // Algo Engine — `algo:metrics:*`, `algo:trade:*`, `algo:state:*`.
        fincept::algo::AlgoEngineProducer::instance().ensure_registered_with_hub();
    };

    // ── Group 2: wallet / treasury / billing + broker session monitor ───────
    auto init_wallet_treasury_and_monitors = []() {
        // Token metadata refresh — network call to Jupiter aggregator.
        fincept::wallet::TokenMetadataService::instance().refresh_from_jupiter_async();
        // Wallet — `wallet:balance:*`, `market:price:token:*`.
        fincept::wallet::WalletService::instance().ensure_registered_with_hub();
        fincept::wallet::WalletService::instance().restore_from_storage();

        // Fee-discount eligibility producer (paid screens only).
        {
            static fincept::billing::FeeDiscountService discount_service;
            auto& hub = fincept::datahub::DataHub::instance();
            hub.register_producer(&discount_service);
            fincept::datahub::TopicPolicy p;
            p.ttl_ms = 60 * 1000;
            p.min_interval_ms = 15 * 1000;
            hub.set_policy_pattern(QStringLiteral("billing:fncpt_discount:*"), p);
        }

        // Buyback & burn / treasury producers (treasury:*).
        {
            static fincept::wallet::BuybackBurnService buyback_burn_service;
            static fincept::wallet::TreasuryService treasury_service;
            auto& hub = fincept::datahub::DataHub::instance();
            hub.register_producer(&buyback_burn_service);
            hub.register_producer(&treasury_service);

            fincept::datahub::TopicPolicy epoch_p;
            epoch_p.ttl_ms = 60 * 1000;
            epoch_p.min_interval_ms = 30 * 1000;
            hub.set_policy_pattern(QStringLiteral("treasury:buyback_epoch"), epoch_p);

            fincept::datahub::TopicPolicy burn_p;
            burn_p.ttl_ms = 5 * 60 * 1000;
            burn_p.min_interval_ms = 60 * 1000;
            hub.set_policy_pattern(QStringLiteral("treasury:burn_total"), burn_p);

            fincept::datahub::TopicPolicy supply_p;
            supply_p.ttl_ms = 60 * 60 * 1000;
            supply_p.min_interval_ms = 5 * 60 * 1000;
            hub.set_policy_pattern(QStringLiteral("treasury:supply_history"), supply_p);

            fincept::datahub::TopicPolicy reserves_p;
            reserves_p.ttl_ms = 5 * 60 * 1000;
            reserves_p.min_interval_ms = 60 * 1000;
            hub.set_policy_pattern(QStringLiteral("treasury:reserves"), reserves_p);

            fincept::datahub::TopicPolicy runway_p;
            runway_p.ttl_ms = 5 * 60 * 1000;
            runway_p.min_interval_ms = 60 * 1000;
            hub.set_policy_pattern(QStringLiteral("treasury:runway"), runway_p);
        }

        // STAKE tab producers (veFNCPT lock + real-yield + tier system).
        {
            auto& hub = fincept::datahub::DataHub::instance();
            hub.register_producer(&fincept::wallet::StakingService::instance());
            hub.register_producer(&fincept::wallet::RealYieldService::instance());
            hub.register_producer(&fincept::billing::TierService::instance());

            fincept::datahub::TopicPolicy locks_p;
            locks_p.ttl_ms = 60 * 1000;
            locks_p.min_interval_ms = 30 * 1000;
            hub.set_policy_pattern(QStringLiteral("wallet:locks:*"), locks_p);
            hub.set_policy_pattern(QStringLiteral("wallet:vefncpt:*"), locks_p);

            fincept::datahub::TopicPolicy yield_p;
            yield_p.ttl_ms = 5 * 60 * 1000;
            yield_p.min_interval_ms = 60 * 1000;
            hub.set_policy_pattern(QStringLiteral("wallet:yield:*"), yield_p);

            fincept::datahub::TopicPolicy revenue_p;
            revenue_p.ttl_ms = 60 * 60 * 1000;
            revenue_p.min_interval_ms = 5 * 60 * 1000;
            hub.set_policy_pattern(QStringLiteral("treasury:revenue"), revenue_p);

            // billing:tier:* derived from wallet:vefncpt:* — TTL is just a
            // safety net; the service republishes whenever vefncpt emits.
            fincept::datahub::TopicPolicy tier_p;
            tier_p.ttl_ms = 60 * 1000;
            tier_p.min_interval_ms = 15 * 1000;
            hub.set_policy_pattern(QStringLiteral("billing:tier:*"), tier_p);
        }

        // Broker session monitor — re-validates each connected broker account's
        // access token on a 5-min cadence and silently refreshes where supported
        // (Zerodha/Angel One TOTP re-login, Fyers refresh token). Keeps the
        // connection indicator honest instead of showing a stale "green".
        fincept::trading::AccountManager::instance().start_session_monitor();

        // Periodically auto-download historical candles for any watchlisted
        // series. Double-gated to a no-op: does nothing unless the Historify
        // watchlist has entries AND a broker account is connected.
        auto* historify_timer = new QTimer(qApp);
        historify_timer->setInterval(15 * 60 * 1000); // 15 min
        QObject::connect(historify_timer, &QTimer::timeout, qApp,
                         []() { fincept::storage::HistoricalDataStore::instance().refresh_watchlist(); });
        historify_timer->start();

        LOG_INFO("App", "Deferred service init complete");
    };

    post_chain({init_hub_producers, init_wallet_treasury_and_monitors});

    // Create all application directories under %LOCALAPPDATA%/com.fincept.terminal
    fincept::AppPaths::ensure_all();

    // ── One-time migration from legacy %APPDATA% location ─────────────────
    // Current locations (under %LOCALAPPDATA%\com.fincept.terminal\):
    //   Log: <root>/logs/fincept.log    DB: <root>/data/fincept.db
    {
        const QString old_base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const auto migrate_file = [](const QString& old_path, const QString& new_path) {
            if (QFile::exists(old_path) && !QFile::exists(new_path))
                QFile::rename(old_path, new_path);
        };
        migrate_file(old_base + "/fincept.db", fincept::AppPaths::data() + "/fincept.db");
        migrate_file(old_base + "/cache.db", fincept::AppPaths::data() + "/cache.db");
        migrate_file(old_base + "/fincept.log", fincept::AppPaths::logs() + "/fincept.log");
        migrate_file(old_base + "/fincept-files", fincept::AppPaths::files());
        // Remove stale WAL/SHM from old location too
        QFile::remove(old_base + "/fincept.db-wal");
        QFile::remove(old_base + "/fincept.db-shm");
        QFile::remove(old_base + "/cache.db-wal");
        QFile::remove(old_base + "/cache.db-shm");
    }

    // SQLite owns its own .db-wal / .db-shm files. Pre-deleting them is
    // destructive: any committed transaction that has not yet been checkpointed
    // back into the main .db file lives entirely in the WAL. Auto-checkpoint
    // only fires past ~1000 pages, and the on-close checkpoint can fail
    // silently — small writes (e.g. pin_hash on the secure_credentials table)
    // routinely persist only in the WAL across runs. SQLite reads the WAL on
    // open to recover any uncheckpointed or crash-truncated state, so we leave
    // these files for SQLite to manage. Single-instance enforcement is owned
    // by InstanceLock above.

    // Clean legacy v3 DB location (these paths are no longer live DBs)
    {
        const QString local_dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        const QString legacy1 = local_dir.section('/', 0, -3) + "/FinceptTerminal/fincept_settings.db";
        const QString legacy2 =
            QString(local_dir).replace("Fincept/FinceptTerminal", "FinceptTerminal") + "/fincept_settings.db";
        QFile::remove(legacy1 + "-wal");
        QFile::remove(legacy1 + "-shm");
        QFile::remove(legacy2 + "-wal");
        QFile::remove(legacy2 + "-shm");
    }

    fincept::Logger::instance().set_file(fincept::AppPaths::logs() + "/fincept.log");

    // Seed the prebuilt Fincept Notebook library into the File Manager on first
    // run (idempotent — guarded by a marker file). Makes the curated notebooks
    // appear in both the Notebook Library and the File Manager out of the box.
    fincept::services::NotebookLibraryService::instance().seed_into_files();

    // P3.18 — route Qt's own qDebug/qWarning/qCritical messages into our log
    // file so framework/3rd-party warnings are visible in Release builds.
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
        const char* category = (ctx.category && *ctx.category) ? ctx.category : "Qt";
        switch (type) {
            case QtDebugMsg:
                fincept::Logger::instance().debug(category, msg);
                break;
            case QtInfoMsg:
                fincept::Logger::instance().info(category, msg);
                break;
            case QtWarningMsg:
                fincept::Logger::instance().warn(category, msg);
                break;
            case QtCriticalMsg:
                fincept::Logger::instance().error(category, msg);
                break;
            case QtFatalMsg:
                fincept::Logger::instance().error(category, msg);
                fincept::Logger::instance().flush_and_close();
                break;
        }
    });
    {
        auto& log = fincept::Logger::instance();
        auto& cfg = fincept::AppConfig::instance();

        // Global level
        const QString gl = cfg.get("log/global_level", "Info").toString();
        const QHash<QString, fincept::LogLevel> lvl_map = {
            {"Trace", fincept::LogLevel::Trace}, {"Debug", fincept::LogLevel::Debug},
            {"Info", fincept::LogLevel::Info},   {"Warn", fincept::LogLevel::Warn},
            {"Error", fincept::LogLevel::Error}, {"Fatal", fincept::LogLevel::Fatal}};
        log.set_level(lvl_map.value(gl, fincept::LogLevel::Info));

        // JSON output mode (persisted in Settings → Logging)
        log.set_json_mode(cfg.get("log/json_mode", false).toBool());

        // Per-tag overrides
        const int count = cfg.get("log/tag_count", 0).toInt();
        for (int i = 0; i < count; ++i) {
            const QString tag = cfg.get(QString("log/tag_%1_name").arg(i)).toString();
            const QString level = cfg.get(QString("log/tag_%1_level").arg(i)).toString();
            if (!tag.isEmpty() && lvl_map.contains(level))
                log.set_tag_level(tag, lvl_map.value(level));
        }
    }
    LOG_INFO("App", "Fincept Terminal v" FINCEPT_VERSION_STRING " starting...");
    LOG_INFO("App", QString("TLS backend: %1 (available: %2)")
                        .arg(QSslSocket::activeBackend(), QSslSocket::availableBackends().join(", ")));

    // Theme is applied after DB is open so saved font/theme are respected from the start.

    // Initialize config
    auto& config = fincept::AppConfig::instance();
    fincept::HttpClient::instance().set_base_url(config.api_base_url());

    // Register migrations explicitly (avoids MSVC /OPT:REF stripping static-init TUs)
    fincept::register_migration_v001();
    fincept::register_migration_v002();
    fincept::register_migration_v003();
    fincept::register_migration_v004();
    fincept::register_migration_v005();
    fincept::register_migration_v006();
    fincept::register_migration_v007();
    fincept::register_migration_v008();
    fincept::register_migration_v009();
    fincept::register_migration_v010();
    fincept::register_migration_v011();
    fincept::register_migration_v012();
    fincept::register_migration_v013();
    fincept::register_migration_v014();
    fincept::register_migration_v015();
    fincept::register_migration_v016();
    fincept::register_migration_v017();
    fincept::register_migration_v018();
    fincept::register_migration_v019();
    fincept::register_migration_v020();
    fincept::register_migration_v021();
    fincept::register_migration_v022();
    fincept::register_migration_v023();
    fincept::register_migration_v024();
    fincept::register_migration_v025();
    fincept::register_migration_v026();
    fincept::register_migration_v027();
    fincept::register_migration_v028();
    fincept::register_migration_v029();
    fincept::register_migration_v030();
    fincept::register_migration_v031();
    fincept::register_migration_v032();
    fincept::register_migration_v033();
    fincept::register_migration_v034();
    fincept::register_migration_v035();
    fincept::register_migration_v036();
    fincept::register_migration_v037();
    fincept::register_migration_v038();
    fincept::register_migration_v039();
    fincept::register_migration_v040();
    fincept::register_migration_v041();
    fincept::register_migration_v042();
    fincept::register_migration_v043();
    fincept::register_migration_v044();
    fincept::register_migration_v045();
    fincept::register_migration_v046();
    fincept::register_migration_v047();
    fincept::register_migration_v048();
    fincept::register_migration_v049();
    fincept::register_migration_v050();
    fincept::register_migration_v051();
    fincept::register_migration_v052();

    // Open main database
    QString db_path = fincept::AppPaths::data() + "/fincept.db";
    auto db_result = fincept::Database::instance().open(db_path);
    if (db_result.is_err()) {
        const std::string db_err = db_result.error();
        LOG_ERROR("App", "Failed to open database: " + QString::fromStdString(db_err));

        // A FAILED migration is fatal. Booting on regardless produced the worst
        // possible outcome: a terminal that looks fully functional but is wired
        // to a half-migrated database, so every repository silently reads and
        // writes a shape that is neither the old nor the new one. The message
        // already carries the backup path and the remediation text, so surface
        // it verbatim and stop.
        //
        // A newer-than-build schema never reaches here: MigrationRunner::run()
        // warns and returns ok() for that case, so the DB opens normally.
        if (fincept::MigrationRunner::is_fatal_error(db_err)) {
            QMessageBox::critical(nullptr, QObject::tr("Fincept Terminal — database error"),
                                  QString::fromStdString(db_err));
            // Returning from main() never reaches exec(), so aboutToQuit never
            // fires and the shell's clean-shutdown marker would never be
            // written — the next launch would greet the user with a spurious
            // crash-recovery dialog on top of the database error. shutdown() is
            // idempotent (it clears initialised_), so calling it here is safe.
            fincept::TerminalShell::instance().shutdown();
            return 1; // do NOT continue into the UI
        }

        // Non-fatal (e.g. the file could not be opened at all) — the app can
        // still run in a degraded, DB-less state. Apply theme with built-in
        // defaults so the UI is at least styled.
        fincept::ui::apply_global_stylesheet();
    } else {
        // Load broker accounts now that the DB is open. The AccountManager
        // singleton loads eagerly in its constructor on first access; if anything
        // touched it before this point (before open()), it found an unusable DB
        // and loaded nothing. This explicit main-thread reload guarantees the
        // account map is populated from the now-open DB, so configured brokers
        // survive restarts instead of vanishing.
        fincept::trading::AccountManager::instance().reload_from_db();

        // Prune news articles older than 30 days — deferred to run after the event loop
        // starts so the startup critical path is not blocked.
        // NewsArticleRepository uses the main-thread DB connection (not thread-safe),
        // so we must not run this on a worker thread — QTimer::singleShot(0) posts it
        // to the main thread's event queue instead.
        {
            int64_t news_cutoff = QDateTime::currentSecsSinceEpoch() - (30LL * 86400);
            QTimer::singleShot(0, [news_cutoff]() {
                fincept::NewsArticleRepository::instance().prune_older_than(news_cutoff);
                LOG_INFO("App", "News articles pruned (keeping 30 days)");
            });
        }

        // Retention sweeper for the append-only tables that had NO reader and NO
        // retention policy, so they grew for the life of the install:
        // workflow_audit_log (>90d), telemetry_events (>30d), and expired
        // unified_cache — which was previously swept once at startup only, so a terminal left
        // open for days never reclaimed anything. Runs once now and every ~15
        // minutes thereafter; idempotent, so a second call creates no second
        // timer. Started here (inside the DB-open branch) because every policy
        // is a DELETE against the main DB.
        //
        // NOT in --smoke-test / --selftest-* runs: those are short headless
        // processes that construct screens and exit. Background maintenance has
        // no value there, and a sweep still in flight when the process tears the
        // databases down is a shutdown crash with no dump (the crash handler is
        // already gone by static-destruction time). Detected by scanning argv
        // because smoke_mode is not computed until much later in main().
        const bool headless_run = [argc, argv]() {
            for (int i = 1; i < argc; ++i) {
                if (qstrcmp(argv[i], "--smoke-test") == 0 || qstrncmp(argv[i], "--selftest", 10) == 0)
                    return true;
            }
            return false;
        }();
        if (!headless_run)
            fincept::StorageManager::instance().start_retention_sweeper();

        // Load persisted font settings and apply before any window is shown
        // — eliminates flash/wrong-font-on-startup. Theme is always Obsidian.
        {
            auto& repo = fincept::SettingsRepository::instance();
            auto& tm = fincept::ui::ThemeManager::instance();
            auto r_family = repo.get("appearance.font_family");
            auto r_size = repo.get("appearance.font_size");
            auto r_density = repo.get("appearance.density");
            QString family = r_family.is_ok() ? r_family.value() : "Consolas";
            QString size_s = r_size.is_ok() ? r_size.value() : "14px";
            int size_px = size_s.left(size_s.indexOf("px")).toInt();
            if (size_px <= 0)
                size_px = 14;
            // Density must be restored here too. Settings → Appearance persists
            // "appearance.density" and applies it live, but startup only ever
            // read family+size — so the user's Compact/Comfortable choice
            // silently reverted to Default on every relaunch.
            // apply_typography_and_density() is deliberately used instead of
            // apply_font() + apply_density(): it batches both into a single
            // qApp->setStyleSheet() (see its docs re: the Plasma 6 / Wayland
            // double-restyle crash, issue #247).
            QString density = r_density.is_ok() && !r_density.value().isEmpty() ? r_density.value() : "Default";
            tm.apply_typography_and_density(family, size_px, density);
            tm.apply_theme("Obsidian");
            LOG_INFO("App", "Theme: Obsidian, font: " + family + " " + size_s + ", density: " + density);
        }

        // Load persisted language and install the matching QTranslator before
        // any windows are shown — eliminates an English-flash on first paint
        // when the user has previously chosen another language.
        fincept::i18n::LanguageManager::instance().initialize();

        // Load persisted display-currency preference so the symbol is correct
        // on first paint of any calculator/analytics surface.
        fincept::currency::CurrencyManager::instance().initialize();
    }

    // Open cache database (non-fatal if fails)
    QString cache_path = fincept::AppPaths::data() + "/cache.db";
    auto cache_result = fincept::CacheDatabase::instance().open(cache_path);
    if (cache_result.is_err()) {
        LOG_WARN("App", "Cache DB failed (non-fatal): " + QString::fromStdString(cache_result.error()));
    }

    // Assign a unique session ID so ScreenStateManager can tag each state write.
    // This lets us distinguish cross-session restores from same-session saves.
    {
        const QString sid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        fincept::ScreenStateManager::instance().set_session_id(sid);
        LOG_INFO("App", "Session ID: " + sid);
    }

    LOG_INFO("App", "Checking settings for legacy migration...");
    // One-time migration: copy settings from old DB (Local\FinceptTerminal\fincept_settings.db)
    // to new DB (Roaming\Fincept\FinceptTerminal\fincept.db) if the new DB has no settings yet.
    {
        LOG_INFO("App", "Querying settings...");
        // "Is the new DB already populated?" — ask the table itself. This used
        // to probe for the `fincept_session` row, which the v052 migration now
        // deletes: that sentinel would read as empty on every launch and re-run
        // the legacy copy forever.
        bool new_db_empty = true;
        {
            QSqlQuery probe(fincept::Database::instance().raw_db());
            if (probe.exec("SELECT 1 FROM settings LIMIT 1") && probe.next())
                new_db_empty = false;
        }
        LOG_INFO("App", "Settings query done");
        if (new_db_empty) {
            QString local_base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
            // AppLocalDataLocation = .../Local/Fincept/FinceptTerminal — strip to .../Local/FinceptTerminal
            QString old_db_path = local_base.section('/', 0, -3) + "/FinceptTerminal/fincept_settings.db";
            if (!QFile::exists(old_db_path)) {
                // Try without the org subfolder
                old_db_path = local_base.replace("Fincept/FinceptTerminal", "FinceptTerminal") + "/fincept_settings.db";
            }
            if (QFile::exists(old_db_path)) {
                QSqlDatabase old_db = QSqlDatabase::addDatabase("QSQLITE", "legacy_migration");
                old_db.setDatabaseName(old_db_path);
                if (old_db.open()) {
                    QSqlQuery src(old_db);
                    if (src.exec("SELECT key, value FROM settings")) {
                        int count = 0;
                        while (src.next()) {
                            fincept::SettingsRepository::instance().set(src.value(0).toString(),
                                                                        src.value(1).toString(), "migrated");
                            ++count;
                        }
                        LOG_INFO("App", QString("Migrated %1 settings from legacy DB").arg(count));
                    }
                    old_db.close();
                }
                QSqlDatabase::removeDatabase("legacy_migration");
            }
        }
    }

    LOG_INFO("App", "Starting session manager...");
    // Start session
    fincept::SessionManager::instance().start_session();

    // Force the ReportBuilderService singleton onto the main thread before
    // MCP tools register — tools route into it via QMetaObject::invokeMethod
    // with BlockingQueuedConnection from worker threads, so the service must
    // already exist with main-thread affinity.
    (void)fincept::services::ReportBuilderService::instance();

    // Initialize MCP tool system — registers all internal tools and starts
    // external MCP servers in the background (non-blocking).
    //
    // ~925 tools across 38 modules, each building nested QJsonObject schemas.
    // Nothing on the first frame consumes any of it, so it is deferred to the
    // first event-loop turn — EXCEPT for --selftest-tools / --dump-tools, which
    // read the registry synchronously and early-return below without ever
    // reaching QApplication::exec(). Those two must have it up front.
    {
        bool tools_needed_synchronously = false;
        for (int i = 1; i < argc; ++i) {
            if (qstrcmp(argv[i], "--selftest-tools") == 0 || qstrcmp(argv[i], "--selftest-llm-tools") == 0 ||
                qstrcmp(argv[i], "--dump-tools") == 0)
                tools_needed_synchronously = true;
        }
        if (tools_needed_synchronously)
            fincept::mcp::initialize_all_tools();
        else
            QTimer::singleShot(0, qApp, []() { fincept::mcp::initialize_all_tools(); });
    }

    // ── Headless tool-system self-test / catalog dump ────────────────────────
    // Runs after the real tool registration above but before any window or
    // network init, so it exercises exactly what ships. Exits without starting
    // the GUI — used by the dev loop and CI to measure tool retrieval recall
    // and registry integrity (no LLM / API key required).
    // Single source of truth for the headless self-test suites. The dispatch below
    // AND --selftest-list both read this table, so CI enumerates the suites at
    // runtime instead of hard-coding them.
    //
    // A hand-maintained list is exactly how --selftest-live-table came to be
    // dispatched here while running in no workflow at all: main.cpp had 10 flags,
    // the CI loop listed 9, and nothing connected the two. Add a suite here and it
    // is picked up by every job automatically.
    //
    // --dump-tools is deliberately NOT in this table: it is a diagnostic dump, not
    // a pass/fail suite, and CI must not run it as one.
    struct SelftestSuite {
        const char* flag;
        int (*run)();
    };
    static constexpr SelftestSuite kSelftestSuites[] = {
        {"--selftest-tools", &fincept::mcp::run_tool_selftest},
        {"--selftest-llm-tools", &fincept::mcp::run_provider_tool_format_selftest},
        {"--selftest-feeds", &fincept::feeds::run_feed_selftest},
        {"--selftest-dock-layout", &fincept::layout::run_dock_layout_selftest},
        {"--selftest-live-table", &fincept::ui::run_live_table_selftest},
        {"--selftest-fno-algo", &fincept::algo::fno::run_fno_algo_selftest},
        {"--selftest-universe-scan", &fincept::algo::run_universe_scan_selftest},
        {"--selftest-paper", &fincept::trading::run_paper_trading_selftest},
        {"--selftest-portfolio-monitor", &fincept::trading::run_portfolio_monitor_selftest},
        {"--selftest-portfolio-replication", &fincept::trading::replication::run_portfolio_replication_selftest},
        {"--selftest-arena", &fincept::arena::run_arena_selftest},
    };

    for (int i = 1; i < argc; ++i) {
        // Machine-readable suite enumeration for CI: one flag per line, exit 0.
        if (qstrcmp(argv[i], "--selftest-list") == 0) {
            for (const auto& suite : kSelftestSuites)
                std::printf("%s\n", suite.flag);
            std::fflush(stdout);
            return 0;
        }
        if (qstrcmp(argv[i], "--dump-tools") == 0)
            return fincept::mcp::dump_tools_json();
        for (const auto& suite : kSelftestSuites) {
            if (qstrcmp(argv[i], suite.flag) == 0)
                return suite.run();
        }
    }

    // Start the scan-watch background service. Runs after Database::open() (which
    // applies the scan_watches migration) and after the headless self-test
    // early-returns above so it is skipped on --selftest-tools / --dump-tools.
    // Placed before the Python-setup branch so both GUI paths (setup screen and
    // normal startup) start it exactly once. Candle fetching is native C++ (broker
    // REST / native Yahoo), so it does not require the Python env to be ready.
    fincept::algo::ScanMonitor::instance().start();

    // Centralized paper mark-to-market + order matching. Runs independent of which
    // screen is open so paper positions (equity AND F&O) keep their P&L live and
    // resting limit/stop/SL-TP orders fill continuously — not only while the
    // Equity tab is focused. Placed alongside ScanMonitor (after the self-test
    // early-returns, so it stays off in headless --selftest runs).
    //
    // Deferred: start() calls resync() inline, which runs pt_get_positions()
    // (SQLite) once per active paper account — pre-window main-thread work with
    // nothing on the first frame depending on it. The singleShot still sits
    // after the self-test early-returns, so the headless guard is preserved.
    QTimer::singleShot(0, qApp, []() { fincept::trading::PaperMarkService::instance().start(); });

    // Native desktop notifications (Win toast / macOS Notification Center / Linux
    // libnotify) via a tray icon — also surfaces every in-app ToastService toast.
    fincept::ui::DesktopNotifier::instance().init();

    // ── Python environment check ─────────────────────────────────────────────
    // check_status() fast path (sentinel + markers present) is synchronous and
    // cheap. The slow path (first run) can spawn processes — but at this point
    // no window is visible yet so the brief block is acceptable. The SetupScreen
    // itself offloads prefill_completed_steps() to a background thread (P1).
    auto setup_status = fincept::python::PythonSetupManager::instance().check_status();

    // --smoke-test: CI/clean-machine screen-construction walk. Force the normal
    // boot path (we need a real WindowFrame + router to navigate every screen),
    // and skip the Python first-run SetupScreen — the smoke test only verifies
    // that screens CONSTRUCT on the bundled runtime, not that data fetches work.
    // The actual walk is scheduled just after the primary window is shown.
    bool smoke_mode = false;
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--smoke-test") == 0)
            smoke_mode = true;
    }
    if (smoke_mode) {
        LOG_INFO("Smoke", "Smoke-test mode — forcing normal boot, skipping setup/recovery");
        setup_status.needs_setup = false;
    }

    if (setup_status.needs_setup) {
        LOG_INFO("App", "Python environment not ready — showing setup screen");

        // Use QPointer so the setup_complete lambda is safe against double-fire
        // (e.g. user somehow triggers it twice before the window is hidden).
        auto* setup_screen = new fincept::screens::SetupScreen;
        QPointer<fincept::screens::SetupScreen> screen_guard(setup_screen);
        setup_screen->setWindowTitle("Fincept Terminal — First-Time Setup");
        setup_screen->resize(800, 600);
        setup_screen->show();

        // When setup completes, hide setup screen and launch main window.
        // The connection uses Qt::SingleShotConnection (Qt 6.0+) so the lambda
        // fires exactly once even if setup_complete is somehow emitted twice.
        QObject::connect(
            setup_screen, &fincept::screens::SetupScreen::setup_complete, [&app, &instance_lock, screen_guard]() {
                if (!screen_guard)
                    return; // already cleaned up — ignore
                screen_guard->hide();
                screen_guard->deleteLater();

                fincept::KeyConfigManager::instance(); // init before WindowFrame registers shortcuts

                // Phase 6 final: if the previous session ended uncleanly and a
                // workspace snapshot is available, give the user the option to
                // restore. On accept, WorkspaceShell::apply already constructs
                // the frames it needs — we skip our own primary-window creation
                // path. On skip (or no recovery available), fall through.
                bool recovered = false;
                if (auto* recovery = fincept::TerminalShell::instance().crash_recovery();
                    recovery && recovery->needs_recovery()) {
                    fincept::screens::CrashRecoveryDialog dlg(recovery,
                                                              fincept::TerminalShell::instance().snapshot_ring());
                    dlg.exec();
                    recovered = dlg.was_restored();
                }

                if (!recovered) {
                    // Single primary window by default — see the matching no-setup
                    // path below for the full rationale. Extra windows stay an
                    // explicit user action ("New Window" / Ctrl+Shift+N / tear-off).
                    const QList<int> saved_ids = fincept::SessionManager::instance().load_window_ids();
                    const int primary_id = saved_ids.isEmpty() ? 0 : saved_ids.first();
                    auto* window = new fincept::WindowFrame(primary_id);
                    window->setAttribute(Qt::WA_DeleteOnClose);
                    window->show();
                    start_post_boot_work(window, recovered);

                    // Enterprise promo, once the frame has painted. Self-
                    // suppresses when the user ticked "Don't show this again"
                    // and when the platform has no window system.
                    QPointer<fincept::WindowFrame> promo_target = window;
                    QTimer::singleShot(1200, &app, [promo_target]() {
                        fincept::ui::UpgradeDialog::maybe_show_at_startup(promo_target.data());
                    });
                }

                // Wire new-window handler + Launchpad surface now that the
                // primary window exists. Single source of truth — see
                // wire_app_lifecycle() at the top of this file.
                wire_app_lifecycle(app, instance_lock);

                if (!fincept::ai_chat::LlmService::instance().is_configured())
                    LOG_WARN(
                        "App",
                        "LLM provider not configured — AI chat will prompt user to configure Settings → LLM Config");

                // Warm agent discovery cache (same reason as the main path).
                QTimer::singleShot(0, &app, []() { fincept::services::AgentService::instance().discover_agents(); });

                LOG_INFO("App", "Application ready (after setup)");
            });

        return app.exec();
    }

    // Ensure KeyConfigManager is initialized before WindowFrame registers shortcuts
    fincept::KeyConfigManager::instance();

    // Phase 6 final: offer crash recovery before constructing the primary
    // window. If the user accepts and restoration succeeds, WorkspaceShell
    // has already built the frames it needs from the snapshot — we skip
    // both the primary-window creation and the SessionManager-based
    // secondary-window restoration paths to avoid duplicating windows.
    bool recovered = false;
    if (auto* recovery = fincept::TerminalShell::instance().crash_recovery();
        !smoke_mode && recovery && recovery->needs_recovery()) {
        fincept::screens::CrashRecoveryDialog dlg(recovery, fincept::TerminalShell::instance().snapshot_ring());
        dlg.exec();
        recovered = dlg.was_restored();
    }

    // Restore a SINGLE primary window at startup. The previous session may
    // have had several windows spread across multiple monitors, but auto-
    // reopening all of them surprised multi-monitor users — every launch
    // popped a second terminal on the second screen. Opening additional
    // windows stays an EXPLICIT action (toolbar "New Window", Ctrl+Shift+N,
    // the Launchpad button, tear-off), consistent with the single-instance
    // relaunch policy in wire_app_lifecycle(). We reopen the lowest saved
    // window_id (the primary) so its geometry + dock layout come back; the
    // user spawns extra windows on demand. closeEvent self-heals the saved
    // id set to the surviving windows, so this converges to [primary] cleanly.
    if (!recovered) {
        const QList<int> saved_ids = fincept::SessionManager::instance().load_window_ids();
        const int primary_id = saved_ids.isEmpty() ? 0 : saved_ids.first();
        auto* primary = new fincept::WindowFrame(primary_id);
        primary->setAttribute(Qt::WA_DeleteOnClose);
        primary->show();
        start_post_boot_work(primary, recovered);

        // Smoke test: once the window has painted, walk every screen and exit
        // with the result. Deferred so the shell + router are fully wired. The
        // CI job runs this with Qt/VS stripped from PATH, so a missing bundled
        // runtime (DLL, plugin, or data file like QtWebEngineProcess.exe) shows
        // up as a hard process abort or a non-constructing screen here — exactly
        // the class of failure the static dependency gate cannot detect.
        // Enterprise promo, once the frame has painted. Never in --smoke-test:
        // that run walks every screen headlessly and a modal would block it.
        if (!smoke_mode) {
            QPointer<fincept::WindowFrame> promo_target = primary;
            QTimer::singleShot(1200, &app, [promo_target]() {
                fincept::ui::UpgradeDialog::maybe_show_at_startup(promo_target.data());
            });
        }

        if (smoke_mode) {
            QPointer<fincept::WindowFrame> w = primary;
            QTimer::singleShot(2500, &app, [w]() {
                const int rc = fincept::run_screen_smoke_test(w ? w->dock_router() : nullptr);
                std::fprintf(stderr, "[Smoke] exit %d\n", rc);
                std::fflush(stderr);
                QCoreApplication::exit(rc);
            });
        }
    }

    // Wire new-window handler + Launchpad surface — see wire_app_lifecycle()
    // at the top of this file for the contract.
    wire_app_lifecycle(app, instance_lock);

    // If requirements files changed (app update), sync packages in background
    // without blocking the user. Connect setup_complete so failures are logged
    // (no SetupScreen in this path, so we only log — don't show UI).
    if (setup_status.needs_package_sync) {
        LOG_INFO("App", "Requirements changed — syncing packages in background");
        auto& mgr = fincept::python::PythonSetupManager::instance();
        QObject::connect(
            &mgr, &fincept::python::PythonSetupManager::setup_complete, &mgr,
            [](bool success, const QString& error) {
                if (success)
                    LOG_INFO("App", "Background package sync completed successfully");
                else
                    LOG_WARN("App", "Background package sync failed (non-fatal): " + error);
            },
            Qt::SingleShotConnection);
        mgr.run_setup();
    }

    // Deferred, and NOT just to save a few ms: is_configured() runs
    // LlmService::ensure_config(), which on its FIRST call bakes the MCP
    // tool-category discovery hint into the system prompt and caches it in a
    // function-local static for the process lifetime. Tool registration is now
    // deferred (see the initialize_all_tools singleShot above), so calling this
    // inline would permanently cache a hint built against an empty registry.
    // Posting it here keeps it strictly after that turn.
    QTimer::singleShot(0, &app, []() {
        if (!fincept::ai_chat::LlmService::instance().is_configured())
            LOG_WARN("App",
                     "LLM provider not configured — AI chat will prompt user to configure Settings → LLM Config");
    });

    // Warm the agent discovery cache on startup. This populates
    // AgentService::cached_agents() so any screen that lists agents
    // (Agent Config, Portfolio → Agent Runner, Node Editor) shows the
    // full finagent_core set immediately instead of falling back to the
    // much smaller DB-only list. Run deferred so Python is fully ready.
    QTimer::singleShot(0, &app, []() { fincept::services::AgentService::instance().discover_agents(); });

    LOG_INFO("App", "Application ready");
    return app.exec();
}
