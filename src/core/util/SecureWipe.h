// SPDX-License-Identifier: GPL-3.0-or-later
// Overwrites secrets (passwords, passphrases) in memory before they are
// released, so they do not linger in freed heap blocks or crash dumps.
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <cstddef>

namespace termx {

// Zeroes `len` bytes in a way the optimiser may not remove.
void secureZero(void *p, std::size_t len);

// Wipes the contents (if this object owns its buffer exclusively) and clears it.
void secureWipe(QByteArray &b);
void secureWipe(QString &s);
void secureWipe(QStringList &l);

} // namespace termx
