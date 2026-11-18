// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "serial/DeviceProfiles.h"
#include "serial/SerialPortWatcher.h"
#include "session/SessionConfig.h"
#include "transport/Transport.h"

#include <QMainWindow>
#include <QPointer>

#include <memory>

class QAction;
class QDockWidget;
class QLabel;
class QSettings;
class QTabWidget;

namespace termx {

class HistoryPanel;
class QuickConnectBar;
class CommandPalette;
class SnippetPanel;
class SnippetStore;
class HistoryStore;
class SerialPanel;
class SessionStore;
class SessionTreePanel;
class TabPage;
class TerminalSession;
class TerminalView;
class Toast;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(bool safeMode, QWidget *parent = nullptr);
    ~MainWindow() override;

    void openSession(const SessionConfig &cfg, int splitOrientation = 0);   // 0 = new tab
    void restoreTabs();

protected:
    void closeEvent(QCloseEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void changeEvent(QEvent *e) override;

private:
    void createActions();
    void createDocks();
    void wireSession(TerminalView *view);
    TabPage *currentPage() const;
    TerminalView *currentView() const;
    TerminalSession *currentSession() const;
    QList<TerminalView *> allViews() const;
    void updateTabFor(TerminalView *view);
    void updateStatus();
    void updateSerialInUse();
    void closeView(TerminalView *view);
    void closeTab(int index);
    void showSessionDialog(const SessionConfig &cfg, bool editingSaved, int splitOrientation = 0);
    void quickConnect();
    void refreshQuickBar();
    void focusQuickBar();
    void newLocalShell();
    void splitCurrent(Qt::Orientation o);
    void onPortAdded(const SerialPortEntry &port);
    void openSerialPort(const SerialPortEntry &port);
    void openSavedSession(const QString &id, int splitOrientation = 0);
    void showPrompt(TerminalSession *session, const PromptRequest &req);
    void setThemeMode(int mode);
    void zoom(int delta);
    void saveState();
    void showAbout();
    void showCommandPalette();
    void runSnippetText(const QString &text, bool pressEnter);
    void setBroadcastMode(int mode);   // 0 off, 1 this tab, 2 all tabs
    QList<TerminalSession *> broadcastTargetsFor(TerminalView *view) const;
    QIcon stateIcon(TransportState s) const;

    bool m_safeMode;
    std::unique_ptr<QSettings> m_settings;
    SessionStore *m_sessions;
    HistoryStore *m_history;
    SerialPortWatcher *m_watcher;
    DeviceProfiles m_profiles;

    QTabWidget *m_tabs;
    QuickConnectBar *m_quickBar;
    Toast *m_toast;
    SessionTreePanel *m_sessionPanel;
    SerialPanel *m_serialPanel;
    HistoryPanel *m_historyPanel;
    SnippetStore *m_snippets;
    CommandPalette *m_palette;
    SnippetPanel *m_snippetPanel;
    QDockWidget *m_snippetDock;
    QDockWidget *m_sessionDock;
    QDockWidget *m_serialDock;
    QDockWidget *m_historyDock;
    QLabel *m_statusState;
    QLabel *m_statusSize;
    QLabel *m_statusLog;
    QLabel *m_statusBroadcast;
    int m_broadcastMode = 0;
    QList<QAction *> m_broadcastActions;

    QAction *m_actReconnect = nullptr;
    QAction *m_actDisconnect = nullptr;
    QAction *m_actBreak = nullptr;
    QAction *m_actLog = nullptr;
    QAction *m_actQuickBar = nullptr;
    bool m_shown = false;
    bool m_quitting = false;
};

} // namespace termx
