// SPDX-License-Identifier: GPL-3.0-or-later
#include "Panels.h"

#include "Theme.h"
#include "serial/DeviceProfiles.h"
#include "session/SessionStore.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace termx {

namespace {
enum Roles { RoleId = Qt::UserRole + 1, RoleFolder, RoleKind, RolePort, RoleEntryId, RoleText };
enum Kind { KindFolder = 1, KindSession = 2 };

QString relativeTime(qint64 secs)
{
    const qint64 d = QDateTime::currentSecsSinceEpoch() - secs;
    if (d < 60) return QObject::tr("just now");
    if (d < 3600) return QObject::tr("%1 min ago").arg(d / 60);
    if (d < 86400) return QObject::tr("%1 h ago").arg(d / 3600);
    if (d < 86400 * 30) return QObject::tr("%1 d ago").arg(d / 86400);
    return QDateTime::fromSecsSinceEpoch(secs).toString(QStringLiteral("yyyy-MM-dd"));
}
} // namespace

// ================================================================ sessions

SessionTreePanel::SessionTreePanel(SessionStore *store, QWidget *parent) : QWidget(parent), m_store(store)
{
    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);
    auto *top = new QHBoxLayout;
    top->setContentsMargins(6, 6, 6, 6);
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Filter sessions"));
    m_filter->setClearButtonEnabled(true);
    auto *add = new QToolButton(this);
    add->setText(QStringLiteral("+"));
    add->setToolTip(tr("New session"));
    top->addWidget(m_filter, 1);
    top->addWidget(add);
    l->addLayout(top);

    m_tree = new QTreeWidget(this);
    m_tree->setHeaderHidden(true);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setExpandsOnDoubleClick(true);
    l->addWidget(m_tree, 1);

    connect(add, &QToolButton::clicked, this, [this] {
        QTreeWidgetItem *it = m_tree->currentItem();
        Q_EMIT newRequested(it ? it->data(0, RoleFolder).toString() : QString());
    });
    connect(m_filter, &QLineEdit::textChanged, this, &SessionTreePanel::rebuild);
    connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *it) {
        if (it->data(0, RoleKind).toInt() == KindSession)
            Q_EMIT openRequested(it->data(0, RoleId).toString());
    });
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &SessionTreePanel::showMenu);
    connect(m_store, &SessionStore::changed, this, &SessionTreePanel::rebuild);
    rebuild();
}

QTreeWidgetItem *SessionTreePanel::folderItem(const QString &path)
{
    if (path.isEmpty())
        return nullptr;
    if (auto *it = m_folderItems.value(path))
        return it;
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    QTreeWidgetItem *parent = slash > 0 ? folderItem(path.left(slash)) : nullptr;
    auto *it = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
    it->setText(0, slash > 0 ? path.mid(slash + 1) : path);
    it->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
    it->setData(0, RoleKind, KindFolder);
    it->setData(0, RoleFolder, path);
    it->setExpanded(true);
    m_folderItems.insert(path, it);
    return it;
}

void SessionTreePanel::rebuild()
{
    m_tree->clear();
    m_folderItems.clear();
    const QString f = m_filter->text().trimmed();
    if (f.isEmpty())
        for (const QString &folder : m_store->folders())
            folderItem(folder);
    for (const SessionConfig &c : m_store->sessions()) {
        if (!f.isEmpty() && !c.displayName().contains(f, Qt::CaseInsensitive) && !c.target().contains(f, Qt::CaseInsensitive))
            continue;
        QTreeWidgetItem *parent = folderItem(c.folder);
        auto *it = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
        it->setText(0, c.displayName());
        it->setToolTip(0, QStringLiteral("%1 · %2").arg(sessionTypeName(c.type).toUpper(), c.target()));
        it->setIcon(0, style()->standardIcon(c.type == SessionType::Serial ? QStyle::SP_DriveHDIcon : QStyle::SP_ComputerIcon));
        it->setData(0, RoleKind, KindSession);
        it->setData(0, RoleId, c.id);
        it->setData(0, RoleFolder, c.folder);
    }
    m_tree->sortItems(0, Qt::AscendingOrder);
}

