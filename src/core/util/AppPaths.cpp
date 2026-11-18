// SPDX-License-Identifier: GPL-3.0-or-later
#include "AppPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace termx {

namespace {
QString g_dataDir;
bool g_portable = false;

QString ensure(const QString &dir)
{
    QDir().mkpath(dir);
    return dir;
}
} // namespace

void AppPaths::initialize(const QString &overrideDataDir)
{
    if (!overrideDataDir.isEmpty()) {
        g_dataDir = overrideDataDir;
    } else {
        const QString exeDir = QCoreApplication::applicationDirPath();
        if (QFileInfo::exists(exeDir + QStringLiteral("/portable.txt"))) {
            g_portable = true;
            g_dataDir = exeDir + QStringLiteral("/data");
        } else {
            g_dataDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
            if (g_dataDir.isEmpty())
                g_dataDir = QDir::homePath() + QStringLiteral("/.termx");
        }
    }
    ensure(g_dataDir);
}

bool AppPaths::isPortable() { return g_portable; }

QString AppPaths::dataDir()
{
    if (g_dataDir.isEmpty())
        initialize();
    return g_dataDir;
}

QString AppPaths::logsDir() { return ensure(dataDir() + QStringLiteral("/logs")); }
QString AppPaths::crashDir() { return ensure(dataDir() + QStringLiteral("/crashes")); }
QString AppPaths::sessionsFile() { return dataDir() + QStringLiteral("/sessions.json"); }
QString AppPaths::devicesFile() { return dataDir() + QStringLiteral("/serial-devices.json"); }
QString AppPaths::historyDb() { return dataDir() + QStringLiteral("/history.sqlite"); }
QString AppPaths::knownHostsFile() { return dataDir() + QStringLiteral("/known_hosts"); }
QString AppPaths::settingsFile() { return dataDir() + QStringLiteral("/settings.ini"); }
QString AppPaths::stateFile() { return dataDir() + QStringLiteral("/state.json"); }
QString AppPaths::snippetsFile() { return dataDir() + QStringLiteral("/snippets.json"); }

} // namespace termx
