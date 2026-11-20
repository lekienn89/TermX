// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "SftpTypes.h"
#include "Transport.h"
#include "session/PortForward.h"
#include "session/SessionConfig.h"

#include <memory>

namespace termx {

struct SshControl;

// SSH2 via libssh2 in non-blocking mode on a dedicated thread. The same
// connection also carries SFTP and port forwarding: one login, one TCP
// connection, like OpenSSH's ssh -L/-R/-D and its sftp subsystem.
class SshTransport : public Transport {
    Q_OBJECT
public:
    SshTransport(const SshOptions &opts, const QString &savedPassword,
                 const QString &knownHostsPath, QObject *parent = nullptr);
    ~SshTransport() override;
    QString kindName() const override { return QStringLiteral("ssh"); }

    static bool globalInit();     // libssh2_init; call once at startup
    static void globalCleanup();

    // ---- SFTP ----------------------------------------------------------
    // Queued until the shell is up. The answer always arrives through
    // sftpFinished(), also when the connection is gone (ok = false).
    quint64 submitSftp(SftpRequest req);
    void cancelSftp(quint64 id);

    // ---- port forwarding -------------------------------------------------
    // Rules saved with the session start by themselves after login.
    quint64 startForward(const PortForward &rule);
    void stopForward(quint64 id);
    QVector<ForwardStatus> forwardStatus() const { return m_forwards; }

Q_SIGNALS:
    void sftpProgress(const termx::SftpProgress &progress);
    void sftpFinished(const termx::SftpResult &result);
    void forwardsChanged(const QVector<termx::ForwardStatus> &all);

protected:
    TransportWorker *createWorker() override;

private:
    void failLater(const SftpRequest &req, const QString &why);

    SshOptions m_opts;
    QString m_password;
    QString m_knownHosts;
    std::shared_ptr<SshControl> m_control;
    QVector<ForwardStatus> m_forwards;
};

void registerSshMetaTypes();

} // namespace termx
