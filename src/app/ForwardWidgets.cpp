// SPDX-License-Identifier: GPL-3.0-or-later
#include "ForwardWidgets.h"

#include "terminal/TerminalSession.h"
#include "transport/SshTransport.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace termx {

namespace {
enum { RoleRule = Qt::UserRole + 1, RoleId };

PortForward ruleFromItem(const QTreeWidgetItem *it)
{
    return it ? it->data(0, RoleRule).value<PortForward>() : PortForward();
}
} // namespace

QString formatBytes(quint64 b)
{
    if (b < 1024)
        return QStringLiteral("%1 B").arg(b);
    const char *units[] = {"KB", "MB", "GB", "TB"};
    double v = double(b) / 1024.0;
    int u = 0;
    while (v >= 1024.0 && u < 3) {
        v /= 1024.0;
        ++u;
    }
    return QStringLiteral("%1 %2").arg(v, 0, 'f', v < 10 ? 1 : 0).arg(QLatin1String(units[u]));
}

// ============================================================== dialog

ForwardDialog::ForwardDialog(const PortForward &rule, bool showEnabled, QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Port Forwarding Rule"));
    setMinimumWidth(460);
    auto *root = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    m_type = new QComboBox(this);
    m_type->addItem(tr("Local (-L): a port here reaches a host via the server"), int(PortForward::Type::Local));
    m_type->addItem(tr("Remote (-R): a port on the server reaches a host via this computer"), int(PortForward::Type::Remote));
    m_type->addItem(tr("Dynamic (-D): SOCKS proxy here, connections leave from the server"), int(PortForward::Type::Dynamic));
    m_bind = new QLineEdit(this);
    m_bindPort = new QSpinBox(this);
    m_bindPort->setRange(0, 65535);
    m_destHost = new QLineEdit(this);
    m_destHost->setPlaceholderText(tr("Host name or IP, as seen from the other side"));
    m_destPort = new QSpinBox(this);
    m_destPort->setRange(1, 65535);
    m_description = new QLineEdit(this);
    m_description->setPlaceholderText(tr("Optional, e.g. \"Postgres on db01\""));
    m_enabled = new QCheckBox(tr("Start automatically when the session connects"), this);
    m_enabled->setVisible(showEnabled);
    m_explain = new QLabel(this);
    m_explain->setWordWrap(true);
    m_explain->setEnabled(false);
    form->addRow(tr("Type"), m_type);
    form->addRow(tr("Listen address"), m_bind);
    form->addRow(tr("Listen port"), m_bindPort);
    m_destHostLabel = new QLabel(tr("Destination host"), this);
    m_destPortLabel = new QLabel(tr("Destination port"), this);
    form->addRow(m_destHostLabel, m_destHost);
    form->addRow(m_destPortLabel, m_destPort);
    form->addRow(tr("Description"), m_description);
    form->addRow(QString(), m_enabled);
    root->addLayout(form);
    root->addWidget(m_explain);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_ok = buttons->button(QDialogButtonBox::Ok);
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    m_type->setCurrentIndex(qMax(0, m_type->findData(int(rule.type))));
    m_bind->setText(rule.bindAddress);
    m_bindPort->setValue(rule.bindPort ? rule.bindPort : (rule.type == PortForward::Type::Dynamic ? 1080 : 8080));
    m_destHost->setText(rule.destHost.isEmpty() ? QStringLiteral("localhost") : rule.destHost);
    m_destPort->setValue(rule.destPort ? rule.destPort : 80);
    m_description->setText(rule.description);
    m_enabled->setChecked(rule.enabled);

    for (QLineEdit *e : {m_bind, m_destHost, m_description})
        connect(e, &QLineEdit::textChanged, this, &ForwardDialog::refresh);
    for (QSpinBox *s : {m_bindPort, m_destPort})
        connect(s, &QSpinBox::valueChanged, this, &ForwardDialog::refresh);
    connect(m_type, &QComboBox::currentIndexChanged, this, &ForwardDialog::refresh);
    refresh();
}

PortForward ForwardDialog::rule() const
{
    PortForward f;
    f.type = PortForward::Type(m_type->currentData().toInt());
    f.bindAddress = m_bind->text().trimmed();
    if (f.bindAddress == QLatin1String("*"))
        f.bindAddress = QStringLiteral("0.0.0.0");
    f.bindPort = m_bindPort->value();
    if (f.type != PortForward::Type::Dynamic) {
        f.destHost = m_destHost->text().trimmed();
        f.destPort = m_destPort->value();
    }
    f.description = m_description->text().trimmed();
    f.enabled = m_enabled->isChecked();
    return f;
}

