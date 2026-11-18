// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "transport/Transport.h"

#include <QDialog>
#include <QVector>

class QCheckBox;
class QLineEdit;

namespace termx {

// Login / passphrase / keyboard-interactive / host-key questions.
class PromptDialog : public QDialog {
    Q_OBJECT
public:
    PromptDialog(const PromptRequest &req, const QString &sessionName, QWidget *parent = nullptr);
    PromptReply reply() const { return m_reply; }
    quint64 requestId() const { return m_req.id; }

private:
    PromptRequest m_req;
    PromptReply m_reply;
    QVector<QLineEdit *> m_edits;
    QCheckBox *m_remember = nullptr;
};

} // namespace termx
