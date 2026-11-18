// SPDX-License-Identifier: GPL-3.0-or-later
#include "TelnetProtocol.h"

namespace termx {

void TelnetProtocol::appendCmd(QByteArray &out, unsigned char cmd, unsigned char opt)
{
    out.append(char(IAC));
    out.append(char(cmd));
    out.append(char(opt));
}

void TelnetProtocol::setWindowSize(int cols, int rows)
{
    m_cols = qBound(1, cols, 65535);
    m_rows = qBound(1, rows, 65535);
}

QByteArray TelnetProtocol::initialNegotiation()
{
    QByteArray out;
    appendCmd(out, WILL, OPT_NAWS);
    appendCmd(out, WILL, OPT_TTYPE);
    appendCmd(out, DO, OPT_SGA);
    return out;
}

QByteArray TelnetProtocol::windowSizeMessage() const
{
    if (!m_local.test(OPT_NAWS))
        return QByteArray();
    QByteArray out;
    out.append(char(IAC));
    out.append(char(SB));
    out.append(char(OPT_NAWS));
    const unsigned char sz[4] = {static_cast<unsigned char>(m_cols >> 8), static_cast<unsigned char>(m_cols & 0xff),
                                 static_cast<unsigned char>(m_rows >> 8), static_cast<unsigned char>(m_rows & 0xff)};
    for (unsigned char b : sz) {
        out.append(char(b));
        if (b == IAC)
            out.append(char(IAC));
    }
    out.append(char(IAC));
    out.append(char(SE));
    return out;
}

void TelnetProtocol::handleCommand(unsigned char cmd, unsigned char opt, QByteArray &reply)
{
    const bool weSupport = opt == OPT_TTYPE || opt == OPT_NAWS || opt == OPT_SGA || opt == OPT_BINARY;
    const bool theyMay = opt == OPT_ECHO || opt == OPT_SGA || opt == OPT_BINARY;
    switch (cmd) {
    case DO:
        if (weSupport) {
            if (!m_local.test(opt)) {
                m_local.set(opt);
                appendCmd(reply, WILL, opt);
            }
            if (opt == OPT_NAWS)
                reply += windowSizeMessage();
        } else if (!m_refusedLocal.test(opt)) {
            m_refusedLocal.set(opt);
            appendCmd(reply, WONT, opt);
        }
        break;
    case DONT:
        if (m_local.test(opt)) {
            m_local.reset(opt);
            appendCmd(reply, WONT, opt);
        }
        break;
    case WILL:
        if (theyMay) {
            if (!m_remote.test(opt)) {
                m_remote.set(opt);
                appendCmd(reply, DO, opt);
            }
        } else if (!m_refusedRemote.test(opt)) {
            m_refusedRemote.set(opt);
            appendCmd(reply, DONT, opt);
        }
        break;
    case WONT:
        if (m_remote.test(opt)) {
            m_remote.reset(opt);
            appendCmd(reply, DONT, opt);
        }
        break;
    default:
        break;
    }
}

void TelnetProtocol::handleSubnegotiation(QByteArray &reply)
{
    if (m_sb.size() >= 2 && static_cast<unsigned char>(m_sb.at(0)) == OPT_TTYPE && m_sb.at(1) == 1 /* SEND */) {
        reply.append(char(IAC));
        reply.append(char(SB));
        reply.append(char(OPT_TTYPE));
        reply.append(char(0)); // IS
        reply.append(m_termType);
        reply.append(char(IAC));
        reply.append(char(SE));
    }
    m_sb.clear();
}

void TelnetProtocol::feed(const char *bytes, qsizetype len, QByteArray &data, QByteArray &reply)
{
    for (qsizetype i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        switch (m_st) {
        case St::Data:
            if (c == IAC) {
                m_st = St::Iac;
            } else if (c == 0 && m_lastWasCR && !m_remote.test(OPT_BINARY)) {
                m_lastWasCR = false;   // CR NUL -> CR
            } else {
                data.append(char(c));
                m_lastWasCR = (c == '\r');
            }
            break;
        case St::Iac:
            switch (c) {
            case IAC:
                data.append(char(IAC));
                m_lastWasCR = false;
                m_st = St::Data;
                break;
            case WILL: case WONT: case DO: case DONT:
                m_cmd = c;
                m_st = St::Opt;
                break;
            case SB:
                m_sb.clear();
                m_st = St::Sb;
                break;
            default:   // NOP, GA, DM, AYT ... ignored
                m_st = St::Data;
                break;
            }
            break;
        case St::Opt:
            handleCommand(m_cmd, c, reply);
            m_st = St::Data;
            break;
        case St::Sb:
            if (c == IAC)
                m_st = St::SbIac;
            else if (m_sb.size() < kMaxSb)
                m_sb.append(char(c));
            break;
        case St::SbIac:
            if (c == SE) {
                handleSubnegotiation(reply);
                m_st = St::Data;
            } else if (c == IAC) {
                if (m_sb.size() < kMaxSb)
                    m_sb.append(char(IAC));
                m_st = St::Sb;
            } else {
                // Malformed: treat as end of sub-negotiation.
                m_sb.clear();
                m_st = St::Data;
            }
            break;
        }
    }
}

QByteArray TelnetProtocol::encode(const QByteArray &in) const
{
    QByteArray out;
    out.reserve(in.size() + 8);
    const bool binary = m_local.test(OPT_BINARY);
    for (qsizetype i = 0; i < in.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(in.at(i));
        if (c == IAC) {
            out.append(char(IAC));
            out.append(char(IAC));
        } else if (c == '\r' && !binary) {
            out.append('\r');
            const bool nextIsLf = i + 1 < in.size() && in.at(i + 1) == '\n';
            if (!nextIsLf)
                out.append('\0');
        } else {
            out.append(char(c));
        }
    }
    return out;
}

} // namespace termx
