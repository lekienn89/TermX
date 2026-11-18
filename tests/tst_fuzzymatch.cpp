// SPDX-License-Identifier: GPL-3.0-or-later
#include "util/FuzzyMatch.h"

#include <QtTest>

using termx::fuzzyScore;

class TstFuzzyMatch : public QObject {
    Q_OBJECT
private slots:
    void basics()
    {
        QCOMPARE(fuzzyScore(QString(), QStringLiteral("anything")), 0);
        QVERIFY(fuzzyScore(QStringLiteral("spr"), QStringLiteral("Split Right")) > 0);
        QVERIFY(fuzzyScore(QStringLiteral("xyz"), QStringLiteral("Split Right")) < 0);
        QVERIFY(fuzzyScore(QStringLiteral("SPLIT"), QStringLiteral("split down")) > 0);
    }

    void ranking()
    {
        // Word starts beat letters in the middle of words.
        QVERIFY(fuzzyScore(QStringLiteral("sr"), QStringLiteral("Split Right"))
                > fuzzyScore(QStringLiteral("sr"), QStringLiteral("Close Tabs Right")));
        // A plain substring beats a scattered match.
        QVERIFY(fuzzyScore(QStringLiteral("log"), QStringLiteral("Log to File"))
                > fuzzyScore(QStringLiteral("log"), QStringLiteral("Lock Group")));
    }
};

QTEST_GUILESS_MAIN(TstFuzzyMatch)
#include "tst_fuzzymatch.moc"
