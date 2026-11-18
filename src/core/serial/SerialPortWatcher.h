// SPDX-License-Identifier: GPL-3.0-or-later
// Detects USB-serial console cables as they are plugged in / removed.
// Windows: WM_DEVICECHANGE notifications + a 3 s polling safety net.
// Enumeration (SetupAPI, via QSerialPortInfo) runs on a worker thread
// because some USB-serial drivers block for seconds.
#pragma once

#include <QAbstractNativeEventFilter>
#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

class QThread;

namespace termx {

struct SerialPortEntry {
    QString portName;        // COM7
    QString description;     // "USB-SERIAL CH340"
    QString manufacturer;
    QString serialNumber;
    QString systemLocation;
    quint16 vendorId = 0;
    quint16 productId = 0;
    bool hasIds = false;

    QString chipName() const;   // "CH340", "FTDI", "CP210x", "PL2303", "Cisco console"...
    QString deviceKey() const;  // stable identity used to remember settings
    QString label() const;      // "COM7 · USB-SERIAL CH340"
    bool isUsb() const { return hasIds; }

    static QString chipNameFor(quint16 vid, quint16 pid);
};

class SerialPortScanner : public QObject {
    Q_OBJECT
public Q_SLOTS:
    void scan();
Q_SIGNALS:
    void scanned(const QVector<termx::SerialPortEntry> &ports);
};

class SerialPortWatcher : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
public:
    explicit SerialPortWatcher(QObject *parent = nullptr);
    ~SerialPortWatcher() override;

    void start();
    // Register a top-level window for device-interface notifications (Windows).
    void registerWindow(quintptr nativeWindowHandle);

    QVector<SerialPortEntry> ports() const { return m_ports; }
    bool hasPort(const QString &portName) const;
    SerialPortEntry findByKey(const QString &deviceKey, bool *found = nullptr) const;
    void requestRescan();

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

Q_SIGNALS:
    void portAdded(const termx::SerialPortEntry &port);
    void portRemoved(const termx::SerialPortEntry &port);
    void portsChanged();

private:
    void onScanned(const QVector<SerialPortEntry> &ports);

    QThread *m_thread = nullptr;
    SerialPortScanner *m_scanner = nullptr;
    QTimer m_poll;
    QTimer m_debounce;
    bool m_scanInFlight = false;
    bool m_scanPending = false;
    bool m_firstScan = true;
    QVector<SerialPortEntry> m_ports;
    QVector<void *> m_notifyHandles;
};

} // namespace termx

Q_DECLARE_METATYPE(termx::SerialPortEntry)
