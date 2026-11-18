// SPDX-License-Identifier: GPL-3.0-or-later
#include "session/SessionImport.h"

#include <QtTest>

using namespace termx;

class TstSessionImport : public QObject {
    Q_OBJECT
private slots:
    void puttyRegUtf16()
    {
        const QString reg = QStringLiteral(
            "Windows Registry Editor Version 5.00\r\n\r\n"
            "[HKEY_CURRENT_USER\\Software\\SimonTatham\\PuTTY\\Sessions]\r\n\r\n"
            "[HKEY_CURRENT_USER\\Software\\SimonTatham\\PuTTY\\Sessions\\Default%20Settings]\r\n"
            "\"HostName\"=\"\"\r\n"
            "\"Protocol\"=\"ssh\"\r\n\r\n"
            "[HKEY_CURRENT_USER\\Software\\SimonTatham\\PuTTY\\Sessions\\Core%20Switch]\r\n"
            "\"HostName\"=\"admin@10.1.1.1\"\r\n"
            "\"PortNumber\"=dword:00000816\r\n"
            "\"Protocol\"=\"ssh\"\r\n"
            "\"PublicKeyFile\"=\"C:\\\\Users\\\\k\\\\key.ppk\"\r\n"
            "\"PingIntervalSecs\"=dword:0000003c\r\n"
            "\"Colour0\"=\"187,187,187\"\r\n"
            "\"Wordness0\"=hex:00,00,\\\r\n  00,00\r\n\r\n"
            "[HKEY_CURRENT_USER\\Software\\SimonTatham\\PuTTY\\Sessions\\Console]\r\n"
            "\"Protocol\"=\"serial\"\r\n"
            "\"SerialLine\"=\"COM7\"\r\n"
            "\"SerialSpeed\"=dword:0001c200\r\n"
            "\"SerialDataBits\"=dword:00000007\r\n"
            "\"SerialStopHalfbits\"=dword:00000004\r\n"
            "\"SerialParity\"=dword:00000002\r\n"
            "\"SerialFlowControl\"=dword:00000002\r\n\r\n"
            "[HKEY_CURRENT_USER\\Software\\SimonTatham\\PuTTY\\Sessions\\Old%20Box]\r\n"
            "\"HostName\"=\"oldbox\"\r\n"
            "\"Protocol\"=\"telnet\"\r\n"
            "\"PortNumber\"=dword:00000017\r\n\r\n"
            "[HKEY_CURRENT_USER\\Software\\SimonTatham\\PuTTY\\Sessions\\rl]\r\n"
            "\"HostName\"=\"x\"\r\n"
            "\"Protocol\"=\"rlogin\"\r\n");
        QByteArray data("\xFF\xFE", 2);
        data += QByteArray(reinterpret_cast<const char *>(reg.utf16()), reg.size() * 2);

        const SessionImport::Result r = SessionImport::parsePuttyReg(data);
        QCOMPARE(r.sessions.size(), 3);

        const SessionConfig &ssh = r.sessions[0];
        QCOMPARE(ssh.name, QStringLiteral("Core Switch"));
        QCOMPARE(ssh.folder, QStringLiteral("PuTTY"));
        QCOMPARE(ssh.type, SessionType::Ssh);
        QCOMPARE(ssh.ssh.host, QStringLiteral("10.1.1.1"));
        QCOMPARE(ssh.ssh.username, QStringLiteral("admin"));
        QCOMPARE(ssh.ssh.port, 2070);
        QCOMPARE(ssh.ssh.keepAliveSeconds, 60);
        QCOMPARE(ssh.ssh.keyFile, QStringLiteral("C:/Users/k/key.ppk"));

        const SessionConfig &ser = r.sessions[1];
        QCOMPARE(ser.type, SessionType::Serial);
        QCOMPARE(ser.serial.portName, QStringLiteral("COM7"));
        QCOMPARE(ser.serial.baudRate, 115200);
        QCOMPARE(ser.serial.dataBits, 7);
        QCOMPARE(ser.serial.stopBits, 2);
        QCOMPARE(ser.serial.parity, QLatin1Char('E'));
        QCOMPARE(ser.serial.flow, SerialOptions::Flow::RtsCts);

        const SessionConfig &tel = r.sessions[2];
        QCOMPARE(tel.type, SessionType::Telnet);
        QCOMPARE(tel.telnet.host, QStringLiteral("oldbox"));
        QVERIFY(!tel.telnet.rawTcp);

        // .ppk warning and unsupported rlogin; Default Settings is silent.
        QCOMPARE(r.warnings.size(), 2);
        QVERIFY(r.warnings[0].contains(QLatin1String(".ppk")));
        QVERIFY(r.warnings[1].contains(QLatin1String("rlogin")));
    }

