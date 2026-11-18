// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "SessionConfig.h"

#include <QObject>
#include <QVector>

namespace termx {

class SessionStore : public QObject {
    Q_OBJECT
public:
    explicit SessionStore(const QString &path, QObject *parent = nullptr);

    bool load(QString *warning = nullptr);
    bool save(QString *error = nullptr) const;

    const QVector<SessionConfig> &sessions() const { return m_sessions; }
    QStringList folders() const;
    const SessionConfig *find(const QString &id) const;

    QString upsert(SessionConfig cfg);     // returns the id (generated if empty)
    bool remove(const QString &id);
    void addFolder(const QString &folder);
    void renameFolder(const QString &from, const QString &to);
    void removeFolder(const QString &folder);   // sessions move to the parent folder

Q_SIGNALS:
    void changed();

private:
    QString m_path;
    QVector<SessionConfig> m_sessions;
    QStringList m_emptyFolders;
};

} // namespace termx
