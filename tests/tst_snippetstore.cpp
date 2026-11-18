// SPDX-License-Identifier: GPL-3.0-or-later
#include "session/SnippetStore.h"

#include <QtTest>

using namespace termx;

class TstSnippetStore : public QObject {
    Q_OBJECT
private slots:
    void roundTripAndSort()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("snippets.json"));
        QString idB;
        {
            SnippetStore st(path);
            st.load();
            st.upsert({QString(), QStringLiteral("show interfaces"), QStringLiteral("show ip int brief"), QStringLiteral("Cisco")});
            idB = st.upsert({QString(), QStringLiteral("disk"), QStringLiteral("df -h\nfree -m"), QString()});
            QCOMPARE(st.snippets().size(), 2);
            QCOMPARE(st.snippets().first().name, QStringLiteral("disk"));   // no group sorts first
        }
        SnippetStore st(path);
        QVERIFY(st.load());
        QCOMPARE(st.snippets().size(), 2);
        const Snippet *b = st.find(idB);
        QVERIFY(b);
        QCOMPARE(b->text, QStringLiteral("df -h\nfree -m"));
        Snippet edited = *b;
        edited.name = QStringLiteral("zz disk");
        st.upsert(edited);
        QCOMPARE(st.snippets().size(), 2);
        QVERIFY(st.remove(idB));
        QVERIFY(!st.remove(idB));
        QCOMPARE(st.snippets().size(), 1);
    }
};

QTEST_GUILESS_MAIN(TstSnippetStore)
#include "tst_snippetstore.moc"
