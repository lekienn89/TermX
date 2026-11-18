// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Transport.h"
#include "session/SessionConfig.h"

namespace termx {

// Serial port via QSerialPort on a dedicated thread. Opening, reading and
// writing never touch the GUI thread, so a misbehaving USB-serial driver
// can only stall its own tab.
class SerialTransport : public Transport {
    Q_OBJECT
public:
    explicit SerialTransport(const SerialOptions &opts, QObject *parent = nullptr);
    QString kindName() const override { return QStringLiteral("serial"); }
    bool supportsBreak() const override { return true; }

protected:
    TransportWorker *createWorker() override;

private:
    SerialOptions m_opts;
};

} // namespace termx
