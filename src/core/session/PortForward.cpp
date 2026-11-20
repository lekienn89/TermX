// SPDX-License-Identifier: GPL-3.0-or-later
#include "PortForward.h"

#include <QStringList>

namespace termx {

namespace {

// Splits "a:[b:c]:d" into {"a", "b:c", "d"}; brackets are removed.
bool splitSpec(const QString &spec, QStringList &out, QString *error)
{
    out.clear();
    QString cur;
    bool bracketed = false;
    for (int i = 0; i < spec.size(); ++i) {
        const QChar ch = spec.at(i);
        if (ch == QLatin1Char('[') && cur.isEmpty() && !bracketed) {
            const int end = spec.indexOf(QLatin1Char(']'), i + 1);
            if (end < 0) {
                if (error)
                    *error = QStringLiteral("Missing ']' in \"%1\"").arg(spec);
                return false;
            }
            cur = spec.mid(i + 1, end - i - 1);
            bracketed = true;
            i = end;
            if (i + 1 < spec.size() && spec.at(i + 1) != QLatin1Char(':')) {
                if (error)
                    *error = QStringLiteral("Expected ':' after ']' in \"%1\"").arg(spec);
                return false;
            }
            continue;
        }
        if (ch == QLatin1Char(':')) {
            out << cur;
            cur.clear();
            bracketed = false;
            continue;
        }
        if (bracketed) {
            if (error)
                *error = QStringLiteral("Unexpected text after ']' in \"%1\"").arg(spec);
            return false;
        }
        cur += ch;
    }
    out << cur;
    return true;
}

bool parsePort(const QString &s, int &port, bool allowZero)
{
    bool ok = false;
    const int p = s.trimmed().toInt(&ok);
    if (!ok || p < (allowZero ? 0 : 1) || p > 65535)
        return false;
    port = p;
    return true;
}

QString hostPart(const QString &h)
{
    return h.contains(QLatin1Char(':')) ? QLatin1Char('[') + h + QLatin1Char(']') : h;
}

bool validHostText(const QString &h)
{
    if (h.isEmpty() || h.size() > 255)
        return false;
    for (const QChar ch : h) {
        if (ch.isSpace() || ch == QLatin1Char('/') || ch == QLatin1Char('\\') || ch.unicode() < 0x20)
            return false;
    }
    return true;
}

} // namespace

QString PortForward::typeLetter(Type t)
{
    switch (t) {
    case Type::Local: return QStringLiteral("L");
    case Type::Remote: return QStringLiteral("R");
    case Type::Dynamic: return QStringLiteral("D");
    }
    return QStringLiteral("L");
}

QString PortForward::typeName(Type t)
{
    switch (t) {
    case Type::Local: return QStringLiteral("Local");
    case Type::Remote: return QStringLiteral("Remote");
    case Type::Dynamic: return QStringLiteral("Dynamic (SOCKS)");
    }
    return QString();
}

std::optional<PortForward> PortForward::parse(Type type, const QString &specIn, QString *error)
{
    const QString spec = specIn.trimmed();
    QStringList parts;
    if (spec.isEmpty()) {
        if (error)
            *error = QStringLiteral("Empty forwarding rule");
        return std::nullopt;
    }
    if (!splitSpec(spec, parts, error))
        return std::nullopt;

    PortForward f;
    f.type = type;
    const bool allowZero = type == Type::Remote;
    if (type == Type::Dynamic) {
        if (parts.size() < 1 || parts.size() > 2) {
            if (error)
                *error = QStringLiteral("Expected [bind_address:]port, got \"%1\"").arg(spec);
            return std::nullopt;
        }
        if (parts.size() == 2)
            f.bindAddress = parts.at(0).trimmed();
        if (!parsePort(parts.last(), f.bindPort, false)) {
            if (error)
                *error = QStringLiteral("Invalid port \"%1\"").arg(parts.last());
            return std::nullopt;
        }
    } else {
        if (parts.size() < 3 || parts.size() > 4) {
            if (error)
                *error = type == Type::Remote && parts.size() <= 2
                             ? QStringLiteral("Remote SOCKS forwarding (-R port) is not supported; use [bind:]port:host:hostport")
                             : QStringLiteral("Expected [bind_address:]port:host:hostport, got \"%1\"").arg(spec);
            return std::nullopt;
        }
        int i = 0;
        if (parts.size() == 4)
            f.bindAddress = parts.at(i++).trimmed();
        if (!parsePort(parts.at(i), f.bindPort, allowZero)) {
            if (error)
                *error = QStringLiteral("Invalid listening port \"%1\"").arg(parts.at(i));
            return std::nullopt;
        }
        ++i;
        f.destHost = parts.at(i++).trimmed();
        if (!parsePort(parts.at(i), f.destPort, false)) {
            if (error)
                *error = QStringLiteral("Invalid destination port \"%1\"").arg(parts.at(i));
            return std::nullopt;
        }
    }
    if (f.bindAddress == QLatin1String("*"))
        f.bindAddress = QStringLiteral("0.0.0.0");
    QString why;
    if (!f.isValid(&why)) {
        if (error)
            *error = why;
        return std::nullopt;
    }
    return f;
}

std::optional<PortForward> PortForward::parseOption(const QString &textIn, QString *error)
{
    QString text = textIn.trimmed();
    if (text.startsWith(QLatin1Char('-')))
        text.remove(0, 1);
    if (text.isEmpty()) {
        if (error)
            *error = QStringLiteral("Empty forwarding rule");
        return std::nullopt;
    }
    const QChar letter = text.at(0).toUpper();
    Type type;
    if (letter == QLatin1Char('L'))
        type = Type::Local;
    else if (letter == QLatin1Char('R'))
        type = Type::Remote;
    else if (letter == QLatin1Char('D'))
        type = Type::Dynamic;
    else {
        if (error)
            *error = QStringLiteral("A rule starts with L, R or D (like -L 8080:host:80)");
        return std::nullopt;
    }
    return parse(type, text.mid(1), error);
}

QVector<PortForward> PortForward::parsePutty(const QString &value, QStringList *warnings)
{
    QVector<PortForward> out;
    const QStringList items = value.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (QString item : items) {
        item = item.trimmed();
        if (!item.isEmpty() && (item.at(0) == QLatin1Char('4') || item.at(0) == QLatin1Char('6')))
            item.remove(0, 1);
        if (item.isEmpty())
            continue;
        const QChar letter = item.at(0).toUpper();
        QString rest = item.mid(1);
        const int eq = rest.indexOf(QLatin1Char('='));
        QString listen = eq < 0 ? rest : rest.left(eq);
        QString dest = eq < 0 ? QString() : rest.mid(eq + 1);
        std::optional<PortForward> f;
        QString err;
        if (letter == QLatin1Char('D'))
            f = parse(Type::Dynamic, listen, &err);
        else if (letter == QLatin1Char('L'))
            f = parse(Type::Local, listen + QLatin1Char(':') + dest, &err);
        else if (letter == QLatin1Char('R'))
            f = parse(Type::Remote, listen + QLatin1Char(':') + dest, &err);
        else
            err = QStringLiteral("unknown type");
        if (f)
            out << *f;
        else if (warnings)
            *warnings << QStringLiteral("Port forwarding \"%1\" skipped: %2").arg(item, err);
    }
    return out;
}

QString PortForward::spec() const
{
    QString s;
    if (!bindAddress.isEmpty())
        s = hostPart(bindAddress) + QLatin1Char(':');
    s += QString::number(bindPort);
    if (type != Type::Dynamic)
        s += QLatin1Char(':') + hostPart(destHost) + QLatin1Char(':') + QString::number(destPort);
    return s;
}

QString PortForward::option() const
{
    return QLatin1Char('-') + typeLetter(type) + QLatin1Char(' ') + spec();
}

QString PortForward::effectiveBindAddress() const
{
    if (!bindAddress.isEmpty())
        return bindAddress;
    return type == Type::Remote ? QStringLiteral("localhost") : QStringLiteral("127.0.0.1");
}

QString PortForward::summary() const
{
    const QString listen = hostPart(effectiveBindAddress()) + QLatin1Char(':') + QString::number(bindPort);
    switch (type) {
    case Type::Local:
        return QStringLiteral("%1 → %2:%3 (via server)").arg(listen, hostPart(destHost)).arg(destPort);
    case Type::Remote:
        return QStringLiteral("server %1 → %2:%3").arg(listen, hostPart(destHost)).arg(destPort);
    case Type::Dynamic:
        return QStringLiteral("SOCKS proxy on %1").arg(listen);
    }
    return QString();
}

bool PortForward::isValid(QString *error) const
{
    auto bad = [&](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    if (bindPort < (type == Type::Remote ? 0 : 1) || bindPort > 65535)
        return bad(QStringLiteral("Listening port must be 1-65535"));
    if (!bindAddress.isEmpty() && !validHostText(bindAddress))
        return bad(QStringLiteral("Invalid bind address \"%1\"").arg(bindAddress));
    if (type != Type::Dynamic) {
        if (!validHostText(destHost))
            return bad(QStringLiteral("Destination host is missing or invalid"));
        if (destPort < 1 || destPort > 65535)
            return bad(QStringLiteral("Destination port must be 1-65535"));
    }
    return true;
}

bool PortForward::sameRule(const PortForward &o) const
{
    return type == o.type && effectiveBindAddress() == o.effectiveBindAddress() && bindPort == o.bindPort
           && (type == Type::Dynamic || (destHost == o.destHost && destPort == o.destPort));
}

QJsonObject PortForward::toJson() const
{
    QJsonObject o;
    o[QStringLiteral("type")] = typeLetter(type);
    o[QStringLiteral("spec")] = spec();
    o[QStringLiteral("enabled")] = enabled;
    if (!description.isEmpty())
        o[QStringLiteral("description")] = description;
    return o;
}

PortForward PortForward::fromJson(const QJsonObject &o)
{
    const QString letter = o.value(QStringLiteral("type")).toString();
    const Type t = letter == QLatin1String("R") ? Type::Remote : letter == QLatin1String("D") ? Type::Dynamic : Type::Local;
    PortForward f;
    if (auto parsed = parse(t, o.value(QStringLiteral("spec")).toString()))
        f = *parsed;
    else
        f.type = t;   // invalid: bindPort stays 0, isValid() is false and it is skipped
    f.enabled = o.value(QStringLiteral("enabled")).toBool(true);
    f.description = o.value(QStringLiteral("description")).toString().left(200);
    return f;
}

QString forwardStateName(ForwardStatus::State s)
{
    switch (s) {
    case ForwardStatus::State::Starting: return QStringLiteral("Starting");
    case ForwardStatus::State::Active: return QStringLiteral("Active");
    case ForwardStatus::State::Failed: return QStringLiteral("Failed");
    case ForwardStatus::State::Stopped: return QStringLiteral("Stopped");
    }
    return QString();
}

} // namespace termx
