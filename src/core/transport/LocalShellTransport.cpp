// SPDX-License-Identifier: GPL-3.0-or-later
#include "LocalShellTransport.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <QFile>

#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif
#endif

namespace termx {

QString LocalShellTransport::defaultProgram()
{
#ifdef _WIN32
    return QStringLiteral("powershell.exe");
#else
    const QByteArray sh = qgetenv("SHELL");
    return sh.isEmpty() ? QStringLiteral("/bin/sh") : QString::fromLocal8Bit(sh);
#endif
}

namespace {

#ifdef _WIN32
// ------------------------------------------------------------------ ConPTY

using HPCON_T = void *;
using PFN_CreatePseudoConsole = HRESULT(WINAPI *)(COORD, HANDLE, HANDLE, DWORD, HPCON_T *);
using PFN_ResizePseudoConsole = HRESULT(WINAPI *)(HPCON_T, COORD);
using PFN_ClosePseudoConsole = void(WINAPI *)(HPCON_T);
constexpr DWORD_PTR kProcThreadAttributePseudoConsole = 0x00020016;

struct ConPtyApi {
    PFN_CreatePseudoConsole create = nullptr;
    PFN_ResizePseudoConsole resize = nullptr;
    PFN_ClosePseudoConsole close = nullptr;
    bool ok() const { return create && resize && close; }
    static const ConPtyApi &get()
    {
        static ConPtyApi api = [] {
            ConPtyApi a;
            if (HMODULE k = GetModuleHandleW(L"kernel32.dll")) {
                a.create = reinterpret_cast<PFN_CreatePseudoConsole>(reinterpret_cast<void *>(GetProcAddress(k, "CreatePseudoConsole")));
                a.resize = reinterpret_cast<PFN_ResizePseudoConsole>(reinterpret_cast<void *>(GetProcAddress(k, "ResizePseudoConsole")));
                a.close = reinterpret_cast<PFN_ClosePseudoConsole>(reinterpret_cast<void *>(GetProcAddress(k, "ClosePseudoConsole")));
            }
            return a;
        }();
        return api;
    }
};

QString winError(DWORD e)
{
    wchar_t *buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, e, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    QString s = buf ? QString::fromWCharArray(buf).trimmed() : QStringLiteral("error %1").arg(e);
    if (buf)
        LocalFree(buf);
    return s;
}

QString quoteArg(const QString &a)
{
    if (!a.isEmpty() && !a.contains(QLatin1Char(' ')) && !a.contains(QLatin1Char('\t')) && !a.contains(QLatin1Char('"')))
        return a;
    QString out = QStringLiteral("\"");
    int backslashes = 0;
    for (QChar c : a) {
        if (c == QLatin1Char('\\')) {
            ++backslashes;
        } else if (c == QLatin1Char('"')) {
            out += QString(backslashes * 2 + 1, QLatin1Char('\\'));
            out += c;
            backslashes = 0;
        } else {
            out += QString(backslashes, QLatin1Char('\\'));
            out += c;
            backslashes = 0;
        }
    }
    out += QString(backslashes * 2, QLatin1Char('\\'));
    out += QLatin1Char('"');
    return out;
}

class ConPtyWorker : public TransportWorker {
public:
    explicit ConPtyWorker(LocalShellOptions o) : m_opts(std::move(o))
    {
        m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }

    ~ConPtyWorker() override
    {
        if (m_shared) {
            std::lock_guard<std::mutex> lock(m_shared->mx);
            m_shared->waker = nullptr;
        }
        teardown();
        if (m_wakeEvent)
            CloseHandle(m_wakeEvent);
    }

