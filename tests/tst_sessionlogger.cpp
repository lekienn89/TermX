// SPDX-License-Identifier: GPL-3.0-or-later
#include "terminal/SessionLogger.h"

#include <QtTest>

using termx::SessionLogger;

class TstSessionLogger : public QObject {
    Q_OBJECT
private slots:
    void stampsEachLine()
    {
        QByteArray out;
        bool atStart = true;
        SessionLogger::appendStamped("a\nb", "[T] ", atStart, out);
        QCOMPARE(out, QByteArray("[T] a\n[T] b"));
        QVERIFY(!atStart);
        // A line continued in the next chunk is not stamped twice.
        SessionLogger::appendStamped("c\n", "[U] ", atStart, out);
        QCOMPARE(out, QByteArray("[T] a\n[T] bc\n"));
        QVERIFY(atStart);
        SessionLogger::appendStamped("\n", "[V] ", atStart, out);
        QCOMPARE(out, QByteArray("[T] a\n[T] bc\n[V] \n"));
    }

    void writesStampedFile()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("s.log"));
        {
            SessionLogger log;
            log.setTimestamps(true);
            QVERIFY(log.start(path));
            log.write("\x1b[32mshow version\x1b[0m\r\nCisco IOS\r\n");
            log.stop();
        }
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QList<QByteArray> lines = f.readAll().split('\n');
        static const QRegularExpression stamp(QStringLiteral(R"(^\[\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3}\] )"));
        int stamped = 0;
        for (const QByteArray &l : lines) {
            if (l.contains("show version") || l.contains("Cisco IOS")) {
                QVERIFY2(stamp.match(QString::fromUtf8(l)).hasMatch(), l.constData());
                ++stamped;
            }
        }
        QCOMPARE(stamped, 2);
    }
};

QTEST_GUILESS_MAIN(TstSessionLogger)
#include "tst_sessionlogger.moc"
