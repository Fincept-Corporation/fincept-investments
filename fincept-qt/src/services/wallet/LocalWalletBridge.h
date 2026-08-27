#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

class QTcpServer;
class QTcpSocket;

namespace fincept::wallet {

/// Tiny single-purpose HTTP server bound to 127.0.0.1 used during wallet-connect.
///
/// Lifecycle:
///   1. start(nonce) — picks a random ephemeral port, listens on loopback only,
///      generates a single-use connect_token, returns the URL the user's
///      browser should open.
///   2. The browser hits GET /connect?token=...   → we serve connect.html.
///   3. Browser-side JS prompts the wallet, signs the nonce, then POSTs the
///      payload {pubkey, signature, label} to /callback?token=....
///   4. The bridge fires `connect_payload(...)`, replies 200 to the browser,
///      and stops listening.
///   5. After timeout_seconds with no callback, the server stops and emits
///      `timed_out()`.
///
/// Security:
///   - listens only on QHostAddress::LocalHost (no LAN, no public).
///   - random port, random connect_token (32 bytes hex) bound to one connect.
///   - any request with a missing/wrong token is rejected with 403; the token
///     is compared in constant time.
///   - Host + Sec-Fetch-* validation rejects DNS rebinding and cross-origin
///     subresource injection from any other page the user has open
///     (core/security/LoopbackGuard.h).
///   - Access-Control-Allow-Origin names our own loopback origin, never `*`.
///   - server self-destructs after success or timeout.
///   - never speaks HTTP/2, never serves anything besides the embedded connect
///     page and a tiny 200 OK on /callback.
class LocalWalletBridge : public QObject {
    Q_OBJECT
  public:
    explicit LocalWalletBridge(QObject* parent = nullptr);
    ~LocalWalletBridge() override;

    /// Spins up the server. Returns the URL to open in the user's browser, or
    /// an empty string if listen() failed.
    QString start(const QByteArray& nonce_hex, int timeout_seconds = 120);

    /// Stops the server immediately. Idempotent.
    void stop();

    /// True if the server is currently listening for the callback.
    bool is_listening() const noexcept;

  signals:
    /// Fired once when /callback receives a valid POST payload.
    void connect_payload(QString pubkey_b58, QString signature_b58, QString wallet_label);
    /// Fired if no callback arrived within timeout_seconds.
    void timed_out();
    /// Fired on internal errors (port bind failure, parse error, etc.)
    void bridge_error(QString message);

  private:
    void on_new_connection();
    void handle_request(QTcpSocket* socket, const QByteArray& request_line, const QByteArray& body,
                        const QByteArray& path, const QByteArray& method);
    QByteArray render_connect_page() const;
    void write_response(QTcpSocket* socket, int status, const QByteArray& content_type, const QByteArray& body);
    /// Port we are actually bound to, or 0 when not listening.
    quint16 own_port() const noexcept;
    /// "http://127.0.0.1:<port>" — the only origin allowed to talk to us.
    QByteArray own_origin() const;

    QTcpServer* server_ = nullptr;
    QByteArray nonce_hex_;
    QByteArray connect_token_;
    int timer_id_ = 0;

  protected:
    void timerEvent(QTimerEvent* e) override;
};

} // namespace fincept::wallet
