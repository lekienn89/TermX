// SPDX-License-Identifier: GPL-3.0-or-later
#include "SessionDialog.h"
#include "ForwardWidgets.h"

#include "transport/LocalShellTransport.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace termx {

namespace {
QSpinBox *spin(int lo, int hi, QWidget *parent)
{
    auto *s = new QSpinBox(parent);
    s->setRange(lo, hi);
    return s;
}
} // namespace

SessionDialog::SessionDialog(const SessionConfig &cfg, const QStringList &folders, const QVector<SerialPortEntry> &ports,
                             bool editingSaved, QWidget *parent)
    : QDialog(parent), m_base(cfg)
{
    setWindowTitle(editingSaved ? tr("Edit Session") : tr("New Session"));
    setMinimumWidth(520);
    auto *root = new QVBoxLayout(this);

    // --- general ---
    auto *general = new QFormLayout;
    m_type = new QComboBox(this);
    m_type->addItem(tr("SSH"), int(SessionType::Ssh));
    m_type->addItem(tr("Serial (COM)"), int(SessionType::Serial));
    m_type->addItem(tr("Telnet / Raw TCP"), int(SessionType::Telnet));
    m_type->addItem(tr("Local shell"), int(SessionType::LocalShell));
    m_name = new QLineEdit(this);
    m_name->setPlaceholderText(tr("Optional; defaults to the target"));
    m_folder = new QComboBox(this);
    m_folder->setEditable(true);
    m_folder->addItem(QString());
    m_folder->addItems(folders);
    general->addRow(tr("Type"), m_type);
    general->addRow(tr("Name"), m_name);
    general->addRow(tr("Folder"), m_folder);
    root->addLayout(general);

    m_stack = new QStackedWidget(this);

    // --- SSH page ---
    auto *ssh = new QWidget;
    auto *sf = new QFormLayout(ssh);
    m_sshHost = new QLineEdit(ssh);
    m_sshHost->setPlaceholderText(tr("Host name or IP address"));
    m_sshPort = spin(1, 65535, ssh);
    m_sshUser = new QLineEdit(ssh);
    m_sshUser->setPlaceholderText(tr("Asked at login if empty"));
    m_sshKey = new QLineEdit(ssh);
    m_sshKey->setPlaceholderText(tr("Private key file (optional)"));
    auto *browse = new QToolButton(ssh);
    browse->setText(QStringLiteral("..."));
    connect(browse, &QToolButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Private key"), QDir::homePath() + QStringLiteral("/.ssh"));
        if (!f.isEmpty())
            m_sshKey->setText(QDir::toNativeSeparators(f));
    });
    auto *keyRow = new QHBoxLayout;
    keyRow->addWidget(m_sshKey, 1);
    keyRow->addWidget(browse);
    m_sshAgent = new QCheckBox(tr("Use SSH agent (Pageant / OpenSSH agent)"), ssh);
    m_sshLegacy = new QCheckBox(tr("Allow legacy algorithms (old switches and routers)"), ssh);
    m_sshLegacy->setToolTip(tr("Adds SHA-1 key exchange, ssh-rsa / ssh-dss host keys and CBC ciphers.\n"
                               "These are weak; enable only for devices that cannot do better.\n"
                               "TermX offers to enable them when a server needs them."));
    m_sshLegacyWarn = new QLabel(tr("\u26A0 Weak algorithms are allowed for this session."), ssh);
    m_sshLegacyWarn->setStyleSheet(QStringLiteral("color: #D29922;"));
    m_sshLegacyWarn->setVisible(false);
    connect(m_sshLegacy, &QCheckBox::toggled, m_sshLegacyWarn, &QWidget::setVisible);
    m_sshHostKey = new QComboBox(ssh);
    m_sshHostKey->addItem(tr("Ask for unknown or changed keys"), int(HostKeyPolicy::Ask));
    m_sshHostKey->addItem(tr("Trust new hosts, refuse changed keys"), int(HostKeyPolicy::AcceptNew));
    m_sshHostKey->addItem(tr("Strict: only hosts already in known_hosts"), int(HostKeyPolicy::Strict));
    m_sshKeepAlive = spin(0, 3600, ssh);
    m_sshKeepAlive->setSuffix(tr(" s"));
    m_sshKeepAlive->setSpecialValueText(tr("Off"));
    sf->addRow(tr("Host"), m_sshHost);
    sf->addRow(tr("Port"), m_sshPort);
    sf->addRow(tr("Username"), m_sshUser);
    sf->addRow(tr("Key file"), keyRow);
    sf->addRow(QString(), m_sshAgent);
    sf->addRow(tr("Host keys"), m_sshHostKey);
    sf->addRow(QString(), m_sshLegacy);
    sf->addRow(QString(), m_sshLegacyWarn);
    sf->addRow(tr("Keep-alive"), m_sshKeepAlive);
    m_sshForwards = new ForwardRulesEditor(ssh);
    sf->addRow(tr("Port forwarding"), m_sshForwards);
    m_stack->addWidget(ssh);

    // --- Serial page ---
    auto *ser = new QWidget;
    auto *rf = new QFormLayout(ser);
    m_serPort = new QComboBox(ser);
    m_serPort->setEditable(true);
    for (const SerialPortEntry &p : ports)
        m_serPort->addItem(p.label(), p.portName);
    m_serBaud = new QComboBox(ser);
    m_serBaud->setEditable(true);
    for (int b : {300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600})
        m_serBaud->addItem(QString::number(b));
    m_serData = new QComboBox(ser);
    m_serData->addItems({QStringLiteral("8"), QStringLiteral("7"), QStringLiteral("6"), QStringLiteral("5")});
    m_serParity = new QComboBox(ser);
    m_serParity->addItem(tr("None"), QStringLiteral("N"));
    m_serParity->addItem(tr("Even"), QStringLiteral("E"));
    m_serParity->addItem(tr("Odd"), QStringLiteral("O"));
    m_serParity->addItem(tr("Mark"), QStringLiteral("M"));
    m_serParity->addItem(tr("Space"), QStringLiteral("S"));
    m_serStop = new QComboBox(ser);
    m_serStop->addItem(QStringLiteral("1"), 1);
    m_serStop->addItem(QStringLiteral("1.5"), 3);
    m_serStop->addItem(QStringLiteral("2"), 2);
    m_serFlow = new QComboBox(ser);
    m_serFlow->addItem(tr("None"), int(SerialOptions::Flow::None));
    m_serFlow->addItem(tr("RTS/CTS (hardware)"), int(SerialOptions::Flow::RtsCts));
    m_serFlow->addItem(tr("XON/XOFF (software)"), int(SerialOptions::Flow::XonXoff));
    m_serLineDelay = spin(0, 5000, ser);
    m_serLineDelay->setSuffix(tr(" ms"));
    m_serCharDelay = spin(0, 1000, ser);
    m_serCharDelay->setSuffix(tr(" ms"));
    m_serReconnect = new QCheckBox(tr("Reconnect automatically when the cable is plugged back in"), ser);
    rf->addRow(tr("Port"), m_serPort);
    rf->addRow(tr("Baud rate"), m_serBaud);
    rf->addRow(tr("Data bits"), m_serData);
    rf->addRow(tr("Parity"), m_serParity);
    rf->addRow(tr("Stop bits"), m_serStop);
    rf->addRow(tr("Flow control"), m_serFlow);
    rf->addRow(tr("Paste delay per line"), m_serLineDelay);
    rf->addRow(tr("Paste delay per char"), m_serCharDelay);
    rf->addRow(QString(), m_serReconnect);
    m_stack->addWidget(ser);

    // --- Telnet page ---
    auto *tel = new QWidget;
    auto *tf = new QFormLayout(tel);
    m_telHost = new QLineEdit(tel);
    m_telPort = spin(1, 65535, tel);
    m_telRaw = new QCheckBox(tr("Raw TCP (no Telnet negotiation)"), tel);
    tf->addRow(tr("Host"), m_telHost);
    tf->addRow(tr("Port"), m_telPort);
    tf->addRow(QString(), m_telRaw);
    m_stack->addWidget(tel);

    // --- Local page ---
    auto *loc = new QWidget;
    auto *lf = new QFormLayout(loc);
    m_locShell = new QComboBox(loc);
    m_locShell->setEditable(true);
