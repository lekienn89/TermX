// SPDX-License-Identifier: GPL-3.0-or-later
#include "MainWindow.h"

#include "Panels.h"
#include "PromptDialog.h"
#include "SessionDialog.h"
#include "TabPage.h"
#include "TerminalView.h"
#include "TerminalWidget.h"
#include "Theme.h"
#include "history/HistoryStore.h"
#include "session/CredentialStore.h"
#include "session/SessionStore.h"
#include "terminal/TerminalSession.h"
#include "util/AppPaths.h"
#include "util/JsonFile.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDir>
#include <QDesktopServices>
#include <QDockWidget>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QSettings>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QUrl>

#include <libssh2.h>

namespace termx {

MainWindow::MainWindow(bool safeMode, QWidget *parent)
    : QMainWindow(parent), m_safeMode(safeMode), m_profiles(AppPaths::devicesFile())
{
    setWindowTitle(QStringLiteral("TermX"));
    m_settings = std::make_unique<QSettings>(AppPaths::settingsFile(), QSettings::IniFormat);

    m_sessions = new SessionStore(AppPaths::sessionsFile(), this);
    QString warning;
    m_sessions->load(&warning);
    m_history = new HistoryStore(this);
    QString err;
    if (!m_history->open(AppPaths::historyDb(), &err))
        QTimer::singleShot(0, this, [this, err] {
            QMessageBox::warning(this, tr("Command history"), tr("Command history is unavailable: %1").arg(err));
        });
    m_profiles.load();
    m_watcher = new SerialPortWatcher(this);

    m_tabs = new QTabWidget(this);
    m_tabs->setDocumentMode(true);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setElideMode(Qt::ElideRight);
    m_tabs->tabBar()->setExpanding(false);
    m_tabs->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    auto *newTab = new QToolButton(m_tabs);
    newTab->setText(QStringLiteral("+"));
    newTab->setAutoRaise(true);
    newTab->setToolTip(tr("New session (Ctrl+Shift+N)"));
    m_tabs->setCornerWidget(newTab, Qt::TopRightCorner);
    connect(newTab, &QToolButton::clicked, this, &MainWindow::quickConnect);
    setCentralWidget(m_tabs);

    m_toast = new Toast(m_tabs);

    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this] {
        if (TerminalView *v = currentView()) {
            v->focusTerminal();
            m_historyPanel->setCurrentHost(v->session()->config().historyHost());
        }
        updateStatus();
    });
    connect(m_tabs->tabBar(), &QTabBar::customContextMenuRequested, this, [this](const QPoint &pos) {
        const int idx = m_tabs->tabBar()->tabAt(pos);
        if (idx < 0)
            return;
        m_tabs->setCurrentIndex(idx);
        QMenu menu(this);
        menu.addAction(tr("Duplicate Tab"), this, [this] {
            if (TerminalSession *s = currentSession())
                openSession(s->config());
        });
        menu.addAction(tr("Reconnect"), this, [this] {
            if (TerminalSession *s = currentSession())
                s->restart();
        });
        menu.addSeparator();
        menu.addAction(tr("Close Tab"), this, [this, idx] { closeTab(idx); });
        menu.addAction(tr("Close Other Tabs"), this, [this, idx] {
            QWidget *keep = m_tabs->widget(idx);
            for (int i = m_tabs->count() - 1; i >= 0; --i)
                if (m_tabs->widget(i) != keep)
                    closeTab(i);
        });
        menu.exec(m_tabs->tabBar()->mapToGlobal(pos));
    });

    createDocks();
    createActions();

    m_statusState = new QLabel(this);
    m_statusSize = new QLabel(this);
    m_statusLog = new QLabel(this);
    statusBar()->addWidget(m_statusState, 1);
    statusBar()->addPermanentWidget(m_statusLog);
    statusBar()->addPermanentWidget(m_statusSize);

    connect(m_watcher, &SerialPortWatcher::portAdded, this, &MainWindow::onPortAdded);
    m_watcher->start();

    restoreGeometry(m_settings->value(QStringLiteral("window/geometry")).toByteArray());
    restoreState(m_settings->value(QStringLiteral("window/state")).toByteArray());
    if (!m_settings->contains(QStringLiteral("window/geometry")))
        resize(1200, 760);

    if (!warning.isEmpty())
        QTimer::singleShot(0, this, [this, warning] { m_toast->showMessage(tr("Sessions restored"), warning, {}); });
    updateStatus();
}