void ForwardDialog::refresh()
{
    const PortForward f = rule();
    const bool dyn = f.type == PortForward::Type::Dynamic;
    m_destHost->setVisible(!dyn);
    m_destPort->setVisible(!dyn);
    m_destHostLabel->setVisible(!dyn);
    m_destPortLabel->setVisible(!dyn);
    m_bindPort->setMinimum(f.type == PortForward::Type::Remote ? 0 : 1);
    m_bindPort->setSpecialValueText(f.type == PortForward::Type::Remote ? tr("0 (server picks)") : QString());
    m_bind->setPlaceholderText(f.type == PortForward::Type::Remote ? tr("localhost on the server (default)")
                                                                   : tr("127.0.0.1 (default; 0.0.0.0 = whole network)"));
    QString err;
    const bool ok = f.isValid(&err);
    m_ok->setEnabled(ok);
    QString text;
    if (!ok) {
        text = QStringLiteral("⚠ ") + err;
    } else {
        switch (f.type) {
        case PortForward::Type::Local:
            text = tr("Programs on this computer connect to %1:%2; the server opens the connection to %3:%4.")
                       .arg(f.effectiveBindAddress())
                       .arg(f.bindPort)
                       .arg(f.destHost)
                       .arg(f.destPort);
            break;
        case PortForward::Type::Remote:
            text = tr("Programs on the server connect to %1:%2; this computer opens the connection to %3:%4.")
                       .arg(f.effectiveBindAddress())
                       .arg(f.bindPort ? QString::number(f.bindPort) : tr("(port chosen by the server)"))
                       .arg(f.destHost)
                       .arg(f.destPort);
            break;
        case PortForward::Type::Dynamic:
            text = tr("Set %1:%2 as SOCKS proxy (v4/v5) in a browser or tool; its connections leave from the server.")
                       .arg(f.effectiveBindAddress())
                       .arg(f.bindPort);
            break;
        }
        text += QStringLiteral("\n") + tr("OpenSSH syntax: %1").arg(f.option());
        const QString bind = f.effectiveBindAddress();
        if (f.type != PortForward::Type::Remote && bind != QLatin1String("127.0.0.1") && bind != QLatin1String("localhost")
            && bind != QLatin1String("::1"))
            text += QStringLiteral("\n⚠ ") + tr("Other computers on the network can use this port.");
    }
    m_explain->setText(text);
}

// ============================================================== editor (session dialog)

ForwardRulesEditor::ForwardRulesEditor(QWidget *parent) : QWidget(parent)
{
    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    m_list = new QTreeWidget(this);
    m_list->setColumnCount(2);
    m_list->setHeaderLabels({tr("Rule"), tr("Description")});
    m_list->setRootIsDecorated(false);
    m_list->setMinimumHeight(90);
    m_list->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_list->header()->setStretchLastSection(true);
    m_list->setToolTip(tr("Checked rules start when the session connects.\nOthers can be started from the Port Forwarding panel."));
    l->addWidget(m_list);
    auto *row = new QHBoxLayout;
    auto *add = new QPushButton(tr("Add..."), this);
    m_edit = new QPushButton(tr("Edit..."), this);
    m_remove = new QPushButton(tr("Remove"), this);
    row->addWidget(add);
    row->addWidget(m_edit);
    row->addWidget(m_remove);
    row->addStretch(1);
    l->addLayout(row);

    connect(add, &QPushButton::clicked, this, [this] {
        ForwardDialog dlg(PortForward{}, true, this);
        if (dlg.exec() == QDialog::Accepted)
            addRule(dlg.rule());
        updateButtons();
    });
    connect(m_edit, &QPushButton::clicked, this, &ForwardRulesEditor::editCurrent);
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, &ForwardRulesEditor::editCurrent);
    connect(m_remove, &QPushButton::clicked, this, [this] {
        delete m_list->currentItem();
        updateButtons();
    });
    connect(m_list, &QTreeWidget::currentItemChanged, this, &ForwardRulesEditor::updateButtons);
    updateButtons();
}

void ForwardRulesEditor::addRule(const PortForward &f)
{
    auto *it = new QTreeWidgetItem(m_list);
    it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
    it->setData(0, RoleRule, QVariant::fromValue(f));
    it->setText(0, f.option());
    it->setText(1, f.description);
    it->setToolTip(0, f.summary());
    it->setCheckState(0, f.enabled ? Qt::Checked : Qt::Unchecked);
    m_list->setCurrentItem(it);
}

