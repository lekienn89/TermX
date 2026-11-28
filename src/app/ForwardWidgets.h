// SPDX-License-Identifier: GPL-3.0-or-later
// Port forwarding UI: rule editor dialog, the rule list in the session
// dialog, and the dock panel that shows (and changes) the live tunnels.
#pragma once

#include "session/PortForward.h"

#include <QDialog>
#include <QPointer>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTreeWidget;

namespace termx {

class SshTransport;
class TerminalSession;

class ForwardDialog : public QDialog {
    Q_OBJECT
public:
    explicit ForwardDialog(const PortForward &rule, bool showEnabled, QWidget *parent = nullptr);
    PortForward rule() const;

private:
    void refresh();

    QComboBox *m_type;
    QLineEdit *m_bind;
    QSpinBox *m_bindPort;
    QLineEdit *m_destHost;
    QSpinBox *m_destPort;
    QLineEdit *m_description;
    QCheckBox *m_enabled;
    QLabel *m_explain;
    QLabel *m_destHostLabel;
    QLabel *m_destPortLabel;
    QPushButton *m_ok;
};

// Rules saved with a session (Session dialog, SSH page).
class ForwardRulesEditor : public QWidget {
    Q_OBJECT
public:
    explicit ForwardRulesEditor(QWidget *parent = nullptr);
    void setRules(const QVector<PortForward> &rules);
    QVector<PortForward> rules() const;

private:
    void addRule(const PortForward &f);
    void editCurrent();
    void updateButtons();

    QTreeWidget *m_list;
    QPushButton *m_edit;
    QPushButton *m_remove;
};

// Live tunnels of the SSH session in the current tab.
class ForwardsPanel : public QWidget {
    Q_OBJECT
public:
    explicit ForwardsPanel(QWidget *parent = nullptr);
    void setSession(TerminalSession *session);

Q_SIGNALS:
    // "Keep" a running rule in the saved session.
    void saveRuleRequested(termx::TerminalSession *session, const termx::PortForward &rule);

private:
    void bindTransport();
    void rebuild();
    void addFromText();
    void addWithDialog();
    void stopSelected();
    void showMenu(const QPoint &pos);
    void start(const PortForward &f);

    QPointer<TerminalSession> m_session;
    QPointer<SshTransport> m_transport;
    QLineEdit *m_quick;
    QPushButton *m_add;
    QTreeWidget *m_list;
    QLabel *m_empty;
    QVector<ForwardStatus> m_status;
};

QString formatBytes(quint64 bytes);

} // namespace termx
