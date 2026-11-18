// SPDX-License-Identifier: GPL-3.0-or-later
#include "SerialPortWatcher.h"

#include <QCoreApplication>
#include <QSerialPortInfo>
#include <QThread>

#ifdef _WIN32
#include <windows.h>
#include <dbt.h>
#endif

namespace termx {

QString SerialPortEntry::chipNameFor(quint16 vid, quint16 pid)
{
    switch (vid) {
    case 0x0403: return QStringLiteral("FTDI");
    case 0x1A86: return pid == 0x7522 || pid == 0x7523 ? QStringLiteral("CH340") : QStringLiteral("WCH CH34x");
    case 0x10C4: return QStringLiteral("CP210x");
    case 0x067B: return QStringLiteral("PL2303");
    case 0x05A6: return QStringLiteral("Cisco console");
    case 0x0557: return QStringLiteral("ATEN");
    case 0x04E2: return QStringLiteral("Exar");
    case 0x0483: return QStringLiteral("STM32 VCP");
    case 0x2341:
    case 0x2A03: return QStringLiteral("Arduino");
    case 0x303A: return QStringLiteral("Espressif");
    case 0x1366: return QStringLiteral("SEGGER");
    default: return QString();
    }
}

QString SerialPortEntry::chipName() const
{
    return hasIds ? chipNameFor(vendorId, productId) : QString();
}

QString SerialPortEntry::deviceKey() const
{
    const QString ids = QStringLiteral("%1:%2").arg(vendorId, 4, 16, QLatin1Char('0')).arg(productId, 4, 16, QLatin1Char('0'));
    if (hasIds && !serialNumber.trimmed().isEmpty())
        return QStringLiteral("usb:") + ids + QLatin1Char(':') + serialNumber.trimmed();
    if (hasIds)
        return QStringLiteral("usb:") + ids + QStringLiteral("@") + portName;
    return QStringLiteral("port:") + portName;
}

QString SerialPortEntry::label() const
{
    QString d = description.trimmed();
    if (d.isEmpty())
        d = chipName();
    return d.isEmpty() ? portName : portName + QStringLiteral(" · ") + d;
}

void SerialPortScanner::scan()
{
    QVector<SerialPortEntry> out;
    const auto infos = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &i : infos) {
        SerialPortEntry e;
        e.portName = i.portName();
        e.description = i.description();
        e.manufacturer = i.manufacturer();
        e.serialNumber = i.serialNumber();
        e.systemLocation = i.systemLocation();
        e.hasIds = i.hasVendorIdentifier() && i.hasProductIdentifier();
        e.vendorId = i.vendorIdentifier();
        e.productId = i.productIdentifier();
#ifndef _WIN32
        // On Linux, skip the dozens of legacy ttyS* entries with no hardware.
        if (!e.hasIds && e.portName.startsWith(QLatin1String("ttyS")))
            continue;
#endif
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const SerialPortEntry &a, const SerialPortEntry &b) {
        // COM3 < COM10
        const QString an = a.portName, bn = b.portName;
        int ai = 0, bi = 0;
        while (ai < an.size() && !an.at(ai).isDigit()) ++ai;
        while (bi < bn.size() && !bn.at(bi).isDigit()) ++bi;
        if (an.left(ai) != bn.left(bi))
            return an < bn;
        return an.mid(ai).toInt() < bn.mid(bi).toInt();
    });
    Q_EMIT scanned(out);
}

SerialPortWatcher::SerialPortWatcher(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<SerialPortEntry>("termx::SerialPortEntry");
    qRegisterMetaType<QVector<SerialPortEntry>>("QVector<termx::SerialPortEntry>");
}

SerialPortWatcher::~SerialPortWatcher()
{
    if (QCoreApplication::instance())
        QCoreApplication::instance()->removeNativeEventFilter(this);
#ifdef _WIN32
    for (void *h : m_notifyHandles)
        UnregisterDeviceNotification(h);
#endif
    if (m_thread) {
        m_thread->quit();
        // A scan stuck in a driver must not block exit: give it a moment, then leave it.
        if (!m_thread->wait(1500))
            m_thread->setParent(nullptr);
        else
            delete m_thread;
    }
}

