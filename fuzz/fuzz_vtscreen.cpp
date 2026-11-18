// SPDX-License-Identifier: GPL-3.0-or-later
// Feeds arbitrary host output to the terminal emulator, with resizes and
// reads in between, the way the UI would.
#include "terminal/VtScreen.h"

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2)
        return 0;
    termx::VtScreen s(2 + data[0] % 60, 2 + data[1] % 200, 200);
    s.onOutput = [](const char *, size_t) {};
    data += 2;
    size -= 2;
    std::vector<termx::Cell> cells;
    size_t pos = 0;
    while (pos < size) {
        const size_t chunk = 1 + (data[pos] % 64);
        const size_t n = chunk < size - pos ? chunk : size - pos;
        s.feed(reinterpret_cast<const char *>(data + pos), n);
        pos += n;
        if (pos < size && data[pos - 1] == 0xfe) {
            s.resize(1 + data[pos] % 50, 1 + data[pos] % 170);
        }
        for (qint64 l = s.firstAvailableLine(); l < s.firstScreenLine() + s.rows(); ++l) {
            s.line(l, cells);
            (void)s.lineContinuesNext(l);
        }
        (void)s.lineText(s.firstScreenLine() + s.cursor().row);
    }
    s.keyboardChar('a', 0);
    s.startPaste();
    s.endPaste();
    return 0;
}
