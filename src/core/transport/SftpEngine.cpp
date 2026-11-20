// SPDX-License-Identifier: GPL-3.0-or-later
#include "SftpEngine.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

#include <libssh2_sftp.h>

#include <algorithm>
#include <vector>

namespace termx::ssh {

namespace {

constexpr int kMaxListEntries = 100000;
constexpr int kMaxTreeItems = 200000;
constexpr int kMaxDepth = 64;
constexpr int kMaxLinkStats = 500;
constexpr size_t kReadBuffer = 2 * 1024 * 1024;   // larger = more requests in flight
constexpr qint64 kWriteChunk = 1024 * 1024;
constexpr qint64 kSliceMs = 15;
constexpr qint64 kProgressMs = 150;
const char *kPartSuffix = ".termx-part";

QString fxText(unsigned long code)
{
    switch (code) {
    case LIBSSH2_FX_EOF: return QStringLiteral("end of file");
    case LIBSSH2_FX_NO_SUCH_FILE: return QStringLiteral("no such file or folder");
    case LIBSSH2_FX_PERMISSION_DENIED: return QStringLiteral("permission denied");
    case LIBSSH2_FX_FAILURE: return QStringLiteral("the server reported a failure");
    case LIBSSH2_FX_BAD_MESSAGE: return QStringLiteral("bad message");
    case LIBSSH2_FX_NO_CONNECTION: return QStringLiteral("no connection");
    case LIBSSH2_FX_CONNECTION_LOST: return QStringLiteral("connection lost");
    case LIBSSH2_FX_OP_UNSUPPORTED: return QStringLiteral("operation not supported by the server");
    case LIBSSH2_FX_INVALID_HANDLE: return QStringLiteral("invalid handle");
    case LIBSSH2_FX_NO_SUCH_PATH: return QStringLiteral("no such path");
    case LIBSSH2_FX_FILE_ALREADY_EXISTS: return QStringLiteral("already exists");
    case LIBSSH2_FX_WRITE_PROTECT: return QStringLiteral("write protected");
    case LIBSSH2_FX_NO_MEDIA: return QStringLiteral("no media");
    case LIBSSH2_FX_NO_SPACE_ON_FILESYSTEM: return QStringLiteral("no space left on the server");
    case LIBSSH2_FX_QUOTA_EXCEEDED: return QStringLiteral("quota exceeded");
    case LIBSSH2_FX_LOCK_CONFLICT: return QStringLiteral("lock conflict");
    case LIBSSH2_FX_DIR_NOT_EMPTY: return QStringLiteral("folder is not empty");
    case LIBSSH2_FX_NOT_A_DIRECTORY: return QStringLiteral("not a folder");
    case LIBSSH2_FX_INVALID_FILENAME: return QStringLiteral("invalid file name");
    case LIBSSH2_FX_LINK_LOOP: return QStringLiteral("too many symbolic links");
    default: return QStringLiteral("SFTP error %1").arg(code);
    }
}

SftpEntry entryFrom(const QString &name, const LIBSSH2_SFTP_ATTRIBUTES &a)
{
    SftpEntry e;
    e.name = name;
    if (a.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) {
        e.permissions = quint32(a.permissions);
        e.isDir = LIBSSH2_SFTP_S_ISDIR(a.permissions);
        e.isLink = LIBSSH2_SFTP_S_ISLNK(a.permissions);
    }
    if (a.flags & LIBSSH2_SFTP_ATTR_SIZE)
        e.size = qint64(a.filesize);
    if (a.flags & LIBSSH2_SFTP_ATTR_ACMODTIME)
        e.mtime = qint64(a.mtime);
    if (a.flags & LIBSSH2_SFTP_ATTR_UIDGID) {
        e.uid = quint32(a.uid);
        e.gid = quint32(a.gid);
    }
    return e;
}

struct TreeItem {
    QString remote;
    QString local;     // relative to the local root, '/' separated
    bool isDir = false;
    qint64 size = 0;
    qint64 mtime = 0;
    quint32 mode = 0;
};

} // namespace

// ============================================================== Lane

class SftpEngine::Lane {
public:
    Lane(Context &ctx, ResultFn &result, ProgressFn &progress, CancelledFn &cancelled)
        : m_ctx(ctx), m_result(result), m_progress(progress), m_cancelled(cancelled)
    {
    }

    ~Lane() { abort(QStringLiteral("Disconnected")); }

    void submit(const SftpRequest &r) { m_queue.push_back(r); }
    bool wantsTurn() const { return !m_exited && (m_busy || !m_queue.empty()); }
    bool busy() const { return m_busy || !m_queue.empty(); }

