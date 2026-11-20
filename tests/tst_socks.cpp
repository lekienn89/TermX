// SPDX-License-Identifier: GPL-3.0-or-later
#include "transport/SocksProtocol.h"

#include <QtTest>

using namespace termx;
using S = SocksHandshake::Status;

class TstSocks : public QObject {
    Q_OBJECT
private slots:
    void socks5Domain()
    {
        SocksHandshake h;
        QCOMPARE(h.feed(QByteArray("\x05\x02\x02\x00", 4)), S::NeedMore);
        QCOMPARE(h.takeReply(), QByteArray("\x05\x00", 2));
        QByteArray req("\x05\x01\x00\x03", 4);
        req.append(char(11)).append("example.com").append("\x01\xBB", 2).append("GET");
        QCOMPARE(h.feed(req), S::Connect);
        QCOMPARE(h.host(), QStringLiteral("example.com"));
        QCOMPARE(h.port(), quint16(443));
        QCOMPARE(h.takeLeftover(), QByteArray("GET"));
        QCOMPARE(h.finalReply(SocksHandshake::Succeeded).size(), 10);
        QCOMPARE(h.finalReply(SocksHandshake::Succeeded).at(1), '\0');
    }

    void socks5ByteByByte()
    {
        QByteArray all("\x05\x01\x00", 3);
        all.append("\x05\x01\x00\x01\x0A\x00\x00\x01\x00\x16", 10);
        SocksHandshake h;
        S last = S::NeedMore;
        for (char c : all)
            last = h.feed(&c, 1);
        QCOMPARE(last, S::Connect);
        QCOMPARE(h.host(), QStringLiteral("10.0.0.1"));
        QCOMPARE(h.port(), quint16(22));
    }

    void socks5Ipv6()
    {
        SocksHandshake h;
        h.feed(QByteArray("\x05\x01\x00", 3));
        QByteArray req("\x05\x01\x00\x04", 4);
        QByteArray addr(16, '\0');
        addr[15] = 1;
        req.append(addr).append("\x00\x50", 2);
        QCOMPARE(h.feed(req), S::Connect);
        QCOMPARE(h.host(), QStringLiteral("::1"));
        QCOMPARE(h.port(), quint16(80));
    }

    void socks5NeedsAuth()
    {
        SocksHandshake h;
        QCOMPARE(h.feed(QByteArray("\x05\x01\x02", 3)), S::Failed);
        QCOMPARE(h.takeReply(), QByteArray("\x05\xFF", 2));
    }

    void socks5Bind()
    {
        SocksHandshake h;
        h.feed(QByteArray("\x05\x01\x00", 3));
        h.takeReply();
        QCOMPARE(h.feed(QByteArray("\x05\x02\x00\x01\x01\x02\x03\x04\x00\x50", 10)), S::Failed);
        const QByteArray r = h.takeReply();
        QCOMPARE(r.size(), 10);
        QCOMPARE(quint8(r.at(1)), quint8(SocksHandshake::CommandNotSupported));
    }

    void socks4()
    {
        SocksHandshake h;
        QByteArray req("\x04\x01\x00\x50\x7F\x00\x00\x01", 8);
        req.append("me").append('\0');
        QCOMPARE(h.feed(req), S::Connect);
        QCOMPARE(h.version(), 4);
        QCOMPARE(h.host(), QStringLiteral("127.0.0.1"));
        QCOMPARE(h.port(), quint16(80));
        QCOMPARE(quint8(h.finalReply(SocksHandshake::Succeeded).at(1)), quint8(0x5A));
        QCOMPARE(quint8(h.finalReply(SocksHandshake::HostUnreachable).at(1)), quint8(0x5B));
    }

    void socks4a()
    {
        SocksHandshake h;
        QByteArray req("\x04\x01\x01\xBB\x00\x00\x00\x09", 8);
        req.append('\0').append("intranet.local").append('\0');
        QCOMPARE(h.feed(req.left(12)), S::NeedMore);
        QCOMPARE(h.feed(req.mid(12)), S::Connect);
        QCOMPARE(h.host(), QStringLiteral("intranet.local"));
        QCOMPARE(h.port(), quint16(443));
    }

    void garbage_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("http") << QByteArray("GET / HTTP/1.1\r\n");
        QTest::newRow("bad atyp") << QByteArray("\x05\x01\x00\x05\x01\x00\x07", 7);
        QTest::newRow("port 0") << QByteArray("\x05\x01\x00\x05\x01\x00\x01\x01\x01\x01\x01\x00\x00", 13);
        QTest::newRow("empty host") << QByteArray("\x05\x01\x00\x05\x01\x00\x03\x00\x00\x50", 10);
        QTest::newRow("host with space") << QByteArray("\x05\x01\x00\x05\x01\x00\x03\x03\x61\x20\x62\x00\x50", 13);
        QTest::newRow("socks4 bind") << QByteArray("\x04\x02\x00\x50\x01\x01\x01\x01\x00", 9);
    }
    void garbage()
    {
        QFETCH(QByteArray, bytes);
        SocksHandshake h;
        QCOMPARE(h.feed(bytes), S::Failed);
        QVERIFY(!h.error().isEmpty());
        // Stays failed.
        QCOMPARE(h.feed(QByteArray("\x05\x01\x00", 3)), S::Failed);
    }

    void tooLong()
    {
        SocksHandshake h;
        QByteArray req("\x04\x01\x00\x50\x01\x02\x03\x04", 8);
        req.append(QByteArray(2000, 'u'));
        QCOMPARE(h.feed(req), S::Failed);
    }
};

QTEST_GUILESS_MAIN(TstSocks)
#include "tst_socks.moc"
