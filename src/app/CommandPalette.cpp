// SPDX-License-Identifier: GPL-3.0-or-later
#include "CommandPalette.h"

#include "util/FuzzyMatch.h"

#include <QApplication>
#include <QGraphicsDropShadowEffect>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace termx {

CommandPalette::CommandPalette(QWidget *parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("commandPalette"));
    setFrameShape(QFrame::StyledPanel);
    setAutoFillBackground(true);
    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(6, 6, 6, 6);
    l->setSpacing(4);
    m_edit = new QLineEdit(this);
    m_edit->setPlaceholderText(tr("Type a command, a saved session or a snippet"));
    m_list = new QListWidget(this);
    m_list->setUniformItemSizes(true);
    m_list->setFocusPolicy(Qt::NoFocus);
    l->addWidget(m_edit);
    l->addWidget(m_list, 1);
    m_edit->installEventFilter(this);
    connect(m_edit, &QLineEdit::textChanged, this, &CommandPalette::refilter);
    connect(m_list, &QListWidget::itemClicked, this, [this] { runCurrent(); });
    hide();
}

void CommandPalette::popup(const QList<Item> &items)
{
    m_items = items;
    QWidget *p = parentWidget();
    const int w = std::min(640, std::max(360, p->width() - 80));
    const int h = std::min(420, std::max(200, p->height() - 120));
    setGeometry((p->width() - w) / 2, 48, w, h);
    m_edit->clear();
    refilter();
    show();
    raise();
    m_edit->setFocus();
}

void CommandPalette::refilter()
{
    const QString q = m_edit->text();
    struct Hit { int score; int index; };
    std::vector<Hit> hits;
    for (int i = 0; i < m_items.size(); ++i) {
        const Item &it = m_items[i];
        int s = fuzzyScore(q, it.title);
        if (s < 0 && !it.detail.isEmpty()) {
            const int d = fuzzyScore(q, it.detail);
            s = d < 0 ? -1 : d / 2;
        }
        if (s >= 0)
            hits.push_back({s, i});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) { return a.score > b.score; });
    m_list->clear();
    for (const Hit &h : hits) {
        const Item &it = m_items[h.index];
        QString label = it.title;
        if (!it.category.isEmpty() && it.category != QLatin1String("Command"))
            label = it.category + QStringLiteral(": ") + label;
        if (!it.detail.isEmpty())
            label += QStringLiteral("    ") + it.detail;
        auto *li = new QListWidgetItem(label, m_list);
        li->setData(Qt::UserRole, h.index);
        if (m_list->count() >= 200)
            break;
    }
    if (m_list->count())
        m_list->setCurrentRow(0);
}

void CommandPalette::runCurrent()
{
    QListWidgetItem *li = m_list->currentItem();
    if (!li)
        return;
    const int idx = li->data(Qt::UserRole).toInt();
    std::function<void()> run = idx >= 0 && idx < m_items.size() ? m_items[idx].run : nullptr;
    hide();
    if (run)
        QTimer::singleShot(0, parentWidget(), run);   // after the palette has closed
}

void CommandPalette::hideEvent(QHideEvent *e)
{
    QFrame::hideEvent(e);
    emit closed();
}

bool CommandPalette::eventFilter(QObject *obj, QEvent *e)
{
    if (obj == m_edit && e->type() == QEvent::FocusOut) {
        QTimer::singleShot(0, this, [this] {
            if (!m_edit->hasFocus() && !m_list->underMouse())
                hide();
        });
    }
    if (obj == m_edit && e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        const int n = m_list->count();
        switch (ke->key()) {
        case Qt::Key_Escape:
            hide();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            runCurrent();
            return true;
        case Qt::Key_Down:
            if (n)
                m_list->setCurrentRow((m_list->currentRow() + 1) % n);
            return true;
        case Qt::Key_Up:
            if (n)
                m_list->setCurrentRow((m_list->currentRow() + n - 1) % n);
            return true;
        case Qt::Key_PageDown:
            if (n)
                m_list->setCurrentRow(std::min(n - 1, m_list->currentRow() + 10));
            return true;
        case Qt::Key_PageUp:
            if (n)
                m_list->setCurrentRow(std::max(0, m_list->currentRow() - 10));
            return true;
        default:
            break;
        }
    }
    return QFrame::eventFilter(obj, e);
}

} // namespace termx
