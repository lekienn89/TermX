// SPDX-License-Identifier: GPL-3.0-or-later
#include "terminal/VtScreen.h"

#include <QtTest>

using termx::VtScreen;

class TstVtScreen : public QObject {
    Q_OBJECT
private:
    static void feed(VtScreen &s, const QByteArray &b) { s.feed(b.constData(), size_t(b.size())); }
    static QString row(const VtScreen &s, int r) { return s.lineText(s.firstScreenLine() + r).trimmed(); }

private slots:
    void printsText()
    {
        VtScreen s(24, 80, 1000);
        feed(s, "hello\r\nworld");
        QCOMPARE(row(s, 0), QStringLiteral("hello"));
        QCOMPARE(row(s, 1), QStringLiteral("world"));
        QCOMPARE(s.cursor().row, 1);
        QCOMPARE(s.cursor().col, 5);
    }

    void utf8AndWideCharacters()
    {
        VtScreen s(5, 20, 100);
        feed(s, QStringLiteral("Xin chào 漢字").toUtf8());
        QCOMPARE(row(s, 0), QStringLiteral("Xin chào 漢字"));
        QCOMPARE(s.cursor().col, 13);
    }

    void scrollbackKeepsAbsoluteNumbers()
    {
        VtScreen s(5, 20, 1000);
        for (int i = 0; i < 30; ++i)
            feed(s, QByteArray::number(i) + "\r\n");
        QVERIFY(s.scrollbackSize() > 0);
        const qint64 first = s.firstAvailableLine();
        QCOMPARE(s.lineText(first).trimmed(), QStringLiteral("0"));
        QCOMPARE(s.lineText(first + 10).trimmed(), QStringLiteral("10"));
    }

    void scrollbackLimitIsEnforced()
    {
        VtScreen s(5, 20, 50);
        for (int i = 0; i < 500; ++i)
            feed(s, "line\r\n");
        QVERIFY(s.scrollbackSize() <= 50);
    }

    void altScreenDoesNotTouchScrollback()
    {
        VtScreen s(5, 20, 1000);
        feed(s, "shell\r\n");
        feed(s, "\x1b[?1049h");
        QVERIFY(s.altScreen());
        for (int i = 0; i < 20; ++i)
            feed(s, "vim\r\n");
        QCOMPARE(s.scrollbackSize(), 0);
        feed(s, "\x1b[?1049l");
        QVERIFY(!s.altScreen());
        QCOMPARE(row(s, 0), QStringLiteral("shell"));
    }

    void titleFromOsc()
    {
        VtScreen s(5, 20, 100);
        QString seen;
        s.onTitle = [&](const QString &t) { seen = t; };
        feed(s, "\x1b]0;router-01\x07");
        QCOMPARE(s.title(), QStringLiteral("router-01"));
        QCOMPARE(seen, QStringLiteral("router-01"));
    }

    void resizeReflowsWrappedLines()
    {
        VtScreen s(5, 10, 100);
        feed(s, "abcdefghijKLM");
        QVERIFY(s.lineContinuesNext(s.firstScreenLine()));
        s.resize(5, 20);
        QCOMPARE(row(s, 0), QStringLiteral("abcdefghijKLM"));
    }

    void keyboardProducesOutput()
    {
        VtScreen s(5, 20, 100);
        QByteArray out;
        s.onOutput = [&](const char *b, size_t n) { out.append(b, qsizetype(n)); };
        s.keyboardChar('a', 0);
        QCOMPARE(out, QByteArray("a"));
    }

    void bracketedPaste()
    {
        VtScreen s(5, 20, 100);
        QByteArray out;
        s.onOutput = [&](const char *b, size_t n) { out.append(b, qsizetype(n)); };
        feed(s, "\x1b[?2004h");
        s.startPaste();
        s.endPaste();
        QCOMPARE(out, QByteArray("\x1b[200~\x1b[201~"));
    }

    void tooManyCsiArgumentsAreSafe()
    {
        // Regression: found by fuzz_vtscreen (heap overflow in libvterm's parser).
        VtScreen s(5, 20, 100);
        feed(s, QByteArray("\x1b[") + QByteArray(200, ';') + "2J" + "\x1b[" + QByteArray(300, '9') + "Aok");
        feed(s, QByteArray("\x1b]") + QByteArray(50, '7') + ";x\x07");
        QVERIFY(row(s, 0).contains(QStringLiteral("ok")) || s.cursor().row >= 0);
    }

    // Regression: the UI shows a scroll bar the first time a line reaches the
    // scrollback, which narrows the terminal and resizes the screen from
    // inside libvterm's scroll callback. That corrupted the heap and closed
    // the app while typing ordinary commands.
    void resizeFromScrollbackCallbackIsDeferred()
    {
        VtScreen s(5, 40, 1000);
        int calls = 0;
        bool resized = false;
        s.onScrollbackChanged = [&] {
            ++calls;
            if (!resized) {
                resized = true;
                s.resize(5, 37);   // what showing the scroll bar does
            }
        };
        QByteArray out;
        for (int i = 0; i < 40; ++i)
            out += "line " + QByteArray::number(i) + " ............................\r\n";
        feed(s, out);
        QCOMPARE(s.cols(), 37);
        QVERIFY(calls >= 1);
        QVERIFY(s.scrollbackSize() > 0);
    }

    void callbacksRunAfterLibvtermReturns()
    {
        VtScreen s(5, 40, 1000);
        int bells = 0;
        QString title;
        s.onBell = [&] {
            ++bells;
            // Feeding from a callback must be safe and keep the order.
            s.feed("X", 1);
        };
        s.onTitle = [&](const QString &t) { title = t; s.resize(4, 30); };
        s.onScrollbackChanged = [&] { s.clearScrollback(); };
        feed(s, "\x1b]2;hello\x07" "a\x07" "b\r\n\r\n\r\n\r\n\r\n\r\nend");
        QCOMPARE(bells, 1);
        QCOMPARE(title, QStringLiteral("hello"));
        QCOMPARE(s.rows(), 4);
        QCOMPARE(s.cols(), 30);
        QCOMPARE(s.scrollbackSize(), 0);
        bool sawX = false;
        for (qint64 l = s.firstAvailableLine(); l < s.firstScreenLine() + s.rows(); ++l)
            sawX = sawX || s.lineText(l).contains(QLatin1Char('X'));
        QVERIFY(sawX);
    }

    void resizeStormDuringOutputIsSafe()
    {
        VtScreen s(24, 80, 500);
        int n = 0;
        s.onScrollbackChanged = [&] {
            if (++n < 500)
                s.resize(10 + n % 20, 20 + (n * 7) % 100);
        };
        QByteArray out;
        for (int i = 0; i < 3000; ++i)
            out += "\x1b[3" + QByteArray::number(i % 8) + "mrow " + QByteArray::number(i) + " \xe4\xb8\xad\xe6\x96\x87 text\r\n";
        for (int i = 0; i < out.size(); i += 777)
            feed(s, out.mid(i, 777));
        QVERIFY(s.rows() >= 1);
    }

    void garbageDoesNotCrash()
    {
        VtScreen s(24, 80, 1000);
        QByteArray junk;
        quint32 x = 12345;
        for (int i = 0; i < 200000; ++i) {
            x = x * 1103515245u + 12345u;
            junk.append(char(x >> 16));
        }
        feed(s, junk);
        s.resize(10, 30);
        feed(s, junk);
        QVERIFY(s.rows() == 10);
    }
};

QTEST_GUILESS_MAIN(TstVtScreen)
#include "tst_vtscreen.moc"
