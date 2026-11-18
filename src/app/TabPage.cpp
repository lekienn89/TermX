// SPDX-License-Identifier: GPL-3.0-or-later
#include "TabPage.h"
#include "TerminalView.h"

#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>

namespace termx {

TabPage::TabPage(TerminalView *first, QWidget *parent) : QWidget(parent)
{
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);
    m_layout->addWidget(first);
    adopt(first);
}

void TabPage::adopt(TerminalView *view)
{
    m_active = view;
    connect(view, &TerminalView::activated, this, [this](TerminalView *v) {
        if (m_active != v) {
            m_active = v;
            Q_EMIT activeViewChanged(v);
        }
    });
}

TerminalView *TabPage::activeView() const
{
    if (m_active)
        return m_active;
    const auto all = views();
    return all.isEmpty() ? nullptr : all.first();
}

QList<TerminalView *> TabPage::views() const
{
    return findChildren<TerminalView *>();
}

void TabPage::split(TerminalView *newView, Qt::Orientation orientation)
{
    TerminalView *cur = activeView();
    if (!cur) {
        m_layout->addWidget(newView);
        adopt(newView);
        return;
    }
    auto *parentSplitter = qobject_cast<QSplitter *>(cur->parentWidget());
    if (parentSplitter && parentSplitter->orientation() == orientation) {
        parentSplitter->insertWidget(parentSplitter->indexOf(cur) + 1, newView);
        QList<int> sizes;
        for (int i = 0; i < parentSplitter->count(); ++i)
            sizes << 1000;
        parentSplitter->setSizes(sizes);
    } else {
        auto *sp = new QSplitter(orientation);
        sp->setChildrenCollapsible(false);
        sp->setHandleWidth(3);
        if (parentSplitter) {
            const int idx = parentSplitter->indexOf(cur);
            const QList<int> sizes = parentSplitter->sizes();
            parentSplitter->insertWidget(idx, sp);
            sp->addWidget(cur);
            parentSplitter->setSizes(sizes);
        } else {
            m_layout->replaceWidget(cur, sp);
            sp->addWidget(cur);
        }
        sp->addWidget(newView);
        sp->setSizes({1000, 1000});
    }
    adopt(newView);
    newView->focusTerminal();
    Q_EMIT activeViewChanged(newView);
}

void TabPage::closeView(TerminalView *view)
{
    if (!view)
        return;
    auto *sp = qobject_cast<QSplitter *>(view->parentWidget());
    view->hide();
    view->setParent(nullptr);
    view->deleteLater();
    if (sp && sp->count() == 1) {
        // Collapse a splitter that now holds a single child.
        QWidget *only = sp->widget(0);
        auto *outer = qobject_cast<QSplitter *>(sp->parentWidget());
        if (outer) {
            const int idx = outer->indexOf(sp);
            const QList<int> sizes = outer->sizes();
            outer->insertWidget(idx, only);
            outer->setSizes(sizes);
        } else {
            m_layout->replaceWidget(sp, only);
        }
        sp->deleteLater();
    }
    const auto remaining = views();
    if (remaining.isEmpty()) {
        Q_EMIT becameEmpty();
        return;
    }
    m_active = remaining.first();
    QTimer::singleShot(0, m_active, [v = m_active] {
        if (v)
            v->focusTerminal();
    });
    Q_EMIT activeViewChanged(m_active);
}

} // namespace termx
