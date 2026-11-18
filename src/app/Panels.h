// SPDX-License-Identifier: GPL-3.0-or-later
// Dock panels: saved sessions, serial ports, command history; and the toast.
#pragma once

#include "history/HistoryStore.h"
#include "serial/SerialPortWatcher.h"

#include <QFrame>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <functional>

class QComboBox;
class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;
class QHBoxLayout;

namespace termx {

class SessionStore;
class DeviceProfiles;

class SessionTreePanel : public QWidget {
    Q_OBJECT
public:
    SessionTreePanel(SessionStore *store, QWidget *parent = nullptr);
    void rebuild();

Q_SIGNALS:
    void openRequested(const QString &id);
    void editRequested(const QString &id);
    void newRequested(const QString &folder);

private:
    void showMenu(const QPoint &pos);
    QTreeWidgetItem *folderItem(const QString &path);

    SessionStore *m_store;
    QLineEdit *m_filter;
    QTreeWidget *m_tree;
    QHash<QString, QTreeWidgetItem *> m_folderItems;
};

class SerialPanel : public QWidget {
    Q_OBJECT
public:
    SerialPanel(SerialPortWatcher *watcher, DeviceProfiles *profiles, QWidget *parent = nullptr);
    void setPortsInUse(const QStringList &ports);
    void rebuild();

Q_SIGNALS:
    void openRequested(const termx::SerialPortEntry &port);
    void openWithSettingsRequested(const termx::SerialPortEntry &port);

private:
    void showMenu(const QPoint &pos);
    SerialPortEntry entryFor(QTreeWidgetItem *item) const;

    SerialPortWatcher *m_watcher;
    DeviceProfiles *m_profiles;
    QTreeWidget *m_tree;
    QLabel *m_empty;
    QStringList m_inUse;
};

class HistoryPanel : public QWidget {
    Q_OBJECT
public:
    HistoryPanel(HistoryStore *store, QWidget *parent = nullptr);
    void setCurrentHost(const QString &host);
    void focusSearch();

Q_SIGNALS:
    void insertRequested(const QString &command, bool execute);

protected:
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    void refresh();
    void showMenu(const QPoint &pos);
    void emitInsert(bool execute);

    QPointer<HistoryStore> m_store;
    QLineEdit *m_search;
    QComboBox *m_scope;
    QTreeWidget *m_tree;
    QString m_host;
    QTimer m_refresh;
};

class Toast : public QFrame {
    Q_OBJECT
public:
    explicit Toast(QWidget *parent);
    using Action = std::pair<QString, std::function<void()>>;
    void showMessage(const QString &title, const QString &text, const QList<Action> &actions, int timeoutMs = 15000);
    void reposition();

private:
    QLabel *m_title;
    QLabel *m_text;
    QHBoxLayout *m_buttons;
    QTimer m_hide;
};

} // namespace termx
