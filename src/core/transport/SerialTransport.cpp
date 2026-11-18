// SPDX-License-Identifier: GPL-3.0-or-later
#include "SerialTransport.h"

#include <QSerialPort>
#include <QTimer>

namespace termx {

namespace {

QSerialPort::Parity toParity(QChar p)
{
    switch (p.toUpper().toLatin1()) {
    case 'E': return QSerialPort::EvenParity;
    case 'O': return QSerialPort::OddParity;
    case 'M': return QSerialPort::MarkParity;
    case 'S': return QSerialPort::SpaceParity;
    default: return QSerialPort::NoParity;
    }
}

QSerialPort::StopBits toStopBits(int s)
{
    switch (s) {
    case 2: return QSerialPort::TwoStop;
    case 3: return QSerialPort::OneAndHalfStop;
    default: return QSerialPort::OneStop;
    }
}

QSerialPort::DataBits toDataBits(int d)
{
    switch (d) {
    case 5: return QSerialPort::Data5;
    case 6: return QSerialPort::Data6;
    case 7: return QSerialPort::Data7;
    default: return QSerialPort::Data8;
    }
}

class SerialWorker : public TransportWorker {
public:
    explicit SerialWorker(SerialOptions o) : m_opts(std::move(o)) {}

    void start() override
    {
        if (stopRequested()) {
            finish();
            return;
        }
        setState(TransportState::Connecting, QStringLiteral("Opening %1...").arg(m_opts.portName));
        m_port = new QSerialPort(this);
        m_port->setPortName(m_opts.portName);
        m_port->setReadBufferSize(1024 * 1024);
        if (!m_port->open(QIODevice::ReadWrite)) {
            QString msg = m_port->errorString();
            if (m_port->error() == QSerialPort::PermissionError)
                msg = QStringLiteral("%1 is in use by another program (or access denied)").arg(m_opts.portName);
            else if (m_port->error() == QSerialPort::DeviceNotFoundError)
                msg = QStringLiteral("%1 not found (cable unplugged?)").arg(m_opts.portName);
            shutdown(TransportState::Failed, msg);
            return;
        }
        bool ok = m_port->setBaudRate(m_opts.baudRate) && m_port->setDataBits(toDataBits(m_opts.dataBits))
                  && m_port->setParity(toParity(m_opts.parity)) && m_port->setStopBits(toStopBits(m_opts.stopBits));
        switch (m_opts.flow) {
        case SerialOptions::Flow::RtsCts: ok = ok && m_port->setFlowControl(QSerialPort::HardwareControl); break;
        case SerialOptions::Flow::XonXoff: ok = ok && m_port->setFlowControl(QSerialPort::SoftwareControl); break;
        case SerialOptions::Flow::None: ok = ok && m_port->setFlowControl(QSerialPort::NoFlowControl); break;
        }
        if (!ok) {
            shutdown(TransportState::Failed, QStringLiteral("Cannot apply port settings: %1").arg(m_port->errorString()));
            return;
        }
        m_port->setDataTerminalReady(true);
        if (m_opts.flow != SerialOptions::Flow::RtsCts)
            m_port->setRequestToSend(true);

        connect(m_port, &QSerialPort::readyRead, this, [this] { readAvailable(); });
        connect(m_port, &QSerialPort::errorOccurred, this, [this](QSerialPort::SerialPortError e) {
            if (e == QSerialPort::NoError || e == QSerialPort::TimeoutError)
                return;
            if (e == QSerialPort::ResourceError || e == QSerialPort::DeviceNotFoundError)
                shutdown(TransportState::Disconnected, QStringLiteral("Device removed (%1)").arg(m_opts.portName));
            else if (e == QSerialPort::WriteError || e == QSerialPort::ReadError)
                shutdown(TransportState::Disconnected, QStringLiteral("I/O error: %1").arg(m_port->errorString()));
        });
        m_breakTimer.setSingleShot(true);
        connect(&m_breakTimer, &QTimer::timeout, this, [this] {
            if (m_port && m_port->isOpen())
                m_port->setBreakEnabled(false);
        });
        setState(TransportState::Connected, QString());
        onWake();
    }

protected:
    void onWake() override
    {
        if (m_done)
            return;
        if (stopRequested()) {
            shutdown(TransportState::Disconnected, QStringLiteral("Disconnected"));
            return;
        }
        if (!m_port || !m_port->isOpen())
            return;
        if (m_shared->breakPending.exchange(false) && !m_breakTimer.isActive()) {
            m_port->setBreakEnabled(true);
            m_breakTimer.start(300);
        }
        std::deque<QByteArray> chunks;
        if (takeWrites(chunks)) {
            for (const QByteArray &c : chunks)
                m_port->write(c);
        }
        readAvailable();
    }

private:
    void readAvailable()
    {
        if (m_done || !m_port)
            return;
        while (!flowBlocked() && m_port->bytesAvailable() > 0) {
            const QByteArray b = m_port->read(64 * 1024);
            if (b.isEmpty())
                break;
            emitData(b);
        }
    }

    void shutdown(TransportState s, const QString &msg)
    {
        if (m_done)
            return;
        m_done = true;
        m_breakTimer.stop();
        if (m_port) {
            m_port->disconnect(this);
            if (m_port->isOpen())
                m_port->close();
        }
        setState(s, msg);
        finish();
    }

    SerialOptions m_opts;
    QSerialPort *m_port = nullptr;
    QTimer m_breakTimer{this};
    bool m_done = false;
};

} // namespace

SerialTransport::SerialTransport(const SerialOptions &opts, QObject *parent)
    : Transport(parent), m_opts(opts)
{
}

TransportWorker *SerialTransport::createWorker()
{
    return new SerialWorker(m_opts);
}

} // namespace termx