    bool hasBufferedData() const
    {
        if (!m_sftp)
            return false;
        LIBSSH2_CHANNEL *ch = libssh2_sftp_get_channel(m_sftp);
        return ch && libssh2_poll_channel_read(ch, 0) > 0;
    }

    // I/O thread: run the lane until it yields. True if it got something done.
    bool resume()
    {
        if (m_exited)
            return false;
        if (!m_started) {
            m_started = true;
            m_thread = std::thread([this] { threadMain(); });
        }
        m_moved = false;
        std::unique_lock<std::mutex> lock(m_mx);
        m_laneTurn = true;
        m_cv.notify_all();
        m_cv.wait(lock, [this] { return !m_laneTurn; });
        return m_moved;
    }

    void abort(const QString &why)
    {
        if (m_started && !m_exited) {
            m_aborting = true;
            m_abortReason = why;
            while (!m_exited)
                resume();
        }
        if (m_thread.joinable())
            m_thread.join();
        m_ctx.forget(this);
        while (!m_queue.empty()) {
            const SftpRequest r = m_queue.front();
            m_queue.pop_front();
            SftpResult res;
            res.id = r.id;
            res.op = r.op;
            res.path = r.path;
            res.localPath = r.localPath;
            res.error = why;
            m_result(res);
        }
        m_sftp = nullptr;   // the session owns its channels; freed with it
    }

private:
    // ---- baton (lane thread side) ----------------------------------------
    void threadMain()
    {
        {
            std::unique_lock<std::mutex> lock(m_mx);
            m_cv.wait(lock, [this] { return m_laneTurn; });
        }
        m_slice.start();
        while (!m_aborting) {
            if (m_queue.empty()) {
                yieldNow();
                continue;
            }
            const SftpRequest req = m_queue.front();
            m_queue.pop_front();
            m_busy = true;
            run(req);
            m_busy = false;
            m_moved = true;
        }
        std::lock_guard<std::mutex> lock(m_mx);
        m_exited = true;
        m_laneTurn = false;
        m_cv.notify_all();
    }

    void yieldNow()
    {
        std::unique_lock<std::mutex> lock(m_mx);
        m_laneTurn = false;
        m_cv.notify_all();
        m_cv.wait(lock, [this] { return m_laneTurn; });
        m_slice.restart();
    }

    // Lets the shell and tunnels run during long work.
    void checkpoint()
    {
        if (m_slice.elapsed() >= kSliceMs) {
            m_moved = true;
            yieldNow();
        }
    }

    // Repeats a libssh2 call until it stops saying EAGAIN.
    template <typename Fn>
    qint64 call(Fn fn)
    {
        for (;;) {
            if (m_aborting)
                return LIBSSH2_ERROR_SOCKET_DISCONNECT;
            if (!m_ctx.mayRun(this)) {
                yieldNow();
                continue;
            }
            const qint64 rc = qint64(fn());
            m_ctx.after(this, rc < 0 ? long(rc) : 0);
            if (rc != LIBSSH2_ERROR_EAGAIN) {
                m_moved = true;
                return rc;
            }
            yieldNow();
        }
    }

    template <typename T, typename Fn>
    T *callPtr(Fn fn)
    {
        for (;;) {
            if (m_aborting) {
                m_lastRc = LIBSSH2_ERROR_SOCKET_DISCONNECT;
                return nullptr;
            }
            if (!m_ctx.mayRun(this)) {
                yieldNow();
                continue;
            }
            T *p = fn();
            const int rc = p ? 0 : libssh2_session_last_errno(m_ctx.session);
            m_ctx.after(this, rc);
            if (p || rc != LIBSSH2_ERROR_EAGAIN) {
                m_lastRc = rc;
                m_moved = true;
                return p;
            }
            yieldNow();
        }
    }

    QString err(qint64 rc) const
    {
        if (m_aborting)
            return m_abortReason;
        if (rc == LIBSSH2_ERROR_SFTP_PROTOCOL && m_sftp)
            return fxText(libssh2_sftp_last_error(m_sftp));
        return lastError(m_ctx.session);
    }

    bool cancelled(quint64 id) const { return m_aborting || (m_cancelled && m_cancelled(id)); }

