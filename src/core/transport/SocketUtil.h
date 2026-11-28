// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal cross-platform socket helpers used by the libssh2 worker.
#pragma once

#include <QString>
#include <atomic>
#include <cstdint>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
namespace termx::net {
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
}
#else
namespace termx::net {
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
}
#endif

namespace termx::net {

bool initialize();               // WSAStartup on Windows; idempotent
void closeSocket(socket_t s);
QString lastErrorString();

// Non-blocking connect with a deadline; polls `stop` every 100 ms.
// Returns a connected, non-blocking socket or kInvalidSocket (error in `err`).
socket_t tcpConnect(const QString &host, uint16_t port, int timeoutMs,
                    const std::atomic<bool> &stop, QString &err);

// Self-addressed UDP socket used to interrupt select() from another thread.
class Waker {
public:
    Waker();
    ~Waker();
    Waker(const Waker &) = delete;
    Waker &operator=(const Waker &) = delete;
    bool isValid() const { return m_sock != kInvalidSocket; }
    socket_t fd() const { return m_sock; }
    void wake();   // thread-safe
    void drain();
private:
    socket_t m_sock = kInvalidSocket;
};

enum WaitResult { WaitTimeout = 0, WaitSocket = 1, WaitWoken = 2, WaitError = -1 };

// Waits until `s` is readable (and/or writable) or the waker fires.
int waitSocket(socket_t s, bool forRead, bool forWrite, const Waker &waker, int timeoutMs);

// ---- helpers for port forwarding (all sockets non-blocking) ----------------

struct Address {
    alignas(8) unsigned char storage[128] = {};   // sockaddr_storage
    int length = 0;
    int family = 0;
};

// Resolves host:port (blocking getaddrinfo). `passive` = for bind().
bool resolve(const QString &host, uint16_t port, bool passive, std::vector<Address> &out, QString &err);
QString addressToString(const Address &a);

// Non-blocking listening socket. On Windows the port is taken exclusively
// (SO_EXCLUSIVEADDRUSE) so another program cannot hijack it.
socket_t tcpListen(const QString &bindAddress, uint16_t port, QString &err, uint16_t *boundPort = nullptr);
// kInvalidSocket when nothing is pending. `peer` gets "ip:port".
socket_t tcpAccept(socket_t listener, QString *peer = nullptr);
// Starts a non-blocking connect; finish it with connectResult() once writable.
socket_t tcpConnectStart(const Address &a, QString &err);
// Checks (without waiting) a connect started by tcpConnectStart():
// 0 = connected, >0 = OS error code (see errorText), -1 = still in progress.
int pollConnect(socket_t s);
QString errorText(int code);

enum IoResult : long { IoWouldBlock = -1, IoError = -2 };
// >0 bytes, 0 = orderly EOF (recv only), IoWouldBlock or IoError.
long recvSome(socket_t s, char *buf, size_t len);
long sendSome(socket_t s, const char *buf, size_t len);
void shutdownWrite(socket_t s);
void shutdownBoth(socket_t s);   // later send/recv fail at once; the descriptor stays valid

struct PollItem {
    socket_t s = kInvalidSocket;
    bool wantRead = false;
    bool wantWrite = false;
    // results
    bool readable = false;
    bool writable = false;
    bool failed = false;      // exceptional condition (e.g. a failed connect on Windows)
};

// Largest number of sockets waitMany() handles in one call.
int maxPollItems();

// Waits for any of `items` (and the waker). Returns WaitResult flags.
int waitMany(std::vector<PollItem> &items, const Waker &waker, int timeoutMs);

} // namespace termx::net