MainWindow::~MainWindow()
{
    m_quitting = true;
    // Views own their sessions; delete tabs first so nothing calls back into panels.
    while (m_tabs->count() > 0) {
        QWidget *w = m_tabs->widget(0);
        m_tabs->removeTab(0);
        delete w;
    }
    m_history->close();
}

// ---------------------------------------------------------------- setup

void MainWindow::createDocks()
{
    m_sessionPanel = new SessionTreePanel(m_sessions, this);
    m_sessionDock = new QDockWidget(tr("Sessions"), this);
    m_sessionDock->setObjectName(QStringLiteral("dock.sessions"));
    m_sessionDock->setWidget(m_sessionPanel);
    addDockWidget(Qt::LeftDockWidgetArea, m_sessionDock);

    m_serialPanel = new SerialPanel(m_watcher, &m_profiles, this);
    m_serialDock = new QDockWidget(tr("Serial Ports"), this);
    m_serialDock->setObjectName(QStringLiteral("dock.serial"));
    m_serialDock->setWidget(m_serialPanel);
    addDockWidget(Qt::LeftDockWidgetArea, m_serialDock);

    m_historyPanel = new HistoryPanel(m_history, this);
    m_historyDock = new QDockWidget(tr("History"), this);
    m_historyDock->setObjectName(QStringLiteral("dock.history"));
    m_historyPanel->setMinimumWidth(360);
    m_historyDock->setWidget(m_historyPanel);
    addDockWidget(Qt::RightDockWidgetArea, m_historyDock);
    m_historyDock->hide();

    connect(m_sessionPanel, &SessionTreePanel::openRequested, this, [this](const QString &id) { openSavedSession(id); });
    connect(m_sessionPanel, &SessionTreePanel::editRequested, this, [this](const QString &id) {
        if (const SessionConfig *c = m_sessions->find(id))
            showSessionDialog(*c, true);
    });
    connect(m_sessionPanel, &SessionTreePanel::newRequested, this, [this](const QString &folder) {
        SessionConfig c;
        c.folder = folder;
        showSessionDialog(c, false);
    });
    connect(m_serialPanel, &SerialPanel::openRequested, this, &MainWindow::openSerialPort);
    connect(m_serialPanel, &SerialPanel::openWithSettingsRequested, this, [this](const SerialPortEntry &p) {
        SessionConfig c;
        c.type = SessionType::Serial;
        c.serial = m_profiles.optionsFor(p.deviceKey(), p.portName);
        c.name = m_profiles.nameFor(p.deviceKey());
        showSessionDialog(c, false);
    });
    connect(m_historyPanel, &HistoryPanel::insertRequested, this, [this](const QString &cmd, bool execute) {
        TerminalView *v = currentView();
        if (!v)
            return;
        v->session()->sendCommand(cmd, execute);
        v->focusTerminal();
    });
}

