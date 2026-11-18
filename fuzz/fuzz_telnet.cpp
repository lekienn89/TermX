// SPDX-License-Identifier: GPL-3.0-or-later
#include "transport/TelnetProtocol.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    termx::TelnetProtocol p;
    p.setWindowSize(80, 24);
    QByteArray out, reply;
    (void)p.initialNegotiation();
    size_t pos = 0;
    while (pos < size) {
        const size_t n = 1 + data[pos] % 17 < size - pos ? 1 + data[pos] % 17 : size - pos;
        p.feed(reinterpret_cast<const char *>(data + pos), qsizetype(n), out, reply);
        pos += n;
    }
    (void)p.encode(out);
    (void)p.windowSizeMessage();
    // A server must never make us reply with more than a bounded amount.
    if (reply.size() > qsizetype(size) * 64 + 4096)
        __builtin_trap();
    return 0;
}
