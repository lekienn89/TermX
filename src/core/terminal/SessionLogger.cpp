// SPDX-License-Identifier: GPL-3.0-or-later
#include "SessionLogger.h"

#include <QDateTime>
#include <QDir>
#include <QRegularExpression>

namespace termx {

SessionLogger::SessionLogger(QObject *parent) : QObject(parent)
{
    m_flush.setInterval(1000);
    connect(&m_flush, &QTimer::timeout, this, [this] {
        if (m_file.isOpen() && !m_buf.isEmpty()) {
            m_file.write(m_buf);
            m_buf.clear();
            m_file.flush();
        }
    });
}

SessionLogger::~SessionLogger()
{
    stop();
}

QString SessionLogger::defaultPath(const QString &dir, const QString &sessionName)
{
    QString safe = sessionName;
    safe.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|\s]+)")), QStringLiteral("_"));
    if (safe.isEmpty())
        safe = QStringLiteral("session");
    return QDir(dir).filePath(QStringLiteral("%1_%2.log").arg(safe.left(80),
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
}

bool SessionLogger::start(const QString &path, QString *error)
{
    stop();
    QDir().mkpath(QFileInfo(path).absolutePath());
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        if (error)
            *error = m_file.errorString();
        return false;
    }
    m_strip = AnsiStripper();
    writeMarker(QStringLiteral("Log started"));
    m_flush.start();
    return true;
}

void SessionLogger::stop()
{
    if (!m_file.isOpen())
        return;
    writeMarker(QStringLiteral("Log stopped"));
    m_file.write(m_buf);
    m_buf.clear();
    m_file.close();
    m_flush.stop();
}

void SessionLogger::write(const QByteArray &raw)
{
    if (!m_file.isOpen())
        return;
    m_strip.feed(raw.constData(), raw.size(), m_buf);
    if (m_buf.size() > 256 * 1024) {
        m_file.write(m_buf);
        m_buf.clear();
    }
}

void SessionLogger::writeMarker(const QString &text)
{
    if (!m_file.isOpen())
        return;
    if (!m_buf.isEmpty() && !m_buf.endsWith('\n'))
        m_buf.append('\n');
    m_buf.append(QStringLiteral("===== %1 %2 =====\n")
                     .arg(text, QDateTime::currentDateTime().toString(Qt::ISODate))
                     .toUtf8());
}

} // namespace termx
