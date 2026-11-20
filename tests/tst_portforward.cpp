// SPDX-License-Identifier: GPL-3.0-or-later
#include "session/PortForward.h"
#include "session/SessionConfig.h"
#include "session/SessionImport.h"
#include "transport/SftpTypes.h"

#include <QtTest>

using namespace termx;
using Type = PortForward::Type;

class TstPortForward : public QObject {
    Q_OBJECT
private slots:
    void parseValid_data()
    {
        QTest::addColumn<QString>("option");
        QTest::addColumn<int>("type");
        QTest::addColumn<QString>("bind");
        QTest::addColumn<int>("bindPort");
        QTest::addColumn<QString>("host");
        QTest::addColumn<int>("port");
        QTest::addColumn<QString>("spec");
        QTest::newRow("L short") << "-L 8080:db:5432" << int(Type::Local) << "" << 8080 << "db" << 5432 << "8080:db:5432";
        QTest::newRow("L bind") << "L127.0.0.2:8080:db:5432" << int(Type::Local) << "127.0.0.2" << 8080 << "db" << 5432
                                << "127.0.0.2:8080:db:5432";
        QTest::newRow("L v6") << "-L [::1]:8080:[fe80::1]:80" << int(Type::Local) << "::1" << 8080 << "fe80::1" << 80
                              << "[::1]:8080:[fe80::1]:80";
        QTest::newRow("L star") << "-L *:8080:web:80" << int(Type::Local) << "0.0.0.0" << 8080 << "web" << 80
                                << "0.0.0.0:8080:web:80";
        QTest::newRow("R") << "-R 2222:localhost:22" << int(Type::Remote) << "" << 2222 << "localhost" << 22
                           << "2222:localhost:22";
        QTest::newRow("R port 0") << "R0:localhost:22" << int(Type::Remote) << "" << 0 << "localhost" << 22 << "0:localhost:22";
        QTest::newRow("D") << "-D 1080" << int(Type::Dynamic) << "" << 1080 << "" << 0 << "1080";
        QTest::newRow("D bind") << "d 0.0.0.0:1080" << int(Type::Dynamic) << "0.0.0.0" << 1080 << "" << 0 << "0.0.0.0:1080";
    }
    void parseValid()
    {
        QFETCH(QString, option);
        QFETCH(int, type);
        QFETCH(QString, bind);
        QFETCH(int, bindPort);
        QFETCH(QString, host);
        QFETCH(int, port);
        QFETCH(QString, spec);
        QString err;
        const auto f = PortForward::parseOption(option, &err);
        QVERIFY2(f.has_value(), qPrintable(err));
        QCOMPARE(int(f->type), type);
        QCOMPARE(f->bindAddress, bind);
        QCOMPARE(f->bindPort, bindPort);
        QCOMPARE(f->destHost, host);
        QCOMPARE(f->destPort, port);
        QCOMPARE(f->spec(), spec);
        // Round trip.
        const auto again = PortForward::parse(f->type, f->spec());
        QVERIFY(again.has_value());
        QVERIFY(again->sameRule(*f));
        const PortForward j = PortForward::fromJson(f->toJson());
        QVERIFY(j.sameRule(*f));
    }

    void parseInvalid_data()
    {
        QTest::addColumn<QString>("option");
        QTest::newRow("empty") << "";
        QTest::newRow("no type") << "8080:db:80";
        QTest::newRow("X") << "-X 8080:db:80";
        QTest::newRow("L missing") << "-L 8080:db";
        QTest::newRow("L port 0") << "-L 0:db:80";
        QTest::newRow("L big port") << "-L 70000:db:80";
        QTest::newRow("L bad dest port") << "-L 8080:db:x";
        QTest::newRow("L empty host") << "-L 8080::80";
        QTest::newRow("D too many") << "-D a:b:1080";
        QTest::newRow("R socks") << "-R 1080";
        QTest::newRow("unclosed bracket") << "-L [::1:8080:db:80";
        QTest::newRow("junk after bracket") << "-L [::1]x:8080:db:80";
        QTest::newRow("space in host") << "-L 8080:d b:80";
    }
    void parseInvalid()
    {
        QFETCH(QString, option);
        QString err;
        QVERIFY(!PortForward::parseOption(option, &err).has_value());
        QVERIFY(!err.isEmpty());
    }

    void putty()
    {
        QStringList warnings;
        const auto v = PortForward::parsePutty(QStringLiteral("L8080=db:5432,R2222=localhost:22,D1080,4L127.0.0.1:3000=web:80,Zbad"), &warnings);
        QCOMPARE(v.size(), 4);
        QCOMPARE(v[0].option(), QStringLiteral("-L 8080:db:5432"));
        QCOMPARE(v[1].option(), QStringLiteral("-R 2222:localhost:22"));
        QCOMPARE(v[2].option(), QStringLiteral("-D 1080"));
        QCOMPARE(v[3].option(), QStringLiteral("-L 127.0.0.1:3000:web:80"));
        QCOMPARE(warnings.size(), 1);
    }

