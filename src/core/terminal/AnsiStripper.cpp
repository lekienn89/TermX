// SPDX-License-Identifier: GPL-3.0-or-later
#include "AnsiStripper.h"

namespace termx {

void AnsiStripper::feed(const char *data, qsizetype len, QByteArray &out)
{
    m_lineStart = out.lastIndexOf('\n') + 1;
    for (qsizetype i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(data[i]);
        switch (m_st) {
        case St::Ground:
            if (c == 0x1b) {
                m_st = St::Esc;
            } else if (c == '\r') {
                m_pendingCR = true;
            } else if (c == '\n') {
                out.append('\n');
                m_lineStart = out.size();
                m_pendingCR = false;
            } else if (c == '\b') {
                if (out.size() > m_lineStart) {
                    // Remove one UTF-8 character.
                    qsizetype p = out.size() - 1;
                    while (p > m_lineStart && (static_cast<unsigned char>(out.at(p)) & 0xC0) == 0x80)
                        --p;
                    out.truncate(p);
                }
            } else if (c == '\t' || c >= 0x20) {
                if (m_pendingCR) {
                    // Bare CR: the line is being rewritten; keep what we have.
                    m_pendingCR = false;
                }
                if (c != 0x7f)
                    out.append(char(c));
            }
            break;
        case St::Esc:
            if (c == '[') m_st = St::Csi;
            else if (c == ']') m_st = St::Osc;
            else if (c == 'P' || c == 'X' || c == '^' || c == '_') m_st = St::Str;
            else if (c >= 0x20 && c <= 0x2f) m_st = St::EscIntermediate;
            else m_st = St::Ground;
            break;
        case St::EscIntermediate:
            if (c >= 0x30 && c <= 0x7e)
                m_st = St::Ground;
            break;
        case St::Csi:
            if (c >= 0x40 && c <= 0x7e)
                m_st = St::Ground;
            else if (c == 0x1b)
                m_st = St::Esc;
            break;
        case St::Osc:
            if (c == 0x07) m_st = St::Ground;
            else if (c == 0x1b) m_st = St::OscEsc;
            break;
        case St::OscEsc:
            m_st = (c == '\\') ? St::Ground : St::Osc;
            break;
        case St::Str:
            if (c == 0x1b) m_st = St::StrEsc;
            break;
        case St::StrEsc:
            m_st = (c == '\\') ? St::Ground : St::Str;
            break;
        }
    }
}

} // namespace termx
