// SPDX-License-Identifier: GPL-3.0-or-later
#include "SnippetStore.h"

#include "util/JsonFile.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include <algorithm>

namespace termx {

SnippetStore::SnippetStore(const QString &path, QObject *parent) : QObject(parent), m_path(path) {}

bool SnippetStore::load()
{
    m_items.clear();
    const QJsonDocument doc = JsonFile::load(m_path);
    const QJsonArray arr = doc.object().value(QStringLiteral("snippets")).toArray();
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        Snippet s;
        s.id = o.value(QStringLiteral("id")).toString();
        s.name = o.value(QStringLiteral("name")).toString().left(200);
        s.text = o.value(QStringLiteral("text")).toString().left(1 << 20);
        s.group = o.value(QStringLiteral("group")).toString().left(200);
        if (s.id.isEmpty() || s.name.isEmpty())
            continue;
        m_items.push_back(s);
    }
    return !doc.isNull();
}

bool SnippetStore::save(QString *error) const
{
    QJsonArray arr;
    for (const Snippet &s : m_items) {
        QJsonObject o;
        o[QStringLiteral("id")] = s.id;
        o[QStringLiteral("name")] = s.name;
        o[QStringLiteral("text")] = s.text;
        if (!s.group.isEmpty())
            o[QStringLiteral("group")] = s.group;
        arr.append(o);
    }
    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("snippets")] = arr;
    return JsonFile::save(m_path, QJsonDocument(root), 3, error);
}

const Snippet *SnippetStore::find(const QString &id) const
{
    for (const Snippet &s : m_items)
        if (s.id == id)
            return &s;
    return nullptr;
}

QString SnippetStore::upsert(Snippet s)
{
    if (s.id.isEmpty())
        s.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto it = std::find_if(m_items.begin(), m_items.end(), [&](const Snippet &x) { return x.id == s.id; });
    if (it != m_items.end())
        *it = s;
    else
        m_items.push_back(s);
    std::stable_sort(m_items.begin(), m_items.end(), [](const Snippet &a, const Snippet &b) {
        const int g = QString::compare(a.group, b.group, Qt::CaseInsensitive);
        return g != 0 ? g < 0 : QString::compare(a.name, b.name, Qt::CaseInsensitive) < 0;
    });
    save();
    emit changed();
    return s.id;
}

bool SnippetStore::remove(const QString &id)
{
    const auto before = m_items.size();
    m_items.erase(std::remove_if(m_items.begin(), m_items.end(), [&](const Snippet &x) { return x.id == id; }),
                  m_items.end());
    if (m_items.size() == before)
        return false;
    save();
    emit changed();
    return true;
}

} // namespace termx