    void start() override
    {
        run();
        teardown();
        finish();
    }

protected:
    void installWaker() override
    {
        std::lock_guard<std::mutex> lock(m_shared->mx);
        HANDLE ev = m_wakeEvent;
        m_shared->waker = [ev] { SetEvent(ev); };
    }

private:
    void run()
    {
        const ConPtyApi &api = ConPtyApi::get();
        if (!api.ok()) {
            setState(TransportState::Failed, QStringLiteral("Local shell requires Windows 10 version 1809 or newer"));
            return;
        }
        HANDLE inRead = nullptr, outWrite = nullptr;
        if (!CreatePipe(&inRead, &m_inWrite, nullptr, 0) || !CreatePipe(&m_outRead, &outWrite, nullptr, 0)) {
            setState(TransportState::Failed, QStringLiteral("CreatePipe failed: %1").arg(winError(GetLastError())));
            if (inRead) CloseHandle(inRead);
            return;
        }
        COORD size{SHORT(qBound(1, m_shared->cols.load(), 32767)), SHORT(qBound(1, m_shared->rows.load(), 32767))};
        m_shared->resizePending.store(false);
        HRESULT hr = api.create(size, inRead, outWrite, 0, &m_hpc);
        CloseHandle(inRead);
        CloseHandle(outWrite);
        if (FAILED(hr)) {
            setState(TransportState::Failed, QStringLiteral("CreatePseudoConsole failed (0x%1)").arg(quint32(hr), 8, 16, QLatin1Char('0')));
            return;
        }

        SIZE_T attrSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
        std::vector<char> attrBuf(attrSize);
        auto *attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
        if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize)
            || !UpdateProcThreadAttribute(attrs, 0, kProcThreadAttributePseudoConsole, m_hpc, sizeof(m_hpc), nullptr, nullptr)) {
            setState(TransportState::Failed, QStringLiteral("Cannot attach pseudo console: %1").arg(winError(GetLastError())));
            return;
        }

        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof(si);
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;   // no inherited std handles
        si.lpAttributeList = attrs;

        const QString program = m_opts.program.isEmpty() ? LocalShellTransport::defaultProgram() : m_opts.program;
        QString cmd = quoteArg(program);
        QStringList args = m_opts.arguments;
        if (m_opts.program.isEmpty())
            args = {QStringLiteral("-NoLogo")};
        for (const QString &a : args)
            cmd += QLatin1Char(' ') + quoteArg(a);
        std::wstring cmdLine = cmd.toStdWString();
        std::wstring cwd = QDir::toNativeSeparators(m_opts.workingDirectory.isEmpty() ? QDir::homePath() : m_opts.workingDirectory).toStdWString();

        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                                       EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, nullptr,
                                       cwd.c_str(), &si.StartupInfo, &pi);
        const DWORD createErr = GetLastError();
        DeleteProcThreadAttributeList(attrs);
        if (!ok) {
            setState(TransportState::Failed, QStringLiteral("Cannot start %1: %2").arg(program, winError(createErr)));
            return;
        }
        m_process = pi.hProcess;
        CloseHandle(pi.hThread);

        // Reader thread: blocking ReadFile until the pipe breaks.
        HANDLE outRead = m_outRead;
        m_reader = std::thread([this, outRead] {
            std::vector<char> buf(64 * 1024);
            for (;;) {
                while (flowBlocked() && !stopRequested())
                    Sleep(10);
                DWORD n = 0;
                if (!ReadFile(outRead, buf.data(), DWORD(buf.size()), &n, nullptr) || n == 0)
                    break;
                emitData(QByteArray(buf.data(), int(n)));
            }
        });

        setState(TransportState::Connected, QString());

