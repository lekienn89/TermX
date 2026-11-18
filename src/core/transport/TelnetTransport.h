// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Transport.h"
#include "session/SessionConfig.h"

namespace termx {

// Telnet or raw TCP over QTcpSocket on a dedicated thread.
class TelnetTransport : public Transport {
    Q_OBJECT
public:
    explicit TelnetTransport(const TelnetOptions &opts, QObject *parent = nullptr);
    QString kindName() const override { return m_opts.rawTcp ? QStringLiteral("raw") : QStringLiteral("telnet"); }
    bool supportsBreak() const override { return !m_opts.rawTcp; }

protected:
    TransportWorker *createWorker() override;

private:
    TelnetOptions m_opts;
};

} // namespace termx
