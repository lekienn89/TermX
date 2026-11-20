// SPDX-License-Identifier: GPL-3.0-or-later
#include "SshForwarding.h"

#include <algorithm>

namespace termx::ssh {

namespace {
constexpr int kChunk = 32 * 1024;
constexpr qint64 kHandshakeTimeoutMs = 30000;
constexpr qint64 kConnectTimeoutMs = 15000;
constexpr qint64 kCloseGiveUpMs = 20000;
constexpr int kPublishIntervalMs = 300;
} // namespace

struct ForwardEngine::Forward {
    enum class Phase { New, Listening, Dead };
    quint64 id = 0;
    PortForward rule;
    ForwardStatus status;
    Phase phase = Phase::New;
    bool removing = false;
    net::socket_t listenSock = net::kInvalidSocket;
    LIBSSH2_LISTENER *listener = nullptr;
    std::vector<net::Address> dest;   // Remote: where accepted connections go
    bool limitWarned = false;
};

struct ForwardEngine::Tunnel {
    enum class St { Socks, Opening, Connecting, Open, Closing, Dead };
    quint64 fwdId = 0;
    St st = St::Opening;
    net::socket_t sock = net::kInvalidSocket;
    LIBSSH2_CHANNEL *ch = nullptr;
    bool viaSocks = false;
    bool cancelled = false;  // stop requested while a channel open was in flight
    SocksHandshake socks;
    QByteArray host;
    int port = 0;
    QByteArray peerHost;
    int peerPort = 0;
    QByteArray toChannel;   // read from the local socket, not yet sent to the server
    QByteArray toSocket;    // received from the server, not yet written locally
    bool sockEof = false;   // local side finished sending
    bool eofSent = false;   // ... and the server was told
    bool chanEof = false;   // server side finished sending
    bool sockShut = false;  // ... and the local side was told
    int phase = 0;          // Open: libssh2 call to resume (0 write, 1 read, 2 eof)
    size_t addrIndex = 0;
    QElapsedTimer since;
};

ForwardEngine::ForwardEngine(Context &ctx, StatusFn status, NoticeFn notice)
    : m_ctx(ctx), m_status(std::move(status)), m_notice(std::move(notice)), m_buf(kChunk * 2, Qt::Uninitialized)
{
}

ForwardEngine::~ForwardEngine()
{
    abandon();
}

void ForwardEngine::abandon()
{
    for (auto &t : m_tunnels) {
        net::closeSocket(t->sock);
        t->sock = net::kInvalidSocket;
        m_ctx.forget(t.get());
    }
    m_tunnels.clear();
    for (auto &f : m_forwards) {
        net::closeSocket(f->listenSock);
        f->listenSock = net::kInvalidSocket;
        m_ctx.forget(f.get());
    }
    m_forwards.clear();
}

ForwardEngine::Forward *ForwardEngine::find(quint64 id)
{
    for (auto &f : m_forwards)
        if (f->id == id)
            return f.get();
    return nullptr;
}

void ForwardEngine::add(quint64 id, const PortForward &rule)
{
    auto f = std::make_unique<Forward>();
    f->id = id;
    f->rule = rule;
    f->status.id = id;
    f->status.rule = rule;
    f->status.state = ForwardStatus::State::Starting;
    auto failed = [&](const QString &why) {
        f->status.state = ForwardStatus::State::Failed;
        f->status.error = why;
        f->phase = Forward::Phase::Dead;
        if (m_notice)
            m_notice(QStringLiteral("Port forwarding %1 failed: %2").arg(rule.option(), why));
    };

    QString err;
    if (!rule.isValid(&err)) {
        failed(err);
    } else if (rule.type == PortForward::Type::Remote) {
        if (!net::resolve(rule.destHost, uint16_t(rule.destPort), false, f->dest, err))
            failed(err);
    } else {
        uint16_t bound = 0;
        f->listenSock = net::tcpListen(rule.effectiveBindAddress(), uint16_t(rule.bindPort), err, &bound);
        if (f->listenSock == net::kInvalidSocket) {
            failed(err);
        } else {
            f->phase = Forward::Phase::Listening;
            f->status.state = ForwardStatus::State::Active;
            f->status.boundPort = bound;
        }
    }
    m_forwards.push_back(std::move(f));
    markDirty();
    publish(true);
}

void ForwardEngine::remove(quint64 id)
{
    Forward *f = find(id);
    if (!f)
        return;
    f->removing = true;
    for (auto &t : m_tunnels) {
        if (t->fwdId != id || t->st == Tunnel::St::Closing || t->st == Tunnel::St::Dead)
            continue;
        // A channel open in flight keeps session-wide state: let it finish first.
        if (t->st == Tunnel::St::Opening && m_ctx.openOwner == t.get())
            t->cancelled = true;
        else
            startClosing(*t);
    }
    markDirty();
}

void ForwardEngine::startClosing(Tunnel &t, const QString &why)
{
    if (!why.isEmpty()) {
        if (Forward *f = find(t.fwdId)) {
            f->status.error = why;
            markDirty();
        }
    }
    net::closeSocket(t.sock);
    t.sock = net::kInvalidSocket;
    t.st = Tunnel::St::Closing;
    t.since.restart();
}

bool ForwardEngine::step()
{
    bool progress = false;
    // Index loop: stepForward() may append tunnels, never forwards.
    for (size_t i = 0; i < m_forwards.size(); ++i)
        progress |= stepForward(*m_forwards[i]);
    for (size_t i = 0; i < m_tunnels.size(); ++i)
        progress |= stepTunnel(*m_tunnels[i]);

    // Reap.
    for (auto it = m_tunnels.begin(); it != m_tunnels.end();) {
        if ((*it)->st == Tunnel::St::Dead) {
            if (Forward *f = find((*it)->fwdId))
                f->status.openConnections = std::max(0, f->status.openConnections - 1);
            m_ctx.forget(it->get());
            it = m_tunnels.erase(it);
            markDirty();
        } else {
            ++it;
        }
    }
    for (auto it = m_forwards.begin(); it != m_forwards.end();) {
        Forward &f = **it;
        const bool tunnelsLeft = std::any_of(m_tunnels.begin(), m_tunnels.end(),
                                             [&](const auto &t) { return t->fwdId == f.id; });
        if (f.removing && f.phase == Forward::Phase::Dead && !tunnelsLeft) {
            m_ctx.forget(&f);
            it = m_forwards.erase(it);
            markDirty();
        } else {
            ++it;
        }
    }
    publish();
    return progress;
}

bool ForwardEngine::stepForward(Forward &f)
{
    if (f.phase == Forward::Phase::Dead)
        return false;
    const bool remote = f.rule.type == PortForward::Type::Remote;

    // A listen request in flight keeps session-wide state: finish it before stopping.
    const bool listenInFlight = remote && f.phase == Forward::Phase::New && m_ctx.openOwner == &f;
    if (f.removing && !listenInFlight) {
        if (remote && f.listener) {
            if (!m_ctx.tryLockOpen(&f) || !m_ctx.mayRun(&f))
                return false;
            const int rc = libssh2_channel_forward_cancel(f.listener);
            m_ctx.after(&f, rc);
            if (rc == LIBSSH2_ERROR_EAGAIN)
                return false;
            m_ctx.unlockOpen(&f);
            f.listener = nullptr;   // freed by libssh2
        }
        net::closeSocket(f.listenSock);
        f.listenSock = net::kInvalidSocket;
        f.phase = Forward::Phase::Dead;
        f.status.state = ForwardStatus::State::Stopped;
        markDirty();
        return true;
    }

    bool progress = false;
    if (remote && f.phase == Forward::Phase::New) {
        if (!m_ctx.tryLockOpen(&f) || !m_ctx.mayRun(&f))
            return false;
        const QByteArray host = f.rule.effectiveBindAddress().toUtf8();
        int bound = 0;
        f.listener = libssh2_channel_forward_listen_ex(m_ctx.session, host.constData(), f.rule.bindPort, &bound, 16);
        if (!f.listener) {
            const int rc = libssh2_session_last_errno(m_ctx.session);
            m_ctx.after(&f, rc);
            if (rc == LIBSSH2_ERROR_EAGAIN)
                return false;
            m_ctx.unlockOpen(&f);
            f.phase = Forward::Phase::Dead;
            f.status.state = ForwardStatus::State::Failed;
            f.status.error = QStringLiteral("The server refused to listen on %1:%2 (%3)")
                                 .arg(f.rule.effectiveBindAddress())
                                 .arg(f.rule.bindPort)
                                 .arg(lastError(m_ctx.session));
            if (m_notice)
                m_notice(QStringLiteral("Port forwarding %1 failed: %2").arg(f.rule.option(), f.status.error));
            markDirty();
            return true;
        }
        m_ctx.after(&f, 0);
        m_ctx.unlockOpen(&f);
        f.phase = Forward::Phase::Listening;
        f.status.state = ForwardStatus::State::Active;
        f.status.boundPort = f.rule.bindPort ? f.rule.bindPort : bound;
        if (f.rule.bindPort == 0 && m_notice)
            m_notice(QStringLiteral("Remote forwarding: the server listens on port %1").arg(bound));
        markDirty();
        progress = true;
    }
    if (f.phase != Forward::Phase::Listening || f.removing)
        return progress;

    const auto activeTunnels = [this] {
        return int(std::count_if(m_tunnels.begin(), m_tunnels.end(), [](const auto &t) {
            return t->st != Tunnel::St::Dead && t->st != Tunnel::St::Closing;
        }));
    };
    auto overLimit = [&] {
        if (activeTunnels() < kMaxTunnels)
            return false;
        if (!f.limitWarned && m_notice)
            m_notice(QStringLiteral("Port forwarding: more than %1 connections at once, new ones are refused").arg(kMaxTunnels));
        f.limitWarned = true;
        return true;
    };

    for (int i = 0; i < 16; ++i) {
        auto t = std::make_unique<Tunnel>();
        t->fwdId = f.id;
        t->since.start();
        if (remote) {
            if (!m_ctx.mayRun(&f))
                break;
            LIBSSH2_CHANNEL *ch = libssh2_channel_forward_accept(f.listener);
            if (!ch) {
                m_ctx.after(&f, libssh2_session_last_errno(m_ctx.session));
                break;
            }
            m_ctx.after(&f, 0);
            t->ch = ch;
            if (overLimit()) {
                startClosing(*t);
            } else {
                QString err;
                t->st = Tunnel::St::Connecting;
                t->sock = net::tcpConnectStart(f.dest.front(), err);
                if (t->sock == net::kInvalidSocket) {
                    t->addrIndex = f.dest.size();
                    startClosing(*t, err);
                }
            }
        } else {
            QString peer;
            const net::socket_t s = net::tcpAccept(f.listenSock, &peer);
            if (s == net::kInvalidSocket)
                break;
            if (overLimit()) {
                net::closeSocket(s);
                continue;
            }
            t->sock = s;
            const int colon = peer.lastIndexOf(QLatin1Char(':'));
            t->peerHost = peer.left(colon).remove(QLatin1Char('[')).remove(QLatin1Char(']')).toUtf8();
            t->peerPort = peer.mid(colon + 1).toInt();
            if (f.rule.type == PortForward::Type::Dynamic) {
                t->st = Tunnel::St::Socks;
                t->viaSocks = true;
            } else {
                t->st = Tunnel::St::Opening;
                t->host = f.rule.destHost.toUtf8();
                t->port = f.rule.destPort;
            }
        }
        f.status.openConnections++;
        f.status.totalConnections++;
        markDirty();
        m_tunnels.push_back(std::move(t));
        progress = true;
    }
    return progress;
}

bool ForwardEngine::stepTunnel(Tunnel &t)
{
    bool progress = false;
    switch (t.st) {
    case Tunnel::St::Socks: {
        if (!t.toSocket.isEmpty()) {
            const long n = net::sendSome(t.sock, t.toSocket.constData(), size_t(t.toSocket.size()));
            if (n > 0) {
                t.toSocket.remove(0, n);
                progress = true;
            } else if (n == net::IoError) {
                startClosing(t);
                return true;
            }
        }
        char buf[512];
        const long n = net::recvSome(t.sock, buf, sizeof(buf));
        if (n == 0 || n == net::IoError) {
            startClosing(t);
            return true;
        }
        if (n > 0) {
            progress = true;
            const auto status = t.socks.feed(buf, int(n));
            t.toSocket += t.socks.takeReply();
            if (status == SocksHandshake::Status::Connect) {
                t.host = t.socks.host().toUtf8();
                t.port = t.socks.port();
                t.toChannel = t.socks.takeLeftover();
                t.st = Tunnel::St::Opening;
                t.since.restart();
            } else if (status == SocksHandshake::Status::Failed) {
                if (!t.toSocket.isEmpty())
                    net::sendSome(t.sock, t.toSocket.constData(), size_t(t.toSocket.size()));
                startClosing(t, QStringLiteral("SOCKS: %1").arg(t.socks.error()));
                return true;
            }
        }
        if (t.st == Tunnel::St::Socks && t.since.elapsed() > kHandshakeTimeoutMs) {
            startClosing(t);
            return true;
        }
        return progress;
    }

    case Tunnel::St::Opening: {
        // Keep reading what the client sends meanwhile (bounded).
        while (!t.sockEof && t.toChannel.size() < kBufferCap) {
            const long n = net::recvSome(t.sock, m_buf.data(), size_t(std::min<qsizetype>(m_buf.size(), kBufferCap - t.toChannel.size())));
            if (n > 0) {
                t.toChannel.append(m_buf.constData(), n);
                progress = true;
            } else if (n == 0) {
                t.sockEof = true;
                progress = true;
            } else if (n == net::IoError) {
                // The client gave up; the channel open may still be running.
                t.sockEof = true;
                break;
            } else {
                break;
            }
        }
        if (!m_ctx.tryLockOpen(&t) || !m_ctx.mayRun(&t))
            return progress;
        LIBSSH2_CHANNEL *ch = libssh2_channel_direct_tcpip_ex(m_ctx.session, t.host.constData(), t.port,
                                                              t.peerHost.isEmpty() ? "127.0.0.1" : t.peerHost.constData(),
                                                              t.peerPort > 0 ? t.peerPort : 0);
        if (!ch) {
            const int rc = libssh2_session_last_errno(m_ctx.session);
            m_ctx.after(&t, rc);
            if (rc == LIBSSH2_ERROR_EAGAIN)
                return progress;
            m_ctx.unlockOpen(&t);
            const QString why = QStringLiteral("cannot reach %1:%2 from the server (%3)")
                                    .arg(QString::fromUtf8(t.host))
                                    .arg(t.port)
                                    .arg(lastError(m_ctx.session));
            if (t.viaSocks && t.sock != net::kInvalidSocket) {
                const QByteArray r = t.socks.finalReply(rc == LIBSSH2_ERROR_CHANNEL_FAILURE
                                                            ? SocksHandshake::ConnectionRefused
                                                            : SocksHandshake::GeneralFailure);
                net::sendSome(t.sock, r.constData(), size_t(r.size()));
            }
            startClosing(t, why);
            return true;
        }
        m_ctx.after(&t, 0);
        m_ctx.unlockOpen(&t);
        t.ch = ch;
        t.st = Tunnel::St::Open;
        if (t.cancelled) {
            startClosing(t);
            return true;
        }
        if (t.viaSocks)
            t.toSocket += t.socks.finalReply(SocksHandshake::Succeeded);
        return true;
    }

    case Tunnel::St::Connecting: {
        const int r = net::pollConnect(t.sock);
        if (r == 0) {
            t.st = Tunnel::St::Open;
            return true;
        }
        Forward *f = find(t.fwdId);
        if (r < 0 && t.since.elapsed() < kConnectTimeoutMs)
            return false;
        // Failed or timed out: try the next address.
        net::closeSocket(t.sock);
        t.sock = net::kInvalidSocket;
        QString err = r > 0 ? net::errorText(r) : QStringLiteral("timed out");
        while (f && ++t.addrIndex < f->dest.size()) {
            t.sock = net::tcpConnectStart(f->dest[t.addrIndex], err);
            if (t.sock != net::kInvalidSocket) {
                t.since.restart();
                return true;
            }
        }
        startClosing(t, f ? QStringLiteral("cannot connect to %1:%2: %3").arg(f->rule.destHost).arg(f->rule.destPort).arg(err)
                          : err);
        return true;
    }

    case Tunnel::St::Open: {
        progress |= tunnelChannelIo(t);
        if (t.st != Tunnel::St::Open)
            return true;
        progress |= tunnelSocketIo(t);
        if (t.st != Tunnel::St::Open)
            return true;
        if (t.sockEof && t.eofSent && t.chanEof && t.toSocket.isEmpty())
            startClosing(t);
        return progress;
    }

    case Tunnel::St::Closing: {
        if (t.sock != net::kInvalidSocket) {
            net::closeSocket(t.sock);
            t.sock = net::kInvalidSocket;
        }
        if (t.ch) {
            if (!m_ctx.mayRun(&t))
                return false;
            const int rc = libssh2_channel_free(t.ch);
            m_ctx.after(&t, rc);
            if (rc == LIBSSH2_ERROR_EAGAIN) {
                // The server never confirmed the close: leave the channel to the session.
                if (t.since.elapsed() > kCloseGiveUpMs && m_ctx.owner != &t)
                    t.ch = nullptr;
                else
                    return false;
            }
            t.ch = nullptr;
        }
        m_ctx.unlockOpen(&t);
        t.st = Tunnel::St::Dead;
        return true;
    }

    case Tunnel::St::Dead:
        return false;
    }
    return progress;
}

bool ForwardEngine::tunnelChannelIo(Tunnel &t)
{
    if (!m_ctx.mayRun(&t))
        return false;
    Forward *f = find(t.fwdId);
    bool progress = false;
    for (;;) {
        switch (t.phase) {
        case 0:   // local -> server
            while (!t.toChannel.isEmpty()) {
                const ssize_t n = libssh2_channel_write(t.ch, t.toChannel.constData(),
                                                        size_t(std::min<qsizetype>(t.toChannel.size(), kChunk)));
                m_ctx.after(&t, n < 0 ? long(n) : 0);
                if (n == LIBSSH2_ERROR_EAGAIN) {
                    if (m_ctx.owner == &t)
                        return progress;
                    break;
                }
                if (n < 0) {
                    startClosing(t, QStringLiteral("write failed: %1").arg(lastError(m_ctx.session)));
                    return true;
                }
                t.toChannel.remove(0, n);
                if (f)
                    f->status.bytesOut += quint64(n);
                markDirty();
                progress = true;
            }
            t.phase = 1;
            break;
        case 1:   // server -> local
            while (!t.chanEof && t.toSocket.size() < kBufferCap) {
                const ssize_t n = libssh2_channel_read(t.ch, m_buf.data(),
                                                       size_t(std::min<qsizetype>(kChunk, kBufferCap - t.toSocket.size())));
                m_ctx.after(&t, n < 0 ? long(n) : 0);
                if (n > 0) {
                    t.toSocket.append(m_buf.constData(), int(n));
                    if (f)
                        f->status.bytesIn += quint64(n);
                    markDirty();
                    progress = true;
                    continue;
                }
                if (n == 0) {
                    t.chanEof = true;
                    progress = true;
                    break;
                }
                if (n == LIBSSH2_ERROR_EAGAIN) {
                    if (m_ctx.owner == &t)
                        return progress;
                    break;
                }
                startClosing(t, QStringLiteral("read failed: %1").arg(lastError(m_ctx.session)));
                return true;
            }
            t.phase = 2;
            break;
        case 2:   // local side finished -> tell the server
            if (t.sockEof && t.toChannel.isEmpty() && !t.eofSent) {
                const int rc = libssh2_channel_send_eof(t.ch);
                m_ctx.after(&t, rc);
                if (rc == LIBSSH2_ERROR_EAGAIN) {
                    if (m_ctx.owner == &t)
                        return progress;
                } else if (rc < 0) {
                    startClosing(t);
                    return true;
                } else {
                    t.eofSent = true;
                    progress = true;
                }
            }
            t.phase = 0;
            return progress;
        default:
            t.phase = 0;
            return progress;
        }
    }
}

bool ForwardEngine::tunnelSocketIo(Tunnel &t)
{
    bool progress = false;
    if (!t.toSocket.isEmpty()) {
        const long n = net::sendSome(t.sock, t.toSocket.constData(), size_t(t.toSocket.size()));
        if (n > 0) {
            t.toSocket.remove(0, n);
            progress = true;
        } else if (n == net::IoError) {
            startClosing(t);
            return true;
        }
    }
    if (t.chanEof && t.toSocket.isEmpty() && !t.sockShut) {
        net::shutdownWrite(t.sock);
        t.sockShut = true;
        progress = true;
    }
    while (!t.sockEof && t.toChannel.size() < kBufferCap) {
        const long n = net::recvSome(t.sock, m_buf.data(), size_t(std::min<qsizetype>(m_buf.size(), kBufferCap - t.toChannel.size())));
        if (n > 0) {
            t.toChannel.append(m_buf.constData(), n);
            progress = true;
        } else if (n == 0) {
            t.sockEof = true;
            progress = true;
        } else if (n == net::IoError) {
            startClosing(t);
            return true;
        } else {
            break;
        }
    }
    return progress;
}

void ForwardEngine::collectPoll(std::vector<net::PollItem> &items) const
{
    for (const auto &f : m_forwards) {
        if (f->listenSock != net::kInvalidSocket && f->phase == Forward::Phase::Listening && !f->removing) {
            net::PollItem it;
            it.s = f->listenSock;
            it.wantRead = true;
            items.push_back(it);
        }
    }
    for (const auto &t : m_tunnels) {
        if (t->sock == net::kInvalidSocket)
            continue;
        net::PollItem it;
        it.s = t->sock;
        switch (t->st) {
        case Tunnel::St::Socks:
            it.wantRead = true;
            it.wantWrite = !t->toSocket.isEmpty();
            break;
        case Tunnel::St::Opening:
            it.wantRead = !t->sockEof && t->toChannel.size() < kBufferCap;
            break;
        case Tunnel::St::Connecting:
            it.wantWrite = true;
            break;
        case Tunnel::St::Open:
            it.wantRead = !t->sockEof && t->toChannel.size() < kBufferCap;
            it.wantWrite = !t->toSocket.isEmpty();
            break;
        default:
            break;
        }
        if (it.wantRead || it.wantWrite)
            items.push_back(it);
    }
}

bool ForwardEngine::hasBufferedData() const
{
    for (const auto &t : m_tunnels) {
        if (t->st != Tunnel::St::Open || !t->ch || t->chanEof || t->toSocket.size() >= kBufferCap)
            continue;
        if (libssh2_poll_channel_read(t->ch, 1) > 0 || libssh2_channel_eof(t->ch) == 1)
            return true;
    }
    return false;
}

bool ForwardEngine::waitingOnSession() const
{
    for (const auto &f : m_forwards) {
        if (f->rule.type == PortForward::Type::Remote
            && ((f->phase == Forward::Phase::New) || (f->removing && f->listener)))
            return true;
    }
    for (const auto &t : m_tunnels) {
        switch (t->st) {
        case Tunnel::St::Opening:
            return true;
        case Tunnel::St::Closing:
            if (t->ch)
                return true;
            break;
        case Tunnel::St::Open:
            if (!t->toChannel.isEmpty() || (t->sockEof && !t->eofSent))
                return true;
            break;
        default:
            break;
        }
    }
    return false;
}

bool ForwardEngine::readsSession() const
{
    for (const auto &f : m_forwards)
        if (f->listener && f->phase == Forward::Phase::Listening)
            return true;
    for (const auto &t : m_tunnels)
        if (t->ch && (t->st == Tunnel::St::Open || t->st == Tunnel::St::Closing))
            return true;
    return false;
}

void ForwardEngine::publish(bool force)
{
    if (!m_status || (!m_dirty && !force))
        return;
    if (!force && m_lastPublish.isValid() && m_lastPublish.elapsed() < kPublishIntervalMs)
        return;
    QVector<ForwardStatus> all;
    all.reserve(int(m_forwards.size()));
    for (const auto &f : m_forwards)
        all << f->status;
    m_status(all);
    m_dirty = false;
    m_lastPublish.restart();
}

} // namespace termx::ssh
