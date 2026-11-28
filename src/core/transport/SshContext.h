// SPDX-License-Identifier: GPL-3.0-or-later
// Internal: rules for sharing one non-blocking libssh2 session between the
// shell, port-forwarding tunnels and SFTP.
//
// libssh2 keeps a single partially-sent packet per session. When a call
// returns EAGAIN while that packet is pending (outbound blocked), the SAME
// call must be repeated before anything else sends: libssh2 1.11.0 would
// otherwise flush the old packet and report the new one as sent without
// sending it, and 1.11.1 refuses every other sender forever. The caller that
// left the packet pending therefore becomes the `owner` and is the only one
// allowed to talk to libssh2 until its call completes.
//
// Opening a channel (session, direct-tcpip, SFTP) and global requests keep
// their progress in session-wide state too, so only one may be in flight.
#pragma once

#include "SocketUtil.h"

#include <QElapsedTimer>

#include <libssh2.h>

namespace termx::ssh {

struct Context {
    LIBSSH2_SESSION *session = nullptr;
    net::socket_t sock = net::kInvalidSocket;
    const void *owner = nullptr;
    const void *openOwner = nullptr;
    QElapsedTimer ownerSince;

    bool outboundBlocked() const
    {
        return session && (libssh2_session_block_directions(session) & LIBSSH2_SESSION_BLOCK_OUTBOUND) != 0;
    }
    bool mayRun(const void *actor) const { return !owner || owner == actor; }

    // Book-keeping after `actor` made a libssh2 call that returned `rc`.
    void after(const void *actor, long rc)
    {
        if (rc == LIBSSH2_ERROR_EAGAIN && outboundBlocked()) {
            if (owner != actor) {
                owner = actor;
                ownerSince.start();
            }
        } else if (owner == actor) {
            owner = nullptr;
        }
    }
    // Owner stuck for this long with the socket writable = the session is wedged.
    bool ownerStalled(qint64 ms) const { return owner && ownerSince.isValid() && ownerSince.elapsed() > ms; }

    bool tryLockOpen(const void *actor)
    {
        if (openOwner && openOwner != actor)
            return false;
        openOwner = actor;
        return true;
    }
    void unlockOpen(const void *actor)
    {
        if (openOwner == actor)
            openOwner = nullptr;
    }
    // Forget an actor that is going away.
    void forget(const void *actor)
    {
        if (owner == actor)
            owner = nullptr;
        unlockOpen(actor);
    }
};

inline QString lastError(LIBSSH2_SESSION *s)
{
    char *msg = nullptr;
    int len = 0;
    libssh2_session_last_error(s, &msg, &len, 0);
    return msg && len > 0 ? QString::fromUtf8(msg, len) : QStringLiteral("unknown SSH error");
}

} // namespace termx::ssh
