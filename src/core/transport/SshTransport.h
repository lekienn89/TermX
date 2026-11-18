// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Transport.h"
#include "session/SessionConfig.h"

namespace termx {

// SSH2 via libssh2 in non-blocking mode on a dedicated thread.
class SshTransport : public Transport {
    Q_OBJECT
public:
    SshTransport(const SshOptions &opts, const QString &savedPassword,
                 const QString &knownHostsPath, QObject *parent = nullptr);
    ~SshTransport() override;
    QString kindName() const override { return QStringLiteral("ssh"); }

    static bool globalInit();     // libssh2_init; call once at startup
    static void globalCleanup();

protected:
    TransportWorker *createWorker() override;

private:
    SshOptions m_opts;
    QString m_password;
    QString m_knownHosts;
};

} // namespace termx
