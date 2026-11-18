// SPDX-License-Identifier: GPL-3.0-or-later
#include "history/CommandCapture.h"
#include "terminal/VtScreen.h"

#include <QtTest>

using termx::CommandCapture;
using termx::VtScreen;

class TstCommandCapture : public QObject {
    Q_OBJECT
private:
    // Simulates a shell that echoes what the user types.
    static QString typeAndEnter(VtScreen &s, CommandCapture &cap, const QString &cmd)
    {
        for (QChar c : cmd) {
            cap.onKey(s, false, true, QString(c), false);
            const QByteArray b = QString(c).toUtf8();
            s.feed(b.constData(), size_t(b.size()));
        }
        const auto r = cap.onKey(s, true, false, QStringLiteral("\r"), false);
        s.feed("\r\n", 2);
        return r.valid ? r.command : QString();
    }

private slots:
    void stripsCommonPrompts_data()
    {
        QTest::addColumn<QString>("line");
        QTest::addColumn<QString>("command");
        QTest::newRow("bash") << "kien@web01:~/app$ git status" << "git status";
        QTest::newRow("root") << "root@vm:~# ls -la" << "ls -la";
        QTest::newRow("rhel") << "[admin@db01 ~]$ df -h" << "df -h";
        QTest::newRow("cisco") << "SW-CORE-01#show ip int brief" << "show ip int brief";
        QTest::newRow("cisco-cfg") << "SW-CORE-01(config-if)# shutdown" << "shutdown";
        QTest::newRow("huawei") << "<HUAWEI>display version" << "display version";
        QTest::newRow("huawei-sys") << "[~HUAWEI-GigabitEthernet0/0/1]undo shutdown" << "undo shutdown";
        QTest::newRow("powershell") << "PS C:\\Users\\kien> Get-Process" << "Get-Process";
        QTest::newRow("cmd") << "C:\\Windows\\System32>ipconfig /all" << "ipconfig /all";
    }

    void stripsCommonPrompts()
    {
        QFETCH(QString, line);
        QFETCH(QString, command);
        bool matched = false;
        QCOMPARE(CommandCapture::stripPrompt(line, CommandCapture::defaultPromptPatterns(), &matched), command);
        QVERIFY(matched);
    }

    void secretPrompts()
    {
        QVERIFY(CommandCapture::looksLikeSecretPrompt(QStringLiteral("Password: ")));
        QVERIFY(CommandCapture::looksLikeSecretPrompt(QStringLiteral("[sudo] password for kien: ")));
        QVERIFY(CommandCapture::looksLikeSecretPrompt(QStringLiteral("Enter passphrase for key '/root/.ssh/id_ed25519': ")));
        QVERIFY(!CommandCapture::looksLikeSecretPrompt(QStringLiteral("root@vm:~# ")));
        QVERIFY(!CommandCapture::looksLikeSecretPrompt(QStringLiteral("Router#show spinning")));
    }

    void secretCommandsAreNotRecorded()
    {
        QVERIFY(CommandCapture::shouldRecord(QStringLiteral("ping 8.8.8.8")));
        QVERIFY(!CommandCapture::shouldRecord(QStringLiteral("mysql -u root --password=hunter2")));
        QVERIFY(!CommandCapture::shouldRecord(QStringLiteral("export API_KEY=abc")));
        QVERIFY(!CommandCapture::shouldRecord(QStringLiteral("sshpass -p secret ssh host")));
        QVERIFY(!CommandCapture::shouldRecord(QString()));
    }

    void capturesFromScreen()
    {
        VtScreen s(24, 80, 1000);
        CommandCapture cap;
        s.feed("root@vm:~# ", 11);
        QCOMPARE(typeAndEnter(s, cap, QStringLiteral("uname -a")), QStringLiteral("uname -a"));
        s.feed("Linux\r\nroot@vm:~# ", 18);
        QCOMPARE(typeAndEnter(s, cap, QStringLiteral("show run")), QStringLiteral("show run"));
    }

    void tabCompletionIsReadFromScreen()
    {
        VtScreen s(24, 80, 1000);
        CommandCapture cap;
        s.feed("Router#", 7);
        cap.onKey(s, false, true, QStringLiteral("s"), false);
        s.feed("s", 1);
        cap.onKey(s, false, true, QStringLiteral("h"), false);
        s.feed("h", 1);
        cap.onKey(s, false, false, QStringLiteral("\t"), true);
        s.feed("ow ", 3);   // device completes the word
        cap.onKey(s, false, true, QStringLiteral("v"), false);
        s.feed("v", 1);
        const auto r = cap.onKey(s, true, false, QStringLiteral("\r"), false);
        QVERIFY(r.valid);
        QCOMPARE(r.command, QStringLiteral("show v"));
    }

    void noEchoInputIsSkipped()
    {
        VtScreen s(24, 80, 1000);
        CommandCapture cap;
        s.feed("Password: ", 10);
        for (QChar c : QStringLiteral("hunter2"))
            cap.onKey(s, false, true, QString(c), false);   // nothing echoed
        const auto r = cap.onKey(s, true, false, QStringLiteral("\r"), false);
        QVERIFY(!r.valid);
    }

    void ctrlCResets()
    {
        VtScreen s(24, 80, 1000);
        CommandCapture cap;
        s.feed("$ ", 2);
        cap.onKey(s, false, true, QStringLiteral("x"), false);
        QVERIFY(cap.hasAnchor());
        cap.onKey(s, false, false, QStringLiteral("\x03"), false);
        QVERIFY(!cap.hasAnchor());
    }
};

QTEST_GUILESS_MAIN(TstCommandCapture)
#include "tst_commandcapture.moc"
