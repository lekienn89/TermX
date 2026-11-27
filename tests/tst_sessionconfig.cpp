// SPDX-License-Identifier: GPL-3.0-or-later
#include "session/SessionConfig.h"

#include <QtTest>

using namespace termx;

class TstSessionConfig : public QObject {
    Q_OBJECT
private slots:
    void sshRoundTrip()
    {
        SessionConfig c;
        c.id = QStringLiteral("abc");
        c.name = QStringLiteral("core switch");
        c.folder = QStringLiteral("DC1/Network");
        c.type = SessionType::Ssh;
        c.ssh.host = QStringLiteral("10.0.0.1");
        c.ssh.port = 2222;
        c.ssh.username = QStringLiteral("admin");
        c.ssh.allowLegacyAlgorithms = true;
        c.ssh.hostKeyPolicy = HostKeyPolicy::AcceptNew;
        c.terminal.enterSends = NewlineMode::CRLF;
        const SessionConfig d = SessionConfig::fromJson(c.toJson());
        QCOMPARE(d.id, c.id);
        QCOMPARE(d.name, c.name);
        QCOMPARE(d.folder, c.folder);
        QCOMPARE(d.type, SessionType::Ssh);
        QCOMPARE(d.ssh.host, c.ssh.host);
        QCOMPARE(d.ssh.port, 2222);
        QCOMPARE(d.ssh.username, c.ssh.username);
        QCOMPARE(d.ssh.allowLegacyAlgorithms, true);
        QCOMPARE(d.ssh.hostKeyPolicy, HostKeyPolicy::AcceptNew);
        QCOMPARE(d.terminal.enterSends, NewlineMode::CRLF);
    }

    void secureDefaults()
    {
        // Missing keys (hand-written or quick-connect sessions) get the safe defaults.
        const SessionConfig d = SessionConfig::fromJson(QJsonObject{{QStringLiteral("type"), QStringLiteral("ssh")}});
        QCOMPARE(d.ssh.allowLegacyAlgorithms, false);
        QCOMPARE(d.ssh.hostKeyPolicy, HostKeyPolicy::Ask);
        QCOMPARE(hostKeyPolicyFromName(QStringLiteral("strict")), HostKeyPolicy::Strict);
        QCOMPARE(hostKeyPolicyName(HostKeyPolicy::Strict), QStringLiteral("strict"));
        QCOMPARE(hostKeyPolicyFromName(QStringLiteral("bogus")), HostKeyPolicy::Ask);
    }

    void serialRoundTrip()
    {
        SessionConfig c;
        c.type = SessionType::Serial;
        c.serial.portName = QStringLiteral("COM7");
        c.serial.baudRate = 115200;
        c.serial.parity = QLatin1Char('E');
        c.serial.stopBits = 2;
        c.serial.flow = SerialOptions::Flow::RtsCts;
        const SessionConfig d = SessionConfig::fromJson(c.toJson());
        QCOMPARE(d.type, SessionType::Serial);
        QCOMPARE(d.serial.portName, QStringLiteral("COM7"));
        QCOMPARE(d.serial.baudRate, 115200);
        QCOMPARE(d.serial.parity, QLatin1Char('E'));
        QCOMPARE(d.serial.stopBits, 2);
        QCOMPARE(d.serial.flow, SerialOptions::Flow::RtsCts);
        QCOMPARE(d.serialSummary(), QStringLiteral("115200 8E2"));
    }

    void badValuesAreClamped()
    {
        QJsonObject o;
        o[QStringLiteral("type")] = QStringLiteral("ssh");
        QJsonObject ssh;
        ssh[QStringLiteral("host")] = QStringLiteral("h");
        ssh[QStringLiteral("port")] = 999999;
        o[QStringLiteral("ssh")] = ssh;
        QJsonObject term;
        term[QStringLiteral("scrollbackLines")] = -5;
        o[QStringLiteral("terminal")] = term;
        const SessionConfig d = SessionConfig::fromJson(o);
        QVERIFY(d.ssh.port >= 1 && d.ssh.port <= 65535);
        QVERIFY(d.terminal.scrollbackLines >= 0);
    }

    void garbageJsonGivesDefaults()
    {
        const SessionConfig d = SessionConfig::fromJson(QJsonObject());
        QCOMPARE(d.ssh.port, 22);
        QCOMPARE(d.serial.baudRate, 115200);
    }
};

QTEST_GUILESS_MAIN(TstSessionConfig)
#include "tst_sessionconfig.moc"
