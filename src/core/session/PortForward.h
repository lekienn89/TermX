// SPDX-License-Identifier: GPL-3.0-or-later
// SSH port forwarding rules (OpenSSH -L / -R / -D) and their text forms.
#pragma once

#include <QJsonObject>
#include <QMetaType>
#include <QString>
#include <QVector>

#include <optional>

namespace termx {

struct PortForward {
    enum class Type {
        Local,    // -L: listen here, connect from the server
        Remote,   // -R: listen on the server, connect from here
        Dynamic   // -D: SOCKS 4/4a/5 proxy here, connect from the server
    };

    Type type = Type::Local;
    QString bindAddress;   // empty = loopback (127.0.0.1 here, "localhost" on the server)
    int bindPort = 0;      // 0 only for Remote: the server picks a port
    QString destHost;      // unused for Dynamic
    int destPort = 0;      // unused for Dynamic
    bool enabled = true;   // saved rules: start automatically on connect
    QString description;

    // OpenSSH argument syntax, without the flag:
    //   Local/Remote:  [bind_address:]port:host:hostport
    //   Dynamic:       [bind_address:]port
    // IPv6 addresses go in brackets: [::1]:8080:[fe80::1]:80
    static std::optional<PortForward> parse(Type type, const QString &spec, QString *error = nullptr);
    // "-L 8080:db:5432", "L8080:db:5432", "-D1080", "R 2222:localhost:22"
    static std::optional<PortForward> parseOption(const QString &text, QString *error = nullptr);
    // PuTTY "PortForwardings" value: "L8080=db:5432,R2222=localhost:22,D1080,4L127.0.0.1:80=web:80"
    static QVector<PortForward> parsePutty(const QString &value, QStringList *warnings = nullptr);

    QString spec() const;                // inverse of parse()
    QString option() const;              // "-L 8080:db:5432"
    QString summary() const;             // "127.0.0.1:8080 -> db:5432 (via server)"
    QString effectiveBindAddress() const;
    bool isValid(QString *error = nullptr) const;
    bool sameRule(const PortForward &o) const;

    QJsonObject toJson() const;
    static PortForward fromJson(const QJsonObject &o);

    static QString typeLetter(Type t);   // "L", "R", "D"
    static QString typeName(Type t);     // "Local", "Remote", "Dynamic (SOCKS)"
};

// Runtime state of one rule on a connected session.
struct ForwardStatus {
    enum class State { Starting, Active, Failed, Stopped };
    quint64 id = 0;
    PortForward rule;
    State state = State::Starting;
    QString error;
    int boundPort = 0;          // the real port (Remote with port 0)
    int openConnections = 0;
    quint64 totalConnections = 0;
    quint64 bytesIn = 0;        // towards this computer
    quint64 bytesOut = 0;       // towards the server
};

QString forwardStateName(ForwardStatus::State s);

} // namespace termx

Q_DECLARE_METATYPE(termx::PortForward)
Q_DECLARE_METATYPE(termx::ForwardStatus)
Q_DECLARE_METATYPE(QVector<termx::ForwardStatus>)
