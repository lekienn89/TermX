// SPDX-License-Identifier: GPL-3.0-or-later
#include "JsonFile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace termx::JsonFile {

bool save(const QString &path, const QJsonDocument &doc, int backups, QString *error)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    if (QFileInfo::exists(path) && backups > 0) {
        QFile::remove(path + QLatin1Char('.') + QString::number(backups));
        for (int i = backups - 1; i >= 1; --i)
            QFile::rename(path + QLatin1Char('.') + QString::number(i), path + QLatin1Char('.') + QString::number(i + 1));
        QFile::copy(path, path + QStringLiteral(".1"));
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    f.write(doc.toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}

static QJsonDocument tryLoad(const QString &path, bool *ok)
{
    *ok = false;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QJsonDocument();
    QJsonParseError err{};
    QJsonDocument d = QJsonDocument::fromJson(f.read(64 * 1024 * 1024), &err);
    *ok = err.error == QJsonParseError::NoError && !d.isNull();
    return d;
}

QJsonDocument load(const QString &path, bool *recoveredFromBackup, int backups)
{
    if (recoveredFromBackup)
        *recoveredFromBackup = false;
    bool ok = false;
    QJsonDocument d = tryLoad(path, &ok);
    if (ok || !QFileInfo::exists(path))
        return d;
    for (int i = 1; i <= backups; ++i) {
        d = tryLoad(path + QLatin1Char('.') + QString::number(i), &ok);
        if (ok) {
            if (recoveredFromBackup)
                *recoveredFromBackup = true;
            return d;
        }
    }
    return QJsonDocument();
}

} // namespace termx::JsonFile
