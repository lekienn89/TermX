// SPDX-License-Identifier: GPL-3.0-or-later
#include "SftpPanel.h"

#include "ForwardWidgets.h"   // formatBytes
#include "terminal/TerminalSession.h"
#include "transport/SftpEngine.h"
#include "transport/SshTransport.h"

#include <QApplication>
#include <algorithm>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QStyle>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace termx {

namespace {

enum { RoleName = Qt::UserRole + 1, RoleIsDir, RoleSort, RoleId };

QString permString(quint32 m)
{
    if (m == 0)
        return QString();
    QString s;
    const quint32 type = m & 0170000;
    s += type == 0040000 ? QLatin1Char('d') : type == 0120000 ? QLatin1Char('l') : QLatin1Char('-');
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i)
        s += (m & (0400 >> i)) ? QLatin1Char(rwx[i]) : QLatin1Char('-');
    return s;
}

QString shellQuote(const QString &s)
{
    QString q = s;
    q.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + q + QLatin1Char('\'');
}

QString parentOf(const QString &path)
{
    if (path.isEmpty() || path == QLatin1String("/"))
        return QStringLiteral("/");
    QString p = path;
    while (p.size() > 1 && p.endsWith(QLatin1Char('/')))
        p.chop(1);
    const int slash = p.lastIndexOf(QLatin1Char('/'));
    return slash <= 0 ? QStringLiteral("/") : p.left(slash);
}

// Folders first, then by the column's sort key.
class EntryItem : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool operator<(const QTreeWidgetItem &other) const override
    {
        const bool a = data(0, RoleIsDir).toBool();
        const bool b = other.data(0, RoleIsDir).toBool();
        const Qt::SortOrder order = treeWidget() ? treeWidget()->header()->sortIndicatorOrder() : Qt::AscendingOrder;
        if (a != b)
            return order == Qt::AscendingOrder ? a : b;
        const int col = treeWidget() ? treeWidget()->sortColumn() : 0;
        const QVariant x = data(col, RoleSort);
        const QVariant y = other.data(col, RoleSort);
        if (x.isValid() && y.isValid() && x.typeId() == QMetaType::LongLong)
            return x.toLongLong() < y.toLongLong();
        return QString::localeAwareCompare(text(col), other.text(col)) < 0;
    }
};

} // namespace

