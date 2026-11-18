// SPDX-License-Identifier: GPL-3.0-or-later
#include "util/JsonFile.h"

#include <QtTest>

using namespace termx;

class TstJsonFile : public QObject {
    Q_OBJECT
private slots:
    void saveAndLoad()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("s.json"));
        QJsonObject o;
        o[QStringLiteral("v")] = 1;
        QVERIFY(JsonFile::save(path, QJsonDocument(o)));
        QCOMPARE(JsonFile::load(path).object().value(QStringLiteral("v")).toInt(), 1);
    }

    void recoversFromBackupWhenCorrupt()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("s.json"));
        for (int i = 1; i <= 3; ++i) {
            QJsonObject o;
            o[QStringLiteral("v")] = i;
            QVERIFY(JsonFile::save(path, QJsonDocument(o)));
        }
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("{ truncated");
        f.close();
        bool recovered = false;
        const QJsonDocument d = JsonFile::load(path, &recovered);
        QVERIFY(recovered);
        QVERIFY(d.object().value(QStringLiteral("v")).toInt() >= 2);
    }

    void missingFileIsEmpty()
    {
        QVERIFY(JsonFile::load(QStringLiteral("/nonexistent/dir/x.json")).isNull());
    }
};

QTEST_GUILESS_MAIN(TstJsonFile)
#include "tst_jsonfile.moc"
