// SPDX-License-Identifier: GPL-3.0-or-later
// One terminal tab's model: a transport, a VtScreen, history capture and logging.
#pragma once

#include "history/CommandCapture.h"
#include "session/SessionConfig.h"
#include "terminal/SessionLogger.h"
#include "terminal/VtScreen.h"
#include "transport/Transport.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <deque>
#include <memory>

namespace termx {

class HistoryStore;

class TerminalSession : public QObject {
    Q_OBJECT
public:
    TerminalSession(const SessionConfig &cfg, HistoryStore *history, QObject *parent = nullptr);
    ~TerminalSession() override;

    const SessionConfig &config() const { return m_cfg; }
    void setConfig(const SessionConfig &cfg) { m_cfg = cfg; }
    VtScreen &screen() { return m_screen; }
    const VtScreen &screen() const { return m_screen; }
    TransportState state() const { return m_transport ? m_transport->state() : TransportState::Idle; }
    QString statusMessage() const { return m_statusMessage; }
    QString title() const;

    void start();
    void stop();
    void disconnectByUser();                 // stop + tell the user
    void restart();                          // reconnect with the same settings
    void restartOnPort(const QString &port); // serial: device came back on another COM number

    // --- user input ---
    void userKey(int vtermKey, int mods);
    void userText(const QString &text, int mods);
    // Control bytes computed by the UI (Ctrl+letter, Ctrl+Shift+6 ...), sent verbatim.
    void userRaw(const QByteArray &bytes);
    void paste(const QString &text);
    void sendBreak();
    bool supportsBreak() const { return m_transport && m_transport->supportsBreak(); }
    void resize(int cols, int rows);
    void answerPrompt(quint64 id, const PromptReply &reply);
    void focusChanged(bool in);

    // --- history ---
    QString currentInput() const { return m_capture.currentInput(m_screen); }
    void acceptSuggestion(const QString &remainder);   // type the rest of a suggestion
    void sendCommand(const QString &command, bool execute);

    // --- logging ---
    bool startLog(const QString &path = QString(), QString *error = nullptr);
    void stopLog();
    bool isLogging() const { return m_logger.isActive(); }
    QString logPath() const { return m_logger.path(); }

    void setPalette(uint32_t fg, uint32_t bg, const std::array<uint32_t, 16> &ansi);

Q_SIGNALS:
    void screenUpdated();
    void scrollbackChanged();
    void stateChanged(termx::TransportState s, const QString &message);
    void titleChanged(const QString &title);
    void bell();
    void promptRequested(const termx::PromptRequest &req);
    void notice(const QString &text);
    void commandExecuted(const QString &command);
    void secretAccepted(const QString &secret);
    void inputChanged();      // the user typed: suggestion popup may update

private:
    void createTransport();
    void onData(const QByteArray &bytes);
    void processPending();
    void writeToHost(const QByteArray &bytes);
    void writeLocalNotice(const QString &text);
    QByteArray enterBytes() const;
    void pumpPaste();

    SessionConfig m_cfg;
    QPointer<HistoryStore> m_history;
    std::unique_ptr<Transport> m_transport;
    VtScreen m_screen;
    CommandCapture m_capture;
    SessionLogger m_logger;
    QString m_statusMessage;
    QString m_savedPassword;

    std::deque<QByteArray> m_pending;
    qint64 m_pendingBytes = 0;
    bool m_processScheduled = false;

    QByteArray m_pasteQueue;
    QTimer m_pasteTimer;
    int m_cols = 80;
    int m_rows = 24;
};

} // namespace termx
