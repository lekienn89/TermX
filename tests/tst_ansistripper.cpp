// SPDX-License-Identifier: GPL-3.0-or-later
#include "terminal/AnsiStripper.h"

#include <QtTest>

using termx::AnsiStripper;

class TstAnsiStripper : public QObject {
    Q_OBJECT
private:
    static QByteArray strip(const QByteArray &in)
    {
        AnsiStripper s;
        QByteArray out;
        s.feed(in.constData(), in.size(), out);
        return out;
    }

private slots:
    void removesColors() { QCOMPARE(strip("\x1b[1;31mred\x1b[0m text"), QByteArray("red text")); }
    void removesOscTitle() { QCOMPARE(strip("\x1b]0;title\x07" "ok"), QByteArray("ok")); }
    void removesOscWithSt() { QCOMPARE(strip("\x1b]2;t\x1b\\ok"), QByteArray("ok")); }
    void crlfBecomesLf() { QCOMPARE(strip("a\r\nb\r\n"), QByteArray("a\nb\n")); }
    void keepsUtf8() { QCOMPARE(strip(QStringLiteral("chào\r\n").toUtf8()), QStringLiteral("chào\n").toUtf8()); }

    void splitAcrossFeeds()
    {
        AnsiStripper s;
        QByteArray out;
        const QByteArray in = "x\x1b[38;5;196my";
        for (char c : in)
            s.feed(&c, 1, out);
        QCOMPARE(out, QByteArray("xy"));
    }
};

QTEST_GUILESS_MAIN(TstAnsiStripper)
#include "tst_ansistripper.moc"
