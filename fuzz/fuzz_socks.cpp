// SPDX-License-Identifier: GPL-3.0-or-later
#include "transport/SocksProtocol.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    termx::SocksHandshake h;
    qsizetype replied = 0;
    size_t pos = 0;
    termx::SocksHandshake::Status st = termx::SocksHandshake::Status::NeedMore;
    while (pos < size && st == termx::SocksHandshake::Status::NeedMore) {
        const size_t n = 1 + data[pos] % 13 < size - pos ? 1 + data[pos] % 13 : size - pos;
        st = h.feed(reinterpret_cast<const char *>(data + pos), int(n));
        replied += h.takeReply().size();
        pos += n;
    }
    if (st == termx::SocksHandshake::Status::Connect) {
        // A parsed request always names a destination.
        if (h.host().isEmpty() || h.port() == 0)
            __builtin_trap();
        (void)h.takeLeftover();
        (void)h.finalReply(termx::SocksHandshake::Succeeded);
        (void)h.finalReply(termx::SocksHandshake::HostUnreachable);
    }
    // Replies before CONNECT are tiny (method choice or an error code).
    if (replied > 64)
        __builtin_trap();
    return 0;
}