    void sshConfigImport()
    {
        const QString cfg = QStringLiteral("Host db\n  HostName 10.0.0.5\n  LocalForward 5432 localhost:5432\n"
                                           "  LocalForward 127.0.0.1:8080 web:80\n  DynamicForward 1080\n"
                                           "  RemoteForward 9000 localhost:3000\n  LocalForward nonsense\n");
        const auto r = SessionImport::parseOpenSshConfig(cfg, QStringLiteral("/home/u"));
        QCOMPARE(r.sessions.size(), 1);
        const auto &fw = r.sessions.first().ssh.forwards;
        QCOMPARE(fw.size(), 4);
        QCOMPARE(fw[0].option(), QStringLiteral("-L 5432:localhost:5432"));
        QCOMPARE(fw[1].option(), QStringLiteral("-L 127.0.0.1:8080:web:80"));
        QCOMPARE(fw[2].option(), QStringLiteral("-D 1080"));
        QCOMPARE(fw[3].option(), QStringLiteral("-R 9000:localhost:3000"));
        QVERIFY(!r.warnings.isEmpty());
    }

    void sessionJson()
    {
        SessionConfig c;
        c.type = SessionType::Ssh;
        c.ssh.host = QStringLiteral("h");
        c.ssh.forwards << *PortForward::parseOption(QStringLiteral("-L 1:a:2"));
        PortForward d = *PortForward::parseOption(QStringLiteral("-D 1080"));
        d.enabled = false;
        d.description = QStringLiteral("browser");
        c.ssh.forwards << d;
        const SessionConfig back = SessionConfig::fromJson(c.toJson());
        QCOMPARE(back.ssh.forwards.size(), 2);
        QVERIFY(back.ssh.forwards[0].enabled);
        QVERIFY(!back.ssh.forwards[1].enabled);
        QCOMPARE(back.ssh.forwards[1].description, QStringLiteral("browser"));
        // Garbage in the file is dropped, not trusted.
        QJsonObject o = c.toJson();
        QJsonObject s = o.value(QStringLiteral("ssh")).toObject();
        s[QStringLiteral("forwards")] = QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("L")},
                                                               {QStringLiteral("spec"), QStringLiteral("99999:x")}}};
        o[QStringLiteral("ssh")] = s;
        QVERIFY(SessionConfig::fromJson(o).ssh.forwards.isEmpty());
    }

    void summary()
    {
        const auto f = PortForward::parseOption(QStringLiteral("-L 8080:db:5432"));
        QVERIFY(f->summary().contains(QStringLiteral("127.0.0.1:8080")));
        const auto r = PortForward::parseOption(QStringLiteral("-R 9000:localhost:3000"));
        QCOMPARE(r->effectiveBindAddress(), QStringLiteral("localhost"));
    }

    void safeLocalNames_data()
    {
        QTest::addColumn<QString>("remote");
        QTest::addColumn<bool>("ok");
        QTest::addColumn<QString>("local");
        QTest::newRow("plain") << "file.txt" << true << "file.txt";
        QTest::newRow("dotdot") << ".." << false << "";
        QTest::newRow("dot") << "." << false << "";
        QTest::newRow("slash") << "../../etc/passwd" << false << "";
        QTest::newRow("backslash") << "..\\..\\evil.exe" << true << ".._.._evil.exe";
        QTest::newRow("colon") << "a:b" << true << "a_b";
        QTest::newRow("stream") << "file.txt:ads" << true << "file.txt_ads";
        QTest::newRow("reserved") << "CON" << true << "_CON";
        QTest::newRow("reserved ext") << "com1.txt" << true << "_com1.txt";
        QTest::newRow("trailing dot") << "name. " << true << "name";
        QTest::newRow("only dots") << "..." << true << "_";
        QTest::newRow("control") << QStringLiteral("a\x01" "b") << true << "a_b";
        QTest::newRow("unicode") << QStringLiteral("tệp tin.txt") << true << QStringLiteral("tệp tin.txt");
    }
    void safeLocalNames()
    {
        QFETCH(QString, remote);
        QFETCH(bool, ok);
        QFETCH(QString, local);
        QString out;
        QCOMPARE(sftpSafeLocalName(remote, out), ok);
        if (ok)
            QCOMPARE(out, local);
    }

    void joinRemote()
    {
        QCOMPARE(sftpJoinRemote(QStringLiteral("/"), QStringLiteral("a")), QStringLiteral("/a"));
        QCOMPARE(sftpJoinRemote(QStringLiteral("/x"), QStringLiteral("a")), QStringLiteral("/x/a"));
        QCOMPARE(sftpJoinRemote(QStringLiteral("/x/"), QStringLiteral("a")), QStringLiteral("/x/a"));
    }
};

QTEST_GUILESS_MAIN(TstPortForward)
#include "tst_portforward.moc"
