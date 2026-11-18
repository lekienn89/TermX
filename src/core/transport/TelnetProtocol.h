// SPDX-License-Identifier: GPL-3.0-or-later
// Telnet (RFC 854) option negotiation, independent of any socket so it can be
// unit-tested and fuzzed.
#pragma once

#include <QByteArray>
#include <bitset>

namespace termx {

class TelnetProtocol {
public:
    enum : unsigned char {
        SE = 240, NOP = 241, DM = 242, BRK = 243, IP = 244, AO = 245, AYT = 246, EC = 247, EL = 248, GA = 249,
        SB = 250, WILL = 251, WONT = 252, DO = 253, DONT = 254, IAC = 255
    };
    enum : unsigned char { OPT_BINARY = 0, OPT_ECHO = 1, OPT_SGA = 3, OPT_TTYPE = 24, OPT_NAWS = 31 };

    void setTerminalType(const QByteArray &t) { m_termType = t; }
    void setWindowSize(int cols, int rows);

    // Options we offer when the connection opens.
    QByteArray initialNegotiation();

    // Feed received bytes. Application data is appended to `data`, protocol
    // replies that must be sent back are appended to `reply`.
    void feed(const char *bytes, qsizetype len, QByteArray &data, QByteArray &reply);

    // Escape outgoing user data (IAC doubling, CR -> CR NUL in NVT mode).
    QByteArray encode(const QByteArray &in) const;

    // NAWS sub-negotiation for the current size, or empty if not negotiated.
    QByteArray windowSizeMessage() const;
    static QByteArray breakCommand() { return QByteArray("\xff\xf3", 2); }

    bool remoteEcho() const { return m_remote.test(OPT_ECHO); }
    bool localOption(unsigned char opt) const { return m_local.test(opt); }
    bool remoteOption(unsigned char opt) const { return m_remote.test(opt); }

private:
    void handleCommand(unsigned char cmd, unsigned char opt, QByteArray &reply);
    void handleSubnegotiation(QByteArray &reply);
    static void appendCmd(QByteArray &out, unsigned char cmd, unsigned char opt);

    enum class St { Data, Iac, Opt, Sb, SbIac } m_st = St::Data;
    unsigned char m_cmd = 0;
    bool m_lastWasCR = false;
    QByteArray m_sb;
    std::bitset<256> m_local;          // options enabled on our side (we said WILL, they said DO)
    std::bitset<256> m_remote;         // options enabled on their side (they said WILL, we said DO)
    std::bitset<256> m_refusedLocal;   // we already sent WONT
    std::bitset<256> m_refusedRemote;  // we already sent DONT
    QByteArray m_termType = "XTERM-256COLOR";
    int m_cols = 80;
    int m_rows = 24;
    static constexpr qsizetype kMaxSb = 4096;
};

} // namespace termx
