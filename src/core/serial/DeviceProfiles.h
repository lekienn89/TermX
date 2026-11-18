// SPDX-License-Identifier: GPL-3.0-or-later
// Remembers line settings per USB-serial adapter (keyed by its USB serial
// number or VID:PID), so a cable keeps its baud rate even when Windows gives
// it a different COM number.
#pragma once

#include "session/SessionConfig.h"

#include <QHash>
#include <QString>

namespace termx {

class DeviceProfiles {
public:
    explicit DeviceProfiles(const QString &path);
    void load();
    bool save() const;

    bool contains(const QString &deviceKey) const { return m_map.contains(deviceKey); }
    // Returns remembered options, or defaults (9600 8N1, no flow control).
    SerialOptions optionsFor(const QString &deviceKey, const QString &portName) const;
    QString nameFor(const QString &deviceKey) const;
    void remember(const QString &deviceKey, const SerialOptions &opts, const QString &name = QString());
    bool autoOpen(const QString &deviceKey) const;
    void setAutoOpen(const QString &deviceKey, bool on);

private:
    struct Profile { SerialOptions opts; QString name; bool autoOpen = false; };
    QString m_path;
    QHash<QString, Profile> m_map;
};

} // namespace termx