void MainWindow::createActions()
{
    auto add = [this](QMenu *menu, const QString &text, const QString &shortcut, auto slot) {
        QAction *a = menu->addAction(text);
        if (!shortcut.isEmpty())
            a->setShortcut(QKeySequence(shortcut));
        a->setShortcutContext(Qt::WindowShortcut);
        connect(a, &QAction::triggered, this, slot);
        addAction(a);
        return a;
    };

    QMenu *file = menuBar()->addMenu(tr("&File"));
    add(file, tr("New Session..."), QStringLiteral("Ctrl+Shift+N"), [this] { quickConnect(); });
    add(file, tr("New Local Shell"), QStringLiteral("Ctrl+Shift+T"), [this] { newLocalShell(); });
    add(file, tr("Duplicate Tab"), QString(), [this] {
        if (TerminalSession *s = currentSession())
            openSession(s->config());
    });
    file->addSeparator();
    add(file, tr("Close Pane"), QStringLiteral("Ctrl+Shift+W"), [this] {
        if (TerminalView *v = currentView())
            closeView(v);
    });
    file->addSeparator();
    add(file, tr("Open Data Folder"), QString(), [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(AppPaths::dataDir()));
    });
    file->addSeparator();
    add(file, tr("Exit"), QString(), [this] { close(); });

    QMenu *edit = menuBar()->addMenu(tr("&Edit"));
    add(edit, tr("Copy"), QStringLiteral("Ctrl+Shift+C"), [this] {
        if (TerminalView *v = currentView())
            v->terminal()->copySelection();
    });
    add(edit, tr("Paste"), QStringLiteral("Ctrl+Shift+V"), [this] {
        if (TerminalView *v = currentView())
            v->terminal()->pasteClipboard();
    });
    add(edit, tr("Select All"), QStringLiteral("Ctrl+Shift+A"), [this] {
        if (TerminalView *v = currentView())
            v->terminal()->selectAll();
    });
    edit->addSeparator();
    add(edit, tr("Find..."), QStringLiteral("Ctrl+Shift+F"), [this] {
        if (TerminalView *v = currentView())
            v->showFindBar();
    });
    add(edit, tr("Clear Scrollback"), QString(), [this] {
        if (TerminalView *v = currentView()) {
            v->session()->screen().clearScrollback();
            v->terminal()->update();
        }
    });

    QMenu *view = menuBar()->addMenu(tr("&View"));
    view->addAction(m_sessionDock->toggleViewAction());
    view->addAction(m_serialDock->toggleViewAction());
    QAction *hist = m_historyDock->toggleViewAction();
    hist->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+H")));
    view->addAction(hist);
    connect(hist, &QAction::toggled, this, [this](bool on) {
        if (on)
            QTimer::singleShot(0, m_historyPanel, &HistoryPanel::focusSearch);
    });
    view->addSeparator();
    QMenu *theme = view->addMenu(tr("Theme"));
    auto *group = new QActionGroup(this);
    const int current = int(Theme::mode());
    const QStringList names = {tr("Follow Windows"), tr("Dark"), tr("Light")};
    for (int i = 0; i < 3; ++i) {
        QAction *a = theme->addAction(names[i]);
        a->setCheckable(true);
        a->setChecked(i == current);
        group->addAction(a);
        connect(a, &QAction::triggered, this, [this, i] { setThemeMode(i); });
    }
    add(view, tr("Zoom In"), QStringLiteral("Ctrl+Alt+="), [this] { zoom(1); });
    add(view, tr("Zoom Out"), QStringLiteral("Ctrl+Alt+-"), [this] { zoom(-1); });
    add(view, tr("Reset Zoom"), QStringLiteral("Ctrl+Alt+0"), [this] { zoom(0); });

    QMenu *session = menuBar()->addMenu(tr("&Session"));
    m_actReconnect = add(session, tr("Reconnect"), QStringLiteral("Ctrl+Shift+R"), [this] {
        if (TerminalSession *s = currentSession())
            s->restart();
    });
    m_actDisconnect = add(session, tr("Disconnect"), QString(), [this] {
        if (TerminalSession *s = currentSession())
            s->disconnectByUser();
    });
    m_actBreak = add(session, tr("Send Break"), QStringLiteral("Ctrl+Shift+B"), [this] {
        if (TerminalSession *s = currentSession())
            s->sendBreak();
    });
    session->addSeparator();
    add(session, tr("Split Right"), QStringLiteral("Ctrl+Shift+D"), [this] { splitCurrent(Qt::Horizontal); });
    add(session, tr("Split Down"), QStringLiteral("Ctrl+Shift+E"), [this] { splitCurrent(Qt::Vertical); });
    session->addSeparator();
    m_actLog = add(session, tr("Log to File"), QStringLiteral("Ctrl+Shift+L"), [this] {
        TerminalSession *s = currentSession();
        if (!s)
            return;
        if (s->isLogging()) {
            s->stopLog();
        } else {
            QString err;
            if (!s->startLog(QString(), &err))
                QMessageBox::warning(this, tr("Log to file"), err);
            else
                m_toast->showMessage(tr("Logging"), s->logPath(), {{tr("Open Folder"), [] {
                    QDesktopServices::openUrl(QUrl::fromLocalFile(AppPaths::logsDir()));
                }}}, 6000);
        }
        updateStatus();
    });
    m_actLog->setCheckable(true);
    add(session, tr("Edit Session..."), QString(), [this] {
        TerminalSession *s = currentSession();
        if (!s)
            return;
        const SessionConfig *saved = s->config().id.isEmpty() ? nullptr : m_sessions->find(s->config().id);
        showSessionDialog(saved ? *saved : s->config(), saved != nullptr);
    });
    add(session, tr("Save Session..."), QString(), [this] {
        if (TerminalSession *s = currentSession()) {
            SessionConfig c = s->config();
            showSessionDialog(c, !c.id.isEmpty());
        }
    });

    // Tab navigation.
    auto *next = new QAction(this);
    next->setShortcuts({QKeySequence(QStringLiteral("Ctrl+Tab")), QKeySequence(QStringLiteral("Ctrl+PgDown"))});
    connect(next, &QAction::triggered, this, [this] {
        if (m_tabs->count() > 1)
            m_tabs->setCurrentIndex((m_tabs->currentIndex() + 1) % m_tabs->count());
    });
    addAction(next);
    auto *prev = new QAction(this);
    prev->setShortcuts({QKeySequence(QStringLiteral("Ctrl+Shift+Tab")), QKeySequence(QStringLiteral("Ctrl+PgUp"))});
    connect(prev, &QAction::triggered, this, [this] {
        if (m_tabs->count() > 1)
            m_tabs->setCurrentIndex((m_tabs->currentIndex() + m_tabs->count() - 1) % m_tabs->count());
    });
    addAction(prev);

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    add(help, tr("Keyboard Shortcuts"), QString(), [this] {
        QMessageBox::information(this, tr("Keyboard shortcuts"),
            tr("Ctrl+Shift+N  New session\nCtrl+Shift+T  New local shell\nCtrl+Shift+W  Close pane\n"
               "Ctrl+Shift+C / V  Copy / Paste (also Ctrl+Insert / Shift+Insert)\n"
               "Ctrl+Shift+F  Find in scrollback\nCtrl+Shift+H  History panel\n"
               "Ctrl+Shift+D / E  Split right / down\nCtrl+Shift+R  Reconnect\nCtrl+Shift+B  Send Break\n"
               "Ctrl+Shift+L  Log to file\nCtrl+Tab  Next tab\nShift+PgUp / PgDn  Scroll\n"
               "Ctrl+Alt+= / -  Zoom\n\nSuggestions while typing: Right or End accepts,\n"
               "Alt+Up / Alt+Down choose, Esc hides.\nSelecting text copies it; middle-click pastes."));
    });
    add(help, tr("About TermX"), QString(), [this] { showAbout(); });
}

