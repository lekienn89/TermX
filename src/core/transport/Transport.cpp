// SPDX-License-Identifier: GPL-3.0-or-later
#include "Transport.h"

#include <QDeadlineTimer>
#include <QPointer>
#include <QThread>

#include <set>

namespace termx {

namespace {
std::mutex g_threadsMx;
std::set<QThread *> g_threads;
} // namespace

std::atomic<quint64> TransportWorker::s_nextPromptId{1};

QString transportStateName(TransportState s)
{
    switch (s) {
    case TransportState::Idle: return QStringLiteral("Idle");
    case TransportState::Connecting: return QStringLiteral("Connecting");
    case TransportState::Connected: return QStringLiteral("Connected");
    case TransportState::Disconnected: return QStringLiteral("Disconnected");
    case TransportState::Failed: return QStringLiteral("Failed");
    }
    return QString();
}

void registerTransportMetaTypes()
{
    qRegisterMetaType<TransportState>("termx::TransportState");
    qRegisterMetaType<PromptRequest>("termx::PromptRequest");
    qRegisterMetaType<PromptReply>("termx::PromptReply");
}

void TransportShared::wake()
{
    std::lock_guard<std::mutex> lock(mx);
    if (waker)
        waker();
}

// ---------------------------------------------------------------- worker

TransportWorker::TransportWorker() = default;

TransportWorker::~TransportWorker()
{
    if (m_shared) {
        std::lock_guard<std::mutex> lock(m_shared->mx);
        m_shared->waker = nullptr;
    }
}

void TransportWorker::attach(std::shared_ptr<TransportShared> shared)
{
    m_shared = std::move(shared);
    installWaker();
}

void TransportWorker::installWaker()
{
    std::lock_guard<std::mutex> lock(m_shared->mx);
    // Posting an event to a live QObject is thread-safe; the destructor clears
    // the waker under the same mutex, so `this` is alive whenever it runs.
    m_shared->waker = [this] {
        QMetaObject::invokeMethod(this, [this] { onWake(); }, Qt::QueuedConnection);
    };
}

bool TransportWorker::takeWrites(std::deque<QByteArray> &out)
{
    std::lock_guard<std::mutex> lock(m_shared->mx);
    if (m_shared->writes.empty())
        return false;
    for (auto &chunk : m_shared->writes)
        out.push_back(std::move(chunk));
    m_shared->writes.clear();
    m_shared->writeBytes = 0;
    return true;
}

void TransportWorker::emitData(const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return;
    m_shared->inFlight.fetch_add(bytes.size());
    Q_EMIT data(bytes);
}

std::optional<PromptReply> TransportWorker::ask(PromptRequest req)
{
    req.id = s_nextPromptId.fetch_add(1);
    const quint64 id = req.id;
    Q_EMIT prompt(req);
    std::unique_lock<std::mutex> lock(m_shared->mx);
    for (;;) {
        if (m_shared->stop.load())
            return std::nullopt;
        auto it = m_shared->replies.find(id);
        if (it != m_shared->replies.end()) {
            PromptReply r = std::move(it->second);
            m_shared->replies.erase(it);
            return r;
        }
        // Bounded wait so a missed notification can never hang the thread.
        m_shared->cv.wait_for(lock, std::chrono::milliseconds(200));
    }
}

void TransportWorker::setState(TransportState s, const QString &message)
{
    Q_EMIT state(s, message);
}

void TransportWorker::finish()
{
    if (m_finished)
        return;
    m_finished = true;
    {
        std::lock_guard<std::mutex> lock(m_shared->mx);
        m_shared->waker = nullptr;
    }
    Q_EMIT finished();
}

// ---------------------------------------------------------------- facade

Transport::Transport(QObject *parent)
    : QObject(parent), m_shared(std::make_shared<TransportShared>())
{
}

Transport::~Transport()
{
    close();
}

void Transport::open()
{
    if (m_opened)
        return;
    m_opened = true;

    TransportWorker *worker = createWorker();
    worker->attach(m_shared);

    m_thread = new QThread;  // no parent: it may outlive this facade
    m_thread->setObjectName(QStringLiteral("termx-io-") + kindName());
    worker->moveToThread(m_thread);

    QThread *thread = m_thread;
    {
        std::lock_guard<std::mutex> lock(g_threadsMx);
        g_threads.insert(thread);
    }

    connect(thread, &QThread::started, worker, &TransportWorker::start);
    connect(worker, &TransportWorker::finished, thread, &QThread::quit);
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, [thread] {
        {
            std::lock_guard<std::mutex> lock(g_threadsMx);
            g_threads.erase(thread);
        }
        thread->deleteLater();
    });

