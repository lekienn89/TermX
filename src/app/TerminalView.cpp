// SPDX-License-Identifier: GPL-3.0-or-later
#include "TerminalView.h"

#include "TerminalWidget.h"
#include "terminal/TerminalSession.h"

#include <QApplication>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QScrollBar>
#include <QShortcut>
#include <QToolButton>
#include <QVBoxLayout>

namespace termx {

TerminalView::TerminalView(TerminalSession *session, HistoryStore *history, QWidget *parent)
    : QWidget(parent), m_session(session)
{
    session->setParent(this);   // the view owns its session
    m_term = new TerminalWidget(session, history, this);
    m_bar = new QScrollBar(Qt::Vertical, this);

    m_findBar = new QWidget(this);
    auto *fl = new QHBoxLayout(m_findBar);
    fl->setContentsMargins(6, 4, 6, 4);
    fl->setSpacing(4);
    m_findEdit = new QLineEdit(m_findBar);
    m_findEdit->setPlaceholderText(tr("Find in scrollback"));
    m_findEdit->setClearButtonEnabled(true);
    m_findCase = new QCheckBox(tr("Match case"), m_findBar);
    auto *prev = new QToolButton(m_findBar);
    prev->setText(tr("Previous"));
    prev->setArrowType(Qt::UpArrow);
    prev->setToolTip(tr("Previous match (Shift+Enter)"));
    auto *next = new QToolButton(m_findBar);
    next->setArrowType(Qt::DownArrow);
    next->setToolTip(tr("Next match (Enter)"));
    m_findStatus = new QLabel(m_findBar);
    auto *close = new QToolButton(m_findBar);
    close->setText(QStringLiteral("✕"));
    close->setToolTip(tr("Close (Esc)"));
    close->setAutoRaise(true);
    fl->addWidget(m_findEdit, 1);
    fl->addWidget(prev);
    fl->addWidget(next);
    fl->addWidget(m_findCase);
    fl->addWidget(m_findStatus);
    fl->addWidget(close);
    m_findBar->hide();

    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    row->addWidget(m_term, 1);
    row->addWidget(m_bar);
    auto *col = new QVBoxLayout(this);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);
    col->addWidget(m_findBar);
    col->addLayout(row, 1);

    connect(m_term, &TerminalWidget::scrollChanged, this, &TerminalView::syncScrollBar);
    connect(m_session, &TerminalSession::scrollbackChanged, this, &TerminalView::syncScrollBar);
    connect(m_bar, &QScrollBar::valueChanged, this, [this](int v) {
        if (!m_syncing)
            m_term->setScrollOffset(m_bar->maximum() - v);
    });
    connect(m_term, &TerminalWidget::searchRequested, this, &TerminalView::showFindBar);
    connect(m_term, &TerminalWidget::focused, this, [this] { Q_EMIT activated(this); });

    connect(m_findEdit, &QLineEdit::returnPressed, this, [this] {
        doFind(QApplication::keyboardModifiers() & Qt::ShiftModifier);
    });
    connect(m_findEdit, &QLineEdit::textChanged, this, [this] { doFind(false); });
    connect(prev, &QToolButton::clicked, this, [this] { doFind(true); });
    connect(next, &QToolButton::clicked, this, [this] { doFind(false); });
    connect(close, &QToolButton::clicked, this, &TerminalView::hideFindBar);
    auto *esc = new QShortcut(QKeySequence(Qt::Key_Escape), m_findEdit);
    esc->setContext(Qt::WidgetShortcut);
    connect(esc, &QShortcut::activated, this, &TerminalView::hideFindBar);

    syncScrollBar();
}

TerminalView::~TerminalView()
{
    // Destroy the widget before the session it renders.
    delete m_term;
    m_term = nullptr;
    if (m_session)
        m_session->stop();
}

void TerminalView::syncScrollBar()
{
    m_syncing = true;
    const int max = m_term->maxScrollOffset();
    m_bar->setRange(0, max);
    m_bar->setPageStep(m_session->screen().rows());
    m_bar->setValue(max - m_term->scrollOffset());
    m_bar->setVisible(max > 0);
    m_syncing = false;
}

void TerminalView::showFindBar()
{
    m_findBar->show();
    const QString sel = m_term->selectedText();
    if (!sel.isEmpty() && !sel.contains(QLatin1Char('\n')))
        m_findEdit->setText(sel);
    m_findEdit->setFocus();
    m_findEdit->selectAll();
}

void TerminalView::hideFindBar()
{
    m_findBar->hide();
    m_term->clearSearch();
    m_findStatus->clear();
    m_term->setFocus();
}

void TerminalView::focusTerminal()
{
    m_term->setFocus();
}

void TerminalView::doFind(bool backwards)
{
    const QString t = m_findEdit->text();
    if (t.isEmpty()) {
        m_term->clearSearch();
        m_findStatus->clear();
        return;
    }
    const bool found = m_term->find(t, backwards, m_findCase->isChecked());
    m_findStatus->setText(found ? QString() : tr("Not found"));
}

} // namespace termx