        HANDLE handles[2] = {m_wakeEvent, m_process};
        QString reason = QStringLiteral("Disconnected");
        while (!stopRequested()) {
            const DWORD w = WaitForMultipleObjects(2, handles, FALSE, 1000);
            if (w == WAIT_OBJECT_0 + 1) {
                DWORD code = 0;
                GetExitCodeProcess(m_process, &code);
                reason = QStringLiteral("Process exited with code %1").arg(code);
                break;
            }
            if (w == WAIT_FAILED) {
                reason = QStringLiteral("Wait failed: %1").arg(winError(GetLastError()));
                break;
            }
            if (m_shared->resizePending.exchange(false)) {
                COORD c{SHORT(qBound(1, m_shared->cols.load(), 32767)), SHORT(qBound(1, m_shared->rows.load(), 32767))};
                api.resize(m_hpc, c);
            }
            std::deque<QByteArray> chunks;
            if (takeWrites(chunks)) {
                for (const QByteArray &c : chunks) {
                    DWORD written = 0;
                    const char *p = c.constData();
                    DWORD left = DWORD(c.size());
                    while (left > 0 && WriteFile(m_inWrite, p, left, &written, nullptr) && written > 0) {
                        p += written;
                        left -= written;
                    }
                }
            }
        }
        setState(TransportState::Disconnected, reason);
    }

    void teardown()
    {
        const ConPtyApi &api = ConPtyApi::get();
        if (m_hpc) {
            // The reader keeps draining the output pipe while this runs; older
            // Windows 10 builds block in ClosePseudoConsole otherwise.
            api.close(m_hpc);
            m_hpc = nullptr;
        }
        if (m_inWrite) {
            CloseHandle(m_inWrite);
            m_inWrite = nullptr;
        }
        if (m_reader.joinable()) {
            CancelSynchronousIo(m_reader.native_handle());
            m_reader.join();
        }
        if (m_outRead) {
            CloseHandle(m_outRead);
            m_outRead = nullptr;
        }
        if (m_process) {
            if (WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT)
                TerminateProcess(m_process, 1);
            CloseHandle(m_process);
            m_process = nullptr;
        }
    }

    LocalShellOptions m_opts;
    HANDLE m_wakeEvent = nullptr;
    HANDLE m_inWrite = nullptr;
    HANDLE m_outRead = nullptr;
    HANDLE m_process = nullptr;
    HPCON_T m_hpc = nullptr;
    std::thread m_reader;
};

using LocalWorker = ConPtyWorker;

#else
// ------------------------------------------------------------------ forkpty

class PtyWorker : public TransportWorker {
public:
    explicit PtyWorker(LocalShellOptions o) : m_opts(std::move(o))
    {
        if (::pipe(m_wakePipe) == 0) {
            ::fcntl(m_wakePipe[0], F_SETFL, O_NONBLOCK);
            ::fcntl(m_wakePipe[1], F_SETFL, O_NONBLOCK);
        }
    }

    ~PtyWorker() override
    {
        if (m_shared) {
            std::lock_guard<std::mutex> lock(m_shared->mx);
            m_shared->waker = nullptr;
        }
        teardown();
        for (int fd : m_wakePipe)
            if (fd >= 0)
                ::close(fd);
    }

