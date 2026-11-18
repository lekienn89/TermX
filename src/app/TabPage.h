// SPDX-License-Identifier: GPL-3.0-or-later
// One tab: one or more TerminalViews arranged with nested splitters.
#pragma once

#include <QList>
#include <QPointer>
#include <QWidget>

class QVBoxLayout;

namespace termx {

class TerminalView;

class TabPage : public QWidget {
    Q_OBJECT
public:
    explicit TabPage(TerminalView *first, QWidget *parent = nullptr);

    TerminalView *activeView() const;
    QList<TerminalView *> views() const;
    void split(TerminalView *newView, Qt::Orientation orientation);
    void closeView(TerminalView *view);

Q_SIGNALS:
    void activeViewChanged(termx::TerminalView *view);
    void becameEmpty();

private:
    void adopt(TerminalView *view);

    QVBoxLayout *m_layout;
    QPointer<TerminalView> m_active;
};

} // namespace termx
