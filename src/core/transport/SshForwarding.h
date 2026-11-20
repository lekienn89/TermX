// SPDX-License-Identifier: GPL-3.0-or-later
// Internal: local (-L), remote (-R) and dynamic SOCKS (-D) port forwarding
// over an authenticated libssh2 session. Runs on the SSH I/O thread.
#pragma once

#include "SocksProtocol.h"
#include "SshContext.h"
#include "session/PortForward.h"

#include <QElapsedTimer>

#include <functional>
#include <memory>
#include <vector>

namespace termx::ssh {

class ForwardEngine {
public:
    using StatusFn = std::function<void(const QVector<ForwardStatus> &)>;
    using NoticeFn = std::function<void(const QString &)>;

    static constexpr int kMaxTunnels = 256;
    static constexpr int kBufferCap = 256 * 1024;   // per direction, per tunnel

    ForwardEngine(Context &ctx, StatusFn status, NoticeFn notice);
    ~ForwardEngine();
    ForwardEngine(const ForwardEngine &) = delete;
    ForwardEngine &operator=(const ForwardEngine &) = delete;

    void add(quint64 id, const PortForward &rule);
    void remove(quint64 id);

    // Does all the work that can be done without blocking. True if anything moved.
    bool step();
    // Sockets to wait on.
    void collectPoll(std::vector<net::PollItem> &items) const;
    // libssh2 already holds data / EOF for a tunnel that has room for it.
    bool hasBufferedData() const;
    // Something is waiting for a libssh2 reply that another channel may pull in.
    bool waitingOnSession() const;
    // Some tunnel or remote listener reads from the SSH session.
    bool readsSession() const;
    // Publishes status now if it changed (throttled unless `force`).
    void publish(bool force = false);
    // Session is going away: close local sockets; channels die with the session.
    void abandon();

    int forwardCount() const { return int(m_forwards.size()); }

private:
    struct Forward;
    struct Tunnel;

    bool stepForward(Forward &f);
    bool stepTunnel(Tunnel &t);
    bool tunnelChannelIo(Tunnel &t);
    bool tunnelSocketIo(Tunnel &t);
    void startClosing(Tunnel &t, const QString &why = QString());
    Forward *find(quint64 id);
    void markDirty() { m_dirty = true; }

    Context &m_ctx;
    StatusFn m_status;
    NoticeFn m_notice;
    std::vector<std::unique_ptr<Forward>> m_forwards;
    std::vector<std::unique_ptr<Tunnel>> m_tunnels;
    bool m_dirty = true;
    QElapsedTimer m_lastPublish;
    QByteArray m_buf;
};

} // namespace termx::ssh