// ---------------------------------------------------------------- sessions

void MainWindow::openSession(const SessionConfig &cfgIn, int splitOrientation)
{
    SessionConfig cfg = cfgIn;
    if (cfg.type == SessionType::Serial && cfg.serial.deviceKey.isEmpty()) {
        for (const SerialPortEntry &p : m_watcher->ports())
            if (p.portName.compare(cfg.serial.portName, Qt::CaseInsensitive) == 0)
                cfg.serial.deviceKey = p.deviceKey();
    }
    auto *session = new TerminalSession(cfg, m_history);
    auto *view = new TerminalView(session, m_history);
    view->terminal()->applyAppearance();

    TabPage *page = currentPage();
    if (splitOrientation != 0 && page) {
        page->split(view, Qt::Orientation(splitOrientation));
    } else {
        page = new TabPage(view);
        const int idx = m_tabs->addTab(page, stateIcon(TransportState::Connecting), cfg.displayName());
        connect(page, &TabPage::activeViewChanged, this, [this, page](TerminalView *) {
            if (page == currentPage()) {
                if (TerminalView *v = currentView())
                    m_historyPanel->setCurrentHost(v->session()->config().historyHost());
                updateStatus();
            }
            for (TerminalView *v : page->views())
                updateTabFor(v);
        });
        connect(page, &TabPage::becameEmpty, this, [this, page] {
            const int i = m_tabs->indexOf(page);
            if (i >= 0)
                m_tabs->removeTab(i);
            page->deleteLater();
            updateSerialInUse();
            updateStatus();
        });
        m_tabs->setCurrentIndex(idx);
    }
    wireSession(view);
    session->start();
    view->focusTerminal();
    updateTabFor(view);
    updateStatus();
}