void ForwardRulesEditor::editCurrent()
{
    QTreeWidgetItem *it = m_list->currentItem();
    if (!it)
        return;
    PortForward f = ruleFromItem(it);
    f.enabled = it->checkState(0) == Qt::Checked;
    ForwardDialog dlg(f, true, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const PortForward n = dlg.rule();
    it->setData(0, RoleRule, QVariant::fromValue(n));
    it->setText(0, n.option());
    it->setText(1, n.description);
    it->setToolTip(0, n.summary());
    it->setCheckState(0, n.enabled ? Qt::Checked : Qt::Unchecked);
}

void ForwardRulesEditor::updateButtons()
{
    const bool has = m_list->currentItem() != nullptr;
    m_edit->setEnabled(has);
    m_remove->setEnabled(has);
}

void ForwardRulesEditor::setRules(const QVector<PortForward> &rules)
{
    m_list->clear();
    for (const PortForward &f : rules)
        addRule(f);
    updateButtons();
}

QVector<PortForward> ForwardRulesEditor::rules() const
{
    QVector<PortForward> out;
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        const QTreeWidgetItem *it = m_list->topLevelItem(i);
        PortForward f = ruleFromItem(it);
        f.enabled = it->checkState(0) == Qt::Checked;
        out << f;
    }
    return out;
}

// ============================================================== live panel

ForwardsPanel::ForwardsPanel(QWidget *parent) : QWidget(parent)
{
    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(4, 4, 4, 4);
    l->setSpacing(4);
    auto *row = new QHBoxLayout;
    m_quick = new QLineEdit(this);
    m_quick->setPlaceholderText(tr("-L 8080:localhost:80   -R 9000:localhost:3000   -D 1080"));
    m_quick->setToolTip(tr("Type a rule in OpenSSH syntax and press Enter.\n"
                           "-L [bind:]port:host:hostport   reach host:hostport through the server\n"
                           "-R [bind:]port:host:hostport   let the server reach host:hostport through this computer\n"
                           "-D [bind:]port                 SOCKS proxy"));
    m_add = new QPushButton(tr("Add..."), this);
    row->addWidget(m_quick, 1);
    row->addWidget(m_add);
    l->addLayout(row);
    m_list = new QTreeWidget(this);
    m_list->setColumnCount(4);
    m_list->setHeaderLabels({tr("Rule"), tr("State"), tr("Connections"), tr("Traffic")});
    m_list->setRootIsDecorated(false);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int c = 1; c < 4; ++c)
        m_list->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    m_empty = new QLabel(this);
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    m_empty->setEnabled(false);
    l->addWidget(m_list, 1);
    l->addWidget(m_empty, 1);

    connect(m_quick, &QLineEdit::returnPressed, this, &ForwardsPanel::addFromText);
    connect(m_add, &QPushButton::clicked, this, &ForwardsPanel::addWithDialog);
    connect(m_list, &QTreeWidget::customContextMenuRequested, this, &ForwardsPanel::showMenu);
    rebuild();
}

void ForwardsPanel::setSession(TerminalSession *session)
{
    if (m_session == session)
        return;
    if (m_session)
        m_session->disconnect(this);
    m_session = session;
    if (m_session) {
        connect(m_session, &TerminalSession::transportChanged, this, &ForwardsPanel::bindTransport);
        connect(m_session, &TerminalSession::stateChanged, this, &ForwardsPanel::rebuild);
    }
    bindTransport();
}

void ForwardsPanel::bindTransport()
{
    SshTransport *t = m_session ? m_session->sshTransport() : nullptr;
    if (m_transport != t) {
        if (m_transport)
            m_transport->disconnect(this);
        m_transport = t;
        if (t) {
            connect(t, &SshTransport::forwardsChanged, this, [this](const QVector<ForwardStatus> &all) {
                m_status = all;
                rebuild();
            });
        }
    }
    m_status = t ? t->forwardStatus() : QVector<ForwardStatus>();
    rebuild();
}

