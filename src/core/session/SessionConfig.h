// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace termx {

enum class SessionType { Ssh, Telnet, Serial, LocalShell };

QString sessionTypeName(SessionType t);
SessionType sessionTypeFromName(const QString &name, SessionType fallback = SessionType::Ssh);

enum class NewlineMode { CR, LF, CRLF };

// What to do with a host key that is not (or not the same as) in known_hosts.
enum class HostKeyPolicy {
    Ask,        // prompt for unknown and changed keys (default)
    AcceptNew,  // save unknown keys silently, refuse changed keys
    Strict      // refuse unknown and changed keys
};

QString hostKeyPolicyName(HostKeyPolicy p);
HostKeyPolicy hostKeyPolicyFromName(const QString &name);

struct SshOptions {
    QString host;
    int port = 22;
    QString username;
    QString keyFile;                 // private key (OpenSSH / PEM); empty = none
    bool useAgent = true;            // Pageant / OpenSSH agent
    bool allowLegacyAlgorithms = false; // old switches/routers: dh-group1, ssh-rsa, cbc (asked on demand)
    HostKeyPolicy hostKeyPolicy = HostKeyPolicy::Ask;
    int keepAliveSeconds = 30;       // 0 = off
    int connectTimeoutSeconds = 15;
    QString termType = QStringLiteral("xterm-256color");
};

struct TelnetOptions {
    QString host;
    int port = 23;
    bool rawTcp = false;             // no option negotiation (raw socket)
    QString termType = QStringLiteral("XTERM-256COLOR");
};

struct SerialOptions {
    QString portName;                // e.g. COM7 (may change between plugs)
    QString deviceKey;               // stable identity of the USB adapter, if known
    int baudRate = 115200;
    int dataBits = 8;
    QChar parity = QLatin1Char('N'); // N E O M S
    int stopBits = 1;                // 1, 2 (3 = 1.5)
    enum class Flow { None, RtsCts, XonXoff } flow = Flow::None;
    bool autoReconnect = true;
    int lineDelayMs = 0;             // delay after each line when pasting
    int charDelayMs = 0;             // delay after each character when pasting
};

struct LocalShellOptions {
    QString program;                 // empty = default (PowerShell on Windows, $SHELL elsewhere)
    QStringList arguments;
    QString workingDirectory;
};

struct TerminalOptions {
    NewlineMode enterSends = NewlineMode::CR;
    bool localEcho = false;
    int scrollbackLines = 100000;
    bool confirmMultilinePaste = true;
};

struct SessionConfig {
    QString id;                      // uuid; empty for quick-connect sessions
    QString name;
    QString folder;                  // "/"-separated path in the session tree
    SessionType type = SessionType::Ssh;
    SshOptions ssh;
    TelnetOptions telnet;
    SerialOptions serial;
    LocalShellOptions local;
    TerminalOptions terminal;
    bool historyEnabled = true;
    bool logEnabled = false;
    bool logTimestamps = true;       // prefix each logged line with its time
    QString logDirectory;            // empty = default logs folder
    QString encryptedPassword;       // base64 DPAPI blob; empty = not saved
    QString notes;

    QJsonObject toJson() const;
    static SessionConfig fromJson(const QJsonObject &o);

    QString displayName() const;     // name, or a generated one
    QString target() const;          // user@host:port, COM7 115200 8N1, ...
    QString historyHost() const;     // key used to group command history
    QString serialSummary() const;   // "115200 8N1"
};

} // namespace termx
