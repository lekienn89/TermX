// SPDX-License-Identifier: GPL-3.0-or-later
#include "SnippetPanel.h"

#include "Theme.h"
#include "session/SnippetStore.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace termx {

SnippetDialog::SnippetDialog(const Snippet &s, QWidget *parent) : QDialog(parent), m_id(s.id)
{
    setWindowTitle(s.id.isEmpty() ? tr("New Snippet") : tr("Edit Snippet"));
    auto *form = new QFormLayout;
    m_name = new QLineEdit(s.name, this);
    m_name->setPlaceholderText(tr("e.g. Interfaces brief"));
    m_group = new QLineEdit(s.group, this);
    m_group->setPlaceholderText(tr("optional, e.g. Cisco"));
    m_text = new QPlainTextEdit(this);
    m_text->setPlainText(s.text);
    m_text->setFont(Theme::terminalFont());
    m_text->setPlaceholderText(tr("show ip interface brief\n\nEach line is sent followed by Enter."));
    m_text->setMinimumSize(460, 180);
    form->addRow(tr("Name"), m_name);
    form->addRow(tr("Group"), m_group);
    form->addRow(tr("Commands"), m_text);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (m_name->text().trimmed().isEmpty()) {
            m_name->setFocus();
            return;
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto *l = new QVBoxLayout(this);
    l->addLayout(form);
    l->addWidget(buttons);
}

Snippet SnippetDialog::snippet() const
{
    Snippet s;
    s.id = m_id;
    s.name = m_name->text().trimmed();
    s.group = m_group->text().trimmed();
    s.text = m_text->toPlainText();
    return s;
}

SnippetPanel::SnippetPanel(SnippetStore *store, QWidget *parent) : QWidget(parent), m_store(store)
{
    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);
    auto *top = new QHBoxLayout;
    top->setContentsMargins(6, 6, 6, 6);
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Filter snippets"));
    m_filter->setClearButtonEnabled(true);
    auto *add = new QToolButton(this);
    add->setText(QStringLiteral("+"));
    add->setToolTip(tr("New snippet"));
    top->addWidget(m_filter, 1);
    top->addWidget(add);
    l->addLayout(top);

    m_tree = new QTreeWidget(this);
    m_tree->setHeaderHidden(true);
    m_tree->setColumnCount(1);
    m_tree->setUniformRowHeights(true);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->installEventFilter(this);
    m_filter->installEventFilter(this);
    l->addWidget(m_tree, 1);
    auto *hint = new QLabel(tr("Double-click or Enter: run · Shift+Enter: insert"), this);
    hint->setContentsMargins(6, 3, 6, 3);
    hint->setEnabled(false);
    l->addWidget(hint);

    connect(add, &QToolButton::clicked, this, [this] { addSnippet(); });
    connect(m_filter, &QLineEdit::textChanged, this, &SnippetPanel::rebuild);
    connect(m_store, &SnippetStore::changed, this, &SnippetPanel::rebuild);
    connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *it) {
        if (const Snippet *s = m_store->find(it->data(0, Qt::UserRole).toString()))
            emit runRequested(s->text);
    });
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QMenu menu(this);
        const QString id = currentId();
        if (const Snippet *s = id.isEmpty() ? nullptr : m_store->find(id)) {
            const QString text = s->text;
            menu.addAction(tr("Run"), this, [this, text] { emit runRequested(text); });
            menu.addAction(tr("Insert Without Enter"), this, [this, text] { emit pasteRequested(text); });
            menu.addSeparator();
            menu.addAction(tr("Edit..."), this, &SnippetPanel::editCurrent);
            menu.addAction(tr("Delete"), this, &SnippetPanel::removeCurrent);
            menu.addSeparator();
        }
        menu.addAction(tr("New Snippet..."), this, [this] { addSnippet(); });
        menu.exec(m_tree->viewport()->mapToGlobal(pos));
    });
    rebuild();
}

void SnippetPanel::focusFilter()
{
    m_filter->setFocus();
    m_filter->selectAll();
}

void SnippetPanel::rebuild()
{
    const QString f = m_filter->text().trimmed();
    m_tree->clear();
    QMap<QString, QTreeWidgetItem *> groups;
    for (const Snippet &s : m_store->snippets()) {
        if (!f.isEmpty() && !s.name.contains(f, Qt::CaseInsensitive) && !s.text.contains(f, Qt::CaseInsensitive)
            && !s.group.contains(f, Qt::CaseInsensitive))
            continue;
        QTreeWidgetItem *parent = nullptr;
        if (!s.group.isEmpty()) {
            parent = groups.value(s.group);
            if (!parent) {
                parent = new QTreeWidgetItem(m_tree, {s.group});
                parent->setFlags(Qt::ItemIsEnabled);
                QFont b = parent->font(0);
                b.setBold(true);
                parent->setFont(0, b);
                parent->setExpanded(true);
                groups.insert(s.group, parent);
            }
        }
        auto *it = parent ? new QTreeWidgetItem(parent, {s.name}) : new QTreeWidgetItem(m_tree, {s.name});
        it->setData(0, Qt::UserRole, s.id);
        it->setToolTip(0, s.text.left(2000));
    }
    m_tree->expandAll();
}

QString SnippetPanel::currentId() const
{
    QTreeWidgetItem *it = m_tree->currentItem();
    return it ? it->data(0, Qt::UserRole).toString() : QString();
}

void SnippetPanel::addSnippet(const QString &initialText)
{
    Snippet s;
    s.text = initialText;
    SnippetDialog dlg(s, this);
    if (dlg.exec() == QDialog::Accepted)
        m_store->upsert(dlg.snippet());
}

void SnippetPanel::editCurrent()
{
    const Snippet *s = m_store->find(currentId());
    if (!s)
        return;
    SnippetDialog dlg(*s, this);
    if (dlg.exec() == QDialog::Accepted)
        m_store->upsert(dlg.snippet());
}

void SnippetPanel::removeCurrent()
{
    const Snippet *s = m_store->find(currentId());
    if (!s)
        return;
    if (QMessageBox::question(this, tr("Delete snippet"), tr("Delete \"%1\"?").arg(s->name)) == QMessageBox::Yes)
        m_store->remove(s->id);
}

bool SnippetPanel::eventFilter(QObject *obj, QEvent *e)
{
    if (e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (obj == m_filter && ke->key() == Qt::Key_Down) {
            m_tree->setFocus();
            if (!m_tree->currentItem() && m_tree->topLevelItemCount())
                m_tree->setCurrentItem(m_tree->topLevelItem(0));
            return true;
        }
        if (obj == m_tree && (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter)) {
            if (const Snippet *s = m_store->find(currentId())) {
                if (ke->modifiers() & Qt::ShiftModifier)
                    emit pasteRequested(s->text);
                else
                    emit runRequested(s->text);
            }
            return true;
        }
        if (obj == m_tree && ke->key() == Qt::Key_Delete) {
            removeCurrent();
            return true;
        }
    }
    return QWidget::eventFilter(obj, e);
}

} // namespace termx
