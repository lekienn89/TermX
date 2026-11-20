// SPDX-License-Identifier: GPL-3.0-or-later
// Requests and results exchanged between the SFTP panel and the SSH I/O thread.
#pragma once

#include <QMetaType>
#include <QString>
#include <QVector>

namespace termx {

struct SftpEntry {
    QString name;
    bool isDir = false;
    bool isLink = false;       // symbolic link (isDir tells whether it points to a folder)
    qint64 size = -1;
    qint64 mtime = 0;          // seconds since the epoch, 0 = unknown
    quint32 permissions = 0;   // POSIX mode bits, 0 = unknown
    quint32 uid = 0;
    quint32 gid = 0;
};

enum class SftpOp {
    Home,       // resolve "." -> absolute home directory (path)
    List,       // list a directory (path)
    Mkdir,      // create a directory (path)
    Rename,     // path -> path2
    Remove,     // file or (recursive) folder (path)
    Chmod,      // path, mode
    Download,   // path (file or folder) -> localPath (target file / folder to create)
    Upload      // localPath (file or folder) -> path (target file / folder to create)
};

QString sftpOpName(SftpOp op);

struct SftpRequest {
    quint64 id = 0;           // set by SshTransport::submitSftp
    SftpOp op = SftpOp::List;
    QString path;             // remote, absolute ("/home/kien") or relative to home
    QString path2;            // Rename target
    QString localPath;
    quint32 mode = 0;         // Chmod
    bool overwrite = false;   // transfers: replace existing files
};

struct SftpResult {
    quint64 id = 0;
    SftpOp op = SftpOp::List;
    bool ok = false;
    bool cancelled = false;
    QString error;
    QString path;                 // canonical path (Home/List), or the request path
    QVector<SftpEntry> entries;   // List
    QString localPath;
    qint64 bytes = 0;             // transfers: bytes moved
    int files = 0;                // transfers / Remove: files handled
    QStringList skipped;          // transfers: items not copied (with reason)
};

struct SftpProgress {
    quint64 id = 0;
    qint64 done = 0;
    qint64 total = -1;        // -1 while still counting
    QString currentFile;
    int filesDone = 0;
    int filesTotal = 0;
    double bytesPerSecond = 0;
};

} // namespace termx

Q_DECLARE_METATYPE(termx::SftpRequest)
Q_DECLARE_METATYPE(termx::SftpResult)
Q_DECLARE_METATYPE(termx::SftpProgress)
