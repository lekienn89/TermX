// SPDX-License-Identifier: GPL-3.0-or-later
#include "HistoryStore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTextStream>

#include <sqlite3.h>

#include <algorithm>
#include <cmath>

namespace termx {

namespace {

bool exec(sqlite3 *db, const char *sql)
{
    char *err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (err)
        sqlite3_free(err);
    return rc == SQLITE_OK;
}

class Stmt {
public:
    Stmt(sqlite3 *db, const char *sql) { sqlite3_prepare_v2(db, sql, -1, &m_s, nullptr); }
    ~Stmt() { sqlite3_finalize(m_s); }
    Stmt(const Stmt &) = delete;
    Stmt &operator=(const Stmt &) = delete;
    bool ok() const { return m_s != nullptr; }
    void bind(int i, const QString &v)
    {
        const QByteArray b = v.toUtf8();
        sqlite3_bind_text(m_s, i, b.constData(), int(b.size()), SQLITE_TRANSIENT);
    }
    void bind(int i, qint64 v) { sqlite3_bind_int64(m_s, i, v); }
    int step() { return m_s ? sqlite3_step(m_s) : SQLITE_ERROR; }
    void reset() { sqlite3_reset(m_s); sqlite3_clear_bindings(m_s); }
    QString text(int col) const
    {
        const auto *p = reinterpret_cast<const char *>(sqlite3_column_text(m_s, col));
        return p ? QString::fromUtf8(p, sqlite3_column_bytes(m_s, col)) : QString();
    }
    qint64 i64(int col) const { return sqlite3_column_int64(m_s, col); }

private:
    sqlite3_stmt *m_s = nullptr;
};

const char *kSchema =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA synchronous=NORMAL;"
    "CREATE TABLE IF NOT EXISTS commands("
    "  id INTEGER PRIMARY KEY,"
    "  text TEXT NOT NULL,"
    "  host TEXT NOT NULL DEFAULT '',"
    "  session_type TEXT NOT NULL DEFAULT '',"
    "  first_used INTEGER NOT NULL,"
    "  last_used INTEGER NOT NULL,"
    "  use_count INTEGER NOT NULL DEFAULT 1,"
    "  pinned INTEGER NOT NULL DEFAULT 0,"
    "  UNIQUE(text, host));"
    "CREATE INDEX IF NOT EXISTS idx_commands_last ON commands(last_used DESC);"
    "CREATE INDEX IF NOT EXISTS idx_commands_host ON commands(host);";

const char *kFtsSchema =
    "CREATE VIRTUAL TABLE IF NOT EXISTS commands_fts USING fts5("
    "  text, content='commands', content_rowid='id', tokenize='trigram');"
    "CREATE TRIGGER IF NOT EXISTS commands_ai AFTER INSERT ON commands BEGIN"
    "  INSERT INTO commands_fts(rowid, text) VALUES (new.id, new.text); END;"
    "CREATE TRIGGER IF NOT EXISTS commands_ad AFTER DELETE ON commands BEGIN"
    "  INSERT INTO commands_fts(commands_fts, rowid, text) VALUES('delete', old.id, old.text); END;"
    "CREATE TRIGGER IF NOT EXISTS commands_au AFTER UPDATE OF text ON commands BEGIN"
    "  INSERT INTO commands_fts(commands_fts, rowid, text) VALUES('delete', old.id, old.text);"
    "  INSERT INTO commands_fts(rowid, text) VALUES (new.id, new.text); END;";

QString csvField(const QString &s)
{
    QString v = s;
    v.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + v + QLatin1Char('"');
}

QStringList parseCsvLine(const QString &line)
{
    QStringList out;
    QString cur;
    bool inQuotes = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (inQuotes) {
            if (c == QLatin1Char('"')) {
                if (i + 1 < line.size() && line.at(i + 1) == QLatin1Char('"')) {
                    cur += c;
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                cur += c;
            }
        } else if (c == QLatin1Char('"')) {
            inQuotes = true;
        } else if (c == QLatin1Char(',')) {
            out << cur;
            cur.clear();
        } else {
            cur += c;
        }
    }
    out << cur;
    return out;
}

} // namespace

HistoryStore::HistoryStore(QObject *parent) : QObject(parent) {}

HistoryStore::~HistoryStore()
{
    close();
}

bool HistoryStore::open(const QString &path, QString *error)
{
    close();
    QDir().mkpath(QFileInfo(path).absolutePath());
    const QByteArray p = QFile::encodeName(path);
    const QByteArray utf8 = path.toUtf8();
    auto openDb = [&](sqlite3 **db) {
        const int rc = sqlite3_open_v2(utf8.constData(), db,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
        if (rc != SQLITE_OK) {
            if (error)
                *error = QString::fromUtf8(*db ? sqlite3_errmsg(*db) : "cannot open database");
            sqlite3_close(*db);
            *db = nullptr;
            return false;
        }
        sqlite3_busy_timeout(*db, 3000);
        return true;
    };
    Q_UNUSED(p);
    if (!openDb(&m_write))
        return false;
    if (!exec(m_write, kSchema)) {
        // Corrupt file: move it aside and start fresh rather than crash later.
        if (error)
            *error = QString::fromUtf8(sqlite3_errmsg(m_write));
        sqlite3_close(m_write);
        m_write = nullptr;
        const QString broken = path + QStringLiteral(".broken-") + QString::number(QDateTime::currentSecsSinceEpoch());
        QFile::rename(path, broken);
        if (!openDb(&m_write) || !exec(m_write, kSchema))
            return false;
    }
    m_fts = exec(m_write, kFtsSchema);
    if (!openDb(&m_read)) {
        sqlite3_close(m_write);
        m_write = nullptr;
        return false;
    }
    loadCache();

    m_stop = false;
    m_writer = std::thread([this] { writerLoop(); });
    return true;
}

void HistoryStore::close()
{
    if (m_writer.joinable()) {
        {
            std::lock_guard<std::mutex> lock(m_mx);
            m_stop = true;
        }
        m_cv.notify_all();
        m_writer.join();
    }
    if (m_read) {
        sqlite3_close(m_read);
        m_read = nullptr;
    }
    if (m_write) {
        sqlite3_close(m_write);
        m_write = nullptr;
    }
    m_cache.clear();
}

void HistoryStore::post(std::function<void(sqlite3 *)> job)
{
    if (!m_write)
        return;
    {
        std::lock_guard<std::mutex> lock(m_mx);
        m_jobs.push_back(std::move(job));
    }
    m_cv.notify_all();
}

void HistoryStore::writerLoop()
{
    for (;;) {
        std::deque<std::function<void(sqlite3 *)>> batch;
        {
            std::unique_lock<std::mutex> lock(m_mx);
            m_cv.wait(lock, [this] { return m_stop || !m_jobs.empty(); });
            if (m_jobs.empty() && m_stop)
                return;
            batch.swap(m_jobs);
            m_busy = true;
        }
        exec(m_write, "BEGIN");
        for (auto &job : batch)
            job(m_write);
        exec(m_write, "COMMIT");
        {
            std::lock_guard<std::mutex> lock(m_mx);
            m_busy = false;
        }
        m_idleCv.notify_all();
        QMetaObject::invokeMethod(this, [this] { Q_EMIT changed(); }, Qt::QueuedConnection);
    }
}

void HistoryStore::flush()
{
    std::unique_lock<std::mutex> lock(m_mx);
    m_idleCv.wait_for(lock, std::chrono::seconds(10), [this] { return m_jobs.empty() && !m_busy; });
}

void HistoryStore::loadCache()
{
    m_cache.clear();
    Stmt s(m_read, "SELECT text, host, last_used, use_count, pinned FROM commands ORDER BY last_used DESC LIMIT 20000");
    while (s.step() == SQLITE_ROW)
        m_cache.push_back(CacheEntry{s.text(0), s.text(1), s.i64(2), int(s.i64(3)), s.i64(4) != 0});
}

void HistoryStore::add(const QString &text, const QString &host, const QString &sessionType)
{
    const QString t = text.trimmed();
    if (t.isEmpty() || !m_write)
        return;
    const qint64 now = QDateTime::currentSecsSinceEpoch();

    auto it = std::find_if(m_cache.begin(), m_cache.end(),
                           [&](const CacheEntry &e) { return e.text == t && e.host == host; });
    if (it != m_cache.end()) {
        it->lastUsed = now;
        ++it->useCount;
    } else {
        m_cache.push_back(CacheEntry{t, host, now, 1, false});
        if (m_cache.size() > kCacheSize) {
            auto oldest = std::min_element(m_cache.begin(), m_cache.end(), [](const CacheEntry &a, const CacheEntry &b) {
                return !a.pinned && (b.pinned || a.lastUsed < b.lastUsed);
            });
            m_cache.erase(oldest);
        }
    }

    post([t, host, sessionType, now](sqlite3 *db) {
        Stmt s(db, "INSERT INTO commands(text, host, session_type, first_used, last_used, use_count) "
                   "VALUES(?1, ?2, ?3, ?4, ?4, 1) "
                   "ON CONFLICT(text, host) DO UPDATE SET last_used = excluded.last_used, use_count = use_count + 1");
        s.bind(1, t);
        s.bind(2, host);
        s.bind(3, sessionType);
        s.bind(4, now);
        s.step();
    });
}

void HistoryStore::remove(qint64 id)
{
    if (!m_read)
        return;
    {
        Stmt s(m_read, "SELECT text, host FROM commands WHERE id = ?1");
        s.bind(1, id);
        if (s.step() == SQLITE_ROW) {
            const QString t = s.text(0), h = s.text(1);
            m_cache.erase(std::remove_if(m_cache.begin(), m_cache.end(),
                                         [&](const CacheEntry &e) { return e.text == t && e.host == h; }),
                          m_cache.end());
        }
    }
    post([id](sqlite3 *db) {
        Stmt s(db, "DELETE FROM commands WHERE id = ?1");
        s.bind(1, id);
        s.step();
    });
}

void HistoryStore::setPinned(qint64 id, bool pinned)
{
    if (!m_read)
        return;
    {
        Stmt s(m_read, "SELECT text, host FROM commands WHERE id = ?1");
        s.bind(1, id);
        if (s.step() == SQLITE_ROW) {
            const QString t = s.text(0), h = s.text(1);
            for (CacheEntry &e : m_cache)
                if (e.text == t && e.host == h)
                    e.pinned = pinned;
        }
    }
    post([id, pinned](sqlite3 *db) {
        Stmt s(db, "UPDATE commands SET pinned = ?2 WHERE id = ?1");
        s.bind(1, id);
        s.bind(2, qint64(pinned ? 1 : 0));
        s.step();
    });
}

void HistoryStore::clearHost(const QString &host)
{
    m_cache.erase(std::remove_if(m_cache.begin(), m_cache.end(), [&](const CacheEntry &e) { return e.host == host; }),
                  m_cache.end());
    post([host](sqlite3 *db) {
        Stmt s(db, "DELETE FROM commands WHERE host = ?1");
        s.bind(1, host);
        s.step();
    });
}

void HistoryStore::clearAll()
{
    m_cache.clear();
    post([](sqlite3 *db) { exec(db, "DELETE FROM commands"); });
}

double HistoryStore::frecency(int useCount, qint64 lastUsed, qint64 now)
{
    const double ageDays = std::max<qint64>(0, now - lastUsed) / 86400.0;
    // Half-life of two weeks; frequent commands keep a long tail.
    return std::log2(1.0 + useCount) * std::pow(0.5, ageDays / 14.0) + 0.05 * std::log2(1.0 + useCount);
}

QVector<HistoryEntry> HistoryStore::suggest(const QString &prefix, const QString &host, int limit) const
{
    QVector<HistoryEntry> out;
    if (prefix.trimmed().isEmpty())
        return out;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    struct Scored { double score; const CacheEntry *e; };
    std::vector<Scored> hits;
    for (const CacheEntry &e : m_cache) {
        if (e.text.size() <= prefix.size() || !e.text.startsWith(prefix))
            continue;
        double s = frecency(e.useCount, e.lastUsed, now);
        if (!host.isEmpty() && e.host == host)
            s = s * 2.0 + 1.0;
        if (e.pinned)
            s += 2.0;
        hits.push_back({s, &e});
    }
    std::sort(hits.begin(), hits.end(), [](const Scored &a, const Scored &b) { return a.score > b.score; });
    QStringList seen;
    for (const Scored &h : hits) {
        if (seen.contains(h.e->text))
            continue;   // same command from several hosts
        seen << h.e->text;
        HistoryEntry he;
        he.text = h.e->text;
        he.host = h.e->host;
        he.lastUsed = h.e->lastUsed;
        he.useCount = h.e->useCount;
        he.pinned = h.e->pinned;
        out.push_back(he);
        if (out.size() >= limit)
            break;
    }
    return out;
}

QVector<HistoryEntry> HistoryStore::search(const QString &query, const QString &host, int limit) const
{
    QVector<HistoryEntry> out;
    if (!m_read)
        return out;
    const QString q = query.trimmed();
    QByteArray sql = "SELECT c.id, c.text, c.host, c.session_type, c.first_used, c.last_used, c.use_count, c.pinned "
                     "FROM commands c ";
    const bool useFts = m_fts && q.size() >= 3;
    if (useFts)
        sql += "JOIN commands_fts f ON f.rowid = c.id WHERE commands_fts MATCH ?1 ";
    else if (!q.isEmpty())
        sql += "WHERE c.text LIKE ?1 ESCAPE '\\' ";
    else
        sql += "WHERE ?1 IS NULL OR 1 ";
    if (!host.isEmpty())
        sql += "AND c.host = ?2 ";
    sql += "ORDER BY c.pinned DESC, c.last_used DESC LIMIT ?3";

    Stmt s(m_read, sql.constData());
    if (!s.ok())
        return out;
    if (useFts) {
        QString phrase = q;
        phrase.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        s.bind(1, QLatin1Char('"') + phrase + QLatin1Char('"'));
    } else if (!q.isEmpty()) {
        QString like = q;
        like.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        like.replace(QLatin1Char('%'), QStringLiteral("\\%"));
        like.replace(QLatin1Char('_'), QStringLiteral("\\_"));
        s.bind(1, QLatin1Char('%') + like + QLatin1Char('%'));
    }
    if (!host.isEmpty())
        s.bind(2, host);
    s.bind(3, qint64(qBound(1, limit, 100000)));
    while (s.step() == SQLITE_ROW) {
        HistoryEntry e;
        e.id = s.i64(0);
        e.text = s.text(1);
        e.host = s.text(2);
        e.sessionType = s.text(3);
        e.firstUsed = s.i64(4);
        e.lastUsed = s.i64(5);
        e.useCount = int(s.i64(6));
        e.pinned = s.i64(7) != 0;
        out.push_back(e);
    }
    return out;
}

QStringList HistoryStore::hosts() const
{
    QStringList out;
    if (!m_read)
        return out;
    Stmt s(m_read, "SELECT DISTINCT host FROM commands WHERE host <> '' ORDER BY host");
    while (s.step() == SQLITE_ROW)
        out << s.text(0);
    return out;
}

bool HistoryStore::exportCsv(const QString &path, QString *error) const
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    QTextStream ts(&f);
    ts << "command,host,type,first_used,last_used,use_count,pinned\n";
    for (const HistoryEntry &e : search(QString(), QString(), 1000000)) {
        ts << csvField(e.text) << ',' << csvField(e.host) << ',' << csvField(e.sessionType) << ','
           << QDateTime::fromSecsSinceEpoch(e.firstUsed).toString(Qt::ISODate) << ','
           << QDateTime::fromSecsSinceEpoch(e.lastUsed).toString(Qt::ISODate) << ',' << e.useCount << ','
           << (e.pinned ? 1 : 0) << '\n';
    }
    ts.flush();
    if (!f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}

int HistoryStore::importCsv(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error)
            *error = f.errorString();
        return -1;
    }
    QTextStream ts(&f);
    int count = 0;
    bool header = true;
    while (!ts.atEnd()) {
        const QString line = ts.readLine();
        if (header) {
            header = false;
            if (line.startsWith(QLatin1String("command,")))
                continue;
        }
        const QStringList fields = parseCsvLine(line);
        if (fields.isEmpty() || fields.at(0).trimmed().isEmpty())
            continue;
        const QString text = fields.value(0).trimmed();
        const QString host = fields.value(1);
        const QString type = fields.value(2);
        qint64 first = QDateTime::fromString(fields.value(3), Qt::ISODate).toSecsSinceEpoch();
        qint64 last = QDateTime::fromString(fields.value(4), Qt::ISODate).toSecsSinceEpoch();
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        if (first <= 0) first = now;
        if (last <= 0) last = first;
        const int uses = qMax(1, fields.value(5).toInt());
        const bool pinned = fields.value(6) == QLatin1String("1");
        post([=](sqlite3 *db) {
            Stmt s(db, "INSERT INTO commands(text, host, session_type, first_used, last_used, use_count, pinned) "
                       "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7) "
                       "ON CONFLICT(text, host) DO UPDATE SET use_count = use_count + excluded.use_count, "
                       "last_used = MAX(last_used, excluded.last_used), pinned = MAX(pinned, excluded.pinned)");
            s.bind(1, text);
            s.bind(2, host);
            s.bind(3, type);
            s.bind(4, first);
            s.bind(5, last);
            s.bind(6, qint64(uses));
            s.bind(7, qint64(pinned ? 1 : 0));
            s.step();
        });
        ++count;
    }
    flush();
    loadCache();
    return count;
}

} // namespace termx