    // ---- SFTP primitives ---------------------------------------------------
    bool ensureSftp(QString &error)
    {
        if (m_sftp)
            return true;
        while (!m_ctx.tryLockOpen(this)) {
            if (m_aborting) {
                error = m_abortReason;
                return false;
            }
            yieldNow();
        }
        m_sftp = callPtr<LIBSSH2_SFTP>([&] { return libssh2_sftp_init(m_ctx.session); });
        m_ctx.unlockOpen(this);
        if (!m_sftp) {
            error = m_aborting ? m_abortReason
                               : QStringLiteral("SFTP is not available on this server (%1)").arg(lastError(m_ctx.session));
            return false;
        }
        return true;
    }

    bool realPath(const QString &path, QString &out, QString &error)
    {
        const QByteArray p = (path.isEmpty() ? QStringLiteral(".") : path).toUtf8();
        QByteArray buf(4096, '\0');
        const qint64 rc = call([&] {
            return libssh2_sftp_symlink_ex(m_sftp, p.constData(), unsigned(p.size()), buf.data(), unsigned(buf.size()),
                                           LIBSSH2_SFTP_REALPATH);
        });
        if (rc <= 0) {
            error = QStringLiteral("%1: %2").arg(path, err(rc));
            return false;
        }
        out = QString::fromUtf8(buf.constData(), int(std::min<qint64>(rc, buf.size())));
        return true;
    }

    qint64 statPath(const QString &path, LIBSSH2_SFTP_ATTRIBUTES &a, bool follow)
    {
        const QByteArray p = path.toUtf8();
        a = LIBSSH2_SFTP_ATTRIBUTES{};
        return call([&] {
            return libssh2_sftp_stat_ex(m_sftp, p.constData(), unsigned(p.size()),
                                        follow ? LIBSSH2_SFTP_STAT : LIBSSH2_SFTP_LSTAT, &a);
        });
    }

    bool closeHandle(LIBSSH2_SFTP_HANDLE *h)
    {
        if (!h)
            return true;
        return call([&] { return libssh2_sftp_close_handle(h); }) == 0;
    }

    bool listDir(const QString &path, QVector<SftpEntry> &out, QString &error, quint64 id)
    {
        const QByteArray p = path.toUtf8();
        LIBSSH2_SFTP_HANDLE *h = callPtr<LIBSSH2_SFTP_HANDLE>([&] {
            return libssh2_sftp_open_ex(m_sftp, p.constData(), unsigned(p.size()), 0, 0, LIBSSH2_SFTP_OPENDIR);
        });
        if (!h) {
            error = QStringLiteral("Cannot open %1: %2").arg(path, err(m_lastRc));
            return false;
        }
        QByteArray name(4096, '\0');
        bool ok = true;
        for (;;) {
            LIBSSH2_SFTP_ATTRIBUTES a{};
            const qint64 n = call([&] { return libssh2_sftp_readdir_ex(h, name.data(), size_t(name.size()), nullptr, 0, &a); });
            if (n == 0)
                break;
            if (n < 0) {
                error = QStringLiteral("Cannot read %1: %2").arg(path, err(n));
                ok = false;
                break;
            }
            const QString nm = QString::fromUtf8(name.constData(), int(std::min<qint64>(n, name.size())));
            if (nm == QLatin1String(".") || nm == QLatin1String(".."))
                continue;
            out << entryFrom(nm, a);
            if (out.size() >= kMaxListEntries) {
                error = QStringLiteral("%1 has more than %2 entries; the list is cut").arg(path).arg(kMaxListEntries);
                break;
            }
            if (cancelled(id)) {
                ok = false;
                error = QStringLiteral("Cancelled");
                break;
            }
            checkpoint();
        }
        closeHandle(h);
        return ok;
    }

