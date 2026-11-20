// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef _WIN32
// Windows fd_set is a counted array: raise the limit for many tunnels.
#define FD_SETSIZE 1024
#endif
#include "SocketUtil.h"

#include <QByteArray>
#include <QDeadlineTimer>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace termx::net {

namespace {

int lastError()
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool wouldBlock(int e)
{
#ifdef _WIN32
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return e == EINPROGRESS || e == EWOULDBLOCK || e == EAGAIN;
#endif
}

bool setNonBlocking(socket_t s)
{
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

QString errorString(int e)
{
#ifdef _WIN32
    wchar_t *buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, DWORD(e), 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    QString s = buf ? QString::fromWCharArray(buf).trimmed() : QStringLiteral("error %1").arg(e);
    if (buf)
        LocalFree(buf);
    return s;
#else
    return QString::fromLocal8Bit(std::strerror(e));
#endif
}

} // namespace

bool initialize()
{
#ifdef _WIN32
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA data;
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    });
    return ok;
#else
    return true;
#endif
}

void closeSocket(socket_t s)
{
    if (s == kInvalidSocket)
        return;
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
}

QString lastErrorString()
{
    return errorString(lastError());
}

socket_t tcpConnect(const QString &host, uint16_t port, int timeoutMs,
                    const std::atomic<bool> &stop, QString &err)
{
    initialize();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo *res = nullptr;
    const QByteArray hostUtf8 = host.trimmed().toUtf8();
    const QByteArray portStr = QByteArray::number(port);
    const int gai = getaddrinfo(hostUtf8.constData(), portStr.constData(), &hints, &res);
    if (gai != 0 || !res) {
#ifdef _WIN32
        err = QStringLiteral("Cannot resolve host \"%1\": %2").arg(host, errorString(gai));
#else
        err = QStringLiteral("Cannot resolve host \"%1\": %2").arg(host, QString::fromLocal8Bit(gai_strerror(gai)));
#endif
        return kInvalidSocket;
    }

    QDeadlineTimer deadline(timeoutMs);
    socket_t result = kInvalidSocket;
    QString lastErr = QStringLiteral("No usable address");

    for (addrinfo *ai = res; ai && result == kInvalidSocket && !stop.load(); ai = ai->ai_next) {
        socket_t s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == kInvalidSocket) {
            lastErr = lastErrorString();
            continue;
        }
        setNonBlocking(s);
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&one), sizeof(one));
        setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char *>(&one), sizeof(one));

        int rc = ::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        if (rc != 0 && !wouldBlock(lastError())) {
            lastErr = lastErrorString();
            closeSocket(s);
            continue;
        }
        bool connected = (rc == 0);
        while (!connected && !stop.load() && !deadline.hasExpired()) {
            fd_set wfds, efds;
            FD_ZERO(&wfds);
            FD_ZERO(&efds);
            FD_SET(s, &wfds);
            FD_SET(s, &efds);
            timeval tv{0, 100 * 1000};
            const int n = ::select(static_cast<int>(s + 1), nullptr, &wfds, &efds, &tv);
            if (n < 0) {
                lastErr = lastErrorString();
                break;
            }
            if (n == 0)
                continue;
            int soErr = 0;
#ifdef _WIN32
            int len = sizeof(soErr);
#else
            socklen_t len = sizeof(soErr);
#endif
            getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&soErr), &len);
            if (soErr != 0) {
                lastErr = errorString(soErr);
                break;
            }
            connected = true;
        }
        if (connected) {
            result = s;
        } else {
            if (deadline.hasExpired())
                lastErr = QStringLiteral("Connection timed out");
            closeSocket(s);
        }
    }
    freeaddrinfo(res);
    if (result == kInvalidSocket)
        err = stop.load() ? QStringLiteral("Cancelled") : lastErr;
    return result;
}

Waker::Waker()
{
    initialize();
    m_sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_sock == kInvalidSocket)
        return;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
#ifdef _WIN32
    int len = sizeof(addr);
#else
    socklen_t len = sizeof(addr);
