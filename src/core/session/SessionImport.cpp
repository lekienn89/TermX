// SPDX-License-Identifier: GPL-3.0-or-later
#include "session/SessionImport.h"

#include <QByteArray>
#include <QDir>
#include <QRegularExpression>
#include <QSettings>
#include <QStringDecoder>

namespace termx::SessionImport {

namespace {
const QString kPuttyFolder = QStringLiteral("PuTTY");
const QString kSshConfigFolder = QStringLiteral("SSH config");

int toInt(const QVariant &v, int fallback)
{
    bool ok = false;
    const int n = v.toInt(&ok);
    return ok ? n : fallback;
}

QString expandHome(QString path, const QString &home)
{
    if (path.startsWith(QLatin1String("~/")) || path.startsWith(QLatin1String("~\\")))
        path = home + path.mid(1);
    else if (path == QLatin1String("~"))
        path = home;
    path.replace(QLatin1String("%d"), home);
    return QDir::fromNativeSeparators(path);
}

// "regedit" string: backslash escapes only \\ and \".
QString unescapeRegString(const QString &s)
{
    QString out;
    out.reserve(s.size());
    for (int i = 0; i < s.size(); ++i) {
        if (s[i] == QLatin1Char('\\') && i + 1 < s.size())
            ++i;
        out += s[i];
    }
    return out;
}
} // namespace

QString decodePuttyName(const QString &encoded)
{
    return QString::fromUtf8(QByteArray::fromPercentEncoding(encoded.toLatin1()));
}

bool fromPuttyValues(const QString &encodedName, const QVariantMap &v, SessionConfig &c, QString *warning)
{
    const QString name = decodePuttyName(encodedName);
    auto warn = [&](const QString &w) {
        if (warning)
            *warning = QStringLiteral("%1: %2").arg(name, w);
    };
    c = SessionConfig();
    c.name = name;
    c.folder = kPuttyFolder;

    const QString protocol = v.value(QStringLiteral("Protocol"), QStringLiteral("ssh")).toString().toLower();
    QString host = v.value(QStringLiteral("HostName")).toString().trimmed();
    QString user = v.value(QStringLiteral("UserName")).toString().trimmed();
    const int at = host.lastIndexOf(QLatin1Char('@'));
    if (at > 0) {
        if (user.isEmpty())
            user = host.left(at);
        host = host.mid(at + 1);
    }
    const int port = toInt(v.value(QStringLiteral("PortNumber")), 0);

    if (protocol == QLatin1String("serial")) {
        c.type = SessionType::Serial;
        c.serial.portName = v.value(QStringLiteral("SerialLine")).toString().trimmed();
        if (c.serial.portName.isEmpty()) {
            warn(QStringLiteral("no serial line set, skipped"));
            return false;
        }
        c.serial.baudRate = toInt(v.value(QStringLiteral("SerialSpeed")), 9600);
        c.serial.dataBits = qBound(5, toInt(v.value(QStringLiteral("SerialDataBits")), 8), 8);
        const int halfBits = toInt(v.value(QStringLiteral("SerialStopHalfbits")), 2);
        c.serial.stopBits = halfBits >= 4 ? 2 : halfBits == 3 ? 3 : 1;
        static const char parities[] = {'N', 'O', 'E', 'M', 'S'};
        const int par = toInt(v.value(QStringLiteral("SerialParity")), 0);
        c.serial.parity = QLatin1Char(par >= 0 && par < 5 ? parities[par] : 'N');
        switch (toInt(v.value(QStringLiteral("SerialFlowControl")), 1)) {
        case 0: c.serial.flow = SerialOptions::Flow::None; break;
        case 2: c.serial.flow = SerialOptions::Flow::RtsCts; break;
        case 3:
            c.serial.flow = SerialOptions::Flow::None;
            warn(QStringLiteral("DSR/DTR flow control is not supported, using none"));
            break;
        default: c.serial.flow = SerialOptions::Flow::XonXoff; break;
        }
        return true;
    }

    if (host.isEmpty()) {
        if (encodedName != QLatin1String("Default%20Settings"))
            warn(QStringLiteral("no host name, skipped"));
        return false;
    }

    if (protocol == QLatin1String("ssh")) {
        c.type = SessionType::Ssh;
        c.ssh.host = host;
        c.ssh.port = port > 0 ? port : 22;
        c.ssh.username = user;
        const QString key = v.value(QStringLiteral("PublicKeyFile")).toString().trimmed();
        if (!key.isEmpty()) {
            c.ssh.keyFile = QString(key).replace(QLatin1Char('\\'), QLatin1Char('/'));
            if (key.endsWith(QLatin1String(".ppk"), Qt::CaseInsensitive))
                warn(QStringLiteral("uses a PuTTY .ppk key; export it in OpenSSH format with PuTTYgen "
                                    "(Conversions > Export OpenSSH key) and pick that file"));
        }
        const int ping = toInt(v.value(QStringLiteral("PingIntervalSecs")), 0);
        if (ping > 0)
            c.ssh.keepAliveSeconds = ping;
        const QString term = v.value(QStringLiteral("TerminalType")).toString();
        if (!term.isEmpty() && term != QLatin1String("xterm"))
            c.ssh.termType = term;
        const QString fw = v.value(QStringLiteral("PortForwardings")).toString();
        if (!fw.isEmpty()) {
            QStringList fwWarnings;
            c.ssh.forwards = PortForward::parsePutty(fw, &fwWarnings);
            for (const QString &w : std::as_const(fwWarnings))
                warn(w);
        }
        return true;
    }
    if (protocol == QLatin1String("telnet") || protocol == QLatin1String("raw")) {
        c.type = SessionType::Telnet;
        c.telnet.host = host;
        c.telnet.rawTcp = protocol == QLatin1String("raw");
        c.telnet.port = port > 0 ? port : 23;
        return true;
    }
    warn(QStringLiteral("protocol \"%1\" is not supported, skipped").arg(protocol));
    return false;
}

Result parsePuttyReg(const QByteArray &data)
{
    Result r;
    QString text;
    if (data.startsWith("\xFF\xFE") || data.startsWith("\xFE\xFF")) {
        auto dec = QStringDecoder(data.startsWith("\xFF\xFE") ? QStringDecoder::Utf16LE : QStringDecoder::Utf16BE);
        text = dec(data.mid(2));
    } else {
        QByteArray d = data;
        if (d.startsWith("\xEF\xBB\xBF"))
            d.remove(0, 3);
        auto dec = QStringDecoder(QStringDecoder::Utf8);
        text = dec(d);
        if (dec.hasError())
            text = QString::fromLatin1(d);
    }

    static const QRegularExpression keyRx(QStringLiteral(R"rx(^\[(-?)HKEY_CURRENT_USER\\Software\\SimonTatham\\PuTTY\\Sessions\\([^\\\]]+)\]\s*$)rx"),
                                          QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression strRx(QStringLiteral(R"rx(^"((?:[^"\\]|\\.)*)"\s*=\s*"((?:[^"\\]|\\.)*)"\s*$)rx"));
    static const QRegularExpression dwordRx(QStringLiteral(R"rx(^"((?:[^"\\]|\\.)*)"\s*=\s*dword:([0-9a-fA-F]{1,8})\s*$)rx"));

    QString current;
    QVariantMap values;
    QStringList order;
    QHash<QString, QVariantMap> all;
    auto flush = [&] {
        if (!current.isEmpty()) {
            if (!all.contains(current))
                order << current;
            all[current] = values;
        }
        current.clear();
        values.clear();
    };

    const QStringList lines = text.split(QRegularExpression(QStringLiteral("\r\n|\n|\r")));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1Char('['))) {
            flush();
            const auto m = keyRx.match(line);
            if (m.hasMatch() && m.captured(1).isEmpty())
                current = m.captured(2);
            continue;
        }
        if (current.isEmpty() || line.isEmpty())
            continue;
        if (auto m = strRx.match(line); m.hasMatch())
            values.insert(unescapeRegString(m.captured(1)), unescapeRegString(m.captured(2)));
        else if (auto d = dwordRx.match(line); d.hasMatch())
            values.insert(unescapeRegString(d.captured(1)), int(d.captured(2).toUInt(nullptr, 16)));
        // hex: values (and their continuation lines) are not needed.
    }
    flush();

    if (order.isEmpty())
        r.warnings << QStringLiteral("No PuTTY sessions found. Export them with: "
                                     "regedit /e putty.reg HKEY_CURRENT_USER\\Software\\SimonTatham\\PuTTY\\Sessions");
    for (const QString &name : std::as_const(order)) {
        SessionConfig c;
        QString w;
        if (fromPuttyValues(name, all.value(name), c, &w))
            r.sessions << c;
        if (!w.isEmpty())
            r.warnings << w;
    }
    return r;
}

Result parseOpenSshConfig(const QString &text, const QString &homeDir)
{
    Result r;
    struct Block {
        QStringList patterns;
        QHash<QString, QString> opts;   // lower-case keyword -> first value
        QStringList forwards;           // "-L spec" for every *Forward line, in order
        bool inMatch = false;
    };
    QList<Block> blocks;
    Block global;
    Block *cur = &global;

    const QStringList lines = text.split(QRegularExpression(QStringLiteral("\r\n|\n|\r")));
    static const QRegularExpression kvRx(QStringLiteral(R"rx(^\s*([A-Za-z][A-Za-z0-9]*)\s*(?:=\s*|\s+)(.*?)\s*$)rx"));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        const auto m = kvRx.match(line);
        if (!m.hasMatch())
            continue;
        const QString key = m.captured(1).toLower();
        QString value = m.captured(2);
        if (value.size() >= 2 && value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))
            value = value.mid(1, value.size() - 2);
        if (key == QLatin1String("host")) {
            Block b;
            b.patterns = value.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            blocks << b;
            cur = &blocks.last();
        } else if (key == QLatin1String("match")) {
            Block b;
            b.inMatch = true;
            blocks << b;
            cur = &blocks.last();
        } else if (key == QLatin1String("localforward") || key == QLatin1String("remoteforward")
                   || key == QLatin1String("dynamicforward")) {
            // "LocalForward [bind:]port host:hostport" -> "L[bind:]port:host:hostport"
            const QStringList words = value.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            cur->forwards << QString(key.at(0).toUpper()) + words.join(QLatin1Char(':'));
        } else if (!cur->opts.contains(key)) {
            cur->opts.insert(key, value);
        }
    }

    for (const Block &b : std::as_const(blocks)) {
        if (b.inMatch)
            continue;
        for (const QString &alias : b.patterns) {
            if (alias.contains(QLatin1Char('*')) || alias.contains(QLatin1Char('?')) || alias.startsWith(QLatin1Char('!')))
                continue;
            auto opt = [&](const char *k) {
                const QString key = QString::fromLatin1(k);
                return b.opts.contains(key) ? b.opts.value(key) : global.opts.value(key);
            };
            SessionConfig c;
            c.type = SessionType::Ssh;
            c.name = alias;
            c.folder = kSshConfigFolder;
            QString host = opt("hostname");
            host.replace(QLatin1String("%h"), alias);
            c.ssh.host = host.isEmpty() ? alias : host;
            bool ok = false;
            const int port = opt("port").toInt(&ok);
            c.ssh.port = ok && port > 0 && port < 65536 ? port : 22;
            c.ssh.username = opt("user");
            const QString key = opt("identityfile");
            if (!key.isEmpty())
                c.ssh.keyFile = expandHome(key, homeDir);
            const int alive = opt("serveraliveinterval").toInt(&ok);
            if (ok && alive >= 0)
                c.ssh.keepAliveSeconds = alive;
            for (const QString &rule : b.forwards + global.forwards) {
                QString err;
                if (auto f = PortForward::parseOption(rule, &err))
                    c.ssh.forwards << *f;
                else
                    r.warnings << QStringLiteral("%1: port forwarding \"%2\" skipped: %3").arg(alias, rule.mid(1), err);
            }
            if (!opt("proxyjump").isEmpty() || !opt("proxycommand").isEmpty())
                r.warnings << QStringLiteral("%1: ProxyJump/ProxyCommand is not supported yet; imported without it").arg(alias);
            r.sessions << c;
        }
    }
    if (r.sessions.isEmpty())
        r.warnings << QStringLiteral("No Host entries found.");
    return r;
}

Result readPuttyRegistry()
{
    Result r;
#ifdef Q_OS_WIN
    QSettings reg(QStringLiteral("HKEY_CURRENT_USER\\Software\\SimonTatham\\PuTTY\\Sessions"), QSettings::NativeFormat);
    const QStringList names = reg.childGroups();
    for (const QString &name : names) {
        reg.beginGroup(name);
        QVariantMap values;
        for (const QString &k : reg.childKeys())
            values.insert(k, reg.value(k));
        reg.endGroup();
        SessionConfig c;
        QString w;
        if (fromPuttyValues(name, values, c, &w))
            r.sessions << c;
        if (!w.isEmpty())
            r.warnings << w;
    }
    if (names.isEmpty())
        r.warnings << QStringLiteral("No PuTTY sessions found in the registry.");
#else
    r.warnings << QStringLiteral("Reading the registry is only available on Windows. Import a .reg export instead.");
#endif
    return r;
}

} // namespace termx::SessionImport