void SerialPortWatcher::start()
{
    if (m_thread)
        return;
    m_thread = new QThread;
    m_thread->setObjectName(QStringLiteral("termx-serial-scan"));
    m_scanner = new SerialPortScanner;
    m_scanner->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_scanner, &QObject::deleteLater);
    connect(m_scanner, &SerialPortScanner::scanned, this, &SerialPortWatcher::onScanned, Qt::QueuedConnection);
    m_thread->start();

    m_debounce.setSingleShot(true);
    m_debounce.setInterval(300);
    connect(&m_debounce, &QTimer::timeout, this, [this] {
        if (m_scanInFlight) {
            m_scanPending = true;
            return;
        }
        m_scanInFlight = true;
        QMetaObject::invokeMethod(m_scanner, &SerialPortScanner::scan, Qt::QueuedConnection);
    });
    m_poll.setInterval(3000);
    connect(&m_poll, &QTimer::timeout, this, &SerialPortWatcher::requestRescan);
    m_poll.start();

    if (QCoreApplication::instance())
        QCoreApplication::instance()->installNativeEventFilter(this);
    requestRescan();
}

void SerialPortWatcher::registerWindow(quintptr hwnd)
{
#ifdef _WIN32
    static const GUID kComPort = {0x86E0D1E0, 0x8089, 0x11D0, {0x9C, 0xE4, 0x08, 0x00, 0x3E, 0x30, 0x1F, 0x73}};
    static const GUID kUsbDevice = {0xA5DCBF10, 0x6530, 0x11D2, {0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED}};
    for (const GUID &g : {kComPort, kUsbDevice}) {
        DEV_BROADCAST_DEVICEINTERFACE_W filter{};
        filter.dbcc_size = sizeof(filter);
        filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        filter.dbcc_classguid = g;
        if (HDEVNOTIFY h = RegisterDeviceNotificationW(reinterpret_cast<HWND>(hwnd), &filter, DEVICE_NOTIFY_WINDOW_HANDLE))
            m_notifyHandles.push_back(h);
    }
#else
    Q_UNUSED(hwnd);
#endif
}

bool SerialPortWatcher::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result)
{
    Q_UNUSED(result);
#ifdef _WIN32
    if (eventType == "windows_generic_MSG" || eventType == "windows_dispatcher_MSG") {
        const MSG *msg = static_cast<const MSG *>(message);
        if (msg && msg->message == WM_DEVICECHANGE
            && (msg->wParam == DBT_DEVICEARRIVAL || msg->wParam == DBT_DEVICEREMOVECOMPLETE || msg->wParam == DBT_DEVNODES_CHANGED))
            requestRescan();
    }
#else
    Q_UNUSED(eventType);
    Q_UNUSED(message);
#endif
    return false;
}

void SerialPortWatcher::requestRescan()
{
    if (!m_debounce.isActive())
        m_debounce.start();
}

bool SerialPortWatcher::hasPort(const QString &portName) const
{
    for (const auto &p : m_ports)
        if (p.portName.compare(portName, Qt::CaseInsensitive) == 0)
            return true;
    return false;
}

SerialPortEntry SerialPortWatcher::findByKey(const QString &key, bool *found) const
{
    for (const auto &p : m_ports) {
        if (p.deviceKey() == key) {
            if (found)
                *found = true;
            return p;
        }
    }
    if (found)
        *found = false;
    return SerialPortEntry();
}

void SerialPortWatcher::onScanned(const QVector<SerialPortEntry> &ports)
{
    m_scanInFlight = false;
    const QVector<SerialPortEntry> old = m_ports;
    m_ports = ports;

    auto contains = [](const QVector<SerialPortEntry> &list, const SerialPortEntry &e) {
        for (const auto &x : list)
            if (x.portName == e.portName && x.deviceKey() == e.deviceKey())
                return true;
        return false;
    };
    bool changed = false;
    for (const auto &e : old) {
        if (!contains(ports, e)) {
            changed = true;
            Q_EMIT portRemoved(e);
        }
    }
    for (const auto &e : ports) {
        if (!contains(old, e)) {
            changed = true;
            if (!m_firstScan)   // ports present at startup are not "plugged in"
                Q_EMIT portAdded(e);
        }
    }
    m_firstScan = false;
    if (changed)
        Q_EMIT portsChanged();
    if (m_scanPending) {
        m_scanPending = false;
        requestRescan();
    }
}

} // namespace termx
