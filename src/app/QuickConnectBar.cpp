// SPDX-License-Identifier: GPL-3.0-or-later
#include "QuickConnectBar.h"

#include "session/QuickConnect.h"

#include <QAbstractItemView>
#include <QCompleter>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QStringListModel>
#include <QStyle>
#include <QToolButton>
#include <QToolTip>

namespace termx {

QuickConnectBar::QuickConnectBar(QWidget *parent) : QWidget(parent)
{
    auto *l = new QHBoxLayout(this);
    l->setContentsMargins(6, 4, 6, 4);
    l->setSpacing(4);

    m_edit = new QLineEdit(this);
    m_edit->setObjectName(QStringLiteral("quickConnect"));
    m_edit->setPlaceholderText(
        tr("Quick connect:  user@host   ssh -p 2222 host   telnet host 23   COM3 115200   local   or a saved session"));
    m_edit->setClearButtonEnabled(true);
    m_edit->setToolTip(tr("Quick connect (Ctrl+Shift+K)\n\n"
                          "admin@10.0.0.1  ·  10.0.0.1:2222  ·  ssh -p 2222 -i key admin@host\n"
                          "telnet 10.0.0.1 2001  ·  raw 10.0.0.1 4001\n"
                          "COM7  ·  COM7 115200\n"
                          "local  ·  cmd  ·  powershell  ·  pwsh  ·  wsl\n"
                          "or the name of a saved session"));
    m_edit->installEventFilter(this);

    m_model = new QStringListModel(this);
    m_completer = new QCompleter(m_model, this);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setFilterMode(Qt::MatchContains);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    m_completer->setMaxVisibleItems(12);
    m_edit->setCompleter(m_completer);
    connect(m_completer, qOverload<const QString &>(&QCompleter::activated), this, [this] {
        QMetaObject::invokeMethod(this, &QuickConnectBar::submit, Qt::QueuedConnection);
    });

    m_go = new QToolButton(this);
    m_go->setText(tr("Connect"));
    m_go->setToolTip(tr("Connect (Enter)"));
    m_go->setAutoRaise(true);
    m_more = new QToolButton(this);
    m_more->setText(QStringLiteral("…"));
    m_more->setToolTip(tr("New session with all options (Ctrl+Shift+N)"));
    m_more->setAutoRaise(true);

    l->addWidget(m_edit, 1);
    l->addWidget(m_go);
    l->addWidget(m_more);

    connect(m_edit, &QLineEdit::returnPressed, this, &QuickConnectBar::submit);
    connect(m_go, &QToolButton::clicked, this, &QuickConnectBar::submit);
    connect(m_more, &QToolButton::clicked, this, &QuickConnectBar::moreRequested);
    connect(m_edit, &QLineEdit::textEdited, this, [this] {
        if (m_edit->property("error").toBool()) {
            m_edit->setProperty("error", false);
            m_edit->style()->unpolish(m_edit);
            m_edit->style()->polish(m_edit);
        }
    });
}

void QuickConnectBar::setSavedSessions(const QList<QPair<QString, QString>> &nameAndId)
{
    m_saved = nameAndId;
    rebuildModel();
}

void QuickConnectBar::setRecent(const QStringList &recent)
{
    m_recent = recent.mid(0, 20);
    rebuildModel();
}

void QuickConnectBar::rebuildModel()
{
    QStringList items = m_recent;
    for (const auto &s : m_saved)
        if (!items.contains(s.first, Qt::CaseInsensitive))
            items << s.first;
    m_model->setStringList(items);
}

void QuickConnectBar::focusInput()
{
    m_edit->setFocus(Qt::ShortcutFocusReason);
    m_edit->selectAll();
}

bool QuickConnectBar::eventFilter(QObject *obj, QEvent *e)
{
    if (obj == m_edit && e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->key() == Qt::Key_Escape && !m_completer->popup()->isVisible()) {
            m_edit->clear();
            emit done();
            return true;
        }
        if (ke->key() == Qt::Key_Down && !m_completer->popup()->isVisible()) {
            m_completer->setCompletionPrefix(m_edit->text());
            m_completer->complete();
            return true;
        }
    }
    return QWidget::eventFilter(obj, e);
}

void QuickConnectBar::submit()
{
    const QString text = m_edit->text().trimmed();
    if (text.isEmpty()) {
        emit moreRequested();
        return;
    }
    for (const auto &s : m_saved) {
        if (s.first.compare(text, Qt::CaseInsensitive) == 0) {
            m_edit->clear();
            emit savedSessionRequested(s.second);
            return;
        }
    }
    SessionConfig cfg;
    QString err;
    if (!QuickConnect::parse(text, cfg, &err)) {
        showError(err);
        return;
    }
    m_recent.removeAll(text);
    m_recent.prepend(text);
    while (m_recent.size() > 20)
        m_recent.removeLast();
    rebuildModel();
    m_edit->clear();
    emit connectRequested(cfg);
}

void QuickConnectBar::showError(const QString &msg)
{
    m_edit->setProperty("error", true);
    m_edit->style()->unpolish(m_edit);
    m_edit->style()->polish(m_edit);
    QToolTip::showText(m_edit->mapToGlobal(QPoint(0, m_edit->height())), msg, m_edit);
}

} // namespace termx