    void puttyRegUtf8Empty()
    {
        const SessionImport::Result r = SessionImport::parsePuttyReg("REGEDIT4\n\n[HKEY_CURRENT_USER\\Software\\Other]\n\"a\"=\"b\"\n");
        QVERIFY(r.sessions.isEmpty());
        QCOMPARE(r.warnings.size(), 1);
    }

    void puttyRaw()
    {
        SessionConfig c;
        QString w;
        QVERIFY(SessionImport::fromPuttyValues(QStringLiteral("lab"),
                                               {{QStringLiteral("HostName"), QStringLiteral("10.0.0.5")},
                                                {QStringLiteral("Protocol"), QStringLiteral("raw")},
                                                {QStringLiteral("PortNumber"), 2001}},
                                               c, &w));
        QCOMPARE(c.type, SessionType::Telnet);
        QVERIFY(c.telnet.rawTcp);
        QCOMPARE(c.telnet.port, 2001);
        QVERIFY(w.isEmpty());
    }

    void decodeName() { QCOMPARE(SessionImport::decodePuttyName(QStringLiteral("a%20b%25c")), QStringLiteral("a b%c")); }

    void openSsh()
    {
        const QString cfg = QStringLiteral(
            "# comment\n"
            "User defaultuser\n"
            "ServerAliveInterval 20\n"
            "Host bastion jump\n"
            "    HostName bastion.example.com\n"
            "    Port 2222\n"
            "    IdentityFile ~/.ssh/id_ed25519\n"
            "Host web-*\n"
            "    User www\n"
            "Host db1\n"
            "    HostName=10.0.0.9\n"
            "    User postgres\n"
            "    ProxyJump bastion\n"
            "Match host foo\n"
            "    User nobody\n"
            "Host plain\n");
        const SessionImport::Result r = SessionImport::parseOpenSshConfig(cfg, QStringLiteral("/home/k"));
        QCOMPARE(r.sessions.size(), 4);
        QCOMPARE(r.sessions[0].name, QStringLiteral("bastion"));
        QCOMPARE(r.sessions[0].ssh.host, QStringLiteral("bastion.example.com"));
        QCOMPARE(r.sessions[0].ssh.port, 2222);
        QCOMPARE(r.sessions[0].ssh.username, QStringLiteral("defaultuser"));
        QCOMPARE(r.sessions[0].ssh.keyFile, QStringLiteral("/home/k/.ssh/id_ed25519"));
        QCOMPARE(r.sessions[0].ssh.keepAliveSeconds, 20);
        QCOMPARE(r.sessions[0].folder, QStringLiteral("SSH config"));
        QCOMPARE(r.sessions[1].name, QStringLiteral("jump"));
        QCOMPARE(r.sessions[2].ssh.host, QStringLiteral("10.0.0.9"));
        QCOMPARE(r.sessions[2].ssh.username, QStringLiteral("postgres"));
        QCOMPARE(r.sessions[3].ssh.host, QStringLiteral("plain"));
        QCOMPARE(r.sessions[3].ssh.port, 22);
        QCOMPARE(r.warnings.size(), 1);
        QVERIFY(r.warnings[0].startsWith(QLatin1String("db1")));
    }
};

QTEST_GUILESS_MAIN(TstSessionImport)
#include "tst_sessionimport.moc"