SftpPanel::SftpPanel(QSettings *settings, QWidget *parent) : QWidget(parent), m_settings(settings)
{
    setAcceptDrops(true);
    m_showHidden = m_settings && m_settings->value(QStringLiteral("sftp/showHidden"), false).toBool();

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(4);

    auto tool = [this](QStyle::StandardPixmap icon, const QString &tip) {
        auto *b = new QToolButton(this);
        b->setIcon(style()->standardIcon(icon));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        return b;
    };
    auto *bar = new QHBoxLayout;
    bar->setSpacing(2);
    m_up = tool(QStyle::SP_FileDialogToParent, tr("Parent folder (Backspace)"));
    m_homeBtn = tool(QStyle::SP_DirHomeIcon, tr("Home folder"));
    m_refresh = tool(QStyle::SP_BrowserReload, tr("Refresh (F5)"));
    m_mkdir = tool(QStyle::SP_FileDialogNewFolder, tr("New folder"));
    m_upload = tool(QStyle::SP_ArrowUp, tr("Upload files or a folder"));
    m_download = tool(QStyle::SP_ArrowDown, tr("Download selected items to the Downloads folder"));
    m_upload->setPopupMode(QToolButton::InstantPopup);
    auto *upMenu = new QMenu(m_upload);
    upMenu->addAction(tr("Upload Files..."), this, &SftpPanel::uploadFilesDialog);
    upMenu->addAction(tr("Upload Folder..."), this, &SftpPanel::uploadFolderDialog);
    m_upload->setMenu(upMenu);
    m_path = new QLineEdit(this);
    m_path->setPlaceholderText(tr("Remote path"));
    m_path->setClearButtonEnabled(false);
    for (QToolButton *b : {m_up, m_homeBtn, m_refresh})
        bar->addWidget(b);
    bar->addWidget(m_path, 1);
    for (QToolButton *b : {m_mkdir, m_upload, m_download})
        bar->addWidget(b);
    root->addLayout(bar);

    m_split = new QSplitter(Qt::Vertical, this);
    auto *browser = new QWidget(m_split);
    auto *bl = new QVBoxLayout(browser);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(2);
    m_list = new QTreeWidget(browser);
    m_list->setColumnCount(4);
    m_list->setHeaderLabels({tr("Name"), tr("Size"), tr("Modified"), tr("Permissions")});
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setSortingEnabled(true);
    m_list->sortByColumn(0, Qt::AscendingOrder);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int c = 1; c < 4; ++c)
        m_list->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    m_list->setAcceptDrops(true);
    m_list->viewport()->setAcceptDrops(true);
    m_list->installEventFilter(this);
    m_list->viewport()->installEventFilter(this);
    m_empty = new QLabel(browser);
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    m_empty->setEnabled(false);
    m_status = new QLabel(browser);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bl->addWidget(m_list, 1);
    bl->addWidget(m_empty, 1);
    bl->addWidget(m_status);

    m_transfers = new QTreeWidget(m_split);
    m_transfers->setColumnCount(3);
    m_transfers->setHeaderLabels({tr("Transfer"), tr("Progress"), tr("Status")});
    m_transfers->setRootIsDecorated(false);
    m_transfers->setContextMenuPolicy(Qt::CustomContextMenu);
    m_transfers->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_transfers->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_transfers->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_transfers->header()->resizeSection(1, 120);
    m_transfers->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_transfers->hide();
    m_split->addWidget(browser);
    m_split->addWidget(m_transfers);
    m_split->setStretchFactor(0, 3);
    m_split->setStretchFactor(1, 1);
    root->addWidget(m_split, 1);

    connect(m_up, &QToolButton::clicked, this, &SftpPanel::goUp);
    connect(m_homeBtn, &QToolButton::clicked, this, [this] { navigate(m_home.isEmpty() ? QStringLiteral(".") : m_home); });
    connect(m_refresh, &QToolButton::clicked, this, &SftpPanel::refresh);
    connect(m_mkdir, &QToolButton::clicked, this, &SftpPanel::newFolder);
    connect(m_download, &QToolButton::clicked, this, [this] { downloadSelected(false); });
    connect(m_path, &QLineEdit::returnPressed, this, [this] {
        QString p = m_path->text().trimmed();
        if (p == QLatin1String("~") || p.isEmpty())
            p = m_home.isEmpty() ? QStringLiteral(".") : m_home;
        else if (p.startsWith(QLatin1String("~/")) && !m_home.isEmpty())
            p = ssh::SftpEngine::joinRemote(m_home, p.mid(2));
        else if (!p.startsWith(QLatin1Char('/')) && !m_cwd.isEmpty())
            p = ssh::SftpEngine::joinRemote(m_cwd, p);
        navigate(p);
    });
    connect(m_list, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *it) {
        if (it->data(0, RoleIsDir).toBool())
            navigate(remotePath(it->data(0, RoleName).toString()));
        else
            downloadSelected(false);
    });
    connect(m_list, &QTreeWidget::customContextMenuRequested, this, &SftpPanel::showMenu);
    connect(m_list, &QTreeWidget::itemSelectionChanged, this, &SftpPanel::updateEnabled);
    connect(m_transfers, &QTreeWidget::customContextMenuRequested, this, &SftpPanel::showTransferMenu);
    updateEnabled();
}

// ---------------------------------------------------------------- binding

void SftpPanel::setSession(TerminalSession *session)
{
    if (m_session == session)
        return;
    if (m_session) {
        if (!m_cwd.isEmpty())
            m_lastPath.insert(m_session.data(), m_cwd);
        m_session->disconnect(this);
    }
    m_session = session;
    if (session) {
        connect(session, &TerminalSession::transportChanged, this, &SftpPanel::bindTransport);
        connect(session, &TerminalSession::stateChanged, this, [this](TransportState s) {
            if (s == TransportState::Connected && isVisible() && m_cwd.isEmpty())
                activate();
            updateEnabled();
        });
        connect(session, &QObject::destroyed, this, [this, session] { m_lastPath.remove(session); });
    }
    m_transport = nullptr;
    m_cwd.clear();
    m_home.clear();
    m_entries.clear();
    m_list->clear();
    bindTransport();
    if (isVisible())
        activate();
}

