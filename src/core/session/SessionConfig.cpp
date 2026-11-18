// SPDX-License-Identifier: GPL-3.0-or-later
#include "SessionConfig.h"

#include <QJsonArray>

namespace termx {

QString sessionTypeName(SessionType t)
{
    switch (t) {
    case SessionType::Ssh: return QStringLiteral("ssh");
    case SessionType::Telnet: return QStringLiteral("telnet");
    case SessionType::Serial: return QStringLiteral("serial");
    case SessionType::LocalShell: return QStringLiteral("local");
    }
    return QStringLiteral("ssh");
}

SessionType sessionTypeFromName(const QString &name, SessionType fallback)
{
    const QString n = name.toLower();
    if (n == QLatin1String("ssh")) return SessionType::Ssh;
    if (n == QLatin1String("telnet")) return SessionType::Telnet;
    if (n == QLatin1String("serial")) return SessionType::Serial;
    if (n == QLatin1String("local")) return SessionType::LocalShell;
    return fallback;
}

static QString newlineName(NewlineMode m)
{
    switch (m) {
    case NewlineMode::CR: return QStringLiteral("cr");
    case NewlineMode::LF: return QStringLiteral("lf");
    case NewlineMode::CRLF: return QStringLiteral("crlf");
    }
    return QStringLiteral("cr");
}

static NewlineMode newlineFromName(const QString &s)
{
    if (s == QLatin1String("lf")) return NewlineMode::LF;
    if (s == QLatin1String("crlf")) return NewlineMode::CRLF;
    return NewlineMode::CR;
}

static int clampInt(const QJsonValue &v, int def, int lo, int hi)
{
    if (!v.isDouble())
        return def;
    const double d = v.toDouble();
    if (d < lo || d > hi)
        return def;
    return int(d);
}

QJsonObject SessionConfig::toJson() const
{
    QJsonObject o;
    o[QStringLiteral("id")] = id;
    o[QStringLiteral("name")] = name;
    o[QStringLiteral("folder")] = folder;
    o[QStringLiteral("type")] = sessionTypeName(type);
    o[QStringLiteral("history")] = historyEnabled;
    o[QStringLiteral("log")] = logEnabled;
    o[QStringLiteral("logDir")] = logDirectory;
    o[QStringLiteral("logTimestamps")] = logTimestamps;
    o[QStringLiteral("password")] = encryptedPassword;
    o[QStringLiteral("notes")] = notes;

    QJsonObject s;
    s[QStringLiteral("host")] = ssh.host;
    s[QStringLiteral("port")] = ssh.port;
    s[QStringLiteral("user")] = ssh.username;
    s[QStringLiteral("keyFile")] = ssh.keyFile;
    s[QStringLiteral("agent")] = ssh.useAgent;
    s[QStringLiteral("legacy")] = ssh.allowLegacyAlgorithms;
    s[QStringLiteral("keepAlive")] = ssh.keepAliveSeconds;
    s[QStringLiteral("timeout")] = ssh.connectTimeoutSeconds;
    s[QStringLiteral("term")] = ssh.termType;
    o[QStringLiteral("ssh")] = s;

    QJsonObject t;
    t[QStringLiteral("host")] = telnet.host;
    t[QStringLiteral("port")] = telnet.port;
    t[QStringLiteral("raw")] = telnet.rawTcp;
    t[QStringLiteral("term")] = telnet.termType;
    o[QStringLiteral("telnet")] = t;

    QJsonObject c;
    c[QStringLiteral("port")] = serial.portName;
    c[QStringLiteral("deviceKey")] = serial.deviceKey;
    c[QStringLiteral("baud")] = serial.baudRate;
    c[QStringLiteral("dataBits")] = serial.dataBits;
    c[QStringLiteral("parity")] = QString(serial.parity);
    c[QStringLiteral("stopBits")] = serial.stopBits;
    c[QStringLiteral("flow")] = serial.flow == SerialOptions::Flow::RtsCts ? QStringLiteral("rtscts")
                              : serial.flow == SerialOptions::Flow::XonXoff ? QStringLiteral("xonxoff")
                                                                           : QStringLiteral("none");
    c[QStringLiteral("autoReconnect")] = serial.autoReconnect;
    c[QStringLiteral("lineDelay")] = serial.lineDelayMs;
    c[QStringLiteral("charDelay")] = serial.charDelayMs;
    o[QStringLiteral("serial")] = c;

    QJsonObject l;
    l[QStringLiteral("program")] = local.program;
    l[QStringLiteral("args")] = QJsonArray::fromStringList(local.arguments);
    l[QStringLiteral("cwd")] = local.workingDirectory;
    o[QStringLiteral("local")] = l;

    QJsonObject term;
    term[QStringLiteral("enter")] = newlineName(terminal.enterSends);
    term[QStringLiteral("localEcho")] = terminal.localEcho;
    term[QStringLiteral("scrollback")] = terminal.scrollbackLines;
    term[QStringLiteral("confirmPaste")] = terminal.confirmMultilinePaste;
    o[QStringLiteral("terminal")] = term;
    return o;
}

SessionConfig SessionConfig::fromJson(const QJsonObject &o)
{
    SessionConfig c;
    c.id = o.value(QStringLiteral("id")).toString();
    c.name = o.value(QStringLiteral("name")).toString();
    c.folder = o.value(QStringLiteral("folder")).toString();
    c.type = sessionTypeFromName(o.value(QStringLiteral("type")).toString());
    c.historyEnabled = o.value(QStringLiteral("history")).toBool(true);
    c.logEnabled = o.value(QStringLiteral("log")).toBool(false);
    c.logDirectory = o.value(QStringLiteral("logDir")).toString();
    c.logTimestamps = o.value(QStringLiteral("logTimestamps")).toBool(true);
    c.encryptedPassword = o.value(QStringLiteral("password")).toString();
    c.notes = o.value(QStringLiteral("notes")).toString();

    const QJsonObject s = o.value(QStringLiteral("ssh")).toObject();
    c.ssh.host = s.value(QStringLiteral("host")).toString();
    c.ssh.port = clampInt(s.value(QStringLiteral("port")), 22, 1, 65535);
    c.ssh.username = s.value(QStringLiteral("user")).toString();
    c.ssh.keyFile = s.value(QStringLiteral("keyFile")).toString();
    c.ssh.useAgent = s.value(QStringLiteral("agent")).toBool(true);
    c.ssh.allowLegacyAlgorithms = s.value(QStringLiteral("legacy")).toBool(true);
    c.ssh.keepAliveSeconds = clampInt(s.value(QStringLiteral("keepAlive")), 30, 0, 3600);
    c.ssh.connectTimeoutSeconds = clampInt(s.value(QStringLiteral("timeout")), 15, 1, 300);
    c.ssh.termType = s.value(QStringLiteral("term")).toString(QStringLiteral("xterm-256color"));

    const QJsonObject t = o.value(QStringLiteral("telnet")).toObject();
    c.telnet.host = t.value(QStringLiteral("host")).toString();
    c.telnet.port = clampInt(t.value(QStringLiteral("port")), 23, 1, 65535);
    c.telnet.rawTcp = t.value(QStringLiteral("raw")).toBool(false);
    c.telnet.termType = t.value(QStringLiteral("term")).toString(QStringLiteral("XTERM-256COLOR"));

    const QJsonObject k = o.value(QStringLiteral("serial")).toObject();
    c.serial.portName = k.value(QStringLiteral("port")).toString();
    c.serial.deviceKey = k.value(QStringLiteral("deviceKey")).toString();
    c.serial.baudRate = clampInt(k.value(QStringLiteral("baud")), 9600, 50, 16000000);
    c.serial.dataBits = clampInt(k.value(QStringLiteral("dataBits")), 8, 5, 8);
    const QString par = k.value(QStringLiteral("parity")).toString(QStringLiteral("N")).toUpper();
    c.serial.parity = (par.size() == 1 && QStringLiteral("NEOMS").contains(par)) ? par.at(0) : QLatin1Char('N');
    c.serial.stopBits = clampInt(k.value(QStringLiteral("stopBits")), 1, 1, 3);
    const QString flow = k.value(QStringLiteral("flow")).toString();
    c.serial.flow = flow == QLatin1String("rtscts") ? SerialOptions::Flow::RtsCts
                  : flow == QLatin1String("xonxoff") ? SerialOptions::Flow::XonXoff
                                                     : SerialOptions::Flow::None;
    c.serial.autoReconnect = k.value(QStringLiteral("autoReconnect")).toBool(true);
    c.serial.lineDelayMs = clampInt(k.value(QStringLiteral("lineDelay")), 0, 0, 5000);
    c.serial.charDelayMs = clampInt(k.value(QStringLiteral("charDelay")), 0, 0, 1000);

    const QJsonObject l = o.value(QStringLiteral("local")).toObject();
    c.local.program = l.value(QStringLiteral("program")).toString();
    for (const QJsonValue &v : l.value(QStringLiteral("args")).toArray())
        c.local.arguments << v.toString();
    c.local.workingDirectory = l.value(QStringLiteral("cwd")).toString();

    const QJsonObject term = o.value(QStringLiteral("terminal")).toObject();
    c.terminal.enterSends = newlineFromName(term.value(QStringLiteral("enter")).toString());
    c.terminal.localEcho = term.value(QStringLiteral("localEcho")).toBool(false);
    c.terminal.scrollbackLines = clampInt(term.value(QStringLiteral("scrollback")), 100000, 100, 1000000);
    c.terminal.confirmMultilinePaste = term.value(QStringLiteral("confirmPaste")).toBool(true);
    return c;
}

QString SessionConfig::serialSummary() const
{
    const QString stop = serial.stopBits == 3 ? QStringLiteral("1.5") : QString::number(serial.stopBits);
    return QStringLiteral("%1 %2%3%4").arg(serial.baudRate).arg(serial.dataBits).arg(serial.parity).arg(stop);
}

QString SessionConfig::target() const
{
    switch (type) {
    case SessionType::Ssh: {
        QString t = ssh.username.isEmpty() ? ssh.host : ssh.username + QLatin1Char('@') + ssh.host;
        if (ssh.port != 22)
            t += QLatin1Char(':') + QString::number(ssh.port);
        return t;
    }
    case SessionType::Telnet:
        return telnet.port == 23 ? telnet.host : telnet.host + QLatin1Char(':') + QString::number(telnet.port);
    case SessionType::Serial:
        return serial.portName + QLatin1Char(' ') + serialSummary();
    case SessionType::LocalShell:
        return local.program.isEmpty() ? QStringLiteral("Local shell") : local.program;
    }
    return QString();
}

QString SessionConfig::displayName() const
{
    return name.trimmed().isEmpty() ? target() : name;
}

QString SessionConfig::historyHost() const
{
    switch (type) {
    case SessionType::Ssh: return ssh.host.toLower();
    case SessionType::Telnet: return telnet.host.toLower();
    case SessionType::Serial: return serial.deviceKey.isEmpty() ? serial.portName : serial.deviceKey;
    case SessionType::LocalShell: return QStringLiteral("localhost");
    }
    return QString();
}

} // namespace termx
