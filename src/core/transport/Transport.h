// TermX - SSH / Telnet / Serial terminal
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

class QThread;

namespace termx {

enum class TransportState {
    Idle,          // created, open() not called yet
    Connecting,    // resolving / handshaking / authenticating
    Connected,     // data flows
    Disconnected,  // closed by the remote side, the user, or the device was removed
    Failed         // could not connect (message says why)
};

QString transportStateName(TransportState s);

struct PromptField {
    QString label;
    bool echo = false;          // false = secret (password)
    QString initialValue;
};

// A question the I/O thread needs the user to answer (password, host key...).
struct PromptRequest {
    enum class Kind {
        Fields,          // one or more text fields (username, password, kbd-interactive)
        HostKeyUnknown,  // first connection to a host: trust this key?
        HostKeyChanged   // key differs from known_hosts: possible MITM
    };
    quint64 id = 0;
    Kind kind = Kind::Fields;
    QString title;
    QString message;
    QVector<PromptField> fields;
    bool offerRemember = false;  // show "Save password" checkbox
};

struct PromptReply {
    bool accepted = false;
    QStringList values;
    bool remember = false;   // Fields: save password; HostKey*: save to known_hosts
};

// State shared between the Transport facade (GUI thread) and its worker
// (I/O thread). Everything here is either atomic or guarded by `mx`.
struct TransportShared {
    std::mutex mx;
    std::condition_variable cv;

    std::deque<QByteArray> writes;     // guarded by mx
    size_t writeBytes = 0;             // guarded by mx
    std::map<quint64, PromptReply> replies;  // guarded by mx
    std::function<void()> waker;       // guarded by mx; set by worker while alive

    std::atomic<bool> stop{false};
    std::atomic<bool> resizePending{false};
    std::atomic<int> cols{80};
    std::atomic<int> rows{24};
    std::atomic<bool> breakPending{false};
    std::atomic<qint64> inFlight{0};   // bytes emitted but not yet consumed by the UI

    static constexpr size_t kMaxQueuedWriteBytes = 8 * 1024 * 1024;
    static constexpr qint64 kMaxInFlightBytes = 4 * 1024 * 1024;

    void wake();
};

// Base class for the object that lives on the I/O thread.
class TransportWorker : public QObject {
    Q_OBJECT
public:
    TransportWorker();
    ~TransportWorker() override;

    void attach(std::shared_ptr<TransportShared> shared);

public Q_SLOTS:
    // Runs on the I/O thread. Blocking workers stay inside start() until
    // stopped; event-driven workers return and rely on the thread's event loop.
    virtual void start() = 0;

Q_SIGNALS:
    void data(const QByteArray &bytes);
    void state(termx::TransportState s, const QString &message);
    void prompt(const termx::PromptRequest &req);
    void notice(const QString &text);
    void secretAccepted(const QString &secret);  // a password the user asked to remember worked
    void finished();

protected:
    // Called from any thread; must be thread-safe. Default: queued call to onWake().
    virtual void installWaker();
    // Event-driven workers: called on the I/O thread after a wake.
    virtual void onWake() {}

    bool stopRequested() const { return m_shared->stop.load(); }
    bool takeWrites(std::deque<QByteArray> &out);
    void emitData(const QByteArray &bytes);
    bool flowBlocked() const { return m_shared->inFlight.load() > TransportShared::kMaxInFlightBytes; }
    // Blocks the I/O thread until the user answers or stop is requested.
    std::optional<PromptReply> ask(PromptRequest req);
    void setState(TransportState s, const QString &message = QString());
    void finish();

    std::shared_ptr<TransportShared> m_shared;

private:
    bool m_finished = false;
    static std::atomic<quint64> s_nextPromptId;
};

// GUI-thread facade. Owns nothing that can block: all I/O happens on a
// dedicated thread that is detached on close, so a hung driver or socket can
// never freeze the window.
class Transport : public QObject {
    Q_OBJECT
public:
    explicit Transport(QObject *parent = nullptr);
    ~Transport() override;

    void open();
    void close();
    bool write(const QByteArray &bytes);
    void resize(int cols, int rows);
    void sendBreak();
    void answerPrompt(quint64 id, const PromptReply &reply);
    void acknowledge(qint64 bytes);

    TransportState state() const { return m_state; }
    bool isConnected() const { return m_state == TransportState::Connected; }
    virtual bool supportsBreak() const { return false; }
    virtual QString kindName() const = 0;

    // Waits (bounded) for every I/O thread still alive; call once at exit.
    // Returns false if some thread is stuck (e.g. inside a broken driver).
    static bool shutdownAll(int timeoutMs);

Q_SIGNALS:
    void dataReceived(const QByteArray &bytes);
    void stateChanged(termx::TransportState s, const QString &message);
    void promptRequested(const termx::PromptRequest &req);
    void notice(const QString &text);
    void secretAccepted(const QString &secret);

protected:
    virtual TransportWorker *createWorker() = 0;

private:
    void onWorkerState(TransportState s, const QString &message);

    std::shared_ptr<TransportShared> m_shared;
    QThread *m_thread = nullptr;
    TransportState m_state = TransportState::Idle;
    bool m_opened = false;
};

void registerTransportMetaTypes();

} // namespace termx

Q_DECLARE_METATYPE(termx::TransportState)
Q_DECLARE_METATYPE(termx::PromptRequest)
Q_DECLARE_METATYPE(termx::PromptReply)