void SftpPanel::bindTransport()
{
    SshTransport *t = m_session ? m_session->sshTransport() : nullptr;
    if (t != m_transport) {
        m_transport = t;
        m_listId = 0;
        if (t)
            hookTransport(t);
    }
    updateEnabled();
}

void SftpPanel::hookTransport(SshTransport *t)
{
    if (m_hooked.contains(t))
        return;
    m_hooked.insert(t);
    connect(t, &SshTransport::sftpFinished, this, &SftpPanel::onFinished);
    connect(t, &SshTransport::sftpProgress, this, &SftpPanel::onProgress);
    connect(t, &QObject::destroyed, this, [this, t] {
        m_hooked.remove(t);
        // Its answers will never come: settle what was waiting for them.
        const QList<quint64> ids = m_pending.keys();
        for (quint64 id : ids) {
            if (m_pending.value(id).transport.isNull()) {
                SftpResult r;
                r.id = id;
                r.op = m_pending.value(id).op;
                r.error = tr("Disconnected");
                onFinished(r);
            }
        }
    });
}

void SftpPanel::activate()
{
    if (!m_transport || m_transport->state() != TransportState::Connected)
        return;
    if (!m_cwd.isEmpty() || m_listId)
        return;
    const QString last = m_session ? m_lastPath.value(m_session.data()) : QString();
    if (m_home.isEmpty()) {
        SftpRequest h;
        h.op = SftpOp::Home;
        submit(h);
    }
    navigate(last.isEmpty() ? QStringLiteral(".") : last);
}

int SftpPanel::activeTransfers() const
{
    int n = 0;
    for (const Pending &p : m_pending)
        n += (p.op == SftpOp::Download || p.op == SftpOp::Upload) ? 1 : 0;
    return n;
}

void SftpPanel::updateEnabled()
{
    const bool up = m_transport && m_transport->state() == TransportState::Connected;
    const bool shown = up && !m_cwd.isEmpty();
    for (QWidget *w : {static_cast<QWidget *>(m_up), static_cast<QWidget *>(m_homeBtn), static_cast<QWidget *>(m_refresh),
                       static_cast<QWidget *>(m_path)})
        w->setEnabled(up);
    m_mkdir->setEnabled(shown);
    m_upload->setEnabled(shown);
    m_download->setEnabled(shown && !m_list->selectedItems().isEmpty());
    const bool listVisible = shown || (up && m_listId);
    m_list->setVisible(listVisible);
    m_empty->setVisible(!listVisible);
    if (!m_session || !m_session->sshTransport())
        m_empty->setText(tr("The SFTP browser works on SSH sessions.\nOpen or select an SSH tab."));
    else if (!up)
        m_empty->setText(tr("Not connected."));
    else
        m_empty->setText(tr("Loading..."));
}

// ---------------------------------------------------------------- requests

quint64 SftpPanel::submit(SftpRequest req, const QString &refreshDir)
{
    if (!m_transport)
        return 0;
    hookTransport(m_transport);
    const quint64 id = m_transport->submitSftp(req);
    Pending p;
    p.transport = m_transport;
    p.op = req.op;
    p.refreshDir = refreshDir;
    if (req.op == SftpOp::Download || req.op == SftpOp::Upload) {
        const bool down = req.op == SftpOp::Download;
        p.name = down ? QFileInfo(req.path).fileName() : QFileInfo(req.localPath).fileName();
        const QString label = (down ? QStringLiteral("↓ ") : QStringLiteral("↑ ")) + p.name;
        const QString tip = down ? tr("%1\n→ %2").arg(req.path, QDir::toNativeSeparators(req.localPath))
                                 : tr("%1\n→ %2").arg(QDir::toNativeSeparators(req.localPath), req.path);
        p.row = addTransferRow(label, tip);
        p.row->setData(0, RoleId, id);
        p.bar = new QProgressBar(m_transfers);
        p.bar->setRange(0, 0);
        p.bar->setTextVisible(true);
        p.bar->setMaximumHeight(16);
        m_transfers->setItemWidget(p.row, 1, p.bar);
    }
    m_pending.insert(id, p);
    return id;
}

QTreeWidgetItem *SftpPanel::addTransferRow(const QString &label, const QString &tooltip)
{
    auto *it = new QTreeWidgetItem(m_transfers);
    it->setText(0, label);
    it->setToolTip(0, tooltip);
    it->setText(2, tr("Queued"));
    m_transfers->show();
    m_transfers->scrollToItem(it);
    return it;
}