#endif
    if (::bind(m_sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0
        || ::getsockname(m_sock, reinterpret_cast<sockaddr *>(&addr), &len) != 0
        || ::connect(m_sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        closeSocket(m_sock);
        m_sock = kInvalidSocket;
        return;
    }
    setNonBlocking(m_sock);
}

Waker::~Waker()
{
    closeSocket(m_sock);
}

void Waker::wake()
{
    if (m_sock == kInvalidSocket)
        return;
    const char b = 1;
    ::send(m_sock, &b, 1, 0);
}

void Waker::drain()
{
    if (m_sock == kInvalidSocket)
        return;
    char buf[64];
    while (::recv(m_sock, buf, sizeof(buf), 0) > 0) {
    }
}

int waitSocket(socket_t s, bool forRead, bool forWrite, const Waker &waker, int timeoutMs)
{
    fd_set rfds, wfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    socket_t maxFd = 0;
    if (s != kInvalidSocket) {
        if (forRead)
            FD_SET(s, &rfds);
        if (forWrite)
            FD_SET(s, &wfds);
        maxFd = s;
    }
    if (waker.isValid()) {
        FD_SET(waker.fd(), &rfds);
        if (waker.fd() > maxFd)
            maxFd = waker.fd();
    }
    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    const int n = ::select(static_cast<int>(maxFd + 1), &rfds, &wfds, nullptr, &tv);
    if (n < 0)
        return WaitError;
    if (n == 0)
        return WaitTimeout;
    int r = 0;
    if (waker.isValid() && FD_ISSET(waker.fd(), &rfds))
        r |= WaitWoken;
    if (s != kInvalidSocket && (FD_ISSET(s, &rfds) || FD_ISSET(s, &wfds)))
        r |= WaitSocket;
    return r;
}

bool resolve(const QString &host, uint16_t port, bool passive, std::vector<Address> &out, QString &err)
{
    initialize();
    out.clear();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (passive)
        hints.ai_flags = AI_PASSIVE;
    addrinfo *res = nullptr;
    const QByteArray hostUtf8 = host.trimmed().toUtf8();
    const QByteArray portStr = QByteArray::number(port);
    const int gai = getaddrinfo(hostUtf8.isEmpty() ? nullptr : hostUtf8.constData(), portStr.constData(), &hints, &res);
    if (gai != 0 || !res) {
#ifdef _WIN32
        err = QStringLiteral("Cannot resolve \"%1\": %2").arg(host, errorString(gai));
#else
        err = QStringLiteral("Cannot resolve \"%1\": %2").arg(host, QString::fromLocal8Bit(gai_strerror(gai)));
#endif
        return false;
    }
    for (addrinfo *ai = res; ai; ai = ai->ai_next) {
        if (ai->ai_addrlen > sizeof(Address::storage))
            continue;
        Address a;
        std::memcpy(a.storage, ai->ai_addr, ai->ai_addrlen);
        a.length = int(ai->ai_addrlen);
        a.family = ai->ai_family;
        out.push_back(a);
    }
    freeaddrinfo(res);
    if (out.empty()) {
        err = QStringLiteral("No usable address for \"%1\"").arg(host);
        return false;
    }
    return true;
}

QString addressToString(const Address &a)
{
    char host[NI_MAXHOST] = {};
    char serv[NI_MAXSERV] = {};
    if (getnameinfo(reinterpret_cast<const sockaddr *>(a.storage), socklen_t(a.length), host, sizeof(host), serv,
                    sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV) != 0)
        return QStringLiteral("?");
    const QString h = QString::fromLatin1(host);
    return (h.contains(QLatin1Char(':')) ? QLatin1Char('[') + h + QLatin1Char(']') : h) + QLatin1Char(':')
           + QString::fromLatin1(serv);
}

socket_t tcpListen(const QString &bindAddress, uint16_t port, QString &err, uint16_t *boundPort)
{
    std::vector<Address> addrs;
    if (!resolve(bindAddress, port, true, addrs, err))
        return kInvalidSocket;
    for (const Address &a : addrs) {
        socket_t s = ::socket(a.family, SOCK_STREAM, IPPROTO_TCP);
        if (s == kInvalidSocket) {
            err = lastErrorString();
            continue;
        }
        int one = 1;
#ifdef _WIN32
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&one), sizeof(one));
#else
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&one), sizeof(one));
#endif
        if (::bind(s, reinterpret_cast<const sockaddr *>(a.storage), socklen_t(a.length)) != 0
            || ::listen(s, 64) != 0 || !setNonBlocking(s)) {
            err = QStringLiteral("Cannot listen on %1: %2").arg(addressToString(a), lastErrorString());
            closeSocket(s);
            continue;
        }
        if (boundPort) {
            Address local;
            socklen_t len = sizeof(local.storage);
            *boundPort = port;
            if (getsockname(s, reinterpret_cast<sockaddr *>(local.storage), &len) == 0) {
                local.length = int(len);
                const auto *sa = reinterpret_cast<const sockaddr *>(local.storage);
                if (sa->sa_family == AF_INET)
                    *boundPort = ntohs(reinterpret_cast<const sockaddr_in *>(local.storage)->sin_port);
                else if (sa->sa_family == AF_INET6)
                    *boundPort = ntohs(reinterpret_cast<const sockaddr_in6 *>(local.storage)->sin6_port);
            }
        }
        return s;
    }
    return kInvalidSocket;
}

socket_t tcpAccept(socket_t listener, QString *peer)
{
    Address a;
    socklen_t len = sizeof(a.storage);
    socket_t s = ::accept(listener, reinterpret_cast<sockaddr *>(a.storage), &len);
    if (s == kInvalidSocket)
        return kInvalidSocket;
    a.length = int(len);
    setNonBlocking(s);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&one), sizeof(one));
    if (peer)
        *peer = addressToString(a);
    return s;
}

