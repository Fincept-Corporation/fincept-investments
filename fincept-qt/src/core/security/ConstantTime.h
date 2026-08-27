#pragma once
// Constant-time comparison helpers for secrets.
//
// Any comparison of a user- or network-supplied value against a secret (PIN
// hash, bridge token, OAuth state nonce) must not short-circuit on the first
// differing byte: the timing difference leaks how many leading bytes were
// correct, which turns an infeasible guess into a byte-at-a-time search.
//
// Header-only so the loopback HTTP bridges (services/wallet/*, mcp/*) and the
// broker OAuth catcher share one implementation instead of each rolling their
// own.

#include <QByteArray>
#include <QString>

namespace fincept::security {

/// Returns true iff `a` and `b` are the same length AND every byte matches.
/// The XOR-accumulate loop touches every byte even on mismatch so execution
/// time does not reveal the first differing index.
///
/// Length is compared first and therefore still observable — that is inherent
/// and harmless for the fixed-width secrets we compare.
inline bool constant_time_equals(const QByteArray& a, const QByteArray& b) {
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    const qsizetype n = a.size();
    for (qsizetype i = 0; i < n; ++i)
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return diff == 0;
}

/// UTF-8 overload for QString secrets (tokens, nonces).
inline bool constant_time_equals(const QString& a, const QString& b) {
    return constant_time_equals(a.toUtf8(), b.toUtf8());
}

} // namespace fincept::security
