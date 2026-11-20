// SPDX-License-Identifier: GPL-3.0-or-later
// Server side of the SOCKS 4, 4a and 5 handshakes (CONNECT only, no auth),
// used by dynamic port forwarding. Pure byte parsing, no I/O: fuzzed.
#pragma once

#include <QByteArray>
#include <QString>

namespace termx {

class SocksHandshake {
public:
    enum class Status {
        NeedMore,   // send takeReply() (may be empty), then wait for more bytes
        Connect,    // request complete: host()/port() are set; leftover() holds early data
        Failed      // protocol error: send takeReply() (may be empty) and close
    };

    // SOCKS5 reply codes (SOCKS4 maps everything but success to 0x5B).
    enum Reply : unsigned char {
        Succeeded = 0x00,
        GeneralFailure = 0x01,
        NotAllowed = 0x02,
        NetworkUnreachable = 0x03,
        HostUnreachable = 0x04,
        ConnectionRefused = 0x05,
        CommandNotSupported = 0x07,
        AddressTypeNotSupported = 0x08
    };

    static constexpr int kMaxBuffer = 1024;

    Status feed(const char *data, int len);
    Status feed(const QByteArray &d) { return feed(d.constData(), int(d.size())); }
    QByteArray takeReply();

    int version() const { return m_version; }
    QString host() const { return m_host; }
    quint16 port() const { return m_port; }
    QString error() const { return m_error; }
    QByteArray takeLeftover();

    // Final answer to the client once the server-side channel opened (or not).
    QByteArray finalReply(Reply code) const;

private:
    Status parse();
    Status fail(const QString &why, Reply code = GeneralFailure);

    enum class Stage { Start, Socks5Request, Done, Error } m_stage = Stage::Start;
    QByteArray m_buf;
    QByteArray m_reply;
    QByteArray m_leftover;
    int m_version = 0;
    QString m_host;
    quint16 m_port = 0;
    QString m_error;
};

} // namespace termx
