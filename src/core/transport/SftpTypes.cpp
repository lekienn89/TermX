// SPDX-License-Identifier: GPL-3.0-or-later
#include "SftpTypes.h"

#include <QStringList>

namespace termx {

QString sftpOpName(SftpOp op)
{
    switch (op) {
    case SftpOp::Home: return QStringLiteral("home");
    case SftpOp::List: return QStringLiteral("list");
    case SftpOp::Mkdir: return QStringLiteral("mkdir");
    case SftpOp::Rename: return QStringLiteral("rename");
    case SftpOp::Remove: return QStringLiteral("delete");
    case SftpOp::Chmod: return QStringLiteral("chmod");
    case SftpOp::Download: return QStringLiteral("download");
    case SftpOp::Upload: return QStringLiteral("upload");
    }
    return QString();
}

bool sftpSafeLocalName(const QString &remoteName, QString &out)
{
    if (remoteName.isEmpty() || remoteName == QLatin1String(".") || remoteName == QLatin1String("..")
        || remoteName.contains(QLatin1Char('/')) || remoteName.contains(QChar(0)))
        return false;
    QString s;
    s.reserve(remoteName.size());
    for (const QChar ch : remoteName) {
        const ushort u = ch.unicode();
        if (u < 0x20 || ch == QLatin1Char('\\') || ch == QLatin1Char(':') || ch == QLatin1Char('*') || ch == QLatin1Char('?')
            || ch == QLatin1Char('"') || ch == QLatin1Char('<') || ch == QLatin1Char('>') || ch == QLatin1Char('|'))
            s += QLatin1Char('_');
        else
            s += ch;
    }
    // Windows drops trailing dots and spaces, which could merge names.
    while (!s.isEmpty() && (s.endsWith(QLatin1Char('.')) || s.endsWith(QLatin1Char(' '))))
        s.chop(1);
    if (s.isEmpty())
        s = QStringLiteral("_");
    static const QStringList reserved = {QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"),
                                         QStringLiteral("NUL"), QStringLiteral("CONIN$"), QStringLiteral("CONOUT$")};
    const QString base = s.section(QLatin1Char('.'), 0, 0).toUpper();
    const bool device = reserved.contains(base)
                        || (base.size() == 4 && (base.startsWith(QLatin1String("COM")) || base.startsWith(QLatin1String("LPT")))
                            && base.at(3).isDigit());
    if (device)
        s.prepend(QLatin1Char('_'));
    out = s;
    return true;
}

QString sftpJoinRemote(const QString &dir, const QString &name)
{
    if (dir.isEmpty())
        return name;
    if (dir.endsWith(QLatin1Char('/')))
        return dir + name;
    return dir + QLatin1Char('/') + name;
}

} // namespace termx
