// SPDX-License-Identifier: GPL-3.0-or-later
#include "history/HistoryStore.h"

#include <QtTest>

using termx::HistoryStore;

class TstHistoryStore : public QObject {
    Q_OBJECT
private slots:
    void addSuggestSearch()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("h.sqlite"));
        {
            HistoryStore h;
            QString err;
            QVERIFY2(h.open(path, &err), qPrintable(err));
            h.add(QStringLiteral("show ip interface brief"), QStringLiteral("sw1"), QStringLiteral("ssh"));
            h.add(QStringLiteral("show running-config"), QStringLiteral("sw1"), QStringLiteral("ssh"));
            h.add(QStringLiteral("show running-config"), QStringLiteral("sw1"), QStringLiteral("ssh"));
            h.add(QStringLiteral("ping 8.8.8.8"), QStringLiteral("sw2"), QStringLiteral("ssh"));

            const auto s = h.suggest(QStringLiteral("show"), QStringLiteral("sw1"));
            QCOMPARE(s.size(), 2);
            QCOMPARE(s.first().text, QStringLiteral("show running-config"));   // used twice
            h.flush();

            const auto found = h.search(QStringLiteral("interface"));
            QCOMPARE(found.size(), 1);
            QCOMPARE(found.first().text, QStringLiteral("show ip interface brief"));
            QCOMPARE(h.search(QString()).size(), 3);
        }
        // Survives reopen.
        HistoryStore h;
        QVERIFY(h.open(path));
        QCOMPARE(h.suggest(QStringLiteral("ping"), QString()).size(), 1);
        const auto found = h.search(QStringLiteral("running"), QStringLiteral("sw1"));
        QCOMPARE(found.size(), 1);
        QCOMPARE(found.first().useCount, 2);
    }

    void csvRoundTrip()
    {
        QTemporaryDir dir;
        HistoryStore a;
        QVERIFY(a.open(dir.filePath(QStringLiteral("a.sqlite"))));
        a.add(QStringLiteral("echo \"quoted, comma\""), QStringLiteral("h"), QStringLiteral("ssh"));
        a.flush();
        const QString csv = dir.filePath(QStringLiteral("x.csv"));
        QVERIFY(a.exportCsv(csv));
        HistoryStore b;
        QVERIFY(b.open(dir.filePath(QStringLiteral("b.sqlite"))));
        QCOMPARE(b.importCsv(csv), 1);
        b.flush();
        QCOMPARE(b.search(QStringLiteral("quoted")).first().text, QStringLiteral("echo \"quoted, comma\""));
    }

    void corruptDatabaseIsReplaced()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("bad.sqlite"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(8192, 'x'));
        f.close();
        HistoryStore h;
        QVERIFY(h.open(path));
        h.add(QStringLiteral("ls"), QString(), QStringLiteral("local"));
        QCOMPARE(h.suggest(QStringLiteral("l"), QString()).size(), 1);
    }

    void frecencyPrefersRecent()
    {
        const qint64 now = 1'800'000'000;
        QVERIFY(HistoryStore::frecency(5, now, now) > HistoryStore::frecency(5, now - 60 * 86400, now));
        QVERIFY(HistoryStore::frecency(10, now, now) > HistoryStore::frecency(1, now, now));
    }
};

QTEST_GUILESS_MAIN(TstHistoryStore)
#include "tst_historystore.moc"
