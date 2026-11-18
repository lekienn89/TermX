// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

namespace termx {

// All files TermX writes. Portable mode: if "portable.txt" exists next to
// the executable, everything lives in <exe dir>/data instead of %LOCALAPPDATA%.
class AppPaths {
public:
    static void initialize(const QString &overrideDataDir = QString());
    static bool isPortable();
    static QString dataDir();
    static QString logsDir();
    static QString crashDir();
    static QString sessionsFile();
    static QString devicesFile();
    static QString historyDb();
    static QString knownHostsFile();
    static QString settingsFile();
    static QString stateFile();
};

} // namespace termx
