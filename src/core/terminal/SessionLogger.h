// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "AnsiStripper.h"

#include <QFile>
#include <QObject>
#include <QTimer>

namespace termx {

// Writes a plain-text transcript of a session (escape sequences removed).
class SessionLogger : public QObject {
    Q_OBJECT
public:
    explicit SessionLogger(QObject *parent = nullptr);
    ~SessionLogger() override;

    bool start(const QString &path, QString *error = nullptr);
    void stop();
    bool isActive() const { return m_file.isOpen(); }
    QString path() const { return m_file.fileName(); }
    void write(const QByteArray &raw);
    void writeMarker(const QString &text);

    static QString defaultPath(const QString &dir, const QString &sessionName);

private:
    QFile m_file;
    AnsiStripper m_strip;
    QByteArray m_buf;
    QTimer m_flush;
};

} // namespace termx