    void start() override
    {
        run();
        teardown();
        finish();
    }

protected:
    void installWaker() override
    {
        std::lock_guard<std::mutex> lock(m_shared->mx);
        const int fd = m_wakePipe[1];
        m_shared->waker = [fd] {
            const char b = 1;
            [[maybe_unused]] auto r = ::write(fd, &b, 1);
        };
    }

private:
    void run()
    {
        const QString program = m_opts.program.isEmpty() ? LocalShellTransport::defaultProgram() : m_opts.program;
        winsize ws{};
        ws.ws_col = static_cast<unsigned short>(qBound(1, m_shared->cols.load(), 65535));
        ws.ws_row = static_cast<unsigned short>(qBound(1, m_shared->rows.load(), 65535));
        m_shared->resizePending.store(false);

        QByteArray prog = QFile::encodeName(program);
        QList<QByteArray> argStore;
        argStore << prog;
        for (const QString &a : m_opts.arguments)
            argStore << a.toLocal8Bit();
        std::vector<char *> argv;
        for (QByteArray &a : argStore)
            argv.push_back(a.data());
        argv.push_back(nullptr);
        const QByteArray cwd = QFile::encodeName(m_opts.workingDirectory.isEmpty() ? QDir::homePath() : m_opts.workingDirectory);

        m_pid = forkpty(&m_master, nullptr, nullptr, &ws);
        if (m_pid < 0) {
            setState(TransportState::Failed, QStringLiteral("forkpty failed: %1").arg(QString::fromLocal8Bit(strerror(errno))));
            return;
        }
        if (m_pid == 0) {
            // Child: only async-signal-safe calls from here on.
            if (::chdir(cwd.constData()) != 0) {
            }
            ::setenv("TERM", "xterm-256color", 1);
            ::setenv("COLORTERM", "truecolor", 1);
            ::execvp(argv[0], argv.data());
            ::_exit(127);
        }
        ::fcntl(m_master, F_SETFL, O_NONBLOCK);
        setState(TransportState::Connected, QString());

        QByteArray buf(64 * 1024, Qt::Uninitialized);
        QString reason = QStringLiteral("Disconnected");
        while (!stopRequested()) {
            pollfd fds[2] = {{m_master, short(flowBlocked() ? 0 : POLLIN), 0}, {m_wakePipe[0], POLLIN, 0}};
            const int n = ::poll(fds, 2, flowBlocked() ? 20 : 1000);
            if (n < 0 && errno != EINTR) {
                reason = QStringLiteral("poll failed");
                break;
            }
            if (fds[1].revents & POLLIN) {
                char tmp[64];
                while (::read(m_wakePipe[0], tmp, sizeof(tmp)) > 0) {
                }
            }
            if (m_shared->resizePending.exchange(false)) {
                winsize w{};
                w.ws_col = static_cast<unsigned short>(qBound(1, m_shared->cols.load(), 65535));
                w.ws_row = static_cast<unsigned short>(qBound(1, m_shared->rows.load(), 65535));
                ::ioctl(m_master, TIOCSWINSZ, &w);
            }
            std::deque<QByteArray> chunks;
            if (takeWrites(chunks)) {
                for (const QByteArray &c : chunks) {
                    qsizetype off = 0;
                    int spins = 0;
                    while (off < c.size() && spins < 1000) {
                        const ssize_t w = ::write(m_master, c.constData() + off, size_t(c.size() - off));
                        if (w > 0) {
                            off += w;
                        } else if (errno == EAGAIN || errno == EINTR) {
                            ++spins;
                            pollfd pf{m_master, POLLOUT, 0};
                            ::poll(&pf, 1, 10);
                        } else {
                            break;
                        }
                    }
                }
            }
            if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
                bool closed = false;
                for (int guard = 0; guard < 32 && !flowBlocked(); ++guard) {
                    const ssize_t r = ::read(m_master, buf.data(), size_t(buf.size()));
                    if (r > 0) {
                        emitData(QByteArray(buf.constData(), int(r)));
                        continue;
                    }
                    if (r < 0 && (errno == EAGAIN || errno == EINTR))
                        break;
                    closed = true;   // EIO: child closed the pty
                    break;
                }
                if (closed) {
                    int status = 0;
                    ::waitpid(m_pid, &status, 0);
                    m_pid = -1;
                    reason = QStringLiteral("Process exited with code %1").arg(WIFEXITED(status) ? WEXITSTATUS(status) : -1);
                    break;
                }
            }
        }
        setState(TransportState::Disconnected, reason);
    }

    void teardown()
    {
        if (m_master >= 0) {
            ::close(m_master);
            m_master = -1;
        }
        if (m_pid > 0) {
            ::kill(m_pid, SIGHUP);
            int status = 0;
            for (int i = 0; i < 50 && ::waitpid(m_pid, &status, WNOHANG) == 0; ++i)
                ::usleep(10000);
            if (::waitpid(m_pid, &status, WNOHANG) == 0) {
                ::kill(m_pid, SIGKILL);
                ::waitpid(m_pid, &status, 0);
            }
            m_pid = -1;
        }
    }

    LocalShellOptions m_opts;
    int m_wakePipe[2] = {-1, -1};
    int m_master = -1;
    pid_t m_pid = -1;
};

using LocalWorker = PtyWorker;
#endif

} // namespace

LocalShellTransport::LocalShellTransport(const LocalShellOptions &opts, QObject *parent)
    : Transport(parent), m_opts(opts)
{
}

TransportWorker *LocalShellTransport::createWorker()
{
    return new LocalWorker(m_opts);
}

} // namespace termx