void SessionTreePanel::showMenu(const QPoint &pos)
{
    QTreeWidgetItem *it = m_tree->itemAt(pos);
    QMenu menu(this);
    const QString folder = it ? it->data(0, RoleFolder).toString() : QString();
    if (it && it->data(0, RoleKind).toInt() == KindSession) {
        const QString id = it->data(0, RoleId).toString();
        menu.addAction(tr("Connect"), this, [this, id] { Q_EMIT openRequested(id); });
        menu.addAction(tr("Edit..."), this, [this, id] { Q_EMIT editRequested(id); });
        menu.addAction(tr("Duplicate"), this, [this, id] {
            if (const SessionConfig *c = m_store->find(id)) {
                SessionConfig copy = *c;
                copy.id.clear();
                copy.name = c->displayName() + tr(" (copy)");
                m_store->upsert(copy);
            }
        });
        menu.addSeparator();
        menu.addAction(tr("Delete"), this, [this, id] {
            const SessionConfig *c = m_store->find(id);
            if (c && QMessageBox::question(this, tr("Delete session"), tr("Delete \"%1\"?").arg(c->displayName()))
                         == QMessageBox::Yes)
                m_store->remove(id);
        });
        menu.addSeparator();
    }
    menu.addAction(tr("New Session..."), this, [this, folder] { Q_EMIT newRequested(folder); });
    menu.addAction(tr("New Folder..."), this, [this, folder] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("New folder"), tr("Folder name"), QLineEdit::Normal, QString(), &ok).trimmed();
        if (ok && !name.isEmpty())
            m_store->addFolder(folder.isEmpty() ? name : folder + QLatin1Char('/') + name);
    });
    if (it && it->data(0, RoleKind).toInt() == KindFolder) {
        menu.addAction(tr("Rename Folder..."), this, [this, folder] {
            bool ok = false;
            const QString leaf = folder.section(QLatin1Char('/'), -1);
            const QString name = QInputDialog::getText(this, tr("Rename folder"), tr("Folder name"), QLineEdit::Normal, leaf, &ok).trimmed();
            if (ok && !name.isEmpty() && name != leaf) {
                const int slash = folder.lastIndexOf(QLatin1Char('/'));
                m_store->renameFolder(folder, slash > 0 ? folder.left(slash + 1) + name : name);
            }
        });
        menu.addAction(tr("Delete Folder"), this, [this, folder] {
            if (QMessageBox::question(this, tr("Delete folder"),
                                      tr("Delete folder \"%1\"? Its sessions move to the parent folder.").arg(folder))
                == QMessageBox::Yes)
                m_store->removeFolder(folder);
        });
    }
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

// ================================================================ serial

SerialPanel::SerialPanel(SerialPortWatcher *watcher, DeviceProfiles *profiles, QWidget *parent)
    : QWidget(parent), m_watcher(watcher), m_profiles(profiles)
{
    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);
    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({tr("Port"), tr("Device"), tr("Settings")});
    m_tree->setRootIsDecorated(false);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_empty = new QLabel(tr("No serial ports.\nPlug in a USB console cable;\nit will show up here automatically."), this);
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    m_empty->setEnabled(false);
    l->addWidget(m_tree, 1);
    l->addWidget(m_empty, 1);

    connect(m_watcher, &SerialPortWatcher::portsChanged, this, &SerialPanel::rebuild);
    connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *it) { Q_EMIT openRequested(entryFor(it)); });
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &SerialPanel::showMenu);
    rebuild();
}

SerialPortEntry SerialPanel::entryFor(QTreeWidgetItem *item) const
{
    const QString name = item ? item->data(0, RolePort).toString() : QString();
    for (const auto &p : m_watcher->ports())
        if (p.portName == name)
            return p;
    return SerialPortEntry();
}