    // ---- requests -----------------------------------------------------------
    void run(const SftpRequest &req)
    {
        SftpResult res;
        res.id = req.id;
        res.op = req.op;
        res.path = req.path;
        res.localPath = req.localPath;
        QString error;
        if (!ensureSftp(error)) {
            res.error = error;
            m_result(res);
            return;
        }
        switch (req.op) {
        case SftpOp::Home:
            res.ok = realPath(req.path, res.path, res.error);
            break;
        case SftpOp::List:
            doList(req, res);
            break;
        case SftpOp::Mkdir: {
            const QByteArray p = req.path.toUtf8();
            const qint64 rc = call([&] { return libssh2_sftp_mkdir_ex(m_sftp, p.constData(), unsigned(p.size()), 0755); });
            res.ok = rc == 0;
            if (!res.ok)
                res.error = QStringLiteral("Cannot create %1: %2").arg(req.path, err(rc));
            break;
        }
        case SftpOp::Rename: {
            const QByteArray a = req.path.toUtf8();
            const QByteArray b = req.path2.toUtf8();
            const qint64 rc = call([&] {
                return libssh2_sftp_rename_ex(m_sftp, a.constData(), unsigned(a.size()), b.constData(), unsigned(b.size()),
                                              LIBSSH2_SFTP_RENAME_ATOMIC | LIBSSH2_SFTP_RENAME_NATIVE);
            });
            res.ok = rc == 0;
            if (!res.ok)
                res.error = QStringLiteral("Cannot rename %1: %2").arg(req.path, err(rc));
            break;
        }
        case SftpOp::Remove:
            doRemove(req, res);
            break;
        case SftpOp::Chmod: {
            LIBSSH2_SFTP_ATTRIBUTES a{};
            a.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS;
            a.permissions = req.mode & 07777;
            const QByteArray p = req.path.toUtf8();
            const qint64 rc = call([&] {
                return libssh2_sftp_stat_ex(m_sftp, p.constData(), unsigned(p.size()), LIBSSH2_SFTP_SETSTAT, &a);
            });
            res.ok = rc == 0;
            if (!res.ok)
                res.error = QStringLiteral("Cannot change permissions of %1: %2").arg(req.path, err(rc));
            break;
        }
        case SftpOp::Download:
            doDownload(req, res);
            break;
        case SftpOp::Upload:
            doUpload(req, res);
            break;
        }
        if (m_aborting && !res.ok && res.error.isEmpty())
            res.error = m_abortReason;
        if (!res.ok && !m_aborting && m_cancelled && m_cancelled(req.id)) {
            res.cancelled = true;
            res.error = QStringLiteral("Cancelled");
        }
        m_result(res);
    }

    void doList(const SftpRequest &req, SftpResult &res)
    {
        QString canonical;
        if (!realPath(req.path, canonical, res.error))
            return;
        res.path = canonical;
        QString warn;
        if (!listDir(canonical, res.entries, warn, req.id)) {
            res.error = warn;
            return;
        }
        if (!warn.isEmpty())
            res.skipped << warn;
        // Symbolic links: does the target behave like a folder?
        int stats = 0;
        for (SftpEntry &e : res.entries) {
            if (!e.isLink || stats++ >= kMaxLinkStats)
                continue;
            LIBSSH2_SFTP_ATTRIBUTES a{};
            if (statPath(sftpJoinRemote(canonical, e.name), a, true) == 0 && (a.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)) {
                e.isDir = LIBSSH2_SFTP_S_ISDIR(a.permissions);
                if (!e.isDir && (a.flags & LIBSSH2_SFTP_ATTR_SIZE))
                    e.size = qint64(a.filesize);
            }
            checkpoint();
        }
        res.ok = true;
    }

    bool removeTree(const QString &dir, int depth, SftpResult &res, quint64 id)
    {
        if (depth > kMaxDepth) {
            res.error = QStringLiteral("%1: folders nested too deeply").arg(dir);
            return false;
        }
        QVector<SftpEntry> entries;
        QString error;
        if (!listDir(dir, entries, error, id)) {
            res.error = error;
            return false;
        }
        for (const SftpEntry &e : std::as_const(entries)) {
            if (cancelled(id))
                return false;
            const QString child = sftpJoinRemote(dir, e.name);
            if (e.isDir && !e.isLink) {
                if (!removeTree(child, depth + 1, res, id))
                    return false;
                continue;
            }
            const QByteArray p = child.toUtf8();
            const qint64 rc = call([&] { return libssh2_sftp_unlink_ex(m_sftp, p.constData(), unsigned(p.size())); });
            if (rc != 0) {
                res.error = QStringLiteral("Cannot delete %1: %2").arg(child, err(rc));
                return false;
            }
            ++res.files;
            checkpoint();
        }
        const QByteArray p = dir.toUtf8();
        const qint64 rc = call([&] { return libssh2_sftp_rmdir_ex(m_sftp, p.constData(), unsigned(p.size())); });
        if (rc != 0) {
            res.error = QStringLiteral("Cannot delete folder %1: %2").arg(dir, err(rc));
            return false;
        }
        return true;
    }

