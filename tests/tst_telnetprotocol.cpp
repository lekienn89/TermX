// SPDX-License-Identifier: GPL-3.0-or-later
#include "transport/TelnetProtocol.h"

#include <QtTest>

using termx::TelnetProtocol;

class TstTelnetProtocol : public QObject {
    Q_OBJECT
private:
    static QByteArray cmd(unsigned char c, unsigned char o) { const char b[3] = {char(0xff), char(c), char(o)}; return QByteArray(b, 3); }

private slots:
    void plainDataPassesThrough()
    {
        TelnetProtocol p;
        QByteArray data, reply;
        const QByteArray in = "hello\r\nworld";
        p.feed(in.constData(), in.size(), data, reply);
        QCOMPARE(data, in);
        QVERIFY(reply.isEmpty());
    }

    void doubledIacIsData()
    {
        TelnetProtocol p;
        QByteArray data, reply;
        const QByteArray in("a\xff\xff" "b", 4);
        p.feed(in.constData(), in.size(), data, reply);
        QCOMPARE(data, QByteArray("a\xff" "b", 3));
    }

    void answersEchoAndRefusesUnknown()
    {
        TelnetProtocol p;
        QByteArray data, reply;
        QByteArray in = cmd(TelnetProtocol::WILL, TelnetProtocol::OPT_ECHO) + cmd(TelnetProtocol::DO, 99);
        p.feed(in.constData(), in.size(), data, reply);
        QVERIFY(data.isEmpty());
        QVERIFY(p.remoteEcho());
        QVERIFY(reply.contains(cmd(TelnetProtocol::DO, TelnetProtocol::OPT_ECHO)));
        QVERIFY(reply.contains(cmd(TelnetProtocol::WONT, 99)));

        // Asking again for a refused option must not cause a reply loop.
        reply.clear();
        in = cmd(TelnetProtocol::DO, 99);
        p.feed(in.constData(), in.size(), data, reply);
        QVERIFY(reply.isEmpty());
    }

    void commandSplitAcrossReads()
    {
        TelnetProtocol p;
        QByteArray data, reply;
        const QByteArray in = "x" + cmd(TelnetProtocol::WILL, TelnetProtocol::OPT_SGA) + "y";
        for (char c : in)
            p.feed(&c, 1, data, reply);
        QCOMPARE(data, QByteArray("xy"));
        QVERIFY(p.remoteOption(TelnetProtocol::OPT_SGA));
    }

    void terminalTypeSubnegotiation()
    {
        TelnetProtocol p;
        p.setTerminalType("XTERM-256COLOR");
        QByteArray data, reply;
        QByteArray in = cmd(TelnetProtocol::DO, TelnetProtocol::OPT_TTYPE);
        in += QByteArray("\xff\xfa\x18\x01\xff\xf0", 6);   // SB TTYPE SEND IAC SE
        p.feed(in.constData(), in.size(), data, reply);
        QVERIFY(data.isEmpty());
        QVERIFY(reply.contains(QByteArray("\xff\xfa\x18\x00" "XTERM-256COLOR\xff\xf0", 20)));
    }

    void nawsReportsSize()
    {
        TelnetProtocol p;
        p.setWindowSize(132, 50);
        QByteArray data, reply;
        const QByteArray in = cmd(TelnetProtocol::DO, TelnetProtocol::OPT_NAWS);
        p.feed(in.constData(), in.size(), data, reply);
        QVERIFY(reply.contains(QByteArray("\xff\xfa\x1f\x00\x84\x00\x32\xff\xf0", 9)));
    }

    void encodeEscapesIacAndCr()
    {
        TelnetProtocol p;
        const QByteArray out = p.encode(QByteArray("a\xff\r", 3));
        QCOMPARE(out, QByteArray("a\xff\xff\r\0", 5));
    }

    void oversizedSubnegotiationIsBounded()
    {
        TelnetProtocol p;
        QByteArray data, reply;
        QByteArray in("\xff\xfa\x18", 3);
        in += QByteArray(100000, 'A');
        in += QByteArray("\xff\xf0" "ok", 4);
        p.feed(in.constData(), in.size(), data, reply);
        QCOMPARE(data, QByteArray("ok"));
    }
};

QTEST_GUILESS_MAIN(TstTelnetProtocol)
#include "tst_telnetprotocol.moc"