    connect(worker, &TransportWorker::data, this, [this](const QByteArray &b) { Q_EMIT dataReceived(b); });
    connect(worker, &TransportWorker::state, this, &Transport::onWorkerState);
    connect(worker, &TransportWorker::prompt, this, &Transport::promptRequested);
    connect(worker, &TransportWorker::notice, this, &Transport::notice);
    connect(worker, &TransportWorker::secretAccepted, this, &Transport::secretAccepted);
    connect(worker, &TransportWorker::legacyAlgorithmsAccepted, this, &Transport::legacyAlgorithmsAccepted);

    m_state = TransportState::Connecting;
    Q_EMIT stateChanged(m_state, QString());
    thread->start();
}

void Transport::close()
{
    if (!m_opened || m_shared->stop.load())
        return;
    m_shared->stop.store(true);
    m_shared->cv.notify_all();
    m_shared->wake();
    // Do not wait for the thread: it finishes and deletes itself.
    m_thread = nullptr;
}

bool Transport::write(const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return true;
    if (m_state != TransportState::Connected)
        return false;
    {
        std::lock_guard<std::mutex> lock(m_shared->mx);
        if (m_shared->writeBytes + size_t(bytes.size()) > TransportShared::kMaxQueuedWriteBytes)
            return false;
        m_shared->writes.push_back(bytes);
        m_shared->writeBytes += size_t(bytes.size());
    }
    m_shared->wake();
    return true;
}

void Transport::resize(int cols, int rows)
{
    if (cols < 1 || rows < 1)
        return;
    m_shared->cols.store(cols);
    m_shared->rows.store(rows);
    m_shared->resizePending.store(true);
    m_shared->wake();
}

void Transport::sendBreak()
{
    if (!supportsBreak() || m_state != TransportState::Connected)
        return;
    m_shared->breakPending.store(true);
    m_shared->wake();
}

void Transport::answerPrompt(quint64 id, const PromptReply &reply)
{
    {
        std::lock_guard<std::mutex> lock(m_shared->mx);
        m_shared->replies[id] = reply;
    }
    m_shared->cv.notify_all();
}

void Transport::acknowledge(qint64 bytes)
{
    const qint64 left = m_shared->inFlight.fetch_sub(bytes) - bytes;
    if (left < 0)
        m_shared->inFlight.store(0);
    if (left <= TransportShared::kMaxInFlightBytes)
        m_shared->wake();
}

void Transport::onWorkerState(TransportState s, const QString &message)
{
    m_state = s;
    Q_EMIT stateChanged(s, message);
}

bool Transport::shutdownAll(int timeoutMs)
{
    QDeadlineTimer deadline(timeoutMs);
    for (;;) {
        QThread *t = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_threadsMx);
            for (QThread *candidate : g_threads) {
                if (candidate->isRunning()) {
                    t = candidate;
                    break;
                }
            }
        }
        if (!t)
            return true;
        if (!t->wait(deadline))
            return false;
        std::lock_guard<std::mutex> lock(g_threadsMx);
        g_threads.erase(t);
    }
}

} // namespace termx
