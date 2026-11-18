// SPDX-License-Identifier: GPL-3.0-or-later
#include "TerminalSession.h"

#include "history/HistoryStore.h"
#include "session/CredentialStore.h"
#include "transport/LocalShellTransport.h"
#include "transport/SerialTransport.h"
#include "transport/SshTransport.h"
#include "transport/TelnetTransport.h"
#include "util/AppPaths.h"

#include <vterm_keycodes.h>

namespace termx {

namespace {
constexpr qint64 kSliceBytes = 256 * 1024;   // parsed per event-loop turn
}

TerminalSession::TerminalSession(const SessionConfig &cfg, HistoryStore *history, QObject *parent)
    : QObject(parent), m_cfg(cfg), m_history(history), m_screen(24, 80, cfg.terminal.scrollbackLines)
{
    m_screen.onOutput = [this](const char *s, size_t len) { writeToHost(QByteArray(s, int(len))); };
    m_screen.onBell = [this] { Q_EMIT bell(); };
    m_screen.onTitle = [this](const QString &t) { Q_EMIT titleChanged(t); };
    m_screen.onScrollbackChanged = [this] { Q_EMIT scrollbackChanged(); };

    m_pasteTimer.setSingleShot(true);
    connect(&m_pasteTimer, &QTimer::timeout, this, &TerminalSession::pumpPaste);

    if (cfg.logEnabled)
        startLog();
}

TerminalSession::~TerminalSession()
{
    stop();
}

QString TerminalSession::title() const
{
    const QString t = m_screen.title();
    return t.isEmpty() ? m_cfg.displayName() : t;
}

void TerminalSession::createTransport()
{
    m_savedPassword = CredentialStore::decrypt(m_cfg.encryptedPassword);
    switch (m_cfg.type) {
    case SessionType::Ssh:
        m_transport = std::make_unique<SshTransport>(m_cfg.ssh, m_savedPassword, AppPaths::knownHostsFile());
        break;
    case SessionType::Telnet:
        m_transport = std::make_unique<TelnetTransport>(m_cfg.telnet);
        break;
    case SessionType::Serial:
        m_transport = std::make_unique<SerialTransport>(m_cfg.serial);
        break;
    case SessionType::LocalShell:
        m_transport = std::make_unique<LocalShellTransport>(m_cfg.local);
        break;
    }
    Transport *t = m_transport.get();
    connect(t, &Transport::dataReceived, this, &TerminalSession::onData);
    connect(t, &Transport::stateChanged, this, [this, t](TransportState s, const QString &msg) {
        if (t != m_transport.get())
            return;
        m_statusMessage = msg;
        if (s == TransportState::Connected) {
            m_transport->resize(m_cols, m_rows);
            if (m_logger.isActive())
                m_logger.writeMarker(QStringLiteral("Connected to %1").arg(m_cfg.target()));
        } else if (s == TransportState::Disconnected || s == TransportState::Failed) {
            processPending();
            const QString what = s == TransportState::Failed ? QStringLiteral("Connection failed") : QStringLiteral("Disconnected");
            writeLocalNotice(msg.isEmpty() ? what : what + QStringLiteral(": ") + msg);
            writeLocalNotice(QStringLiteral("Press Enter to reconnect."));
            if (m_logger.isActive())
                m_logger.writeMarker(what);
            m_capture.reset();
        }
        Q_EMIT stateChanged(s, msg);
    });
    connect(t, &Transport::promptRequested, this, &TerminalSession::promptRequested);
    connect(t, &Transport::notice, this, [this](const QString &n) {
        writeLocalNotice(n);
        Q_EMIT notice(n);
    });
    connect(t, &Transport::secretAccepted, this, &TerminalSession::secretAccepted);
}

void TerminalSession::start()
{
    if (m_transport && (m_transport->state() == TransportState::Connecting || m_transport->state() == TransportState::Connected))
        return;
    m_pending.clear();
    m_pendingBytes = 0;
    createTransport();
    m_transport->resize(m_cols, m_rows);
    m_transport->open();
}

void TerminalSession::stop()
{
    m_pasteTimer.stop();
    m_pasteQueue.clear();
    if (m_transport) {
        Transport *t = m_transport.release();
        t->disconnect(this);
        t->close();
        t->deleteLater();
    }
}

void TerminalSession::disconnectByUser()
{
    if (!m_transport)
        return;
    stop();
    m_statusMessage = QStringLiteral("Disconnected");
    writeLocalNotice(QStringLiteral("Disconnected. Press Enter to reconnect."));
    m_capture.reset();
    Q_EMIT stateChanged(TransportState::Disconnected, m_statusMessage);
}

void TerminalSession::restart()
{
    stop();
    writeLocalNotice(QStringLiteral("Reconnecting to %1...").arg(m_cfg.target()));
    start();
}

void TerminalSession::restartOnPort(const QString &port)
{
    m_cfg.serial.portName = port;
    restart();
}

void TerminalSession::onData(const QByteArray &bytes)
{
    m_pending.push_back(bytes);
    m_pendingBytes += bytes.size();
    if (!m_processScheduled) {
        m_processScheduled = true;
        QTimer::singleShot(0, this, &TerminalSession::processPending);
    }
}

void TerminalSession::processPending()
{
    m_processScheduled = false;
    qint64 budget = kSliceBytes;
    qint64 consumed = 0;
    while (!m_pending.empty() && budget > 0) {
        QByteArray &chunk = m_pending.front();
        if (chunk.size() <= budget) {
            m_screen.feed(chunk.constData(), size_t(chunk.size()));
            m_logger.write(chunk);
            budget -= chunk.size();
            consumed += chunk.size();
            m_pending.pop_front();
        } else {
            const QByteArray part = chunk.left(int(budget));
            m_screen.feed(part.constData(), size_t(part.size()));
            m_logger.write(part);
            chunk.remove(0, int(budget));
            consumed += budget;
            budget = 0;
        }
    }
    m_pendingBytes -= consumed;
    if (m_transport && consumed > 0)
        m_transport->acknowledge(consumed);
    if (m_screen.takeDirty())
        Q_EMIT screenUpdated();
    if (!m_pending.empty() && !m_processScheduled) {
        m_processScheduled = true;
        QTimer::singleShot(0, this, &TerminalSession::processPending);
    }
}

void TerminalSession::writeToHost(const QByteArray &bytes)
{
    if (!m_transport || !m_transport->isConnected())
        return;
    if (!m_transport->write(bytes))
        Q_EMIT notice(QStringLiteral("Send buffer is full; input was dropped."));
}

void TerminalSession::writeLocalNotice(const QString &text)
{
    const QByteArray msg = "\r\n\x1b[0;33m[TermX] " + text.toUtf8() + "\x1b[0m\r\n";
    m_screen.feed(msg.constData(), size_t(msg.size()));
    if (m_screen.takeDirty())
        Q_EMIT screenUpdated();
}

QByteArray TerminalSession::enterBytes() const
{
    if (m_cfg.type == SessionType::Ssh || m_cfg.type == SessionType::LocalShell)
        return QByteArray("\r");
    switch (m_cfg.terminal.enterSends) {
    case NewlineMode::LF: return QByteArray("\n");
    case NewlineMode::CRLF: return QByteArray("\r\n");
    case NewlineMode::CR: break;
    }
    return QByteArray("\r");
}

void TerminalSession::userKey(int key, int mods)
{
    const bool isEnter = (key == VTERM_KEY_ENTER || key == VTERM_KEY_KP_ENTER) && (mods & ~VTERM_MOD_SHIFT) == 0;
    if (isEnter) {
        const TransportState s = state();
        if (s == TransportState::Disconnected || s == TransportState::Failed || s == TransportState::Idle) {
            restart();
            return;
        }
        if (m_cfg.historyEnabled) {
            const auto r = m_capture.onKey(m_screen, true, false, QString(), false);
            if (r.valid) {
                if (m_history)
                    m_history->add(r.command, m_cfg.historyHost(), sessionTypeName(m_cfg.type));
                Q_EMIT commandExecuted(r.command);
            }
        }
        writeToHost(enterBytes());
        if (m_cfg.terminal.localEcho) {
            m_screen.feed("\r\n", 2);
            Q_EMIT screenUpdated();
        }
        Q_EMIT inputChanged();
        return;
    }
    if (m_cfg.historyEnabled) {
        const QString text = key == VTERM_KEY_BACKSPACE ? QStringLiteral("\x7f") : QString();
        m_capture.onKey(m_screen, false, false, text, true);
    }
    m_screen.keyboardKey(key, mods);
    Q_EMIT inputChanged();
}

void TerminalSession::userText(const QString &text, int mods)
{
    const auto ucs4 = text.toUcs4();
    for (uint ch : ucs4) {
        if (m_cfg.historyEnabled) {
            const bool ctrl = (mods & VTERM_MOD_CTRL) != 0;
            const bool alt = (mods & VTERM_MOD_ALT) != 0;
            QString t;
            bool printable = false;
            if (ctrl && ch < 128 && QChar(char16_t(ch)).isLetter())
                t = QString(QChar(char16_t(ch & 0x1f)));
            else if (!ctrl && !alt && ch >= 0x20 && ch != 0x7f) {
                t = QString::fromUcs4(reinterpret_cast<const char32_t *>(&ch), 1);
                printable = true;
            }
            m_capture.onKey(m_screen, false, printable, t, !printable);
        }
        m_screen.keyboardChar(ch, mods);
        if (m_cfg.terminal.localEcho && !(mods & (VTERM_MOD_CTRL | VTERM_MOD_ALT)) && ch >= 0x20) {
            const QByteArray b = QString::fromUcs4(reinterpret_cast<const char32_t *>(&ch), 1).toUtf8();
            m_screen.feed(b.constData(), size_t(b.size()));
        }
    }
    if (m_screen.takeDirty())
        Q_EMIT screenUpdated();
    Q_EMIT inputChanged();
}

void TerminalSession::userRaw(const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return;
    if (m_cfg.historyEnabled) {
        const QString t = QString::fromLatin1(bytes.right(1));
        if (bytes.size() == 1 && (bytes.at(0) == '\x7f' || bytes.at(0) == '\b'))
            m_capture.onKey(m_screen, false, false, QStringLiteral("\x7f"), true);
        else
            m_capture.onKey(m_screen, false, false, t, true);
    }
    writeToHost(bytes);
    Q_EMIT inputChanged();
}

void TerminalSession::paste(const QString &text)
{
    if (text.isEmpty() || !m_transport || !m_transport->isConnected())
        return;
    QString t = text;
    t.replace(QStringLiteral("\r\n"), QStringLiteral("\r"));
    t.replace(QLatin1Char('\n'), QLatin1Char('\r'));
    if (m_cfg.historyEnabled)
        m_capture.onKey(m_screen, false, false, QString(), true);

    const QByteArray bytes = t.toUtf8();
    const bool delayed = m_cfg.type == SessionType::Serial && (m_cfg.serial.charDelayMs > 0 || m_cfg.serial.lineDelayMs > 0);
    if (delayed) {
        m_pasteQueue += bytes;
        if (!m_pasteTimer.isActive())
            pumpPaste();
        return;
    }
    m_screen.startPaste();
    writeToHost(bytes);
    m_screen.endPaste();
}

void TerminalSession::pumpPaste()
{
    if (m_pasteQueue.isEmpty() || !m_transport || !m_transport->isConnected()) {
        m_pasteQueue.clear();
        return;
    }
    QByteArray piece;
    int delay = 0;
    if (m_cfg.serial.charDelayMs > 0) {
        // One UTF-8 character.
        int n = 1;
        while (n < m_pasteQueue.size() && (static_cast<unsigned char>(m_pasteQueue.at(n)) & 0xC0) == 0x80)
            ++n;
        piece = m_pasteQueue.left(n);
        delay = m_cfg.serial.charDelayMs;
        if (piece == "\r")
            delay = qMax(delay, m_cfg.serial.lineDelayMs);
    } else {
        const int cr = m_pasteQueue.indexOf('\r');
        piece = cr < 0 ? m_pasteQueue : m_pasteQueue.left(cr + 1);
        delay = m_cfg.serial.lineDelayMs;
    }
    m_pasteQueue.remove(0, piece.size());
    if (piece.endsWith('\r'))
        piece = piece.left(piece.size() - 1) + enterBytes();
    writeToHost(piece);
    if (!m_pasteQueue.isEmpty())
        m_pasteTimer.start(qMax(1, delay));
}

void TerminalSession::sendBreak()
{
    if (m_transport)
        m_transport->sendBreak();
}

void TerminalSession::resize(int cols, int rows)
{
    if (cols == m_cols && rows == m_rows)
        return;
    m_cols = cols;
    m_rows = rows;
    m_screen.resize(rows, cols);
    if (m_transport)
        m_transport->resize(cols, rows);
}

void TerminalSession::answerPrompt(quint64 id, const PromptReply &reply)
{
    if (m_transport)
        m_transport->answerPrompt(id, reply);
}

void TerminalSession::focusChanged(bool in)
{
    if (in)
        m_screen.focusIn();
    else
        m_screen.focusOut();
}

void TerminalSession::acceptSuggestion(const QString &remainder)
{
    userText(remainder, 0);
}

void TerminalSession::sendCommand(const QString &command, bool execute)
{
    if (!m_transport || !m_transport->isConnected())
        return;
    userText(command, 0);
    if (execute)
        userKey(VTERM_KEY_ENTER, 0);
}

bool TerminalSession::startLog(const QString &path, QString *error)
{
    const QString dir = m_cfg.logDirectory.isEmpty() ? AppPaths::logsDir() : m_cfg.logDirectory;
    m_logger.setTimestamps(m_cfg.logTimestamps);
    return m_logger.start(path.isEmpty() ? SessionLogger::defaultPath(dir, m_cfg.displayName()) : path, error);
}

void TerminalSession::stopLog()
{
    m_logger.stop();
}

void TerminalSession::setPalette(uint32_t fg, uint32_t bg, const std::array<uint32_t, 16> &ansi)
{
    m_screen.setPalette(fg, bg, ansi);
    Q_EMIT screenUpdated();
}

} // namespace termx
