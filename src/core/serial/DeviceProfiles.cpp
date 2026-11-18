// SPDX-License-Identifier: GPL-3.0-or-later
#include "DeviceProfiles.h"
#include "util/JsonFile.h"

#include <QJsonObject>

namespace termx {

DeviceProfiles::DeviceProfiles(const QString &path) : m_path(path) {}

void DeviceProfiles::load()
{
    m_map.clear();
    const QJsonObject root = JsonFile::load(m_path).object();
    const QJsonObject devices = root.value(QStringLiteral("devices")).toObject();
    for (auto it = devices.begin(); it != devices.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        Profile p;
        SessionConfig tmp = SessionConfig::fromJson(QJsonObject{{QStringLiteral("serial"), o.value(QStringLiteral("serial"))}});
        p.opts = tmp.serial;
        p.name = o.value(QStringLiteral("name")).toString();
        p.autoOpen = o.value(QStringLiteral("autoOpen")).toBool(false);
        m_map.insert(it.key(), p);
    }
}

bool DeviceProfiles::save() const
{
    QJsonObject devices;
    for (auto it = m_map.begin(); it != m_map.end(); ++it) {
        SessionConfig tmp;
        tmp.serial = it.value().opts;
        QJsonObject o;
        o[QStringLiteral("serial")] = tmp.toJson().value(QStringLiteral("serial"));
        o[QStringLiteral("name")] = it.value().name;
        o[QStringLiteral("autoOpen")] = it.value().autoOpen;
        devices[it.key()] = o;
    }
    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("devices")] = devices;
    return JsonFile::save(m_path, QJsonDocument(root), 2);
}

SerialOptions DeviceProfiles::optionsFor(const QString &deviceKey, const QString &portName) const
{
    SerialOptions o;
    auto it = m_map.find(deviceKey);
    if (it != m_map.end())
        o = it.value().opts;
    o.portName = portName;
    o.deviceKey = deviceKey;
    return o;
}

QString DeviceProfiles::nameFor(const QString &deviceKey) const
{
    return m_map.value(deviceKey).name;
}

void DeviceProfiles::remember(const QString &deviceKey, const SerialOptions &opts, const QString &name)
{
    if (deviceKey.isEmpty())
        return;
    Profile &p = m_map[deviceKey];
    p.opts = opts;
    if (!name.isEmpty())
        p.name = name;
    save();
}

bool DeviceProfiles::autoOpen(const QString &deviceKey) const
{
    return m_map.value(deviceKey).autoOpen;
}

void DeviceProfiles::setAutoOpen(const QString &deviceKey, bool on)
{
    m_map[deviceKey].autoOpen = on;
    save();
}

} // namespace termx
