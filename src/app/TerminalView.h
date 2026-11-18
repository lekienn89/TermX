// SPDX-License-Identifier: GPL-3.0-or-later
// A terminal pane: TerminalWidget + scroll bar + find bar.
#pragma once

#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QScrollBar;
class QToolButton;

namespace termx {

class TerminalSession;
class TerminalWidget;
class HistoryStore;

class TerminalView : public QWidget {
    Q_OBJECT
public:
    TerminalView(TerminalSession *session, HistoryStore *history, QWidget *parent = nullptr);
    ~TerminalView() override;

    TerminalSession *session() const { return m_session; }
    TerminalWidget *terminal() const { return m_term; }
    void showFindBar();
    void hideFindBar();
    void focusTerminal();

Q_SIGNALS:
    void activated(termx::TerminalView *view);

private:
    void syncScrollBar();
    void doFind(bool backwards);

    TerminalSession *m_session;
    TerminalWidget *m_term;
    QScrollBar *m_bar;
    QWidget *m_findBar;
    QLineEdit *m_findEdit;
    QCheckBox *m_findCase;
    QLabel *m_findStatus;
    bool m_syncing = false;
};

} // namespace termx