void SerialPanel::setPortsInUse(const QStringList &ports)
{
    if (ports == m_inUse)
        return;
    m_inUse = ports;
    rebuild();
}

void SerialPanel::rebuild()
{
    m_tree->clear();
    const auto ports = m_watcher->ports();
    for (const SerialPortEntry &p : ports) {
        auto *it = new QTreeWidgetItem(m_tree);
        const bool busy = m_inUse.contains(p.portName, Qt::CaseInsensitive);
        it->setText(0, busy ? p.portName + tr(" (open)") : p.portName);
        QString dev = p.description;
        if (!p.chipName().isEmpty() && !dev.contains(p.chipName(), Qt::CaseInsensitive))
            dev += QStringLiteral(" [%1]").arg(p.chipName());
        it->setText(1, dev);
        SessionConfig tmp;
        tmp.serial = m_profiles->optionsFor(p.deviceKey(), p.portName);
        it->setText(2, tmp.serialSummary());
        it->setData(0, RolePort, p.portName);
        QString tip = QStringLiteral("%1\n%2").arg(p.label(), p.manufacturer);
        if (p.hasIds)
            tip += QStringLiteral("\nVID:PID %1:%2").arg(p.vendorId, 4, 16, QLatin1Char('0')).arg(p.productId, 4, 16, QLatin1Char('0'));
        if (!p.serialNumber.isEmpty())
            tip += QStringLiteral("\nSerial number %1").arg(p.serialNumber);
        if (p.vendorId == 0x067B)
            tip += tr("\nNote: PL2303 clones often hang with the latest Prolific driver.");
        it->setToolTip(0, tip);
        it->setToolTip(1, tip);
        if (busy) {
            QFont f = it->font(0);
            f.setBold(true);
            it->setFont(0, f);
        }
    }
    m_tree->setVisible(!ports.isEmpty());
    m_empty->setVisible(ports.isEmpty());
}

void SerialPanel::showMenu(const QPoint &pos)
{
    QTreeWidgetItem *it = m_tree->itemAt(pos);
    QMenu menu(this);
    if (it) {
        const SerialPortEntry e = entryFor(it);
        menu.addAction(tr("Open"), this, [this, e] { Q_EMIT openRequested(e); });
        menu.addAction(tr("Open with Settings..."), this, [this, e] { Q_EMIT openWithSettingsRequested(e); });
        QAction *autoOpen = menu.addAction(tr("Open Automatically When Plugged In"));
        autoOpen->setCheckable(true);
        autoOpen->setChecked(m_profiles->autoOpen(e.deviceKey()));
        connect(autoOpen, &QAction::toggled, this, [this, e](bool on) { m_profiles->setAutoOpen(e.deviceKey(), on); });
        menu.addAction(tr("Copy Device ID"), this, [e] { QApplication::clipboard()->setText(e.deviceKey()); });
        menu.addSeparator();
    }
    menu.addAction(tr("Rescan Ports"), m_watcher, &SerialPortWatcher::requestRescan);
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

// ================================================================ history

HistoryPanel::HistoryPanel(HistoryStore *store, QWidget *parent) : QWidget(parent), m_store(store)
{
    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);
    auto *top = new QHBoxLayout;
    top->setContentsMargins(6, 6, 6, 6);
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search commands"));
    m_search->setClearButtonEnabled(true);
    m_scope = new QComboBox(this);
    m_scope->addItem(tr("All hosts"));
    m_scope->addItem(tr("Current host"));
    top->addWidget(m_search, 1);
    top->addWidget(m_scope);
    l->addLayout(top);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(4);
    m_tree->setHeaderLabels({tr("Command"), tr("Host"), tr("Last used"), tr("Uses")});
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setMinimumSectionSize(40);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int c = 1; c < 4; ++c)
        m_tree->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    m_tree->setTextElideMode(Qt::ElideMiddle);
    m_tree->installEventFilter(this);
    m_search->installEventFilter(this);
    l->addWidget(m_tree, 1);

    auto *hint = new QLabel(tr("Double-click or Enter: insert · Ctrl+Enter: run"), this);
    hint->setContentsMargins(6, 3, 6, 3);
    hint->setEnabled(false);
    l->addWidget(hint);

    m_refresh.setSingleShot(true);
    m_refresh.setInterval(150);
    connect(&m_refresh, &QTimer::timeout, this, &HistoryPanel::refresh);
    connect(m_search, &QLineEdit::textChanged, &m_refresh, qOverload<>(&QTimer::start));
    connect(m_scope, qOverload<int>(&QComboBox::currentIndexChanged), this, &HistoryPanel::refresh);
    connect(m_store, &HistoryStore::changed, &m_refresh, qOverload<>(&QTimer::start));
    connect(m_tree, &QTreeWidget::itemActivated, this, [this] { emitInsert(false); });
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &HistoryPanel::showMenu);
    refresh();
}

