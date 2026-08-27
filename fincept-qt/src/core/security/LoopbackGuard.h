#pragma once
// Request-origin validation for the terminal's loopback HTTP listeners.
//
// Four servers in this codebase bind 127.0.0.1 and hand out or accept something
// sensitive: the broker OAuth redirect catcher (trading/auth/RedirectServer),
// the wallet connect bridge and the wallet transaction bridge
// (services/wallet/*), and the agent MCP bridge (mcp/TerminalMcpBridge).
// Binding loopback keeps other hosts
// out, but it does NOT keep out the user's own browser — and that is the actual
// attacker here:
//
//   * Any web page the user has open can issue a cross-origin subresource
//     request to http://127.0.0.1:<port>/... An `<img src>` or a no-cors
//     `fetch()` is a CORS *simple* request: the page cannot read the response,
//     but the request still arrives and is still acted on. Against a fixed,
//     guessable port (5010 / 5011) that is enough to feed an attacker-chosen
//     OAuth token into the terminal, or to race the real redirect.
//   * A page can also point a hostname it controls at 127.0.0.1 (DNS
//     rebinding) and then talk to the listener as same-origin, defeating any
//     origin check that only looks at the socket address.
//
// Two header checks close both holes, and neither costs anything legitimate:
//
//   Host      — must be our own loopback authority. A rebound request carries
//               the attacker's hostname, so this rejects it outright.
//   Sec-Fetch-* — sent by every current browser and not forgeable by page JS.
//               A genuine OAuth redirect or a wallet return trip is a top-level
//               navigation (`Sec-Fetch-Mode: navigate`). An injected `<img>`,
//               `<script>`, `fetch()` or XHR is not. Rejecting cross-site
//               non-navigations kills the injection while leaving the real
//               flows untouched. Requests from our own served page are
//               `Sec-Fetch-Site: same-origin` and always pass.
//
// A non-browser client (curl, a local script, an older browser) sends no
// Sec-Fetch-* headers at all. Those requests are allowed through: the header
// set is a browser-only signal and every listener still has its own token /
// nonce / single-use gate behind this. This is defence in depth, not the only
// lock on the door.
//
// Header-only so all five listeners can share it without a new translation
// unit.

#include <QByteArray>
#include <QString>

namespace fincept::security {

/// Case-insensitive lookup of an HTTP header value in a raw request buffer.
/// `raw_request` may be the whole request or just the head; parsing stops at
/// the blank line that ends the header block. Returns an empty QByteArray when
/// the header is absent.
inline QByteArray http_header_value(const QByteArray& raw_request, const char* name) {
    const QByteArray needle = QByteArray(name).toLower();
    qsizetype pos = raw_request.indexOf('\n');
    if (pos < 0)
        return {};
    ++pos; // skip the request line
    while (pos < raw_request.size()) {
        qsizetype eol = raw_request.indexOf('\n', pos);
        if (eol < 0)
            eol = raw_request.size();
        QByteArray line = raw_request.mid(pos, eol - pos);
        if (line.endsWith('\r'))
            line.chop(1);
        if (line.isEmpty())
            break; // end of header block
        const qsizetype colon = line.indexOf(':');
        if (colon > 0 && line.left(colon).trimmed().toLower() == needle)
            return line.mid(colon + 1).trimmed();
        pos = eol + 1;
    }
    return {};
}

/// True when `authority` is one of our own loopback authorities for `port`.
inline bool is_loopback_authority(const QByteArray& authority, quint16 port) {
    if (authority.isEmpty())
        return false;
    const QByteArray suffix = ':' + QByteArray::number(port);
    if (!authority.endsWith(suffix))
        return false;
    const QByteArray host = authority.left(authority.size() - suffix.size()).toLower();
    return host == "127.0.0.1" || host == "localhost" || host == "[::1]";
}

/// True when `origin` is one of our own loopback origins for `port`.
/// An absent origin, or the opaque `null` origin, is not our origin.
inline bool is_own_loopback_origin(const QByteArray& origin, quint16 port) {
    QByteArray o = origin.trimmed();
    if (o.isEmpty() || o.toLower() == "null")
        return false;
    if (o.startsWith("http://"))
        o = o.mid(7);
    else if (o.startsWith("https://"))
        o = o.mid(8);
    else
        return false;
    return is_loopback_authority(o, port);
}

struct LoopbackCheck {
    bool allowed = true;
    QString reason; ///< populated only when allowed == false; safe to log
};

/// Validate a raw HTTP request against the two browser-facing threats above.
///
/// `port` is the port this listener is actually bound to.
/// `allow_cross_site_navigation` must be true for the OAuth / wallet callbacks,
/// which legitimately arrive as a top-level navigation from a third-party site.
/// Set it false for a listener that should only ever be talked to by the page
/// it served itself.
inline LoopbackCheck check_loopback_request(const QByteArray& raw_request, quint16 port,
                                            bool allow_cross_site_navigation = true) {
    // ── Host: blocks DNS rebinding ──────────────────────────────────────────
    const QByteArray host = http_header_value(raw_request, "Host");
    if (host.isEmpty())
        return {false, QStringLiteral("missing Host header")};
    if (!is_loopback_authority(host, port))
        return {false, QStringLiteral("unexpected Host '%1'").arg(QString::fromLatin1(host))};

    // ── Fetch metadata: blocks cross-origin subresource injection ───────────
    const QByteArray site = http_header_value(raw_request, "Sec-Fetch-Site").toLower();
    if (!site.isEmpty() && site != "same-origin" && site != "none") {
        const QByteArray mode = http_header_value(raw_request, "Sec-Fetch-Mode").toLower();
        if (!(allow_cross_site_navigation && mode == "navigate")) {
            return {false, QStringLiteral("cross-site request (Sec-Fetch-Site: %1, Sec-Fetch-Mode: %2)")
                               .arg(QString::fromLatin1(site), QString::fromLatin1(mode))};
        }
    }

    // ── Origin: a cross-origin CORS request never belongs here ──────────────
    // Top-level GET navigations send no Origin at all, so the real callbacks
    // are unaffected. Anything that DOES send one must be our own page.
    const QByteArray origin = http_header_value(raw_request, "Origin");
    if (!origin.isEmpty() && origin.toLower() != "null" && !is_own_loopback_origin(origin, port)) {
        return {false, QStringLiteral("cross-origin request from '%1'").arg(QString::fromLatin1(origin))};
    }

    return {true, {}};
}

} // namespace fincept::security
