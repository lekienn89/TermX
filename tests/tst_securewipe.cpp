// SPDX-License-Identifier: GPL-3.0-or-later
#include "util/SecureWipe.h"

#include <QtTest>

using namespace termx;

class TstSecureWipe : public QObject {
    Q_OBJECT
private slots:
    void zeroes()
    {
        char buf[8] = {'s', 'e', 'c', 'r', 'e', 't', '!', '!'};
        secureZero(buf, sizeof(buf));
        for (char c : buf)
            QCOMPARE(c, '\0');
    }

    void byteArray()
    {
        QByteArray b("hunter2");
        b.detach();
        secureWipe(b);
        QVERIFY(b.isEmpty());
    }

    void sharedCopyIsLeftIntact()
    {
        QString a = QStringLiteral("p@ss");
        a.detach();
        QString copy = a;   // shared: wiping `a` must not corrupt `copy`
        secureWipe(a);
        QVERIFY(a.isEmpty());
        QCOMPARE(copy, QStringLiteral("p@ss"));
    }

    void list()
    {
        QStringList l{QStringLiteral("a"), QStringLiteral("b")};
        secureWipe(l);
        QVERIFY(l.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TstSecureWipe)
#include "tst_securewipe.moc"
