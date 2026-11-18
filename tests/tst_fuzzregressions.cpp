// SPDX-License-Identifier: GPL-3.0-or-later
// Replays every input in fuzz/regressions/<target>/ through the fuzz harness,
// so crashes found by fuzzing stay fixed on every compiler (including MSVC).
#include <QtTest>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#ifndef TERMX_REGRESSION_DIR
#error TERMX_REGRESSION_DIR must be defined
#endif

class TstFuzzRegressions : public QObject {
    Q_OBJECT
private slots:
    void replay()
    {
        QDir dir(QStringLiteral(TERMX_REGRESSION_DIR));
        const QStringList files = dir.entryList(QDir::Files);
        QVERIFY2(!files.isEmpty(), qPrintable(dir.absolutePath()));
        for (const QString &f : files) {
            QFile file(dir.filePath(f));
            QVERIFY(file.open(QIODevice::ReadOnly));
            const QByteArray data = file.readAll();
            LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t *>(data.constData()), size_t(data.size()));
        }
    }
};

QTEST_GUILESS_MAIN(TstFuzzRegressions)
#include "tst_fuzzregressions.moc"