void MainWindow::wireSession(TerminalView *view)
{
    TerminalSession *s = view->session();
    connect(s, &TerminalSession::stateChanged, this, [this, view, s](TransportState st, const QString &) {
        if (st == TransportState::Connected && s->config().type == SessionType::Serial) {
            const SerialOptions &o = s->config().serial;
            if (!o.deviceKey.isEmpty())
                m_profiles.remember(o.deviceKey, o, s->config().name);
        }
        updateTabFor(view);
        updateSerialInUse();
        if (view->session() == currentSession())
            updateStatus();
    });
    connect(s, &TerminalSession::titleChanged, this, [this, view] { updateTabFor(view); });
    connect(s, &TerminalSession::promptRequested, this, [this, s](const PromptRequest &req) { showPrompt(s, req); });
    connect(s, &TerminalSession::secretAccepted, this, [this, s](const QString &secret) {
        const QString id = s->config().id;
        if (id.isEmpty() || !CredentialStore::isAvailable())
            return;
        if (const SessionConfig *saved = m_sessions->find(id)) {
            SessionConfig c = *saved;
            c.encryptedPassword = CredentialStore::encrypt(secret);
            if (!c.encryptedPassword.isEmpty()) {
                m_sessions->upsert(c);
                SessionConfig live = s->config();
                live.encryptedPassword = c.encryptedPassword;
                s->setConfig(live);
            }
        }
    });
    connect(s, &TerminalSession::bell, this, [this] {
        if (!isActiveWindow())
            QApplication::alert(this, 1500);
    });
    connect(view->terminal(), &TerminalWidget::gridSizeChanged, this, [this, view] {
        if (view == currentView())
            updateStatus();
    });
}

