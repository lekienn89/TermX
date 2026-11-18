// SPDX-License-Identifier: GPL-3.0-or-later
// Ctrl+Shift+P: type a few letters to run any menu command, open a saved
// session or send a snippet, without touching the mouse.
#pragma once

#include <QFrame>

#include <functional>

class QLineEdit;
class QListWidget;

namespace termx {

class CommandPalette : public QFrame {
    Q_OBJECT
public:
    struct Item {
        QString title;
        QString detail;      // shown dimmed on the right (shortcut, target, ...)
        QString category;    // "Command", "Session", "Snippet"
        std::function<void()> run;
    };

    explicit CommandPalette(QWidget *parent);
    void popup(const QList<Item> &items);

Q_SIGNALS:
    void closed();

protected:
    bool eventFilter(QObject *obj, QEvent *e) override;
    void hideEvent(QHideEvent *e) override;

private:
    void refilter();
    void runCurrent();

    QLineEdit *m_edit;
    QListWidget *m_list;
    QList<Item> m_items;
};

} // namespace termx
