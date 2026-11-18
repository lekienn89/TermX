// SPDX-License-Identifier: GPL-3.0-or-later
// Command history in SQLite (WAL + FTS5 trigram index). Writes go through a
// background thread in batches; suggestions come from an in-memory cache so
// typing never waits for the disk.
#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

struct sqlite3;

namespace termx {

struct HistoryEntry {
    qint64 id = 0;
    QString text;
    QString host;
    QString sessionType;
    qint64 firstUsed = 0;   // unix seconds
    qint64 lastUsed = 0;
    int useCount = 0;
    bool pinned = false;
};

class HistoryStore : public QObject {
    Q_OBJECT
public:
    explicit HistoryStore(QObject *parent = nullptr);
    ~HistoryStore() override;

    bool open(const QString &path, QString *error = nullptr);
    void close();
    bool isOpen() const { return m_read != nullptr; }
    bool hasFullText() const { return m_fts; }

    void add(const QString &text, const QString &host, const QString &sessionType);
    void remove(qint64 id);
    void setPinned(qint64 id, bool pinned);
    void clearHost(const QString &host);
    void clearAll();

    // Prefix suggestions ranked by frecency, same-host commands first.
    QVector<HistoryEntry> suggest(const QString &prefix, const QString &host, int limit = 8) const;
    // Full-text search for the history panel (empty query = most recent).
    QVector<HistoryEntry> search(const QString &query, const QString &host = QString(), int limit = 500) const;
    QStringList hosts() const;

    bool exportCsv(const QString &path, QString *error = nullptr) const;
    int importCsv(const QString &path, QString *error = nullptr);

    // Blocks until queued writes are done (tests / shutdown).
    void flush();

    static double frecency(int useCount, qint64 lastUsed, qint64 now);

Q_SIGNALS:
    void changed();

private:
    void post(std::function<void(sqlite3 *)> job);
    void writerLoop();
    void loadCache();

    struct CacheEntry {
        QString text;
        QString host;
        qint64 lastUsed;
        int useCount;
        bool pinned;
    };

    sqlite3 *m_read = nullptr;
    sqlite3 *m_write = nullptr;
    bool m_fts = false;

    std::thread m_writer;
    std::mutex m_mx;
    std::condition_variable m_cv;
    std::condition_variable m_idleCv;
    std::deque<std::function<void(sqlite3 *)>> m_jobs;
    bool m_busy = false;
    bool m_stop = false;

    std::vector<CacheEntry> m_cache;   // GUI thread only
    static constexpr size_t kCacheSize = 20000;
};

} // namespace termx
