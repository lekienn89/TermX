// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "serial/SerialPortWatcher.h"
#include "session/SessionConfig.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QStackedWidget;

namespace termx {

class ForwardRulesEditor;

class SessionDialog : public QDialog {
    Q_OBJECT
public:
    enum Action { Cancel, ConnectOnly, SaveOnly, SaveAndConnect };

    SessionDialog(const SessionConfig &cfg, const QStringList &folders, const QVector<SerialPortEntry> &ports,
                  bool editingSaved, QWidget *parent = nullptr);
    SessionConfig config() const;
    Action action() const { return m_action; }

private:
    void load(const SessionConfig &cfg);
    void finish(Action a);
    bool validate();

    SessionConfig m_base;
    Action m_action = Cancel;

    QComboBox *m_type;
    QLineEdit *m_name;
    QComboBox *m_folder;
    QStackedWidget *m_stack;

    // SSH
    QLineEdit *m_sshHost;
    QSpinBox *m_sshPort;
    QLineEdit *m_sshUser;
    QLineEdit *m_sshKey;
    QCheckBox *m_sshAgent;
    QCheckBox *m_sshLegacy;
    QLabel *m_sshLegacyWarn;
    QComboBox *m_sshHostKey;
    QSpinBox *m_sshKeepAlive;
    ForwardRulesEditor *m_sshForwards;
    // Telnet
    QLineEdit *m_telHost;
    QSpinBox *m_telPort;
    QCheckBox *m_telRaw;
    // Serial
    QComboBox *m_serPort;
    QComboBox *m_serBaud;
    QComboBox *m_serData;
    QComboBox *m_serParity;
    QComboBox *m_serStop;
    QComboBox *m_serFlow;
    QSpinBox *m_serLineDelay;
    QSpinBox *m_serCharDelay;
    QCheckBox *m_serReconnect;
    // Local
    QComboBox *m_locShell;
    QLineEdit *m_locArgs;
    QLineEdit *m_locCwd;
    // Terminal
    QComboBox *m_enter;
    QCheckBox *m_localEcho;
    QCheckBox *m_history;
    QCheckBox *m_log;
    QCheckBox *m_logTimes;
    QSpinBox *m_scrollback;
    QCheckBox *m_forgetPassword = nullptr;
};

} // namespace termx