#ifdef _WIN32
    m_locShell->addItem(tr("Windows PowerShell"), QString());
    m_locShell->addItem(tr("PowerShell 7 (pwsh)"), QStringLiteral("pwsh.exe"));
    m_locShell->addItem(tr("Command Prompt"), QStringLiteral("cmd.exe"));
    m_locShell->addItem(tr("WSL"), QStringLiteral("wsl.exe"));
#else
    m_locShell->addItem(tr("Default shell (%1)").arg(LocalShellTransport::defaultProgram()), QString());
    m_locShell->addItem(QStringLiteral("bash"), QStringLiteral("bash"));
#endif
    m_locArgs = new QLineEdit(loc);
    m_locCwd = new QLineEdit(loc);
    m_locCwd->setPlaceholderText(tr("Home folder"));
    lf->addRow(tr("Shell"), m_locShell);
    lf->addRow(tr("Arguments"), m_locArgs);
    lf->addRow(tr("Start in"), m_locCwd);
    m_stack->addWidget(loc);

    root->addWidget(m_stack);

    // --- terminal options ---
    auto *termBox = new QGroupBox(tr("Terminal"), this);
    auto *of = new QFormLayout(termBox);
    m_enter = new QComboBox(termBox);
    m_enter->addItem(QStringLiteral("CR"), int(NewlineMode::CR));
    m_enter->addItem(QStringLiteral("LF"), int(NewlineMode::LF));
    m_enter->addItem(QStringLiteral("CR+LF"), int(NewlineMode::CRLF));
    m_localEcho = new QCheckBox(tr("Local echo"), termBox);
    m_history = new QCheckBox(tr("Record command history"), termBox);
    m_log = new QCheckBox(tr("Log session to file automatically"), termBox);
    m_scrollback = spin(100, 1000000, termBox);
    m_scrollback->setSingleStep(1000);
    m_scrollback->setSuffix(tr(" lines"));
    of->addRow(tr("Enter key sends"), m_enter);
    of->addRow(tr("Scrollback"), m_scrollback);
    of->addRow(QString(), m_localEcho);
    of->addRow(QString(), m_history);
    of->addRow(QString(), m_log);
    m_logTimes = new QCheckBox(tr("Add a timestamp to every logged line"), termBox);
    of->addRow(QString(), m_logTimes);
    if (!cfg.encryptedPassword.isEmpty()) {
        m_forgetPassword = new QCheckBox(tr("Forget saved password"), termBox);
        of->addRow(QString(), m_forgetPassword);
    }
    root->addWidget(termBox);

    // --- buttons ---
    auto *buttons = new QDialogButtonBox(this);
    QPushButton *connectBtn = buttons->addButton(tr("Connect"), QDialogButtonBox::ActionRole);
    QPushButton *saveConnect = buttons->addButton(tr("Save && Connect"), QDialogButtonBox::AcceptRole);
    QPushButton *save = buttons->addButton(tr("Save"), QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    saveConnect->setDefault(true);
    connect(connectBtn, &QPushButton::clicked, this, [this] { finish(ConnectOnly); });
    connect(saveConnect, &QPushButton::clicked, this, [this] { finish(SaveAndConnect); });
    connect(save, &QPushButton::clicked, this, [this] { finish(SaveOnly); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    connect(m_type, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i) {
        m_stack->setCurrentIndex(i);
        const auto t = SessionType(m_type->itemData(i).toInt());
        m_enter->setEnabled(t == SessionType::Serial || t == SessionType::Telnet);
        m_localEcho->setEnabled(t == SessionType::Serial || t == SessionType::Telnet);
    });

    load(cfg);
}

void SessionDialog::load(const SessionConfig &c)
{
    m_type->setCurrentIndex(m_type->findData(int(c.type)));
    m_stack->setCurrentIndex(m_type->currentIndex());
    m_name->setText(c.name);
    m_folder->setCurrentText(c.folder);

    m_sshHost->setText(c.ssh.host);
    m_sshPort->setValue(c.ssh.port);
    m_sshUser->setText(c.ssh.username);
    m_sshKey->setText(c.ssh.keyFile);
    m_sshAgent->setChecked(c.ssh.useAgent);
    m_sshLegacy->setChecked(c.ssh.allowLegacyAlgorithms);
    m_sshLegacyWarn->setVisible(c.ssh.allowLegacyAlgorithms);
    m_sshHostKey->setCurrentIndex(qMax(0, m_sshHostKey->findData(int(c.ssh.hostKeyPolicy))));
    m_sshKeepAlive->setValue(c.ssh.keepAliveSeconds);
    m_sshForwards->setRules(c.ssh.forwards);

    m_telHost->setText(c.telnet.host);
    m_telPort->setValue(c.telnet.port);
    m_telRaw->setChecked(c.telnet.rawTcp);

    const int pi = m_serPort->findData(c.serial.portName);
    if (pi >= 0)
        m_serPort->setCurrentIndex(pi);
    else if (!c.serial.portName.isEmpty())
        m_serPort->setEditText(c.serial.portName);
    m_serBaud->setCurrentText(QString::number(c.serial.baudRate));
    m_serData->setCurrentText(QString::number(c.serial.dataBits));
    m_serParity->setCurrentIndex(std::max(0, m_serParity->findData(QString(c.serial.parity))));
    m_serStop->setCurrentIndex(std::max(0, m_serStop->findData(c.serial.stopBits)));
    m_serFlow->setCurrentIndex(std::max(0, m_serFlow->findData(int(c.serial.flow))));
    m_serLineDelay->setValue(c.serial.lineDelayMs);
    m_serCharDelay->setValue(c.serial.charDelayMs);
    m_serReconnect->setChecked(c.serial.autoReconnect);

    const int si = m_locShell->findData(c.local.program);
    if (si >= 0)
        m_locShell->setCurrentIndex(si);
    else
        m_locShell->setEditText(c.local.program);
    QStringList quoted;
    for (const QString &a : c.local.arguments)
        quoted << (a.contains(QLatin1Char(' ')) ? QLatin1Char('"') + a + QLatin1Char('"') : a);
    m_locArgs->setText(quoted.join(QLatin1Char(' ')));
    m_locCwd->setText(c.local.workingDirectory);

    m_enter->setCurrentIndex(std::max(0, m_enter->findData(int(c.terminal.enterSends))));
    m_localEcho->setChecked(c.terminal.localEcho);
    m_history->setChecked(c.historyEnabled);
    m_log->setChecked(c.logEnabled);
    m_logTimes->setChecked(c.logTimestamps);
    m_scrollback->setValue(c.terminal.scrollbackLines);
    const auto t = c.type;
    m_enter->setEnabled(t == SessionType::Serial || t == SessionType::Telnet);
    m_localEcho->setEnabled(t == SessionType::Serial || t == SessionType::Telnet);
}

SessionConfig SessionDialog::config() const
{
    SessionConfig c = m_base;
    c.type = SessionType(m_type->currentData().toInt());
    c.name = m_name->text().trimmed();
    c.folder = m_folder->currentText().trimmed();

    c.ssh.host = m_sshHost->text().trimmed();
    c.ssh.port = m_sshPort->value();
    c.ssh.username = m_sshUser->text().trimmed();
    c.ssh.keyFile = m_sshKey->text().trimmed();
    c.ssh.useAgent = m_sshAgent->isChecked();
    c.ssh.allowLegacyAlgorithms = m_sshLegacy->isChecked();
    c.ssh.hostKeyPolicy = HostKeyPolicy(m_sshHostKey->currentData().toInt());
    c.ssh.keepAliveSeconds = m_sshKeepAlive->value();
    c.ssh.forwards = m_sshForwards->rules();

    c.telnet.host = m_telHost->text().trimmed();
    c.telnet.port = m_telPort->value();
    c.telnet.rawTcp = m_telRaw->isChecked();

    QString port = m_serPort->currentData().toString();
    if (port.isEmpty() || m_serPort->currentText() != m_serPort->itemText(m_serPort->currentIndex()))
        port = m_serPort->currentText().section(QLatin1Char(' '), 0, 0).trimmed();
    if (port != c.serial.portName)
        c.serial.deviceKey.clear();   // a different port: the device identity is resolved again
    c.serial.portName = port;
    c.serial.baudRate = std::max(50, m_serBaud->currentText().toInt());
    c.serial.dataBits = m_serData->currentText().toInt();
    c.serial.parity = m_serParity->currentData().toString().at(0);
    c.serial.stopBits = m_serStop->currentData().toInt();
    c.serial.flow = SerialOptions::Flow(m_serFlow->currentData().toInt());
    c.serial.lineDelayMs = m_serLineDelay->value();
    c.serial.charDelayMs = m_serCharDelay->value();
    c.serial.autoReconnect = m_serReconnect->isChecked();

    const QString shellData = m_locShell->currentData().toString();
    const bool shellFromList = m_locShell->currentIndex() >= 0
                               && m_locShell->currentText() == m_locShell->itemText(m_locShell->currentIndex());
    c.local.program = shellFromList ? shellData : m_locShell->currentText().trimmed();
    c.local.arguments = QProcess::splitCommand(m_locArgs->text());
    c.local.workingDirectory = m_locCwd->text().trimmed();

    c.terminal.enterSends = NewlineMode(m_enter->currentData().toInt());
    c.terminal.localEcho = m_localEcho->isChecked();
    c.terminal.scrollbackLines = m_scrollback->value();
    c.historyEnabled = m_history->isChecked();
    c.logEnabled = m_log->isChecked();
    c.logTimestamps = m_logTimes->isChecked();
    if (m_forgetPassword && m_forgetPassword->isChecked())
        c.encryptedPassword.clear();
    return c;
}

bool SessionDialog::validate()
{
    const SessionConfig c = config();
    QString problem;
    switch (c.type) {
    case SessionType::Ssh:
        if (c.ssh.host.isEmpty())
            problem = tr("Enter a host name.");
        break;
    case SessionType::Telnet:
        if (c.telnet.host.isEmpty())
            problem = tr("Enter a host name.");
        break;
    case SessionType::Serial:
        if (c.serial.portName.isEmpty())
            problem = tr("Choose a serial port.");
        break;
    case SessionType::LocalShell:
        break;
    }
    if (!problem.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), problem);
        return false;
    }
    return true;
}

void SessionDialog::finish(Action a)
{
    if (!validate())
        return;
    m_action = a;
    accept();
}

} // namespace termx
