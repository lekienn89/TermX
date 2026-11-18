// SPDX-License-Identifier: GPL-3.0-or-later
#include "CrashHandler.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cstdlib>
#include <exception>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <cwchar>
#endif

namespace termx::CrashHandler {

#ifdef _WIN32
namespace {
wchar_t g_dir[MAX_PATH] = {0};

void writeDump(EXCEPTION_POINTERS *info)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t path[MAX_PATH + 64];
    _snwprintf_s(path, _countof(path), _TRUNCATE, L"%s\\termx_%04d%02d%02d_%02d%02d%02d_%lu.dmp", g_dir, t.wYear,
                 t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId());
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return;
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = info;
    mei.ClientPointers = FALSE;
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                      MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo),
                      info ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(f);
}

LONG WINAPI onUnhandled(EXCEPTION_POINTERS *info)
{
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1) == 0)
        writeDump(info);
    return EXCEPTION_EXECUTE_HANDLER;
}

void onTerminate()
{
    // Raise a real SEH exception so the dump carries a usable context.
    RaiseException(0xE0000001, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    std::abort();
}

void onPureCall()
{
    RaiseException(0xE0000002, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}

void onInvalidParameter(const wchar_t *, const wchar_t *, const wchar_t *, unsigned int, uintptr_t)
{
    RaiseException(0xE0000003, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}
} // namespace

void install(const QString &dir)
{
    QDir().mkpath(dir);
    const std::wstring w = QDir::toNativeSeparators(dir).toStdWString();
    wcsncpy_s(g_dir, w.c_str(), _TRUNCATE);
    SetUnhandledExceptionFilter(onUnhandled);
    std::set_terminate(onTerminate);
    _set_purecall_handler(onPureCall);
    _set_invalid_parameter_handler(onInvalidParameter);
    // Keep Windows Error Reporting dialogs from hanging the process.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
}
#else
void install(const QString &dir)
{
    QDir().mkpath(dir);
}
#endif

QStringList pendingReports(const QString &dir)
{
    QDir d(dir);
    const QFileInfo marker(d.filePath(QStringLiteral(".seen")));
    QStringList out;
    for (const QFileInfo &fi : d.entryInfoList({QStringLiteral("*.dmp")}, QDir::Files, QDir::Time)) {
        if (!marker.exists() || fi.lastModified() > marker.lastModified())
            out << fi.absoluteFilePath();
    }
    return out;
}

void acknowledge(const QString &dir)
{
    QFile f(QDir(dir).filePath(QStringLiteral(".seen")));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write("seen\n");
}

} // namespace termx::CrashHandler
