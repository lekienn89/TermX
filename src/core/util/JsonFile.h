// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonDocument>
#include <QString>

namespace termx {

// Crash-safe JSON persistence: write to a temp file and atomically replace,
// keeping `backups` rotated copies (file.1 newest ... file.N oldest).
// load() falls back to the newest readable backup if the main file is corrupt.
namespace JsonFile {
bool save(const QString &path, const QJsonDocument &doc, int backups = 5, QString *error = nullptr);
QJsonDocument load(const QString &path, bool *recoveredFromBackup = nullptr, int backups = 5);
}

} // namespace termx
