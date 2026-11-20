// SPDX-License-Identifier: GPL-3.0-or-later
// Internal: SFTP over the shell's libssh2 session.
//
// Each lane runs its requests as plain sequential code on a helper thread,
// but only while the SSH I/O thread hands it the baton (resume()/yield()), so
// libssh2 is never used by two threads at once. A libssh2 call that would
// block yields back to the I/O thread, which keeps the shell and tunnels
// moving and resumes the lane when the socket is ready.
#pragma once

#include "SftpTypes.h"
#include "SshContext.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace termx::ssh {

class SftpEngine {
public:
    using ResultFn = std::function<void(const SftpResult &)>;
    using ProgressFn = std::function<void(const SftpProgress &)>;
    using CancelledFn = std::function<bool(quint64)>;

    SftpEngine(Context &ctx, ResultFn result, ProgressFn progress, CancelledFn cancelled);
    ~SftpEngine();
    SftpEngine(const SftpEngine &) = delete;
    SftpEngine &operator=(const SftpEngine &) = delete;

    void submit(const SftpRequest &req);
    bool step();                       // gives every lane that can run a time slice
    bool busy() const;                 // a request is queued or running
    bool hasBufferedData() const;      // an SFTP channel has data libssh2 already read
    // Stops every lane without touching libssh2 again and fails what is left.
    void abort(const QString &why);

    // Helpers shared with tests.
    static bool safeLocalName(const QString &remoteName, QString &out);
    static QString joinRemote(const QString &dir, const QString &name);

private:
    class Lane;
    // Declared before the lanes: they hold references to these.
    ResultFn m_resultFn;
    ProgressFn m_progressFn;
    CancelledFn m_cancelledFn;
    std::unique_ptr<Lane> m_browse;    // listing, rename, delete...
    std::unique_ptr<Lane> m_transfer;  // downloads and uploads
};

} // namespace termx::ssh
