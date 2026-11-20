// SPDX-License-Identifier: GPL-3.0-or-later
// SFTP file browser for the SSH session in the current tab, over the same
// connection as the terminal (no second login), with a transfer queue.
#pragma once

#include "transport/SftpTypes.h"

#include <QHash>
#include <QPointer>
#include <QSet>
#include <QWidget>

class QLabel;
class QLineEdit;
class QProgressBar;
class QSettings;
class QSplitter;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace termx {

class SshTransport;
class TerminalSession;

class SftpPanel : public QWidget {
    Q_OBJECT
public:
    SftpPanel(QSettings *settings, QWidget *parent = nullptr);
    void setSession(TerminalSession *session);
    // The dock became visible: load the folder if nothing is shown yet.
    void activate();
    int activeTransfers() const;

protected:
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    struct Pending {
        QPointer<SshTransport> transport;
        SftpOp op = SftpOp::List;
        QString refreshDir;          // reload this folder when done (if still shown)
        QTreeWidgetItem *row = nullptr;   // transfer row
        QProgressBar *bar = nullptr;
        QString name;
    };

    void bindTransport();
    void hookTransport(SshTransport *t);
    void onFinished(const SftpResult &r);
    void onProgress(const SftpProgress &p);
    quint64 submit(SftpRequest req, const QString &refreshDir = QString());
    void navigate(const QString &path);
    void refresh();
    void goUp();
    void showEntries(const QString &path, const QVector<SftpEntry> &entries);
    void setMessage(const QString &text, bool error = false);
    void updateEnabled();

    QStringList selectedNames(bool *anyDir = nullptr) const;
    QString remotePath(const QString &name) const;
    void downloadSelected(bool chooseFolder);
    void upload(const QStringList &localPaths, const QString &remoteDir);
    void uploadFilesDialog();
    void uploadFolderDialog();
    void newFolder();
    void renameSelected();
    void deleteSelected();
    void chmodSelected();
    void cdInTerminal(const QString &path);
    void handleDrop(QDropEvent *e, const QString &target);
    void showMenu(const QPoint &pos);
    void showTransferMenu(const QPoint &pos);
    QTreeWidgetItem *addTransferRow(const QString &label, const QString &tooltip);
    QString downloadDir() const;

    QSettings *m_settings;
    QPointer<TerminalSession> m_session;
    QPointer<SshTransport> m_transport;
    QSet<SshTransport *> m_hooked;
    QHash<TerminalSession *, QString> m_lastPath;

    QString m_cwd;
    QString m_home;
    quint64 m_listId = 0;
    QVector<SftpEntry> m_entries;
    bool m_showHidden = false;
    QHash<quint64, Pending> m_pending;

    QLineEdit *m_path;
    QToolButton *m_up;
    QToolButton *m_homeBtn;
    QToolButton *m_refresh;
    QToolButton *m_mkdir;
    QToolButton *m_upload;
    QToolButton *m_download;
    QTreeWidget *m_list;
    QLabel *m_status;
    QLabel *m_empty;
    QTreeWidget *m_transfers;
    QSplitter *m_split;
};

} // namespace termx
