// SPDX-License-Identifier: GPL-3.0-or-later
#include "SocksProtocol.h"

#include <QHostAddress>

namespace termx {

namespace {
unsigned char u8(const QByteArray &b, int i)
{
    return static_cast<unsigned char>(b.at(i));
}

bool printableHost(const QByteArray &h)
{
    if (h.isEmpty())
        return false;
    for (char c : h) {
        const auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7F)
            return false;
    }
    return true;
}
} // namespace

SocksHandshake::Status SocksHandshake::feed(const char *data, int len)
{
    if (m_stage == Stage::Done)
        return Status::Connect;
    if (m_stage == Stage::Error)
        return Status::Failed;
    if (len > 0 && data)
        m_buf.append(data, len);
    if (m_buf.size() > kMaxBuffer)
        return fail(QStringLiteral("SOCKS request too long"));
    return parse();
}

QByteArray SocksHandshake::takeReply()
{
    QByteArray r;
    r.swap(m_reply);
    return r;
}

QByteArray SocksHandshake::takeLeftover()
{
    QByteArray r;
    r.swap(m_leftover);
    return r;
}

SocksHandshake::Status SocksHandshake::fail(const QString &why, Reply code)
{
    m_error = why;
    if (m_version == 5)
        m_reply += finalReply(code);
    else if (m_version == 4)
        m_reply += finalReply(code);
    m_stage = Stage::Error;
    m_buf.clear();
    return Status::Failed;
}

QByteArray SocksHandshake::finalReply(Reply code) const
{
    if (m_version == 4) {
        QByteArray r(8, '\0');
        r[1] = char(code == Succeeded ? 0x5A : 0x5B);
        return r;
    }
    // VER REP RSV ATYP=IPv4 0.0.0.0 port 0
    QByteArray r(10, '\0');
    r[0] = 0x05;
    r[1] = char(code);
    r[3] = 0x01;
    return r;
}

SocksHandshake::Status SocksHandshake::parse()
{
    for (;;) {
        switch (m_stage) {
        case Stage::Start: {
            if (m_buf.isEmpty())
                return Status::NeedMore;
            const unsigned char ver = u8(m_buf, 0);
            if (ver == 5) {
                m_version = 5;
                if (m_buf.size() < 2)
                    return Status::NeedMore;
                const int n = u8(m_buf, 1);
                if (m_buf.size() < 2 + n)
                    return Status::NeedMore;
                bool noAuth = false;
                for (int i = 0; i < n; ++i)
                    noAuth = noAuth || u8(m_buf, 2 + i) == 0x00;
                m_buf.remove(0, 2 + n);
                if (!noAuth) {
                    m_reply += QByteArray("\x05\xFF", 2);
                    m_stage = Stage::Error;
                    m_error = QStringLiteral("SOCKS5 client requires authentication");
                    return Status::Failed;
                }
                m_reply += QByteArray("\x05\x00", 2);
                m_stage = Stage::Socks5Request;
                continue;
            }
            if (ver == 4) {
                m_version = 4;
                // VER CMD PORT(2) IP(4) USERID\0 [HOST\0]
                if (m_buf.size() < 9)
                    return Status::NeedMore;
                if (u8(m_buf, 1) != 0x01)
                    return fail(QStringLiteral("SOCKS4: only CONNECT is supported"));
                const int userEnd = m_buf.indexOf('\0', 8);
                if (userEnd < 0)
                    return m_buf.size() > 8 + 255 ? fail(QStringLiteral("SOCKS4 user id too long")) : Status::NeedMore;
                m_port = quint16((u8(m_buf, 2) << 8) | u8(m_buf, 3));
                const bool is4a = u8(m_buf, 4) == 0 && u8(m_buf, 5) == 0 && u8(m_buf, 6) == 0 && u8(m_buf, 7) != 0;
                int consumed = userEnd + 1;
                if (is4a) {
                    const int hostEnd = m_buf.indexOf('\0', userEnd + 1);
                    if (hostEnd < 0)
                        return m_buf.size() - userEnd > 256 ? fail(QStringLiteral("SOCKS4a host name too long"))
                                                            : Status::NeedMore;
                    const QByteArray h = m_buf.mid(userEnd + 1, hostEnd - userEnd - 1);
                    if (!printableHost(h))
                        return fail(QStringLiteral("SOCKS4a: invalid host name"));
                    m_host = QString::fromUtf8(h);
                    consumed = hostEnd + 1;
                } else {
                    m_host = QStringLiteral("%1.%2.%3.%4").arg(u8(m_buf, 4)).arg(u8(m_buf, 5)).arg(u8(m_buf, 6)).arg(u8(m_buf, 7));
                }
                if (m_port == 0)
                    return fail(QStringLiteral("SOCKS4: port 0"));
                m_leftover = m_buf.mid(consumed);
                m_buf.clear();
                m_stage = Stage::Done;
                return Status::Connect;
            }
            return fail(QStringLiteral("Not a SOCKS request (version byte %1)").arg(int(ver)));
        }
        case Stage::Socks5Request: {
            // VER CMD RSV ATYP ADDR PORT(2)
            if (m_buf.size() < 4)
                return Status::NeedMore;
            if (u8(m_buf, 0) != 5)
                return fail(QStringLiteral("SOCKS5: bad request version"));
            const unsigned char cmd = u8(m_buf, 1);
            const unsigned char atyp = u8(m_buf, 3);
            int need = 0;
            if (atyp == 0x01)
                need = 4 + 4 + 2;
            else if (atyp == 0x04)
                need = 4 + 16 + 2;
            else if (atyp == 0x03) {
                if (m_buf.size() < 5)
                    return Status::NeedMore;
                need = 4 + 1 + u8(m_buf, 4) + 2;
            } else
                return fail(QStringLiteral("SOCKS5: unknown address type"), AddressTypeNotSupported);
            if (m_buf.size() < need)
                return Status::NeedMore;
            if (cmd != 0x01)
                return fail(QStringLiteral("SOCKS5: only CONNECT is supported"), CommandNotSupported);
            if (atyp == 0x01) {
                m_host = QStringLiteral("%1.%2.%3.%4").arg(u8(m_buf, 4)).arg(u8(m_buf, 5)).arg(u8(m_buf, 6)).arg(u8(m_buf, 7));
            } else if (atyp == 0x04) {
                Q_IPV6ADDR a;
                for (int i = 0; i < 16; ++i)
                    a[i] = u8(m_buf, 4 + i);
                m_host = QHostAddress(a).toString();
            } else {
                const QByteArray h = m_buf.mid(5, u8(m_buf, 4));
                if (!printableHost(h))
                    return fail(QStringLiteral("SOCKS5: invalid host name"), HostUnreachable);
                m_host = QString::fromUtf8(h);
            }
            m_port = quint16((u8(m_buf, need - 2) << 8) | u8(m_buf, need - 1));
            if (m_port == 0)
                return fail(QStringLiteral("SOCKS5: port 0"), HostUnreachable);
            m_leftover = m_buf.mid(need);
            m_buf.clear();
            m_stage = Stage::Done;
            return Status::Connect;
        }
        case Stage::Done:
            return Status::Connect;
        case Stage::Error:
            return Status::Failed;
        }
    }
}

} // namespace termx