void HistoryPanel::setCurrentHost(const QString &host)
{
    if (host == m_host)
        return;
    m_host = host;
    if (m_scope->currentIndex() == 1)
        refresh();
}

void HistoryPanel::focusSearch()
{
    m_search->setFocus();
    m_search->selectAll();
}

void HistoryPanel::refresh()
{
    if (!m_store)
        return;
    const QString host = m_scope->currentIndex() == 1 ? m_host : QString();
    const auto rows = m_store->search(m_search->text(), host, 1000);
    m_tree->setUpdatesEnabled(false);
    m_tree->clear();
    for (const HistoryEntry &e : rows) {
        auto *it = new QTreeWidgetItem(m_tree);
        it->setText(0, (e.pinned ? QStringLiteral("★ ") : QString()) + e.text);
        it->setToolTip(0, e.text);
        it->setText(1, e.host);
        it->setText(2, relativeTime(e.lastUsed));
        it->setToolTip(2, QDateTime::fromSecsSinceEpoch(e.lastUsed).toString(Qt::TextDate));
        it->setText(3, QString::number(e.useCount));
        it->setData(0, RoleEntryId, e.id);
        it->setData(0, RoleText, e.text);
        it->setData(0, RoleKind, e.pinned);
    }
    m_tree->setUpdatesEnabled(true);
}

void HistoryPanel::emitInsert(bool execute)
{
    QTreeWidgetItem *it = m_tree->currentItem();
    if (it)
        Q_EMIT insertRequested(it->data(0, RoleText).toString(), execute);
}

bool HistoryPanel::eventFilter(QObject *obj, QEvent *e)
{
    if (e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
            if (obj == m_search && !m_tree->currentItem() && m_tree->topLevelItemCount() > 0)
                m_tree->setCurrentItem(m_tree->topLevelItem(0));
            emitInsert(ke->modifiers() & Qt::ControlModifier);
            return true;
        }
        if (obj == m_search && (ke->key() == Qt::Key_Down || ke->key() == Qt::Key_Up)) {
            m_tree->setFocus();
            if (!m_tree->currentItem() && m_tree->topLevelItemCount() > 0)
                m_tree->setCurrentItem(m_tree->topLevelItem(0));
            return true;
        }
        if (obj == m_tree && ke->key() == Qt::Key_Delete) {
            if (QTreeWidgetItem *it = m_tree->currentItem())
                m_store->remove(it->data(0, RoleEntryId).toLongLong());
            return true;
        }
    }
    return QWidget::eventFilter(obj, e);
}

