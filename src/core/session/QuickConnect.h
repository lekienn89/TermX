// SPDX-License-Identifier: GPL-3.0-or-later
// Turns what a user types into the Quick Connect bar (or passes on the
// command line) into a session:
//   admin@10.0.0.1          10.0.0.1:2222          [fe80::1]:22
//   ssh -p 2222 -l admin router    ssh admin@router -i C:\keys\id_ed25519
//   telnet 10.0.0.1 2001    telnet://host:23        raw://host:4001
//   COM7    COM7 115200    COM7:115200    serial://COM7?baud=9600    /dev/ttyUSB0
//   local   cmd   powershell   pwsh   wsl
#pragma once

#include "SessionConfig.h"

#include <QString>

namespace termx {

namespace QuickConnect {

bool parse(const QString &text, SessionConfig &out, QString *error = nullptr);

// Short text that would recreate the session in the bar (for recents).
QString toText(const SessionConfig &cfg);

} // namespace QuickConnect

} // namespace termx
