// SPDX-License-Identifier: GPL-3.0-or-later
#include "TelnetTransport.h"
#include "TelnetProtocol.h"

#include <QTcpSocket>
#include <QTimer>

namespace termx {

namespace {

class TelnetWorker : public TransportWorker {
public:
    explicit TelnetWorker(TelnetOptions o) : m_opts(std::move(o)) {}

    void start() override
    {
        if (stopRequested()) {
            finish();
            return;
        }
        m_sock = new QTcpSocket(this);
        // Bounded buffer: when the UI is behind we stop reading and TCP
        // flow control slows the sender down.
        m_sock->setReadBufferSize(256 * 1024);
        m_proto.setTerminalType(m_opts.termType.toLatin1());
        m_proto.setWindowSize(m_shared->cols.load(), m_shared->rows.load());

        connect(m_sock, &QTcpSocket::connected, this, [this] {
            m_connectTimer.stop();
            m_sock->setSocketOption(QAbstractSocket::LowDelayOption, 1);
            m_sock->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
            if (!m_opts.rawTcp)
                m_sock->write(m_proto.initialNegotiation());
            m_connected = true;
            setState(TransportState::Connected, QString());
            onWake();
        });
        connect(m_sock, &QTcpSocket::readyRead, this, [this] { readAvailable(); });
        connect(m_sock, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError e) {
            if (e == QAbstractSocket::RemoteHostClosedError)
                shutdown(TransportState::Disconnected, QStringLiteral("Connection closed by remote host"));
            else
                shutdown(m_connected ? TransportState::Disconnected : TransportState::Failed, m_sock->errorString());
        });
        connect(m_sock, &QTcpSocket::disconnected, this, [this] {
            shutdown(TransportState::Disconnected, QStringLiteral("Connection closed"));
        });

        m_connectTimer.setSingleShot(true);
        connect(&m_connectTimer, &QTimer::timeout, this, [this] {
            shutdown(TransportState::Failed, QStringLiteral("Connection timed out"));
        });
        m_connectTimer.start(15000);

        setState(TransportState::Connecting, QStringLiteral("Connecting to %1:%2...").arg(m_opts.host).arg(m_opts.port));
        m_sock->connectToHost(m_opts.host.trimmed(), quint16(m_opts.port));
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
        if (!m_connected)
            return;
        if (m_shared->resizePending.exchange(false)) {
            m_proto.setWindowSize(m_shared->cols.load(), m_shared->rows.load());
            if (!m_opts.rawTcp)
                m_sock->write(m_proto.windowSizeMessage());
        }
        if (m_shared->breakPending.exchange(false) && !m_opts.rawTcp)
            m_sock->write(TelnetProtocol::breakCommand());
        std::deque<QByteArray> chunks;
        if (takeWrites(chunks)) {
            for (const QByteArray &c : chunks)
                m_sock->write(m_opts.rawTcp ? c : m_proto.encode(c));
        }
        readAvailable();
    }

private:
    void readAvailable()
    {
        if (m_done || !m_sock)
            return;
        while (!flowBlocked() && m_sock->bytesAvailable() > 0) {
            const QByteArray raw = m_sock->read(64 * 1024);
            if (raw.isEmpty())
                break;
            if (m_opts.rawTcp) {
                emitData(raw);
                continue;
            }
            QByteArray data, reply;
            m_proto.feed(raw.constData(), raw.size(), data, reply);
            if (!reply.isEmpty())
                m_sock->write(reply);
            emitData(data);
        }
    }

    void shutdown(TransportState s, const QString &msg)
    {
        if (m_done)
            return;
        m_done = true;
        m_connectTimer.stop();
        if (m_sock) {
            m_sock->disconnect(this);
            m_sock->abort();
        }
        setState(s, msg);
        finish();
    }

    TelnetOptions m_opts;
    TelnetProtocol m_proto;
    QTcpSocket *m_sock = nullptr;
    QTimer m_connectTimer{this};
    bool m_connected = false;
    bool m_done = false;
};

} // namespace

TelnetTransport::TelnetTransport(const TelnetOptions &opts, QObject *parent)
    : Transport(parent), m_opts(opts)
{
}

TransportWorker *TelnetTransport::createWorker()
{
    return new TelnetWorker(m_opts);
}

} // namespace termx
