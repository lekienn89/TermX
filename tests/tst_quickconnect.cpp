// SPDX-License-Identifier: GPL-3.0-or-later
#include "session/QuickConnect.h"

#include <QtTest>

using namespace termx;

class TstQuickConnect : public QObject {
    Q_OBJECT
private slots:
    void ssh_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("user");
        QTest::addColumn<QString>("host");
        QTest::addColumn<int>("port");
        QTest::newRow("host") << "router1" << "" << "router1" << 22;
        QTest::newRow("user@host") << "admin@10.0.0.1" << "admin" << "10.0.0.1" << 22;
        QTest::newRow("host:port") << "10.0.0.1:2222" << "" << "10.0.0.1" << 2222;
        QTest::newRow("user@host:port") << "kien@web01:2200" << "kien" << "web01" << 2200;
        QTest::newRow("v6 brackets") << "admin@[fe80::1]:830" << "admin" << "fe80::1" << 830;
        QTest::newRow("v6 bare") << "2001:db8::10" << "" << "2001:db8::10" << 22;
        QTest::newRow("ssh cmd") << "ssh -p 2222 admin@router" << "admin" << "router" << 2222;
        QTest::newRow("ssh -l") << "ssh -l root -p2200 box" << "root" << "box" << 2200;
        QTest::newRow("ssh skips -o") << "ssh -o StrictHostKeyChecking=no u@h" << "u" << "h" << 22;
        QTest::newRow("url") << "ssh://ops@bastion:2022" << "ops" << "bastion" << 2022;
        QTest::newRow("trailing opts") << "admin@host -p 2022" << "admin" << "host" << 2022;
        QTest::newRow("user with @") << "kien@corp.local@jump" << "kien@corp.local" << "jump" << 22;
    }

    void ssh()
    {
        QFETCH(QString, text);
        QFETCH(QString, user);
        QFETCH(QString, host);
        QFETCH(int, port);
        SessionConfig c;
        QString err;
        QVERIFY2(QuickConnect::parse(text, c, &err), qPrintable(err));
        QCOMPARE(c.type, SessionType::Ssh);
        QCOMPARE(c.ssh.username, user);
        QCOMPARE(c.ssh.host, host);
        QCOMPARE(c.ssh.port, port);
    }

    void sshKeyFileWithSpaces()
    {
        SessionConfig c;
        QVERIFY(QuickConnect::parse(QStringLiteral("ssh -i \"C:\\My Keys\\id_ed25519\" admin@h"), c));
        QCOMPARE(c.ssh.keyFile, QStringLiteral("C:\\My Keys\\id_ed25519"));
    }

    void telnet()
    {
        SessionConfig c;
        QVERIFY(QuickConnect::parse(QStringLiteral("telnet 10.1.1.1 2001"), c));
        QCOMPARE(c.type, SessionType::Telnet);
        QCOMPARE(c.telnet.host, QStringLiteral("10.1.1.1"));
        QCOMPARE(c.telnet.port, 2001);
        QVERIFY(!c.telnet.rawTcp);
        QVERIFY(QuickConnect::parse(QStringLiteral("telnet://switch"), c));
        QCOMPARE(c.telnet.port, 23);
        QVERIFY(QuickConnect::parse(QStringLiteral("raw 10.0.0.5 4001"), c));
        QVERIFY(c.telnet.rawTcp);
        QCOMPARE(c.telnet.port, 4001);
        QVERIFY(!QuickConnect::parse(QStringLiteral("raw 10.0.0.5"), c));
    }

    void serial_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("port");
        QTest::addColumn<int>("baud");
        QTest::newRow("com") << "COM7" << "COM7" << 115200;
        QTest::newRow("lower") << "com12 115200" << "COM12" << 115200;
        QTest::newRow("colon") << "COM3:38400" << "COM3" << 38400;
        QTest::newRow("url") << "serial://COM7?baud=57600" << "COM7" << 57600;
        QTest::newRow("dev") << "/dev/ttyUSB0 115200" << "/dev/ttyUSB0" << 115200;
        QTest::newRow("serial cmd") << "serial COM9 19200" << "COM9" << 19200;
    }

    void serial()
    {
        QFETCH(QString, text);
        QFETCH(QString, port);
        QFETCH(int, baud);
        SessionConfig c;
        QString err;
        QVERIFY2(QuickConnect::parse(text, c, &err), qPrintable(err));
        QCOMPARE(c.type, SessionType::Serial);
        QCOMPARE(c.serial.portName, port);
        QCOMPARE(c.serial.baudRate, baud);
    }

    void local()
    {
        SessionConfig c;
        QVERIFY(QuickConnect::parse(QStringLiteral("local"), c));
        QCOMPARE(c.type, SessionType::LocalShell);
        QVERIFY(c.local.program.isEmpty());
        QVERIFY(QuickConnect::parse(QStringLiteral("wsl -d Ubuntu"), c));
        QCOMPARE(c.local.program, QStringLiteral("wsl.exe"));
        QCOMPARE(c.local.arguments, QStringList({QStringLiteral("-d"), QStringLiteral("Ubuntu")}));
    }

    void errors_data()
    {
        QTest::addColumn<QString>("text");
        QTest::newRow("empty") << "   ";
        QTest::newRow("bad port") << "host:99999";
        QTest::newRow("bad baud") << "COM3 fast";
        QTest::newRow("unknown url") << "ftp://x";
        QTest::newRow("remote cmd") << "ssh host uptime";
        QTest::newRow("no host") << "ssh -p 22";
        QTest::newRow("slash") << "host/name";
    }

    void errors()
    {
        QFETCH(QString, text);
        SessionConfig c;
        QString err;
        QVERIFY(!QuickConnect::parse(text, c, &err));
        QVERIFY(!err.isEmpty());
    }

    void roundTrip()
    {
        for (const char *t : {"admin@10.0.0.1:2222", "telnet 10.1.1.1 2001", "COM7 115200", "local", "[fe80::1]:830"}) {
            SessionConfig a, b;
            QVERIFY(QuickConnect::parse(QString::fromLatin1(t), a));
            QVERIFY2(QuickConnect::parse(QuickConnect::toText(a), b), t);
            QCOMPARE(QuickConnect::toText(a), QuickConnect::toText(b));
            QCOMPARE(a.target(), b.target());
        }
    }
    void sshForwards()
    {
        SessionConfig c;
        QString err;
        QVERIFY2(QuickConnect::parse(QStringLiteral("ssh -L 8080:localhost:80 -D1080 -R 9000:localhost:3000 admin@bastion"), c, &err),
                 qPrintable(err));
        QCOMPARE(c.ssh.host, QStringLiteral("bastion"));
        QCOMPARE(c.ssh.forwards.size(), 3);
        QCOMPARE(c.ssh.forwards[0].option(), QStringLiteral("-L 8080:localhost:80"));
        QCOMPARE(c.ssh.forwards[1].option(), QStringLiteral("-D 1080"));
        QCOMPARE(c.ssh.forwards[2].option(), QStringLiteral("-R 9000:localhost:3000"));
        const QString text = QuickConnect::toText(c);
        SessionConfig again;
        QVERIFY2(QuickConnect::parse(text, again, &err), qPrintable(err));
        QCOMPARE(again.ssh.forwards.size(), 3);
        QVERIFY(!QuickConnect::parse(QStringLiteral("ssh -L nonsense host"), c, &err));
        QVERIFY(err.contains(QStringLiteral("-L")));
    }
};

QTEST_GUILESS_MAIN(TstQuickConnect)
#include "tst_quickconnect.moc"
