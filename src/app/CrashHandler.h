// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QStringList>

namespace termx::CrashHandler {

// Writes a minidump to `dir` if the process crashes (Windows). The dump holds
// no terminal contents beyond what is in memory at the time; nothing is sent
// anywhere automatically.
void install(const QString &dir);

// Dumps written since the last call to acknowledge().
QStringList pendingReports(const QString &dir);
void acknowledge(const QString &dir);

} // namespace termx::CrashHandler