void SftpPanel::onProgress(const SftpProgress &p)
{
    const auto it = m_pending.find(p.id);
    if (it == m_pending.end() || !it->row)
        return;
    if (p.total > 0) {
        // Progress bars hold an int: scale to permille.
        it->bar->setRange(0, 1000);
        it->bar->setValue(int(std::clamp<qint64>(p.done * 1000 / p.total, 0, 1000)));
        it->bar->setFormat(QStringLiteral("%1%").arg(p.done * 100 / p.total));
    } else {
        it->bar->setRange(0, 0);
    }
    QString status = formatBytes(quint64(qMax<qint64>(0, p.done)));
    if (p.total > 0)
        status += QStringLiteral(" / ") + formatBytes(quint64(p.total));
    if (p.bytesPerSecond > 0)
        status += QStringLiteral(" · %1/s").arg(formatBytes(quint64(p.bytesPerSecond)));
    if (p.filesTotal > 1)
        status += tr(" · %1/%2 files").arg(p.filesDone).arg(p.filesTotal);
    it->row->setText(2, status);
    if (!p.currentFile.isEmpty())
        it->row->setToolTip(2, p.currentFile);
}

void SftpPanel::onFinished(const SftpResult &r)
{
    const auto it = m_pending.find(r.id);
    if (it == m_pending.end())
        return;
    const Pending p = *it;
    m_pending.erase(it);

    if (p.row) {
        if (p.bar) {
            m_transfers->removeItemWidget(p.row, 1);
        }
        QString status;
        if (r.cancelled)
            status = tr("Cancelled");
        else if (!r.ok)
            status = tr("Failed: %1").arg(r.error);
        else {
            status = tr("Done · %1").arg(formatBytes(quint64(r.bytes)));
            if (r.files > 1)
                status += tr(" · %1 files").arg(r.files);
            if (!r.skipped.isEmpty())
                status += tr(" · %1 skipped").arg(r.skipped.size());
        }
        p.row->setText(1, r.ok ? QStringLiteral("100%") : QString());
        p.row->setText(2, status);
        p.row->setData(0, RoleId, QVariant());
        QStringList tip;
        if (!r.error.isEmpty())
            tip << r.error;
        tip << r.skipped.mid(0, 30);
        if (r.skipped.size() > 30)
            tip << tr("... and %1 more").arg(r.skipped.size() - 30);
        p.row->setToolTip(2, tip.join(QLatin1Char('\n')));
        if (!r.ok && !r.cancelled)
            p.row->setForeground(2, QColor(0xE5, 0x53, 0x4B));
    }

    if (p.transport != m_transport)
        return;   // another session

    switch (r.op) {
    case SftpOp::Home:
        if (r.ok)
            m_home = r.path;
        break;
    case SftpOp::List:
        if (r.id != m_listId)
            break;
        m_listId = 0;
        if (r.ok) {
            showEntries(r.path, r.entries);
            setMessage(r.skipped.isEmpty() ? QString() : r.skipped.join(QLatin1Char('\n')));
        } else {
            setMessage(r.error, true);
            m_path->setText(m_cwd);
        }
        break;
    default:
        if (!r.ok && !r.cancelled && !p.row)
            setMessage(r.error, true);
        break;
    }
    if (!p.refreshDir.isEmpty() && p.refreshDir == m_cwd && r.op != SftpOp::Download)
        refresh();
    updateEnabled();
}

void SftpPanel::setMessage(const QString &text, bool error)
{
    m_status->setStyleSheet(error ? QStringLiteral("color: #E5534B;") : QString());
    if (text.isEmpty() && !m_cwd.isEmpty()) {
        int files = 0, dirs = 0;
        for (const SftpEntry &e : std::as_const(m_entries))
            (e.isDir ? dirs : files)++;
        m_status->setText(tr("%1 folders, %2 files").arg(dirs).arg(files));
    } else {
        m_status->setText(text);
    }
}

void SftpPanel::navigate(const QString &path)
{
    if (!m_transport || m_transport->state() != TransportState::Connected)
        return;
    SftpRequest req;
    req.op = SftpOp::List;
    req.path = path;
    m_listId = submit(req);
    setMessage(tr("Loading %1...").arg(path));
    updateEnabled();
}

