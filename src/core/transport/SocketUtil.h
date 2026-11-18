// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal cross-platform socket helpers used by the libssh2 worker.
#pragma once

#include <QString>
#include <atomic>
#include <cstdint>

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

} // namespace termx::net
