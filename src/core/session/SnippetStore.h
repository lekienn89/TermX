// SPDX-License-Identifier: GPL-3.0-or-later
// Saved command snippets ("show run | i interface", multi-line config blocks...).
#pragma once

#include <QObject>
#include <QString>
#include <QVector>

namespace termx {

struct Snippet {
    QString id;
    QString name;
    QString text;       // may hold several lines; each line is sent followed by Enter
    QString group;      // optional, e.g. "Cisco"
};

class SnippetStore : public QObject {
    Q_OBJECT
public:
    explicit SnippetStore(const QString &path, QObject *parent = nullptr);

    bool load();
    bool save(QString *error = nullptr) const;

    const QVector<Snippet> &snippets() const { return m_items; }
    const Snippet *find(const QString &id) const;
    QString upsert(Snippet s);      // returns the id (generated if empty)
    bool remove(const QString &id);

Q_SIGNALS:
    void changed();

private:
    QString m_path;
    QVector<Snippet> m_items;
};

} // namespace termx