void SftpPanel::refresh()
{
    navigate(m_cwd.isEmpty() ? QStringLiteral(".") : m_cwd);
}

void SftpPanel::goUp()
{
    if (!m_cwd.isEmpty() && m_cwd != QLatin1String("/"))
        navigate(parentOf(m_cwd));
}

void SftpPanel::showEntries(const QString &path, const QVector<SftpEntry> &entries)
{
    const bool samePlace = path == m_cwd;
    QStringList keepSelected;
    if (samePlace)
        keepSelected = selectedNames();
    m_cwd = path;
    m_entries = entries;
    m_path->setText(path);
    if (m_session)
        m_lastPath.insert(m_session.data(), path);

    m_list->setUpdatesEnabled(false);
    m_list->setSortingEnabled(false);
    m_list->clear();
    const QIcon dirIcon = style()->standardIcon(QStyle::SP_DirIcon);
    const QIcon fileIcon = style()->standardIcon(QStyle::SP_FileIcon);
    const QIcon linkIcon = style()->standardIcon(QStyle::SP_FileLinkIcon);
    const QIcon dirLinkIcon = style()->standardIcon(QStyle::SP_DirLinkIcon);
    QList<QTreeWidgetItem *> items;
    items.reserve(entries.size());
    for (const SftpEntry &e : entries) {
        if (!m_showHidden && e.name.startsWith(QLatin1Char('.')))
            continue;
        auto *it = new EntryItem;
        it->setText(0, e.name);
        it->setData(0, RoleName, e.name);
        it->setData(0, RoleIsDir, e.isDir);
        it->setIcon(0, e.isLink ? (e.isDir ? dirLinkIcon : linkIcon) : (e.isDir ? dirIcon : fileIcon));
        if (!e.isDir && e.size >= 0) {
            it->setText(1, formatBytes(quint64(e.size)));
            it->setData(1, RoleSort, qint64(e.size));
            it->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        }
        if (e.mtime > 0) {
            it->setText(2, QDateTime::fromSecsSinceEpoch(e.mtime).toString(QStringLiteral("yyyy-MM-dd HH:mm")));
            it->setData(2, RoleSort, qint64(e.mtime));
        }
        it->setText(3, permString(e.permissions));
        it->setToolTip(0, e.isLink ? tr("%1 (symbolic link)").arg(e.name) : e.name);
        items << it;
    }
    m_list->addTopLevelItems(items);
    m_list->setSortingEnabled(true);
    for (QTreeWidgetItem *it : std::as_const(items))
        if (keepSelected.contains(it->data(0, RoleName).toString()))
            it->setSelected(true);
    m_list->setUpdatesEnabled(true);
    setMessage(QString());
    updateEnabled();
}

QStringList SftpPanel::selectedNames(bool *anyDir) const
{
    QStringList out;
    if (anyDir)
        *anyDir = false;
    for (QTreeWidgetItem *it : m_list->selectedItems()) {
        out << it->data(0, RoleName).toString();
        if (anyDir && it->data(0, RoleIsDir).toBool())
            *anyDir = true;
    }
    return out;
}

QString SftpPanel::remotePath(const QString &name) const
{
    return ssh::SftpEngine::joinRemote(m_cwd, name);
}

QString SftpPanel::downloadDir() const
{
    const QString def = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    return m_settings ? m_settings->value(QStringLiteral("sftp/downloadDir"), def).toString() : def;
}

// ---------------------------------------------------------------- actions

