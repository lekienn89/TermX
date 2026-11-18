// SPDX-License-Identifier: GPL-3.0-or-later
#include "QuickConnect.h"

#include <QRegularExpression>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>

namespace termx {
namespace QuickConnect {

namespace {

bool fail(QString *error, const QString &msg)
{
    if (error)
        *error = msg;
    return false;
}

bool parsePort(const QString &s, int &port)
{
    bool ok = false;
    const int p = s.toInt(&ok);
    if (!ok || p < 1 || p > 65535)
        return false;
    port = p;
    return true;
}

// "user@host", "host:port", "user@[v6]:port", "user@v6" (several colons = no port)
bool parseHostSpec(QString spec, QString &user, QString &host, int &port, QString *error)
{
    const qsizetype at = spec.lastIndexOf(QLatin1Char('@'));
    if (at >= 0) {
        user = spec.left(at);
        spec = spec.mid(at + 1);
    }
    if (spec.startsWith(QLatin1Char('['))) {
        const qsizetype close = spec.indexOf(QLatin1Char(']'));
        if (close < 0)
            return fail(error, QStringLiteral("Missing ']' in IPv6 address"));
        host = spec.mid(1, close - 1);
        const QString rest = spec.mid(close + 1);
        if (rest.startsWith(QLatin1Char(':')) && !parsePort(rest.mid(1), port))
            return fail(error, QStringLiteral("Invalid port: %1").arg(rest.mid(1)));
    } else if (spec.count(QLatin1Char(':')) == 1) {
        const qsizetype colon = spec.indexOf(QLatin1Char(':'));
        host = spec.left(colon);
        if (!parsePort(spec.mid(colon + 1), port))
            return fail(error, QStringLiteral("Invalid port: %1").arg(spec.mid(colon + 1)));
    } else {
        host = spec;   // plain host or bare IPv6
    }
    if (host.isEmpty())
        return fail(error, QStringLiteral("No host given"));
    static const QRegularExpression bad(QStringLiteral(R"([\s/\\?#])"));
    if (bad.match(host).hasMatch())
        return fail(error, QStringLiteral("Invalid host name: %1").arg(host));
    return true;
}

bool looksLikeSerial(const QString &s)
{
    static const QRegularExpression com(QStringLiteral(R"(^(COM\d{1,3}|/dev/tty\S+)$)"),
                                        QRegularExpression::CaseInsensitiveOption);
    return com.match(s).hasMatch();
}

QString normaliseSerial(const QString &s)
{
    return s.startsWith(QLatin1String("/dev/")) ? s : s.toUpper();
}

bool parseSerial(const QStringList &args, SessionConfig &c, QString *error)
{
    // COM7 | COM7 115200 | COM7:115200
    QString port = args.value(0);
    QString baud = args.value(1);
    if (!port.startsWith(QLatin1String("/dev/")) && port.contains(QLatin1Char(':'))) {
        baud = port.section(QLatin1Char(':'), 1);
        port = port.section(QLatin1Char(':'), 0, 0);
    }
    c.type = SessionType::Serial;
    c.serial.portName = normaliseSerial(port);
    if (!baud.isEmpty()) {
        bool ok = false;
        const int b = baud.toInt(&ok);
        if (!ok || b < 50 || b > 12000000)
            return fail(error, QStringLiteral("Invalid baud rate: %1").arg(baud));
        c.serial.baudRate = b;
    }
    return true;
}

// Splits on whitespace, honouring "double quotes" (for key paths with spaces).
QStringList splitArgs(const QString &text)
{
    QStringList out;
    QString cur;
    bool quoted = false, any = false;
    for (QChar ch : text) {
        if (ch == QLatin1Char('"')) {
            quoted = !quoted;
            any = true;
        } else if (ch.isSpace() && !quoted) {
            if (any || !cur.isEmpty())
                out << cur;
            cur.clear();
            any = false;
        } else {
            cur += ch;
        }
    }
    if (any || !cur.isEmpty())
        out << cur;
    return out;
}

bool parseSshCommand(const QStringList &args, SessionConfig &c, QString *error)
{
    c.type = SessionType::Ssh;
    QString target;
    QString user;
    int port = 0;
    for (int i = 0; i < args.size(); ++i) {
        const QString &a = args[i];
        auto value = [&](QString &out) {
            if (a.size() > 2) {   // -p2222
                out = a.mid(2);
                return true;
            }
            if (i + 1 >= args.size())
                return false;
            out = args[++i];
            return true;
        };
        QString v;
        if (a.startsWith(QLatin1String("-p"))) {
            if (!value(v) || !parsePort(v, port))
                return fail(error, QStringLiteral("Invalid port after -p"));
        } else if (a.startsWith(QLatin1String("-l"))) {
            if (!value(v))
                return fail(error, QStringLiteral("Missing user name after -l"));
            user = v;
        } else if (a.startsWith(QLatin1String("-i"))) {
            if (!value(v))
                return fail(error, QStringLiteral("Missing key file after -i"));
            c.ssh.keyFile = v;
        } else if (a.startsWith(QLatin1Char('-'))) {
            // Options that take a value we do not use; skip it so it is not taken as the host.
            static const QString withValue = QStringLiteral("bcDEeFIJLmOoQRSWw");
            if (a.size() == 2 && withValue.contains(a[1]))
                ++i;
        } else if (target.isEmpty()) {
            target = a;
        } else {
            return fail(error, QStringLiteral("Remote commands are not supported: %1").arg(a));
        }
    }
    if (target.isEmpty())
        return fail(error, QStringLiteral("No host given"));
    QString specUser, host;
    int specPort = 22;
    if (!parseHostSpec(target, specUser, host, specPort, error))
        return false;
    c.ssh.host = host;
    c.ssh.username = user.isEmpty() ? specUser : user;
    c.ssh.port = port ? port : specPort;
    return true;
}

bool parseTelnetCommand(const QStringList &args, SessionConfig &c, bool raw, QString *error)
{
    if (args.isEmpty())
        return fail(error, QStringLiteral("No host given"));
    QString user, host;
    int port = raw ? 0 : 23;
    if (!parseHostSpec(args[0], user, host, port, error))
        return false;
    if (args.size() > 1 && !parsePort(args[1], port))
        return fail(error, QStringLiteral("Invalid port: %1").arg(args[1]));
    if (raw && port == 0)
        return fail(error, QStringLiteral("Raw TCP needs a port"));
    c.type = SessionType::Telnet;
    c.telnet.host = host;
    c.telnet.port = port;
    c.telnet.rawTcp = raw;
    return true;
}

bool parseUrl(const QString &text, SessionConfig &c, QString *error)
{
    const QUrl url(text);
    if (!url.isValid())
        return fail(error, QStringLiteral("Invalid address: %1").arg(text));
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("ssh")) {
        if (url.host().isEmpty())
            return fail(error, QStringLiteral("No host given"));
        c.type = SessionType::Ssh;
        c.ssh.host = url.host();
        c.ssh.port = url.port(22);
        c.ssh.username = url.userName();
        return true;
    }
    if (scheme == QLatin1String("telnet") || scheme == QLatin1String("raw")) {
        if (url.host().isEmpty())
            return fail(error, QStringLiteral("No host given"));
        const bool raw = scheme == QLatin1String("raw");
        if (raw && url.port() < 0)
            return fail(error, QStringLiteral("Raw TCP needs a port"));
        c.type = SessionType::Telnet;
        c.telnet.host = url.host();
        c.telnet.port = url.port(23);
        c.telnet.rawTcp = raw;
        return true;
    }
    if (scheme == QLatin1String("serial")) {
        QString port = url.host().isEmpty() ? url.path() : url.host();
        while (port.startsWith(QLatin1String("//")))
            port.remove(0, 1);
        if (port.startsWith(QLatin1Char('/')) && !port.startsWith(QLatin1String("/dev/")))
            port.remove(0, 1);
        QStringList args{port};
        const QUrlQuery q(url);
        if (q.hasQueryItem(QStringLiteral("baud")))
            args << q.queryItemValue(QStringLiteral("baud"));
        if (port.isEmpty())
            return fail(error, QStringLiteral("No serial port given"));
        return parseSerial(args, c, error);
    }
    return fail(error, QStringLiteral("Unknown protocol: %1").arg(scheme));
}

} // namespace

bool parse(const QString &textIn, SessionConfig &out, QString *error)
{
    const QString text = textIn.trimmed();
    if (text.isEmpty())
        return fail(error, QStringLiteral("Type a host, user@host, COM port or 'local'"));
    SessionConfig c;
    c.type = SessionType::Ssh;

    if (text.contains(QStringLiteral("://"))) {
        if (!parseUrl(text, c, error))
            return false;
        out = c;
        return true;
    }

    const QStringList args = splitArgs(text);
    const QString cmd = args.value(0).toLower();
    const QStringList rest = args.mid(1);

    static const QStringList shells = {QStringLiteral("local"), QStringLiteral("cmd"), QStringLiteral("powershell"),
                                       QStringLiteral("pwsh"), QStringLiteral("wsl"), QStringLiteral("bash")};
    if (shells.contains(cmd)) {
        c.type = SessionType::LocalShell;
        if (cmd != QLatin1String("local")) {
            c.local.program = cmd == QLatin1String("cmd") ? QStringLiteral("cmd.exe")
                            : cmd == QLatin1String("powershell") ? QStringLiteral("powershell.exe")
                            : cmd == QLatin1String("pwsh") ? QStringLiteral("pwsh.exe")
                            : cmd == QLatin1String("wsl") ? QStringLiteral("wsl.exe")
                                                          : QStringLiteral("bash");
            c.local.arguments = rest;
        }
        out = c;
        return true;
    }
    bool ok;
    if (cmd == QLatin1String("ssh"))
        ok = parseSshCommand(rest, c, error);
    else if (cmd == QLatin1String("telnet"))
        ok = parseTelnetCommand(rest, c, false, error);
    else if (cmd == QLatin1String("raw") || cmd == QLatin1String("nc"))
        ok = parseTelnetCommand(rest, c, true, error);
    else if (cmd == QLatin1String("serial"))
        ok = rest.isEmpty() ? fail(error, QStringLiteral("No serial port given")) : parseSerial(rest, c, error);
    else if (looksLikeSerial(args[0].section(QLatin1Char(':'), 0, 0)))
        ok = parseSerial(args, c, error);
    else
        ok = parseSshCommand(args, c, error);   // "host", "admin@host:2222", "admin@host -p 2222"
    if (!ok)
        return false;
    out = c;
    return true;
}

QString toText(const SessionConfig &c)
{
    switch (c.type) {
    case SessionType::Ssh: {
        QString host = c.ssh.host.contains(QLatin1Char(':')) ? QLatin1Char('[') + c.ssh.host + QLatin1Char(']') : c.ssh.host;
        QString s = c.ssh.username.isEmpty() ? host : c.ssh.username + QLatin1Char('@') + host;
        if (c.ssh.port != 22)
            s += QLatin1Char(':') + QString::number(c.ssh.port);
        return s;
    }
    case SessionType::Telnet:
        return QStringLiteral("%1 %2 %3").arg(c.telnet.rawTcp ? QStringLiteral("raw") : QStringLiteral("telnet"),
                                             c.telnet.host).arg(c.telnet.port);
    case SessionType::Serial:
        return QStringLiteral("%1 %2").arg(c.serial.portName).arg(c.serial.baudRate);
    case SessionType::LocalShell:
        return c.local.program.isEmpty() ? QStringLiteral("local") : c.local.program.section(QLatin1Char('.'), 0, 0);
    }
    return QString();
}

} // namespace QuickConnect
} // namespace termx