void ForwardsPanel::rebuild()
{
    const bool ssh = m_transport != nullptr;
    const bool connected = ssh && m_transport->state() == TransportState::Connected;
    m_quick->setEnabled(connected);
    m_add->setEnabled(connected);

    QSet<quint64> selected;
    for (QTreeWidgetItem *it : m_list->selectedItems())
        selected.insert(it->data(0, RoleId).toULongLong());
    m_list->clear();
    for (const ForwardStatus &s : std::as_const(m_status)) {
        auto *it = new QTreeWidgetItem(m_list);
        it->setData(0, RoleId, s.id);
        it->setData(0, RoleRule, QVariant::fromValue(s.rule));
        QString rule = s.rule.option();
        if (s.rule.type == PortForward::Type::Remote && s.rule.bindPort == 0 && s.boundPort > 0)
            rule += tr("  (port %1)").arg(s.boundPort);
        it->setText(0, s.rule.description.isEmpty() ? rule : QStringLiteral("%1 — %2").arg(rule, s.rule.description));
        it->setToolTip(0, s.rule.summary());
        it->setText(1, forwardStateName(s.state));
        if (s.state == ForwardStatus::State::Failed)
            it->setForeground(1, QColor(0xE5, 0x53, 0x4B));
        if (!s.error.isEmpty())
            it->setToolTip(1, s.error);
        it->setText(2, QStringLiteral("%1 / %2").arg(s.openConnections).arg(s.totalConnections));
        it->setToolTip(2, tr("open now / total"));
        it->setText(3, QStringLiteral("↓ %1  ↑ %2").arg(formatBytes(s.bytesIn), formatBytes(s.bytesOut)));
        if (selected.contains(s.id))
            it->setSelected(true);
    }
    const bool empty = m_status.isEmpty();
    m_list->setVisible(!empty);
    m_empty->setVisible(empty);
    if (!ssh)
        m_empty->setText(tr("Port forwarding works on SSH sessions.\nOpen or select an SSH tab."));
    else if (!connected)
        m_empty->setText(tr("Not connected."));
    else
        m_empty->setText(tr("No tunnels on this connection.\nType a rule above, e.g. -L 8080:localhost:80,\n"
                            "or save rules with the session (Edit Session > Port forwarding)."));
}

void ForwardsPanel::start(const PortForward &f)
{
    if (!m_transport)
        return;
    m_transport->startForward(f);
}

void ForwardsPanel::addFromText()
{
    QString err;
    const auto f = PortForward::parseOption(m_quick->text(), &err);
    if (!f) {
        m_quick->setToolTip(err);
        m_quick->setStyleSheet(QStringLiteral("QLineEdit { border: 1px solid #E5534B; }"));
        return;
    }
    m_quick->setStyleSheet(QString());
    m_quick->clear();
    start(*f);
}

void ForwardsPanel::addWithDialog()
{
    PortForward seed;
    if (auto f = PortForward::parseOption(m_quick->text()))
        seed = *f;
    ForwardDialog dlg(seed, false, this);
    if (dlg.exec() == QDialog::Accepted)
        start(dlg.rule());
}

void ForwardsPanel::stopSelected()
{
    if (!m_transport)
        return;
    for (QTreeWidgetItem *it : m_list->selectedItems())
        m_transport->stopForward(it->data(0, RoleId).toULongLong());
}

void ForwardsPanel::showMenu(const QPoint &pos)
{
    QTreeWidgetItem *it = m_list->itemAt(pos);
    QMenu menu(this);
    if (it) {
        const PortForward rule = ruleFromItem(it);
        menu.addAction(tr("Stop"), this, &ForwardsPanel::stopSelected);
        menu.addAction(tr("Restart"), this, [this, it, rule] {
            if (!m_transport)
                return;
            m_transport->stopForward(it->data(0, RoleId).toULongLong());
            m_transport->startForward(rule);
        });
        if (m_session && !m_session->config().id.isEmpty()) {
            const bool saved = std::any_of(m_session->config().ssh.forwards.begin(), m_session->config().ssh.forwards.end(),
                                           [&](const PortForward &f) { return f.sameRule(rule); });
            QAction *keep = menu.addAction(tr("Save with Session"), this, [this, rule] {
                if (m_session)
                    Q_EMIT saveRuleRequested(m_session, rule);
            });
            keep->setEnabled(!saved);
        }
        menu.addAction(tr("Copy as OpenSSH Option"), this,
                       [rule] { QApplication::clipboard()->setText(rule.option()); });
        if (rule.type != PortForward::Type::Remote) {
            menu.addAction(tr("Copy Local Address"), this, [rule] {
                QApplication::clipboard()->setText(QStringLiteral("%1:%2").arg(rule.effectiveBindAddress()).arg(rule.bindPort));
            });
        }
        menu.addSeparator();
    }
    QAction *add = menu.addAction(tr("Add Rule..."), this, &ForwardsPanel::addWithDialog);
    add->setEnabled(m_add->isEnabled());
    menu.exec(m_list->viewport()->mapToGlobal(pos));
}

} // namespace termx