void SftpPanel::downloadSelected(bool chooseFolder)
{
    const QStringList names = selectedNames();
    if (names.isEmpty() || !m_transport)
        return;
    QString dir = downloadDir();
    if (chooseFolder || !QFileInfo(dir).isDir()) {
        dir = QFileDialog::getExistingDirectory(this, tr("Download to"), dir);
        if (dir.isEmpty())
            return;
        if (m_settings)
            m_settings->setValue(QStringLiteral("sftp/downloadDir"), dir);
    }
    QStringList existing;
    for (const QString &n : names) {
        QString safe;
        if (ssh::SftpEngine::safeLocalName(n, safe) && QFileInfo::exists(QDir(dir).filePath(safe)))
            existing << safe;
    }
    bool overwrite = false;
    if (!existing.isEmpty()) {
        const auto b = QMessageBox::question(
            this, tr("Replace files?"),
            tr("%n item(s) already exist in %1:\n%2\n\nReplace existing files? (No = keep them and skip)", nullptr,
               int(existing.size()))
                .arg(QDir::toNativeSeparators(dir), existing.mid(0, 10).join(QLatin1Char('\n'))),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::No);
        if (b == QMessageBox::Cancel)
            return;
        overwrite = b == QMessageBox::Yes;
    }
    for (const QString &n : names) {
        QString safe;
        if (!ssh::SftpEngine::safeLocalName(n, safe)) {
            setMessage(tr("\"%1\" is not a safe file name here; skipped").arg(n), true);
            continue;
        }
        SftpRequest r;
        r.op = SftpOp::Download;
        r.path = remotePath(n);
        r.localPath = QDir(dir).filePath(safe);
        r.overwrite = overwrite;
        submit(r);
    }
}

void SftpPanel::upload(const QStringList &localPaths, const QString &remoteDir)
{
    if (localPaths.isEmpty() || !m_transport || remoteDir.isEmpty())
        return;
    QStringList existing;
    if (remoteDir == m_cwd) {
        for (const QString &l : localPaths) {
            const QString name = QFileInfo(l).fileName();
            for (const SftpEntry &e : std::as_const(m_entries))
                if (e.name == name)
                    existing << name;
        }
    }
    bool overwrite = false;
    if (!existing.isEmpty()) {
        const auto b = QMessageBox::question(
            this, tr("Replace files?"),
            tr("%n item(s) already exist in %1:\n%2\n\nReplace existing files? (No = keep them and skip)", nullptr,
               int(existing.size()))
                .arg(remoteDir, existing.mid(0, 10).join(QLatin1Char('\n'))),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::No);
        if (b == QMessageBox::Cancel)
            return;
        overwrite = b == QMessageBox::Yes;
    }
    for (const QString &l : localPaths) {
        const QFileInfo fi(l);
        if (!fi.exists())
            continue;
        SftpRequest r;
        r.op = SftpOp::Upload;
        r.localPath = fi.absoluteFilePath();
        r.path = ssh::SftpEngine::joinRemote(remoteDir, fi.fileName());
        r.overwrite = overwrite;
        submit(r, remoteDir);
    }
}

void SftpPanel::uploadFilesDialog()
{
    const QString start = m_settings ? m_settings->value(QStringLiteral("sftp/uploadDir")).toString() : QString();
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Upload to %1").arg(m_cwd), start);
    if (files.isEmpty())
        return;
    if (m_settings)
        m_settings->setValue(QStringLiteral("sftp/uploadDir"), QFileInfo(files.first()).absolutePath());
    upload(files, m_cwd);
}

void SftpPanel::uploadFolderDialog()
{
    const QString start = m_settings ? m_settings->value(QStringLiteral("sftp/uploadDir")).toString() : QString();
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Upload folder to %1").arg(m_cwd), start);
    if (dir.isEmpty())
        return;
    if (m_settings)
        m_settings->setValue(QStringLiteral("sftp/uploadDir"), QFileInfo(dir).absolutePath());
    upload({dir}, m_cwd);
}

void SftpPanel::newFolder()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("New Folder"), tr("Folder name in %1:").arg(m_cwd),
                                               QLineEdit::Normal, QString(), &ok)
                             .trimmed();
    if (!ok || name.isEmpty())
        return;
    if (name.contains(QLatin1Char('/')) || name == QLatin1String(".") || name == QLatin1String("..")) {
        setMessage(tr("A folder name cannot contain '/'"), true);
        return;
    }
    SftpRequest r;
    r.op = SftpOp::Mkdir;
    r.path = remotePath(name);
    submit(r, m_cwd);
}

void SftpPanel::renameSelected()
{
    const QStringList names = selectedNames();
    if (names.size() != 1)
        return;
    bool ok = false;
    const QString to = QInputDialog::getText(this, tr("Rename"), tr("New name for %1:").arg(names.first()),
                                             QLineEdit::Normal, names.first(), &ok)
                           .trimmed();
    if (!ok || to.isEmpty() || to == names.first())
        return;
    SftpRequest r;
    r.op = SftpOp::Rename;
    r.path = remotePath(names.first());
    r.path2 = to.startsWith(QLatin1Char('/')) ? to : remotePath(to);
    submit(r, m_cwd);
}