void MainWindow::showPrompt(TerminalSession *session, const PromptRequest &req)
{
    // Bring the tab to front so the user sees which session asks.
    for (TerminalView *v : allViews()) {
        if (v->session() == session) {
            for (int i = 0; i < m_tabs->count(); ++i) {
                auto *page = qobject_cast<TabPage *>(m_tabs->widget(i));
                if (page && page->views().contains(v))
                    m_tabs->setCurrentIndex(i);
            }
        }
    }
    auto *dlg = new PromptDialog(req, session->config().displayName(), this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    QPointer<TerminalSession> guard(session);
    const quint64 id = req.id;
    connect(dlg, &QDialog::finished, this, [guard, dlg, id](int) {
        if (guard)
            guard->answerPrompt(id, dlg->reply());
    });
    // If the session dies while the dialog is open, close the dialog.
    connect(session, &QObject::destroyed, dlg, &QDialog::reject);
    dlg->open();
}

void MainWindow::showSessionDialog(const SessionConfig &cfg, bool editingSaved, int splitOrientation)
{
    SessionDialog dlg(cfg, m_sessions->folders(), m_watcher->ports(), editingSaved, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    SessionConfig c = dlg.config();
    if (dlg.action() == SessionDialog::SaveOnly || dlg.action() == SessionDialog::SaveAndConnect)
        c.id = m_sessions->upsert(c);
    if (c.type == SessionType::Serial && !c.serial.deviceKey.isEmpty())
        m_profiles.remember(c.serial.deviceKey, c.serial, c.name);
    if (dlg.action() == SessionDialog::ConnectOnly || dlg.action() == SessionDialog::SaveAndConnect)
        openSession(c, splitOrientation);
}

void MainWindow::openSavedSession(const QString &id, int splitOrientation)
{
    if (const SessionConfig *c = m_sessions->find(id))
        openSession(*c, splitOrientation);
}

void MainWindow::quickConnect()
{
    SessionConfig c;
    showSessionDialog(c, false);
}

void MainWindow::newLocalShell()
{
    SessionConfig c;
    c.type = SessionType::LocalShell;
    c.name = tr("Local shell");
    openSession(c);
}

void MainWindow::splitCurrent(Qt::Orientation o)
{
    TerminalSession *s = currentSession();
    if (!s) {
        newLocalShell();
        return;
    }
    // Duplicate the current connection into the new pane (serial ports cannot be shared).
    if (s->config().type == SessionType::Serial) {
        SessionConfig c;
        c.type = SessionType::LocalShell;
        c.name = tr("Local shell");
        openSession(c, int(o));
    } else {
        openSession(s->config(), int(o));
    }
}

void MainWindow::closeView(TerminalView *view)
{
    auto *page = qobject_cast<TabPage *>(m_tabs->currentWidget());
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *p = qobject_cast<TabPage *>(m_tabs->widget(i));
        if (p && p->views().contains(view))
            page = p;
    }
    if (page)
        page->closeView(view);
}

void MainWindow::closeTab(int index)
{
    auto *page = qobject_cast<TabPage *>(m_tabs->widget(index));
    if (!page)
        return;
    int connected = 0;
    for (TerminalView *v : page->views())
        if (v->session()->state() == TransportState::Connected && v->session()->config().type != SessionType::LocalShell)
            ++connected;
    if (connected > 0 && m_settings->value(QStringLiteral("ui/confirmCloseTab"), true).toBool()) {
        QMessageBox box(QMessageBox::Question, tr("Close tab"),
                        tr("Disconnect %1 and close the tab?").arg(m_tabs->tabText(index)),
                        QMessageBox::Yes | QMessageBox::No, this);
        box.setCheckBox(new QCheckBox(tr("Do not ask again"), &box));
        if (box.exec() != QMessageBox::Yes)
            return;
        if (box.checkBox()->isChecked())
            m_settings->setValue(QStringLiteral("ui/confirmCloseTab"), false);
    }
    m_tabs->removeTab(index);
    page->deleteLater();
    updateSerialInUse();
    updateStatus();
}

// ---------------------------------------------------------------- serial hot-plug

void MainWindow::onPortAdded(const SerialPortEntry &port)
{
    const QString key = port.deviceKey();
    // 1) A tab was waiting for this cable: reconnect it, whatever COM number it got.
    for (TerminalView *v : allViews()) {
        TerminalSession *s = v->session();
        const SessionConfig &c = s->config();
        if (c.type != SessionType::Serial || !c.serial.autoReconnect)
            continue;
        const TransportState st = s->state();
        if (st == TransportState::Connected || st == TransportState::Connecting)
            continue;
        const bool same = (!c.serial.deviceKey.isEmpty() && c.serial.deviceKey == key)
                          || (c.serial.deviceKey.isEmpty() && c.serial.portName.compare(port.portName, Qt::CaseInsensitive) == 0);
        if (same) {
            s->restartOnPort(port.portName);
            m_toast->showMessage(tr("Console cable reconnected"),
                                 tr("%1 → %2").arg(port.label(), c.displayName()), {}, 5000);
            return;
        }
    }
    // 2) The user asked to open this adapter automatically.
    if (m_profiles.autoOpen(key)) {
        openSerialPort(port);
        return;
    }
    // 3) Offer to open it.
    SessionConfig tmp;
    tmp.serial = m_profiles.optionsFor(key, port.portName);
    const QString name = m_profiles.nameFor(key);
    QString text = port.label();
    if (!port.chipName().isEmpty() && !text.contains(port.chipName(), Qt::CaseInsensitive))
        text += QStringLiteral(" [%1]").arg(port.chipName());
    if (!name.isEmpty())
        text += QStringLiteral("\n") + tr("Last used as \"%1\"").arg(name);
    m_toast->showMessage(tr("Console cable connected"), text,
                         {{tr("Open (%1)").arg(tmp.serialSummary()), [this, port] { openSerialPort(port); }},
                          {tr("Settings..."), [this, port] {
                               SessionConfig c;
                               c.type = SessionType::Serial;
                               c.serial = m_profiles.optionsFor(port.deviceKey(), port.portName);
                               c.name = m_profiles.nameFor(port.deviceKey());
                               showSessionDialog(c, false);
                           }}});
    QApplication::alert(this, 2000);
}

void MainWindow::openSerialPort(const SerialPortEntry &port)
{
    // Already open in a tab? Switch to it instead of failing with "port in use".
    for (TerminalView *v : allViews()) {
        const SessionConfig &c = v->session()->config();
        if (c.type == SessionType::Serial && c.serial.portName.compare(port.portName, Qt::CaseInsensitive) == 0
            && (v->session()->state() == TransportState::Connected || v->session()->state() == TransportState::Connecting)) {
            for (int i = 0; i < m_tabs->count(); ++i) {
                auto *page = qobject_cast<TabPage *>(m_tabs->widget(i));
                if (page && page->views().contains(v))
                    m_tabs->setCurrentIndex(i);
            }
            v->focusTerminal();
            return;
        }
    }
    SessionConfig c;
    c.type = SessionType::Serial;
    c.serial = m_profiles.optionsFor(port.deviceKey(), port.portName);
    c.name = m_profiles.nameFor(port.deviceKey());
    if (c.name.isEmpty())
        c.name = port.portName;
    openSession(c);
}

void MainWindow::updateSerialInUse()
{
    if (m_quitting)
        return;
    QStringList ports;
    for (TerminalView *v : allViews()) {
        const SessionConfig &c = v->session()->config();
        const TransportState st = v->session()->state();
        if (c.type == SessionType::Serial && (st == TransportState::Connected || st == TransportState::Connecting))
            ports << c.serial.portName;
    }
    m_serialPanel->setPortsInUse(ports);
}

// ---------------------------------------------------------------- tabs & status

TabPage *MainWindow::currentPage() const
{
    return qobject_cast<TabPage *>(m_tabs->currentWidget());
}

TerminalView *MainWindow::currentView() const
{
    TabPage *p = currentPage();
    return p ? p->activeView() : nullptr;
}

TerminalSession *MainWindow::currentSession() const
{
    TerminalView *v = currentView();
    return v ? v->session() : nullptr;
}

QList<TerminalView *> MainWindow::allViews() const
{
    QList<TerminalView *> out;
    for (int i = 0; i < m_tabs->count(); ++i)
        if (auto *p = qobject_cast<TabPage *>(m_tabs->widget(i)))
            out += p->views();
    return out;
}

QIcon MainWindow::stateIcon(TransportState s) const
{
    QColor c;
    switch (s) {
    case TransportState::Connected: c = QColor(0x3FB950); break;
    case TransportState::Connecting: c = QColor(0xD29922); break;
    case TransportState::Failed: c = QColor(0xF85149); break;
    default: c = QColor(0x8B949E); break;
    }
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawEllipse(QRectF(4, 4, 8, 8));
    return QIcon(pm);
}

void MainWindow::updateTabFor(TerminalView *view)
{
    if (m_quitting)
        return;
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *page = qobject_cast<TabPage *>(m_tabs->widget(i));
        if (!page || !page->views().contains(view))
            continue;
        TerminalView *active = page->activeView();
        TerminalSession *s = active ? active->session() : view->session();
        QString title = s->config().displayName();
        const int panes = int(page->views().size());
        if (panes > 1)
            title += QStringLiteral(" (+%1)").arg(panes - 1);
        m_tabs->setTabText(i, title);
        m_tabs->setTabIcon(i, stateIcon(s->state()));
        QString tip = QStringLiteral("%1\n%2").arg(s->config().target(), transportStateName(s->state()));
        if (!s->screen().title().isEmpty())
            tip += QStringLiteral("\n") + s->screen().title();
        m_tabs->setTabToolTip(i, tip);
    }
}

