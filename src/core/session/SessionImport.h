// SPDX-License-Identifier: GPL-3.0-or-later
// Imports saved sessions from other tools: PuTTY (registry or a .reg
// export) and OpenSSH client config files.
#pragma once

#include "session/SessionConfig.h"

#include <QList>
#include <QStringList>
#include <QVariantMap>

namespace termx::SessionImport {

struct Result {
    QList<SessionConfig> sessions;
    QStringList warnings;          // one line per skipped or partly imported entry
};

// One PuTTY session from its registry values. `name` is the encoded key
// name ("My%20Router"). Returns false (with *warning) when it cannot be used.
bool fromPuttyValues(const QString &name, const QVariantMap &values, SessionConfig &out, QString *warning);

// A "regedit /e" export of HKCU\Software\SimonTatham\PuTTY\Sessions.
// UTF-16 (regedit default) and 8-bit files are both accepted.
Result parsePuttyReg(const QByteArray &data);

// ~/.ssh/config. Wildcard hosts are skipped; their options are not merged.
Result parseOpenSshConfig(const QString &text, const QString &homeDir);

// Reads PuTTY sessions from the current user's registry (Windows only).
Result readPuttyRegistry();

QString decodePuttyName(const QString &encoded);

} // namespace termx::SessionImport