void SftpPanel::deleteSelected()
{
    bool anyDir = false;
    const QStringList names = selectedNames(&anyDir);
    if (names.isEmpty())
        return;
    QString text = names.size() == 1 ? tr("Delete \"%1\" from the server?").arg(names.first())
                                     : tr("Delete these %1 items from the server?\n%2")
                                           .arg(names.size())
                                           .arg(names.mid(0, 12).join(QLatin1Char('\n')));
    if (anyDir)
        text += QStringLiteral("\n\n") + tr("Folders are deleted with everything in them.");
    text += QStringLiteral("\n") + tr("This cannot be undone.");
    if (QMessageBox::warning(this, tr("Delete"), text, QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)
        != QMessageBox::Yes)
        return;
    for (const QString &n : names) {
        SftpRequest r;
        r.op = SftpOp::Remove;
        r.path = remotePath(n);
        submit(r, m_cwd);
    }
}

void SftpPanel::chmodSelected()
{
    const QStringList names = selectedNames();
    if (names.isEmpty())
        return;
    quint32 current = 0644;
    for (const SftpEntry &e : std::as_const(m_entries))
        if (e.name == names.first() && e.permissions)
            current = e.permissions & 07777;
    bool ok = false;
    const QString text = QInputDialog::getText(this, tr("Permissions"),
                                               tr("Octal mode for %1 (e.g. 644, 755, 600):").arg(names.join(QStringLiteral(", "))),
                                               QLineEdit::Normal, QString::number(current, 8), &ok)
                             .trimmed();
    if (!ok || text.isEmpty())
        return;
    bool valid = false;
    const uint mode = text.toUInt(&valid, 8);
    if (!valid || mode > 07777) {
        setMessage(tr("\"%1\" is not an octal mode").arg(text), true);
        return;
    }
    for (const QString &n : names) {
        SftpRequest r;
        r.op = SftpOp::Chmod;
        r.path = remotePath(n);
        r.mode = mode;
        submit(r, m_cwd);
    }
}

void SftpPanel::cdInTerminal(const QString &path)
{
    if (m_session)
        m_session->sendCommand(QStringLiteral("cd -- %1").arg(shellQuote(path)), true);
}

void SftpPanel::showMenu(const QPoint &pos)
{
    QMenu menu(this);
    const bool up = m_transport && m_transport->state() == TransportState::Connected && !m_cwd.isEmpty();
    QTreeWidgetItem *it = m_list->itemAt(pos);
    if (it && !it->isSelected()) {
        m_list->clearSelection();
        it->setSelected(true);
    }
    const QStringList names = selectedNames();
    if (!names.isEmpty() && up) {
        if (names.size() == 1 && it && it->data(0, RoleIsDir).toBool()) {
            menu.addAction(tr("Open"), this, [this, n = names.first()] { navigate(remotePath(n)); });
            menu.addAction(tr("Open in Terminal (cd)"), this, [this, n = names.first()] { cdInTerminal(remotePath(n)); });
            menu.addSeparator();
        }
        menu.addAction(style()->standardIcon(QStyle::SP_ArrowDown), tr("Download"), this, [this] { downloadSelected(false); });
        menu.addAction(tr("Download To..."), this, [this] { downloadSelected(true); });
        menu.addSeparator();
        QAction *ren = menu.addAction(tr("Rename..."), this, &SftpPanel::renameSelected);
        ren->setEnabled(names.size() == 1);
        menu.addAction(tr("Permissions..."), this, &SftpPanel::chmodSelected);
        menu.addAction(tr("Copy Path"), this, [this, names] {
            QStringList paths;
            for (const QString &n : names)
                paths << remotePath(n);
            QApplication::clipboard()->setText(paths.join(QLatin1Char('\n')));
        });
        menu.addSeparator();
        menu.addAction(style()->standardIcon(QStyle::SP_TrashIcon), tr("Delete..."), this, &SftpPanel::deleteSelected);
        menu.addSeparator();
    }
    if (up) {
        menu.addAction(tr("Upload Files..."), this, &SftpPanel::uploadFilesDialog);
        menu.addAction(tr("Upload Folder..."), this, &SftpPanel::uploadFolderDialog);
        menu.addAction(tr("New Folder..."), this, &SftpPanel::newFolder);
        menu.addAction(tr("Open This Folder in Terminal (cd)"), this, [this] { cdInTerminal(m_cwd); });
        menu.addAction(tr("Refresh"), this, &SftpPanel::refresh);
        menu.addSeparator();
    }
    QAction *hidden = menu.addAction(tr("Show Hidden Files"));
    hidden->setCheckable(true);
    hidden->setChecked(m_showHidden);
    connect(hidden, &QAction::toggled, this, [this](bool on) {
        m_showHidden = on;
        if (m_settings)
            m_settings->setValue(QStringLiteral("sftp/showHidden"), on);
        showEntries(m_cwd, m_entries);
    });
    menu.addAction(tr("Set Download Folder..."), this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Download folder"), downloadDir());
        if (!dir.isEmpty() && m_settings)
            m_settings->setValue(QStringLiteral("sftp/downloadDir"), dir);
    });
    menu.exec(m_list->viewport()->mapToGlobal(pos));
}