socket_t tcpConnectStart(const Address &a, QString &err)
{
    socket_t s = ::socket(a.family, SOCK_STREAM, IPPROTO_TCP);
    if (s == kInvalidSocket) {
        err = lastErrorString();
        return kInvalidSocket;
    }
    setNonBlocking(s);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&one), sizeof(one));
    const int rc = ::connect(s, reinterpret_cast<const sockaddr *>(a.storage), socklen_t(a.length));
    if (rc != 0 && !wouldBlock(lastError())) {
        err = QStringLiteral("Cannot connect to %1: %2").arg(addressToString(a), lastErrorString());
        closeSocket(s);
        return kInvalidSocket;
    }
    return s;
}

int pollConnect(socket_t s)
{
#ifndef _WIN32
    if (s < 0 || s >= FD_SETSIZE)
        return EMFILE;
#endif
    fd_set wfds, efds;
    FD_ZERO(&wfds);
    FD_ZERO(&efds);
    FD_SET(s, &wfds);
    FD_SET(s, &efds);
    timeval tv{0, 0};
    const int n = ::select(static_cast<int>(s + 1), nullptr, &wfds, &efds, &tv);
    if (n < 0)
        return lastError() ? lastError() : 1;
    if (n == 0)
        return -1;
    int soErr = 0;
#ifdef _WIN32
    int len = sizeof(soErr);
#else
    socklen_t len = sizeof(soErr);
#endif
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&soErr), &len) != 0)
        return lastError() ? lastError() : 1;
    if (soErr != 0)
        return wouldBlock(soErr) ? -1 : soErr;
    // Windows reports a failed connect through the exception set.
    return FD_ISSET(s, &efds) ? 1 : 0;
}

QString errorText(int code)
{
    return errorString(code);
}

long recvSome(socket_t s, char *buf, size_t len)
{
#ifdef _WIN32
    const int n = ::recv(s, buf, int(qMin<size_t>(len, 1 << 30)), 0);
#else
    const ssize_t n = ::recv(s, buf, len, 0);
#endif
    if (n >= 0)
        return long(n);
    const int e = lastError();
#ifndef _WIN32
    if (e == EINTR)
        return IoWouldBlock;
#endif
    return wouldBlock(e) ? IoWouldBlock : IoError;
}

long sendSome(socket_t s, const char *buf, size_t len)
{
#ifdef _WIN32
    const int n = ::send(s, buf, int(qMin<size_t>(len, 1 << 30)), 0);
#elif defined(MSG_NOSIGNAL)
    const ssize_t n = ::send(s, buf, len, MSG_NOSIGNAL);
#else
    const ssize_t n = ::send(s, buf, len, 0);
#endif
    if (n >= 0)
        return long(n);
    const int e = lastError();
#ifndef _WIN32
    if (e == EINTR)
        return IoWouldBlock;
#endif
    return wouldBlock(e) ? IoWouldBlock : IoError;
}

void shutdownWrite(socket_t s)
{
#ifdef _WIN32
    ::shutdown(s, SD_SEND);
#else
    ::shutdown(s, SHUT_WR);
#endif
}

void shutdownBoth(socket_t s)
{
    if (s == kInvalidSocket)
        return;
#ifdef _WIN32
    ::shutdown(s, SD_BOTH);
#else
    ::shutdown(s, SHUT_RDWR);
#endif
}

int maxPollItems()
{
    return FD_SETSIZE - 8;
}

int waitMany(std::vector<PollItem> &items, const Waker &waker, int timeoutMs)
{
    fd_set rfds, wfds, efds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&efds);
    socket_t maxFd = 0;
    int used = 0;
    const int limit = maxPollItems();
    for (PollItem &it : items) {
        it.readable = it.writable = it.failed = false;
        if (it.s == kInvalidSocket || (!it.wantRead && !it.wantWrite) || used >= limit)
            continue;
#ifndef _WIN32
        if (it.s >= FD_SETSIZE)
            continue;
#endif
        ++used;
        if (it.wantRead)
            FD_SET(it.s, &rfds);
        if (it.wantWrite) {
            FD_SET(it.s, &wfds);
            FD_SET(it.s, &efds);
        }
        if (it.s > maxFd)
            maxFd = it.s;
    }
    if (waker.isValid()) {
        FD_SET(waker.fd(), &rfds);
        if (waker.fd() > maxFd)
            maxFd = waker.fd();
    }
    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    const int n = ::select(static_cast<int>(maxFd + 1), &rfds, &wfds, &efds, &tv);
    if (n < 0) {
#ifndef _WIN32
        if (errno == EINTR)
            return WaitTimeout;
#endif
        return WaitError;
    }
    if (n == 0)
        return WaitTimeout;
    int r = 0;
    if (waker.isValid() && FD_ISSET(waker.fd(), &rfds))
        r |= WaitWoken;
    for (PollItem &it : items) {
        if (it.s == kInvalidSocket)
            continue;
#ifndef _WIN32
        if (it.s >= FD_SETSIZE)
            continue;
#endif
        it.readable = it.wantRead && FD_ISSET(it.s, &rfds);
        it.writable = it.wantWrite && FD_ISSET(it.s, &wfds);
        it.failed = it.wantWrite && FD_ISSET(it.s, &efds);
        if (it.readable || it.writable || it.failed)
            r |= WaitSocket;
    }
    return r;
}

} // namespace termx::net
