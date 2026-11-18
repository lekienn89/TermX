// SPDX-License-Identifier: GPL-3.0-or-later
// Application preferences: appearance, terminal behaviour, keyboard shortcuts.
#pragma once

#include "Theme.h"

#include <QDialog>
#include <QHash>
#include <QKeySequence>
#include <QList>

class QAction;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFontComboBox;
class QKeySequenceEdit;
class QLabel;
class QLineEdit;
class QPushButton;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace termx {

class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    // `actions` are the rebindable actions; each carries the properties
    // "shortcutId", "menuPath" and "defaultShortcuts" (see MainWindow).
    PreferencesDialog(const QList<QAction *> &actions, QWidget *parent = nullptr);

    int themeMode() const;
    qreal fontSize() const;
    TerminalPrefs prefs() const;
    // shortcutId -> chosen key sequences, for every action.
    QHash<QString, QList<QKeySequence>> shortcuts() const { return m_keys; }

    void showKeyboardPage();

private:
    QWidget *buildAppearancePage();
    QWidget *buildTerminalPage();
    QWidget *buildKeyboardPage();
    void updatePreview();
    void refreshKeyRow(QTreeWidgetItem *item);
    void filterKeys(const QString &text);
    void onKeyItemChanged();
    void assignKey(const QKeySequence &seq);
    QTreeWidgetItem *currentKeyItem() const;

    QTabWidget *m_pages = nullptr;
    QComboBox *m_theme = nullptr;
    QFontComboBox *m_font = nullptr;
    QDoubleSpinBox *m_fontSize = nullptr;
    QLabel *m_preview = nullptr;
    QComboBox *m_cursorShape = nullptr;
    QCheckBox *m_cursorBlink = nullptr;
    QCheckBox *m_copyOnSelect = nullptr;
    QCheckBox *m_rightClickPaste = nullptr;

    QLineEdit *m_keyFilter = nullptr;
    QTreeWidget *m_keyTree = nullptr;
    QKeySequenceEdit *m_keyEdit = nullptr;
    QPushButton *m_keyClear = nullptr;
    QPushButton *m_keyReset = nullptr;
    QLabel *m_keyNote = nullptr;
    QHash<QString, QList<QKeySequence>> m_keys;
    QHash<QString, QList<QKeySequence>> m_defaults;
    QHash<QString, QString> m_titles;
    bool m_updatingEdit = false;
};

} // namespace termx
