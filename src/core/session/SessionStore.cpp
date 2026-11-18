// SPDX-License-Identifier: GPL-3.0-or-later
#include "SessionStore.h"
#include "util/JsonFile.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QUuid>

namespace termx {

SessionStore::SessionStore(const QString &path, QObject *parent) : QObject(parent), m_path(path) {}

bool SessionStore::load(QString *warning)
{
    bool recovered = false;
    const QJsonDocument doc = JsonFile::load(m_path, &recovered);
    if (recovered && warning)
        *warning = QStringLiteral("sessions.json was damaged; restored from the latest backup.");
    m_sessions.clear();
    m_emptyFolders.clear();
    const QJsonObject root = doc.object();
    QSet<QString> ids;
    for (const QJsonValue &v : root.value(QStringLiteral("sessions")).toArray()) {
        SessionConfig c = SessionConfig::fromJson(v.toObject());
        if (c.id.isEmpty() || ids.contains(c.id))
            c.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        ids.insert(c.id);
        m_sessions.push_back(c);
    }
    for (const QJsonValue &v : root.value(QStringLiteral("folders")).toArray())
        m_emptyFolders << v.toString();
    Q_EMIT changed();
    return true;
}

bool SessionStore::save(QString *error) const
{
    QJsonArray arr;
    for (const SessionConfig &c : m_sessions)
        arr.append(c.toJson());
    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("sessions")] = arr;
    root[QStringLiteral("folders")] = QJsonArray::fromStringList(m_emptyFolders);
    return JsonFile::save(m_path, QJsonDocument(root), 5, error);
}

QStringList SessionStore::folders() const
{
    QSet<QString> set(m_emptyFolders.begin(), m_emptyFolders.end());
    for (const SessionConfig &c : m_sessions)
        if (!c.folder.isEmpty())
            set.insert(c.folder);
    QStringList out(set.begin(), set.end());
    out.sort(Qt::CaseInsensitive);
    return out;
}

const SessionConfig *SessionStore::find(const QString &id) const
{
    for (const SessionConfig &c : m_sessions)
        if (c.id == id)
            return &c;
    return nullptr;
}

QString SessionStore::upsert(SessionConfig cfg)
{
    if (cfg.id.isEmpty())
        cfg.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool replaced = false;
    for (SessionConfig &c : m_sessions) {
        if (c.id == cfg.id) {
            c = cfg;
            replaced = true;
            break;
        }
    }
    if (!replaced)
        m_sessions.push_back(cfg);
    m_emptyFolders.removeAll(cfg.folder);
    save();
    Q_EMIT changed();
    return cfg.id;
}

bool SessionStore::remove(const QString &id)
{
    for (int i = 0; i < m_sessions.size(); ++i) {
        if (m_sessions[i].id == id) {
            m_sessions.removeAt(i);
            save();
            Q_EMIT changed();
            return true;
        }
    }
    return false;
}

void SessionStore::addFolder(const QString &folder)
{
    const QString f = folder.trimmed();
    if (f.isEmpty() || folders().contains(f))
        return;
    m_emptyFolders << f;
    save();
    Q_EMIT changed();
}

void SessionStore::renameFolder(const QString &from, const QString &to)
{
    const QString target = to.trimmed();
    if (target.isEmpty())
        return;
    for (SessionConfig &c : m_sessions) {
        if (c.folder == from)
            c.folder = target;
        else if (c.folder.startsWith(from + QLatin1Char('/')))
            c.folder = target + c.folder.mid(from.size());
    }
    for (QString &f : m_emptyFolders) {
        if (f == from)
            f = target;
        else if (f.startsWith(from + QLatin1Char('/')))
            f = target + f.mid(from.size());
    }
    save();
    Q_EMIT changed();
}

void SessionStore::removeFolder(const QString &folder)
{
    const int slash = folder.lastIndexOf(QLatin1Char('/'));
    const QString parent = slash > 0 ? folder.left(slash) : QString();
    for (SessionConfig &c : m_sessions) {
        if (c.folder == folder || c.folder.startsWith(folder + QLatin1Char('/')))
            c.folder = parent;
    }
    m_emptyFolders.erase(std::remove_if(m_emptyFolders.begin(), m_emptyFolders.end(),
                                        [&](const QString &f) { return f == folder || f.startsWith(folder + QLatin1Char('/')); }),
                         m_emptyFolders.end());
    save();
    Q_EMIT changed();
}

} // namespace termx
