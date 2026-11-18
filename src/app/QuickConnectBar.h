// SPDX-License-Identifier: GPL-3.0-or-later
// One-line connect box: type "admin@10.0.0.1", "telnet host 2001", "COM7 115200",
// "local" or the name of a saved session and press Enter.
#pragma once

#include "session/SessionConfig.h"

#include <QWidget>

class QCompleter;
class QLineEdit;
class QStringListModel;
class QToolButton;

namespace termx {

class QuickConnectBar : public QWidget {
    Q_OBJECT
public:
    explicit QuickConnectBar(QWidget *parent = nullptr);

    // Saved sessions offered by the completer (display name -> id).
    void setSavedSessions(const QList<QPair<QString, QString>> &nameAndId);
    void setRecent(const QStringList &recent);
    QStringList recent() const { return m_recent; }
    void focusInput();

Q_SIGNALS:
    void connectRequested(const termx::SessionConfig &cfg);
    void savedSessionRequested(const QString &id);
    void moreRequested();    // open the full session dialog
    void done();             // Esc: give focus back to the terminal

protected:
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    void submit();
    void showError(const QString &msg);
    void rebuildModel();

    QLineEdit *m_edit;
    QToolButton *m_go;
    QToolButton *m_more;
    QCompleter *m_completer;
    QStringListModel *m_model;
    QList<QPair<QString, QString>> m_saved;
    QStringList m_recent;
};

} // namespace termx