    void doRemove(const SftpRequest &req, SftpResult &res)
    {
        if (req.path.isEmpty() || req.path == QLatin1String("/")) {
            res.error = QStringLiteral("Refusing to delete \"%1\"").arg(req.path);
            return;
        }
        LIBSSH2_SFTP_ATTRIBUTES a{};
        const qint64 rc = statPath(req.path, a, false);
        if (rc != 0) {
            res.error = QStringLiteral("%1: %2").arg(req.path, err(rc));
            return;
        }
        const bool isDir = (a.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISDIR(a.permissions);
        if (isDir) {
            res.ok = removeTree(req.path, 0, res, req.id);
            return;
        }
        const QByteArray p = req.path.toUtf8();
        const qint64 u = call([&] { return libssh2_sftp_unlink_ex(m_sftp, p.constData(), unsigned(p.size())); });
        res.ok = u == 0;
        if (res.ok)
            res.files = 1;
        else
            res.error = QStringLiteral("Cannot delete %1: %2").arg(req.path, err(u));
    }

    // ---- transfers -----------------------------------------------------------
    void startProgress(quint64 id)
    {
        m_prog = SftpProgress{};
        m_prog.id = id;
        m_progTimer.start();
        m_progStart.start();
        m_lastSent.invalidate();
    }

    void reportProgress(bool force = false)
    {
        if (!force && m_lastSent.isValid() && m_lastSent.elapsed() < kProgressMs)
            return;
        const qint64 ms = std::max<qint64>(1, m_progStart.elapsed());
        m_prog.bytesPerSecond = double(m_prog.done) * 1000.0 / double(ms);
        if (m_progress)
            m_progress(m_prog);
        m_lastSent.restart();
    }

    bool collectRemoteTree(const QString &root, QVector<TreeItem> &items, SftpResult &res, quint64 id)
    {
        struct Pending {
            QString remote;
            QString local;
            int depth;
        };
        std::vector<Pending> stack{{root, QString(), 0}};
        while (!stack.empty()) {
            const Pending cur = stack.back();
            stack.pop_back();
            if (cur.depth > kMaxDepth) {
                res.skipped << QStringLiteral("%1: nested too deeply").arg(cur.remote);
                continue;
            }
            QVector<SftpEntry> entries;
            QString error;
            if (!listDir(cur.remote, entries, error, id)) {
                if (cancelled(id))
                    return false;
                res.skipped << error;
                continue;
            }
            for (const SftpEntry &e : std::as_const(entries)) {
                QString safe;
                if (!sftpSafeLocalName(e.name, safe)) {
                    res.skipped << QStringLiteral("%1: unsafe file name").arg(sftpJoinRemote(cur.remote, e.name));
                    continue;
                }
                TreeItem it;
                it.remote = sftpJoinRemote(cur.remote, e.name);
                it.local = cur.local.isEmpty() ? safe : cur.local + QLatin1Char('/') + safe;
                if (e.isLink) {
                    res.skipped << QStringLiteral("%1: symbolic link skipped").arg(it.remote);
                    continue;
                }
                if (e.isDir) {
                    it.isDir = true;
                    items << it;
                    stack.push_back({it.remote, it.local, cur.depth + 1});
                } else if ((e.permissions & LIBSSH2_SFTP_S_IFMT) == LIBSSH2_SFTP_S_IFREG || e.permissions == 0) {
                    it.size = std::max<qint64>(0, e.size);
                    it.mtime = e.mtime;
                    it.mode = e.permissions;
                    items << it;
                } else {
                    res.skipped << QStringLiteral("%1: not a regular file").arg(it.remote);
                }
                if (items.size() > kMaxTreeItems) {
                    res.error = QStringLiteral("More than %1 items; transfer smaller folders").arg(kMaxTreeItems);
                    return false;
                }
            }
            if (cancelled(id))
                return false;
        }
        return true;
    }

    // 1 = copied, 0 = skipped (reason added), -1 = failed / cancelled (res.error)
    int downloadFile(const QString &remote, const QString &local, qint64 mtime, const SftpRequest &req, SftpResult &res)
    {
        if (QFileInfo::exists(local) && !req.overwrite) {
            res.skipped << QStringLiteral("%1: already exists").arg(QDir::toNativeSeparators(local));
            return 0;
        }
        const QByteArray p = remote.toUtf8();
        LIBSSH2_SFTP_HANDLE *h = callPtr<LIBSSH2_SFTP_HANDLE>([&] {
            return libssh2_sftp_open_ex(m_sftp, p.constData(), unsigned(p.size()), LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE);
        });
        if (!h) {
            res.error = QStringLiteral("Cannot open %1: %2").arg(remote, err(m_lastRc));
            return -1;
        }
        const QString partPath = local + QLatin1String(kPartSuffix);
        QFile part(partPath);
        if (!part.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            res.error = QStringLiteral("Cannot write %1: %2").arg(QDir::toNativeSeparators(local), part.errorString());
            closeHandle(h);
            return -1;
        }
        m_prog.currentFile = remote;
        bool ok = true;
        if (m_buffer.size() < qsizetype(kReadBuffer))
            m_buffer.resize(qsizetype(kReadBuffer));
        for (;;) {
            if (cancelled(req.id)) {
                ok = false;
                break;
            }
            const qint64 n = call([&] { return libssh2_sftp_read(h, m_buffer.data(), size_t(m_buffer.size())); });
            if (n == 0)
                break;
            if (n < 0) {
                res.error = QStringLiteral("Download of %1 failed: %2").arg(remote, err(n));
                ok = false;
                break;
            }
            if (part.write(m_buffer.constData(), n) != n) {
                res.error = QStringLiteral("Cannot write %1: %2").arg(QDir::toNativeSeparators(local), part.errorString());
                ok = false;
                break;
            }
            m_prog.done += n;
            res.bytes += n;
            reportProgress();
            checkpoint();
        }
        closeHandle(h);
        if (ok && mtime > 0)
            part.setFileTime(QDateTime::fromSecsSinceEpoch(mtime), QFileDevice::FileModificationTime);
        part.close();
        if (ok && part.error() != QFileDevice::NoError) {
            res.error = QStringLiteral("Cannot write %1: %2").arg(QDir::toNativeSeparators(local), part.errorString());
            ok = false;
        }
        if (!ok) {
            QFile::remove(partPath);
            return -1;
        }
        if (QFileInfo::exists(local) && !QFile::remove(local)) {
            res.error = QStringLiteral("Cannot replace %1").arg(QDir::toNativeSeparators(local));
            QFile::remove(partPath);
            return -1;
        }
        if (!QFile::rename(partPath, local)) {
            res.error = QStringLiteral("Cannot rename %1").arg(QDir::toNativeSeparators(partPath));
            QFile::remove(partPath);
            return -1;
        }
        return 1;
    }

    void doDownload(const SftpRequest &req, SftpResult &res)
    {
        startProgress(req.id);
        LIBSSH2_SFTP_ATTRIBUTES a{};
        const qint64 rc = statPath(req.path, a, true);
        if (rc != 0) {
            res.error = QStringLiteral("%1: %2").arg(req.path, err(rc));
            return;
        }
        const bool isDir = (a.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISDIR(a.permissions);
        if (!isDir) {
            m_prog.total = (a.flags & LIBSSH2_SFTP_ATTR_SIZE) ? qint64(a.filesize) : -1;
            m_prog.filesTotal = 1;
            reportProgress(true);
            QDir().mkpath(QFileInfo(req.localPath).absolutePath());
            const int r = downloadFile(req.path, req.localPath, (a.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) ? qint64(a.mtime) : 0, req, res);
            if (r > 0) {
                res.files = 1;
                m_prog.filesDone = 1;
            }
            res.ok = r >= 0;
            reportProgress(true);
            return;
        }

        m_prog.currentFile = QStringLiteral("Listing %1").arg(req.path);
        reportProgress(true);
        QVector<TreeItem> items;
        if (!collectRemoteTree(req.path, items, res, req.id))
            return;
        qint64 total = 0;
        for (const TreeItem &it : std::as_const(items)) {
            if (!it.isDir) {
                total += it.size;
                ++m_prog.filesTotal;
            }
        }
        m_prog.total = total;
        reportProgress(true);
        const QDir root(req.localPath);
        if (!QDir().mkpath(req.localPath)) {
            res.error = QStringLiteral("Cannot create folder %1").arg(QDir::toNativeSeparators(req.localPath));
            return;
        }
        for (const TreeItem &it : std::as_const(items)) {
            if (cancelled(req.id))
                return;
            const QString local = root.filePath(it.local);
            if (it.isDir) {
                if (!QDir().mkpath(local))
                    res.skipped << QStringLiteral("%1: cannot create folder").arg(QDir::toNativeSeparators(local));
                continue;
            }
            QDir().mkpath(QFileInfo(local).absolutePath());
            const int r = downloadFile(it.remote, local, it.mtime, req, res);
            if (r < 0) {
                if (cancelled(req.id))
                    return;
                res.skipped << res.error;
                res.error.clear();
            } else if (r > 0) {
                ++res.files;
            }
            ++m_prog.filesDone;
            reportProgress();
        }
        res.ok = true;
        reportProgress(true);
    }

    // 1 = copied, 0 = skipped, -1 = failed / cancelled
    int uploadFile(const QString &local, const QString &remote, const SftpRequest &req, SftpResult &res)
    {
        if (!req.overwrite) {
            LIBSSH2_SFTP_ATTRIBUTES a{};
            if (statPath(remote, a, false) == 0) {
                res.skipped << QStringLiteral("%1: already exists on the server").arg(remote);
                return 0;
            }
        }
        QFile in(local);
        if (!in.open(QIODevice::ReadOnly)) {
            res.error = QStringLiteral("Cannot read %1: %2").arg(QDir::toNativeSeparators(local), in.errorString());
            return -1;
        }
        const QFileInfo fi(local);
        long mode = 0644;
#ifndef Q_OS_WIN
        if (fi.permission(QFileDevice::ExeOwner))
            mode = 0755;
#endif
        const QByteArray p = remote.toUtf8();
        LIBSSH2_SFTP_HANDLE *h = callPtr<LIBSSH2_SFTP_HANDLE>([&] {
            return libssh2_sftp_open_ex(m_sftp, p.constData(), unsigned(p.size()),
                                        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC, mode, LIBSSH2_SFTP_OPENFILE);
        });
        if (!h) {
            res.error = QStringLiteral("Cannot create %1: %2").arg(remote, err(m_lastRc));
            return -1;
        }
        m_prog.currentFile = remote;
        bool ok = true;
        if (m_buffer.size() < kWriteChunk)
            m_buffer.resize(kWriteChunk);
        while (ok) {
            if (cancelled(req.id)) {
                ok = false;
                break;
            }
            const qint64 got = in.read(m_buffer.data(), kWriteChunk);
            if (got < 0) {
                res.error = QStringLiteral("Cannot read %1: %2").arg(QDir::toNativeSeparators(local), in.errorString());
                ok = false;
                break;
            }
            if (got == 0)
                break;
            qint64 off = 0;
            while (off < got) {
                const qint64 n = call([&] { return libssh2_sftp_write(h, m_buffer.constData() + off, size_t(got - off)); });
                if (n < 0) {
                    res.error = QStringLiteral("Upload of %1 failed: %2").arg(remote, err(n));
                    ok = false;
                    break;
                }
                off += n;
                m_prog.done += n;
                res.bytes += n;
                reportProgress();
                checkpoint();
                if (cancelled(req.id)) {
                    ok = false;
                    break;
                }
            }
        }
        const bool closed = closeHandle(h);
        if (ok && !closed) {
            res.error = QStringLiteral("Upload of %1 failed while closing the file").arg(remote);
            ok = false;
        }
        if (!ok) {
            if (!m_aborting) {
                // Do not leave a truncated file behind.
                call([&] { return libssh2_sftp_unlink_ex(m_sftp, p.constData(), unsigned(p.size())); });
            }
            return -1;
        }
        // Keep the modification time.
        LIBSSH2_SFTP_ATTRIBUTES a{};
        a.flags = LIBSSH2_SFTP_ATTR_ACMODTIME;
        a.mtime = (unsigned long)std::max<qint64>(0, fi.lastModified().toSecsSinceEpoch());
        a.atime = a.mtime;
        call([&] { return libssh2_sftp_stat_ex(m_sftp, p.constData(), unsigned(p.size()), LIBSSH2_SFTP_SETSTAT, &a); });
        return 1;
    }

    bool ensureRemoteDir(const QString &path, SftpResult &res)
    {
        const QByteArray p = path.toUtf8();
        const qint64 rc = call([&] { return libssh2_sftp_mkdir_ex(m_sftp, p.constData(), unsigned(p.size()), 0755); });
        if (rc == 0)
            return true;
        LIBSSH2_SFTP_ATTRIBUTES a{};
        if (statPath(path, a, true) == 0 && (a.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISDIR(a.permissions))
            return true;
        res.skipped << QStringLiteral("%1: cannot create folder (%2)").arg(path, err(rc));
        return false;
    }

    void doUpload(const SftpRequest &req, SftpResult &res)
    {
        startProgress(req.id);
        const QFileInfo fi(req.localPath);
        if (!fi.exists()) {
            res.error = QStringLiteral("%1 does not exist").arg(QDir::toNativeSeparators(req.localPath));
            return;
        }
        if (!fi.isDir()) {
            m_prog.total = fi.size();
            m_prog.filesTotal = 1;
            reportProgress(true);
            const int r = uploadFile(req.localPath, req.path, req, res);
            if (r > 0) {
                res.files = 1;
                m_prog.filesDone = 1;
            }
            res.ok = r >= 0;
            reportProgress(true);
            return;
        }

        struct Item {
            QString local;
            QString remote;
            bool isDir;
            qint64 size;
        };
        QVector<Item> items;
        const QDir root(req.localPath);
        QDirIterator walk(req.localPath, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                          QDirIterator::Subdirectories);
        qint64 total = 0;
        while (walk.hasNext()) {
            const QString path = walk.next();
            const QFileInfo info = walk.fileInfo();
            const QString rel = root.relativeFilePath(path);
            if (info.isSymLink()) {
                res.skipped << QStringLiteral("%1: symbolic link skipped").arg(QDir::toNativeSeparators(path));
                continue;
            }
            if (rel.startsWith(QLatin1String("..")) || rel.contains(QLatin1Char('\\')))
                continue;
            Item it{path, sftpJoinRemote(req.path, rel), info.isDir(), info.isDir() ? 0 : info.size()};
            if (!it.isDir) {
                if (!info.isFile()) {
                    res.skipped << QStringLiteral("%1: not a regular file").arg(QDir::toNativeSeparators(path));
                    continue;
                }
                total += it.size;
                ++m_prog.filesTotal;
            }
            items << it;
            if (items.size() > kMaxTreeItems) {
                res.error = QStringLiteral("More than %1 items; transfer smaller folders").arg(kMaxTreeItems);
                return;
            }
        }
        // Parents before children.
        std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
            return a.remote.count(QLatin1Char('/')) < b.remote.count(QLatin1Char('/'));
        });
        m_prog.total = total;
        reportProgress(true);
        if (!ensureRemoteDir(req.path, res)) {
            res.error = res.skipped.isEmpty() ? QStringLiteral("Cannot create %1").arg(req.path) : res.skipped.takeLast();
            return;
        }
        for (const Item &it : std::as_const(items)) {
            if (cancelled(req.id))
                return;
            if (it.isDir) {
                ensureRemoteDir(it.remote, res);
                continue;
            }
            const int r = uploadFile(it.local, it.remote, req, res);
            if (r < 0) {
                if (cancelled(req.id))
                    return;
                res.skipped << res.error;
                res.error.clear();
            } else if (r > 0) {
                ++res.files;
            }
            ++m_prog.filesDone;
            reportProgress();
        }
        res.ok = true;
        reportProgress(true);
    }

    Context &m_ctx;
    ResultFn &m_result;
    ProgressFn &m_progress;
    CancelledFn &m_cancelled;

    std::thread m_thread;
    std::mutex m_mx;
    std::condition_variable m_cv;
    bool m_laneTurn = false;      // guarded by m_mx
    bool m_started = false;       // I/O thread only
    bool m_exited = false;        // written by the lane while holding the baton
    bool m_aborting = false;      // written by the I/O thread while holding the baton
    QString m_abortReason;
    bool m_moved = false;

    // Only touched by whoever holds the baton.
    std::deque<SftpRequest> m_queue;
    bool m_busy = false;
    LIBSSH2_SFTP *m_sftp = nullptr;
    int m_lastRc = 0;
    QElapsedTimer m_slice;
    QByteArray m_buffer;
    SftpProgress m_prog;
    QElapsedTimer m_progTimer;
    QElapsedTimer m_progStart;
    QElapsedTimer m_lastSent;
};

// ============================================================== engine

SftpEngine::SftpEngine(Context &ctx, ResultFn result, ProgressFn progress, CancelledFn cancelled)
    : m_resultFn(std::move(result)), m_progressFn(std::move(progress)), m_cancelledFn(std::move(cancelled))
{
    m_browse = std::make_unique<Lane>(ctx, m_resultFn, m_progressFn, m_cancelledFn);
    m_transfer = std::make_unique<Lane>(ctx, m_resultFn, m_progressFn, m_cancelledFn);
}

SftpEngine::~SftpEngine()
{
    abort(QStringLiteral("Disconnected"));
}

void SftpEngine::submit(const SftpRequest &req)
{
    if (req.op == SftpOp::Download || req.op == SftpOp::Upload)
        m_transfer->submit(req);
    else
        m_browse->submit(req);
}

bool SftpEngine::step()
{
    bool progress = false;
    if (m_browse->wantsTurn())
        progress |= m_browse->resume();
    if (m_transfer->wantsTurn())
        progress |= m_transfer->resume();
    return progress;
}

bool SftpEngine::busy() const
{
    return m_browse->busy() || m_transfer->busy();
}

bool SftpEngine::hasBufferedData() const
{
    return (m_browse->busy() && m_browse->hasBufferedData()) || (m_transfer->busy() && m_transfer->hasBufferedData());
}

void SftpEngine::abort(const QString &why)
{
    m_browse->abort(why);
    m_transfer->abort(why);
}

} // namespace termx::ssh
