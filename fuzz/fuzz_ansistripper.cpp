// SPDX-License-Identifier: GPL-3.0-or-later
#include "terminal/AnsiStripper.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    termx::AnsiStripper s;
    QByteArray out;
    const size_t half = size / 2;
    s.feed(reinterpret_cast<const char *>(data), qsizetype(half), out);
    s.feed(reinterpret_cast<const char *>(data + half), qsizetype(size - half), out);
    if (out.size() > qsizetype(size) * 2 + 16)
        __builtin_trap();
    return 0;
}