void HistoryPanel::showMenu(const QPoint &pos)
{
    QTreeWidgetItem *it = m_tree->itemAt(pos);
    QMenu menu(this);
    if (it) {
        const qint64 id = it->data(0, RoleEntryId).toLongLong();
        const QString text = it->data(0, RoleText).toString();
        const bool pinned = it->data(0, RoleKind).toBool();
        const QString host = it->text(1);
        menu.addAction(tr("Insert"), this, [this] { emitInsert(false); });
        menu.addAction(tr("Run"), this, [this] { emitInsert(true); });
        menu.addAction(tr("Copy"), this, [text] { QApplication::clipboard()->setText(text); });
        menu.addAction(pinned ? tr("Unpin") : tr("Pin"), this, [this, id, pinned] { m_store->setPinned(id, !pinned); });
        menu.addAction(tr("Delete"), this, [this, id] { m_store->remove(id); });
        menu.addSeparator();
        if (!host.isEmpty())
            menu.addAction(tr("Clear History for %1...").arg(host), this, [this, host] {
                if (QMessageBox::question(this, tr("Clear history"), tr("Delete all commands recorded for %1?").arg(host))
                    == QMessageBox::Yes)
                    m_store->clearHost(host);
            });
    }
    menu.addAction(tr("Export to CSV..."), this, [this] {
        const QString f = QFileDialog::getSaveFileName(this, tr("Export history"), QStringLiteral("termx-history.csv"),
                                                       tr("CSV files (*.csv)"));
        QString err;
        if (!f.isEmpty() && !m_store->exportCsv(f, &err))
            QMessageBox::warning(this, tr("Export history"), err);
    });
    menu.addAction(tr("Import from CSV..."), this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Import history"), QString(), tr("CSV files (*.csv)"));
        QString err;
        if (!f.isEmpty() && m_store->importCsv(f, &err) < 0)
            QMessageBox::warning(this, tr("Import history"), err);
        refresh();
    });
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

// ================================================================ toast

Toast::Toast(QWidget *parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("termxToast"));
    setFrameShape(QFrame::StyledPanel);
    setAutoFillBackground(true);
    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(12, 10, 12, 10);
    auto *head = new QHBoxLayout;
    m_title = new QLabel(this);
    QFont f = m_title->font();
    f.setBold(true);
    m_title->setFont(f);
    auto *close = new QToolButton(this);
    close->setText(QStringLiteral("✕"));
    close->setAutoRaise(true);
    connect(close, &QToolButton::clicked, this, &QWidget::hide);
    head->addWidget(m_title, 1);
    head->addWidget(close);
    m_text = new QLabel(this);
    m_text->setWordWrap(true);
    m_buttons = new QHBoxLayout;
    m_buttons->addStretch(1);
    l->addLayout(head);
    l->addWidget(m_text);
    l->addLayout(m_buttons);
    m_hide.setSingleShot(true);
    connect(&m_hide, &QTimer::timeout, this, &QWidget::hide);
    hide();
}

void Toast::showMessage(const QString &title, const QString &text, const QList<Action> &actions, int timeoutMs)
{
    m_title->setText(title);
    m_text->setText(text);
    m_text->setVisible(!text.isEmpty());
    while (m_buttons->count() > 1) {
        QLayoutItem *item = m_buttons->takeAt(1);
        delete item->widget();
        delete item;
    }
    bool first = true;
    for (const Action &a : actions) {
        auto *b = new QPushButton(a.first, this);
        if (first)
            b->setDefault(true);
        first = false;
        auto fn = a.second;
        connect(b, &QPushButton::clicked, this, [this, fn] {
            hide();
            if (fn)
                fn();
        });
        m_buttons->addWidget(b);
    }
    setStyleSheet(QStringLiteral("#termxToast { background: %1; border: 1px solid %2; border-radius: 6px; }")
                      .arg((Theme::isDark() ? QColor(0x262A31) : QColor(0xFFFFFF)).name(), Theme::accent().name()));
    adjustSize();
    setFixedWidth(std::max(340, sizeHint().width()));
    adjustSize();
    reposition();
    show();
    raise();
    if (timeoutMs > 0)
        m_hide.start(timeoutMs);
}

void Toast::reposition()
{
    if (!parentWidget())
        return;
    const QRect r = parentWidget()->rect();
    move(r.right() - width() - 16, r.bottom() - height() - 16);
}

} // namespace termx