void SftpPanel::showTransferMenu(const QPoint &pos)
{
    QMenu menu(this);
    QList<quint64> running;
    for (QTreeWidgetItem *it : m_transfers->selectedItems()) {
        const QVariant id = it->data(0, RoleId);
        if (id.isValid())
            running << id.toULongLong();
    }
    QAction *cancel = menu.addAction(tr("Cancel"), this, [this, running] {
        for (quint64 id : running) {
            const Pending p = m_pending.value(id);
            if (p.transport)
                p.transport->cancelSftp(id);
        }
    });
    cancel->setEnabled(!running.isEmpty());
    menu.addAction(tr("Clear Finished"), this, [this] {
        for (int i = m_transfers->topLevelItemCount() - 1; i >= 0; --i)
            if (!m_transfers->topLevelItem(i)->data(0, RoleId).isValid())
                delete m_transfers->takeTopLevelItem(i);
        if (m_transfers->topLevelItemCount() == 0)
            m_transfers->hide();
    });
    menu.exec(m_transfers->viewport()->mapToGlobal(pos));
}

// ---------------------------------------------------------------- keys, drag & drop

bool SftpPanel::eventFilter(QObject *obj, QEvent *e)
{
    if (obj == m_list && e->type() == QEvent::KeyPress) {
        auto *k = static_cast<QKeyEvent *>(e);
        if (k->key() == Qt::Key_Backspace) {
            goUp();
            return true;
        }
        if (k->key() == Qt::Key_F5) {
            refresh();
            return true;
        }
        if (k->key() == Qt::Key_Delete) {
            deleteSelected();
            return true;
        }
        if (k->key() == Qt::Key_F2) {
            renameSelected();
            return true;
        }
    }
    if (obj == m_list->viewport() && (e->type() == QEvent::DragEnter || e->type() == QEvent::DragMove)) {
        auto *d = static_cast<QDragMoveEvent *>(e);
        if (d->mimeData()->hasUrls() && !m_cwd.isEmpty()) {
            d->acceptProposedAction();
            return true;
        }
    }
    if (obj == m_list->viewport() && e->type() == QEvent::Drop) {
        auto *d = static_cast<QDropEvent *>(e);
        QString target = m_cwd;
        // Dropped onto a folder row: upload into that folder.
        if (QTreeWidgetItem *it = m_list->itemAt(d->position().toPoint()))
            if (it->data(0, RoleIsDir).toBool())
                target = remotePath(it->data(0, RoleName).toString());
        handleDrop(d, target);
        return true;
    }
    return QWidget::eventFilter(obj, e);
}

void SftpPanel::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls() && !m_cwd.isEmpty())
        e->acceptProposedAction();
}

void SftpPanel::dropEvent(QDropEvent *e)
{
    handleDrop(e, m_cwd);
}

void SftpPanel::handleDrop(QDropEvent *e, const QString &target)
{
    QStringList paths;
    for (const QUrl &u : e->mimeData()->urls())
        if (u.isLocalFile())
            paths << u.toLocalFile();
    if (paths.isEmpty() || target.isEmpty() || e->source() == m_list)
        return;
    e->acceptProposedAction();
    upload(paths, target);
}

} // namespace termx
