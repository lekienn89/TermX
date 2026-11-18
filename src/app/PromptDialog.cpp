// SPDX-License-Identifier: GPL-3.0-or-later
#include "PromptDialog.h"

#include "session/CredentialStore.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

namespace termx {

PromptDialog::PromptDialog(const PromptRequest &req, const QString &sessionName, QWidget *parent)
    : QDialog(parent), m_req(req)
{
    setWindowTitle(req.title.isEmpty() ? sessionName : req.title + QStringLiteral(" — ") + sessionName);
    setMinimumWidth(420);
    auto *layout = new QVBoxLayout(this);

    const bool hostKey = req.kind != PromptRequest::Kind::Fields;
    auto *top = new QHBoxLayout;
    if (hostKey) {
        auto *icon = new QLabel(this);
        const auto sp = req.kind == PromptRequest::Kind::HostKeyUnknown ? QStyle::SP_MessageBoxQuestion
                                                                        : QStyle::SP_MessageBoxWarning;
        icon->setPixmap(style()->standardIcon(sp).pixmap(40, 40));
        icon->setAlignment(Qt::AlignTop);
        top->addWidget(icon);
    }
    auto *msg = new QLabel(req.message, this);
    msg->setWordWrap(true);
    msg->setTextInteractionFlags(Qt::TextSelectableByMouse);
    top->addWidget(msg, 1);
    layout->addLayout(top);

    if (!hostKey) {
        auto *form = new QFormLayout;
        for (const PromptField &f : req.fields) {
            auto *e = new QLineEdit(this);
            e->setText(f.initialValue);
            e->setEchoMode(f.echo ? QLineEdit::Normal : QLineEdit::Password);
            QString label = f.label;
            if (label.endsWith(QLatin1Char(':')))
                label.chop(1);
            form->addRow(label.isEmpty() ? tr("Response") : label, e);
            m_edits.push_back(e);
        }
        layout->addLayout(form);
        if (req.offerRemember && CredentialStore::isAvailable()) {
            m_remember = new QCheckBox(tr("Save password (encrypted with your Windows account)"), this);
            layout->addWidget(m_remember);
        }
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        connect(buttons, &QDialogButtonBox::accepted, this, [this] {
            m_reply.accepted = true;
            for (QLineEdit *e : m_edits)
                m_reply.values << e->text();
            m_reply.remember = m_remember && m_remember->isChecked();
            accept();
        });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);
        if (!m_edits.isEmpty())
            m_edits.first()->setFocus();
    } else {
        auto *buttons = new QDialogButtonBox(this);
        const bool changed = req.kind == PromptRequest::Kind::HostKeyChanged;
        const bool legacy = req.kind == PromptRequest::Kind::LegacyAlgorithms;
        QPushButton *save = buttons->addButton(changed  ? tr("Replace saved key and connect")
                                               : legacy ? tr("Always allow for this session")
                                                        : tr("Trust and save"),
                                               QDialogButtonBox::AcceptRole);
        QPushButton *once = buttons->addButton(tr("Connect once"), QDialogButtonBox::AcceptRole);
        QPushButton *cancel = buttons->addButton(QDialogButtonBox::Cancel);
        connect(save, &QPushButton::clicked, this, [this] {
            m_reply.accepted = true;
            m_reply.remember = true;
            accept();
        });
        connect(once, &QPushButton::clicked, this, [this] {
            m_reply.accepted = true;
            m_reply.remember = false;
            accept();
        });
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        // Safer default when the key changed.
        (changed ? cancel : save)->setDefault(true);
        (changed ? cancel : save)->setFocus();
        layout->addWidget(buttons);
    }
}

} // namespace termx
