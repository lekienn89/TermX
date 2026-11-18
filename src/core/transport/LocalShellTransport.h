// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Transport.h"
#include "session/SessionConfig.h"

namespace termx {

// Local shell: ConPTY on Windows 10 1809+, forkpty elsewhere (development).
class LocalShellTransport : public Transport {
    Q_OBJECT
public:
    explicit LocalShellTransport(const LocalShellOptions &opts, QObject *parent = nullptr);
    QString kindName() const override { return QStringLiteral("local"); }

    static QString defaultProgram();

protected:
    TransportWorker *createWorker() override;

private:
    LocalShellOptions m_opts;
};

} // namespace termx