void MainWindow::updateStatus()
{
    if (m_quitting)
        return;
    TerminalSession *s = currentSession();
    const bool has = s != nullptr;
    m_actReconnect->setEnabled(has);
    m_actDisconnect->setEnabled(has && s->state() == TransportState::Connected);
    m_actBreak->setEnabled(has && s->supportsBreak() && s->state() == TransportState::Connected);
    m_actLog->setEnabled(has);
    m_actLog->setChecked(has && s->isLogging());
    if (!has) {
        m_statusState->setText(tr("No session. Press Ctrl+Shift+N to connect or plug in a console cable."));
        m_statusSize->clear();
        m_statusLog->clear();
        setWindowTitle(QStringLiteral("TermX"));
        return;
    }
    QString st = QStringLiteral("%1 · %2").arg(transportStateName(s->state()), s->config().target());
    if (!s->statusMessage().isEmpty() && s->state() != TransportState::Connected)
        st += QStringLiteral(" · ") + s->statusMessage();
    m_statusState->setText(st);
    m_statusSize->setText(QStringLiteral("%1×%2").arg(s->screen().cols()).arg(s->screen().rows()));
    m_statusLog->setText(s->isLogging() ? tr("Logging") : QString());
    m_statusLog->setToolTip(s->isLogging() ? s->logPath() : QString());
    setWindowTitle(QStringLiteral("%1 — TermX").arg(s->config().displayName()));
}

// ---------------------------------------------------------------- appearance

void MainWindow::setThemeMode(int mode)
{
    Theme::apply(Theme::Mode(mode));
    m_settings->setValue(QStringLiteral("ui/theme"), Theme::modeName(Theme::Mode(mode)));
    for (TerminalView *v : allViews())
        v->terminal()->applyAppearance();
    for (int i = 0; i < m_tabs->count(); ++i)
        if (auto *p = qobject_cast<TabPage *>(m_tabs->widget(i)))
            if (!p->views().isEmpty())
                updateTabFor(p->views().first());
}

