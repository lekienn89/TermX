// SPDX-License-Identifier: GPL-3.0-or-later
#include "util/SecureWipe.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace termx {

void secureZero(void *p, std::size_t len)
{
    if (!p || len == 0)
        return;
#ifdef _WIN32
    SecureZeroMemory(p, len);
#else
    volatile unsigned char *v = static_cast<volatile unsigned char *>(p);
    while (len--)
        *v++ = 0;
#endif
}

// Only wipe buffers this object owns alone: data() on a shared buffer would
// detach and leave the original copy elsewhere untouched anyway, and the
// other owner may still need it. Callers wipe each copy they hold.
void secureWipe(QByteArray &b)
{
    if (!b.isEmpty() && b.isDetached())
        secureZero(b.data(), std::size_t(b.size()));
    b.clear();
}

void secureWipe(QString &s)
{
    if (!s.isEmpty() && s.isDetached())
        secureZero(s.data(), std::size_t(s.size()) * sizeof(QChar));
    s.clear();
}

void secureWipe(QStringList &l)
{
    for (QString &s : l)
        secureWipe(s);
    l.clear();
}

} // namespace termx
