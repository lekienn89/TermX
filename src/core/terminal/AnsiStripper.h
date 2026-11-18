// SPDX-License-Identifier: GPL-3.0-or-later
// Turns raw terminal output into plain text for session logs.
#pragma once

#include <QByteArray>

namespace termx {

class AnsiStripper {
public:
    // Appends plain text (UTF-8 bytes, '\n' line ends) to `out`.
    void feed(const char *data, qsizetype len, QByteArray &out);

private:
    enum class St { Ground, Esc, EscIntermediate, Csi, Osc, OscEsc, Str, StrEsc } m_st = St::Ground;
    bool m_pendingCR = false;
    qsizetype m_lineStart = 0;  // index in `out` where the current line starts (per feed call)
};

} // namespace termx