void MainWindow::zoom(int delta)
{
    const qreal size = delta == 0 ? 11.0 : Theme::terminalFontSize() + delta;
    Theme::setTerminalFontSize(size);
    m_settings->setValue(QStringLiteral("ui/fontSize"), Theme::terminalFontSize());
    for (TerminalView *v : allViews())
        v->terminal()->applyAppearance();
}

void MainWindow::changeEvent(QEvent *e)
{
    QMainWindow::changeEvent(e);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (e->type() == QEvent::ThemeChange && Theme::mode() == Theme::Mode::System) {
        static bool busy = false;
        if (!busy) {
            busy = true;
            QTimer::singleShot(0, this, [this] {
                setThemeMode(int(Theme::Mode::System));
                busy = false;
            });
        }
    }
#endif
}

// ---------------------------------------------------------------- lifecycle

void MainWindow::showEvent(QShowEvent *e)
{
    QMainWindow::showEvent(e);
    if (!m_shown) {
        m_shown = true;
        m_watcher->registerWindow(quintptr(winId()));
        Theme::applyTitleBar(this);
    }
}

void MainWindow::resizeEvent(QResizeEvent *e)
{
    QMainWindow::resizeEvent(e);
    if (m_toast && m_toast->isVisible())
        m_toast->reposition();
}

void MainWindow::saveState()
{
    m_settings->setValue(QStringLiteral("window/geometry"), saveGeometry());
    m_settings->setValue(QStringLiteral("window/state"), QMainWindow::saveState());
    QJsonArray tabs;
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *page = qobject_cast<TabPage *>(m_tabs->widget(i));
        if (!page)
            continue;
        for (TerminalView *v : page->views()) {
            SessionConfig c = v->session()->config();
            c.encryptedPassword.clear();   // never duplicated into state.json
            QJsonObject o;
            o[QStringLiteral("config")] = c.toJson();
            o[QStringLiteral("savedId")] = c.id;
            tabs.append(o);
            break;   // restore one session per tab
        }
    }
    QJsonObject root;
    root[QStringLiteral("tabs")] = tabs;
    root[QStringLiteral("current")] = m_tabs->currentIndex();
    JsonFile::save(AppPaths::stateFile(), QJsonDocument(root), 1);
}

void MainWindow::restoreTabs()
{
    if (m_safeMode)
        return;
    if (!m_settings->value(QStringLiteral("ui/restoreTabs"), true).toBool())
        return;
    const QJsonObject root = JsonFile::load(AppPaths::stateFile()).object();
    for (const QJsonValue &v : root.value(QStringLiteral("tabs")).toArray()) {
        const QJsonObject o = v.toObject();
        const QString id = o.value(QStringLiteral("savedId")).toString();
        if (const SessionConfig *saved = id.isEmpty() ? nullptr : m_sessions->find(id))
            openSession(*saved);
        else
            openSession(SessionConfig::fromJson(o.value(QStringLiteral("config")).toObject()));
    }
    const int cur = root.value(QStringLiteral("current")).toInt(0);
    if (cur >= 0 && cur < m_tabs->count())
        m_tabs->setCurrentIndex(cur);
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    int connected = 0;
    for (TerminalView *v : allViews())
        if (v->session()->state() == TransportState::Connected && v->session()->config().type != SessionType::LocalShell)
            ++connected;
    if (connected > 0) {
        const auto r = QMessageBox::question(this, tr("Exit TermX"),
                                             tr("%n session(s) are still connected. Disconnect and exit?", nullptr, connected));
        if (r != QMessageBox::Yes) {
            e->ignore();
            return;
        }
    }
    saveState();
    m_settings->sync();
    m_quitting = true;
    e->accept();
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, tr("About TermX"),
        tr("<h3>TermX %1</h3>"
           "<p>SSH, Telnet and serial console terminal for Windows 10 and 11.</p>"
           "<p>Licensed under the GNU General Public License v3.</p>"
           "<p>Built with Qt %2, libssh2 %3, libvterm 0.3.3, SQLite and OpenSSL.<br>"
           "See THIRD_PARTY_NOTICES.txt for their licenses.</p>"
           "<p>Data folder: %4</p>")
            .arg(QStringLiteral(TERMX_VERSION), QString::fromLatin1(qVersion()),
                 QString::fromLatin1(libssh2_version(0) ? libssh2_version(0) : "?"),
                 QDir::toNativeSeparators(AppPaths::dataDir()).toHtmlEscaped()));
}

} // namespace termx
