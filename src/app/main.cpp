// SPDX-License-Identifier: GPL-3.0-or-later
#include "CrashHandler.h"
#include "MainWindow.h"
#include "Theme.h"
#include "session/SessionConfig.h"
#include "transport/SshTransport.h"
#include "transport/Transport.h"
#include "util/AppPaths.h"

#include <QApplication>
#include <QIcon>
#include <QCommandLineParser>
#include <QDesktopServices>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <cstdio>
#include <cstdlib>
#include <memory>

using namespace termx;

namespace {

// Detects repeated crashes during start-up and offers a safe mode that does
// not reopen the previous tabs.
class StartupGuard {
public:
    explicit StartupGuard(QSettings &s) : m_s(s)
    {
        m_failures = m_s.value(QStringLiteral("startup/failures"), 0).toInt();
        if (m_s.value(QStringLiteral("startup/inProgress"), false).toBool())
            ++m_failures;
        m_s.setValue(QStringLiteral("startup/failures"), m_failures);
        m_s.setValue(QStringLiteral("startup/inProgress"), true);
        m_s.sync();
    }
    bool suggestSafeMode() const { return m_failures >= 2; }
    void startedOk()
    {
        m_s.setValue(QStringLiteral("startup/inProgress"), false);
        m_s.setValue(QStringLiteral("startup/failures"), 0);
        m_s.sync();
    }

private:
    QSettings &m_s;
    int m_failures = 0;
};

// termx local  ssh://user@host:22  telnet://host:23  serial://COM7?baud=115200  or  user@host
bool sessionFromArgument(const QString &arg, SessionConfig &c)
{
    if (arg.compare(QLatin1String("local"), Qt::CaseInsensitive) == 0) {
        c.type = SessionType::LocalShell;
        return true;
    }
    QUrl url(arg.contains(QStringLiteral("://")) ? arg : QStringLiteral("ssh://") + arg);
    if (!url.isValid())
        return false;
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("ssh")) {
        c.type = SessionType::Ssh;
        c.ssh.host = url.host();
        c.ssh.port = url.port(22);
        c.ssh.username = url.userName();
        return !c.ssh.host.isEmpty();
    }
    if (scheme == QLatin1String("telnet") || scheme == QLatin1String("raw")) {
        c.type = SessionType::Telnet;
        c.telnet.host = url.host();
        c.telnet.port = url.port(23);
        c.telnet.rawTcp = scheme == QLatin1String("raw");
        return !c.telnet.host.isEmpty();
    }
    if (scheme == QLatin1String("serial")) {
        c.type = SessionType::Serial;
        c.serial.portName = url.host().isEmpty() ? url.path().remove(QLatin1Char('/')) : url.host().toUpper();
        const QUrlQuery q(url);
        if (q.hasQueryItem(QStringLiteral("baud")))
            c.serial.baudRate = qMax(50, q.queryItemValue(QStringLiteral("baud")).toInt());
        return !c.serial.portName.isEmpty();
    }
    return false;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("TermX"));
    QApplication::setOrganizationName(QStringLiteral("TermX"));
    QApplication::setApplicationVersion(QStringLiteral(TERMX_VERSION));
    {
        QIcon icon;
        for (int size : {16, 24, 32, 48, 64, 128, 256})
            icon.addFile(QStringLiteral(":/icons/termx-%1.png").arg(size), QSize(size, size));
        QApplication::setWindowIcon(icon);
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("TermX terminal"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption safeOpt(QStringLiteral("safe-mode"), QStringLiteral("Start without restoring tabs."));
    QCommandLineOption dataOpt(QStringLiteral("data-dir"), QStringLiteral("Use another data folder."), QStringLiteral("dir"));
    parser.addOption(safeOpt);
    parser.addOption(dataOpt);
    parser.addPositionalArgument(QStringLiteral("target"), QStringLiteral("ssh://user@host, telnet://host, serial://COM7?baud=9600"));
    parser.process(app);

    AppPaths::initialize(parser.value(dataOpt));
    CrashHandler::install(AppPaths::crashDir());
    registerTransportMetaTypes();
    if (!SshTransport::globalInit())
        std::fprintf(stderr, "libssh2 initialisation failed\n");

    QSettings settings(AppPaths::settingsFile(), QSettings::IniFormat);
    Theme::setTerminalFontSize(settings.value(QStringLiteral("ui/fontSize"), 11.0).toDouble());
    Theme::apply(Theme::modeFromName(settings.value(QStringLiteral("ui/theme"), QStringLiteral("system")).toString()));

    StartupGuard guard(settings);
    bool safeMode = parser.isSet(safeOpt);
    if (!safeMode && guard.suggestSafeMode()) {
        safeMode = QMessageBox::question(nullptr, QStringLiteral("TermX"),
                                         QStringLiteral("TermX did not start correctly the last two times.\n\n"
                                                        "Start in safe mode (do not reopen the previous tabs)?"))
                   == QMessageBox::Yes;
    }

    int rc = 0;
    {
        auto window = std::make_unique<MainWindow>(safeMode);
        window->show();

        const QStringList reports = CrashHandler::pendingReports(AppPaths::crashDir());
        if (!reports.isEmpty()) {
            CrashHandler::acknowledge(AppPaths::crashDir());
            QTimer::singleShot(500, window.get(), [w = window.get()] {
                QMessageBox box(QMessageBox::Warning, QStringLiteral("TermX"),
                                QStringLiteral("TermX closed unexpectedly last time. A crash report was saved "
                                               "on this computer. Please send it to the developers so the problem can be fixed."),
                                QMessageBox::Ok, w);
                QPushButton *open = box.addButton(QStringLiteral("Open Folder"), QMessageBox::ActionRole);
                box.exec();
                if (box.clickedButton() == open)
                    QDesktopServices::openUrl(QUrl::fromLocalFile(AppPaths::crashDir()));
            });
        }

        window->restoreTabs();
        for (const QString &arg : parser.positionalArguments()) {
            SessionConfig c;
            if (sessionFromArgument(arg, c))
                window->openSession(c);
        }
        QTimer::singleShot(15000, &app, [&guard] { guard.startedOk(); });

        rc = app.exec();
        guard.startedOk();
        window.reset();   // closes every session (I/O threads are told to stop)
    }

    if (!Transport::shutdownAll(3000)) {
        // An I/O thread is stuck inside a driver call: do not hang on exit.
        std::fflush(nullptr);
        std::_Exit(rc);
    }
    SshTransport::globalCleanup();
    return rc;
}
