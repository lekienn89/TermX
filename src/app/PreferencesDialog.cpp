// SPDX-License-Identifier: GPL-3.0-or-later
#include "PreferencesDialog.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QFontInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace termx {

namespace {
constexpr int kIdRole = Qt::UserRole + 1;

QString keysText(const QList<QKeySequence> &keys)
{
    QStringList parts;
    for (const QKeySequence &k : keys)
        if (!k.isEmpty())
            parts << k.toString(QKeySequence::NativeText);
    return parts.join(QStringLiteral(", "));
}

// Ctrl+letter and Alt+key normally belong to the program in the terminal.
bool terminalKey(const QKeySequence &k)
{
    if (k.isEmpty())
        return false;
    const QKeyCombination c = k[0];
    const Qt::KeyboardModifiers m = c.keyboardModifiers() & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier);
    if (m == Qt::ControlModifier || m == Qt::AltModifier || m == (Qt::AltModifier | Qt::ShiftModifier))
        return c.key() != Qt::Key_Tab && c.key() != Qt::Key_Comma;
    return m == Qt::NoModifier || m == Qt::ShiftModifier;
}
} // namespace

PreferencesDialog::PreferencesDialog(const QList<QAction *> &actions, QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Preferences"));
    resize(640, 520);
    for (QAction *a : actions) {
        const QString id = a->property("shortcutId").toString();
        if (id.isEmpty())
            continue;
        m_keys.insert(id, a->shortcuts());
        m_defaults.insert(id, QKeySequence::listFromString(a->property("defaultShortcuts").toString(), QKeySequence::PortableText));
        m_titles.insert(id, a->property("menuPath").toString());
    }

    m_pages = new QTabWidget(this);
    m_pages->addTab(buildAppearancePage(), tr("Appearance"));
    m_pages->addTab(buildTerminalPage(), tr("Terminal"));
    m_pages->addTab(buildKeyboardPage(), tr("Keyboard"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *lay = new QVBoxLayout(this);
    lay->addWidget(m_pages, 1);
    lay->addWidget(buttons);
    updatePreview();
}

void PreferencesDialog::showKeyboardPage() { m_pages->setCurrentIndex(2); }

QWidget *PreferencesDialog::buildAppearancePage()
{
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);

    m_theme = new QComboBox(page);
    m_theme->addItems({tr("Follow Windows"), tr("Dark"), tr("Light")});
    m_theme->setCurrentIndex(int(Theme::mode()));
    form->addRow(tr("Theme"), m_theme);

    m_font = new QFontComboBox(page);
    m_font->setFontFilters(QFontComboBox::MonospacedFonts);
    m_font->setCurrentFont(QFont(QFontInfo(Theme::terminalFont()).family()));
    form->addRow(tr("Terminal font"), m_font);

    m_fontSize = new QDoubleSpinBox(page);
    m_fontSize->setRange(6, 48);
    m_fontSize->setDecimals(1);
    m_fontSize->setSingleStep(0.5);
    m_fontSize->setSuffix(tr(" pt"));
    m_fontSize->setValue(Theme::terminalFontSize());
    form->addRow(tr("Font size"), m_fontSize);

    m_preview = new QLabel(page);
    m_preview->setTextFormat(Qt::PlainText);
    m_preview->setMinimumHeight(110);
    m_preview->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->addRow(tr("Preview"), m_preview);

    auto *hint = new QLabel(tr("Ctrl+Alt+= and Ctrl+Alt+- change the size at any time."), page);
    hint->setEnabled(false);
    form->addRow(QString(), hint);

    connect(m_theme, &QComboBox::currentIndexChanged, this, &PreferencesDialog::updatePreview);
    connect(m_font, &QFontComboBox::currentFontChanged, this, &PreferencesDialog::updatePreview);
    connect(m_fontSize, &QDoubleSpinBox::valueChanged, this, &PreferencesDialog::updatePreview);
    return page;
}

QWidget *PreferencesDialog::buildTerminalPage()
{
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);
    const TerminalPrefs &p = Theme::prefs();

    m_cursorShape = new QComboBox(page);
    m_cursorShape->addItems({tr("As requested by the program"), tr("Block"), tr("Underline"), tr("Bar")});
    m_cursorShape->setCurrentIndex(p.cursorShape);
    form->addRow(tr("Cursor"), m_cursorShape);

    m_cursorBlink = new QCheckBox(tr("Allow the cursor to blink"), page);
    m_cursorBlink->setChecked(p.cursorBlink);
    form->addRow(QString(), m_cursorBlink);

    m_copyOnSelect = new QCheckBox(tr("Copy selected text to the clipboard automatically"), page);
    m_copyOnSelect->setChecked(p.copyOnSelect);
    form->addRow(tr("Mouse"), m_copyOnSelect);

    m_rightClickPaste = new QCheckBox(tr("Right-click pastes (Shift+right-click opens the menu)"), page);
    m_rightClickPaste->setChecked(p.rightClickPaste);
    form->addRow(QString(), m_rightClickPaste);

    auto *note = new QLabel(tr("Middle-click always pastes. Per-session options such as scrollback size, "
                               "encoding and paste confirmation are in each session's settings."),
                            page);
    note->setWordWrap(true);
    note->setEnabled(false);
    form->addRow(QString(), note);
    return page;
}

QWidget *PreferencesDialog::buildKeyboardPage()
{
    auto *page = new QWidget(this);
    auto *lay = new QVBoxLayout(page);

    m_keyFilter = new QLineEdit(page);
    m_keyFilter->setPlaceholderText(tr("Filter by action or key"));
    m_keyFilter->setClearButtonEnabled(true);
    lay->addWidget(m_keyFilter);

    m_keyTree = new QTreeWidget(page);
    m_keyTree->setColumnCount(2);
    m_keyTree->setHeaderLabels({tr("Action"), tr("Shortcut")});
    m_keyTree->setRootIsDecorated(false);
    m_keyTree->setUniformRowHeights(true);
    m_keyTree->setAlternatingRowColors(true);
    m_keyTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_keyTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    QStringList ids = m_titles.keys();
    std::sort(ids.begin(), ids.end(), [this](const QString &a, const QString &b) {
        return m_titles.value(a).localeAwareCompare(m_titles.value(b)) < 0;
    });
    for (const QString &id : ids) {
        auto *item = new QTreeWidgetItem(m_keyTree);
        item->setData(0, kIdRole, id);
        item->setText(0, m_titles.value(id));
        refreshKeyRow(item);
    }
    lay->addWidget(m_keyTree, 1);

    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Shortcut:"), page));
    m_keyEdit = new QKeySequenceEdit(page);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    m_keyEdit->setMaximumSequenceLength(1);
#endif
    m_keyEdit->setToolTip(tr("Click here and press the new key combination"));
    row->addWidget(m_keyEdit, 1);
    m_keyClear = new QPushButton(tr("Clear"), page);
    m_keyClear->setToolTip(tr("Remove the shortcut from this action"));
    row->addWidget(m_keyClear);
    m_keyReset = new QPushButton(tr("Default"), page);
    m_keyReset->setToolTip(tr("Restore this action's default shortcut"));
    row->addWidget(m_keyReset);
    lay->addLayout(row);

    m_keyNote = new QLabel(page);
    m_keyNote->setWordWrap(true);
    lay->addWidget(m_keyNote);

    auto *resetAll = new QPushButton(tr("Restore All Defaults"), page);
    auto *bottom = new QHBoxLayout;
    bottom->addStretch();
    bottom->addWidget(resetAll);
    lay->addLayout(bottom);

    connect(m_keyFilter, &QLineEdit::textChanged, this, &PreferencesDialog::filterKeys);
    connect(m_keyTree, &QTreeWidget::currentItemChanged, this, &PreferencesDialog::onKeyItemChanged);
    connect(m_keyEdit, &QKeySequenceEdit::editingFinished, this, [this] {
        if (!m_updatingEdit)
            assignKey(m_keyEdit->keySequence());
    });
    connect(m_keyClear, &QPushButton::clicked, this, [this] { assignKey(QKeySequence()); });
    connect(m_keyReset, &QPushButton::clicked, this, [this] {
        QTreeWidgetItem *item = currentKeyItem();
        if (!item)
            return;
        const QString id = item->data(0, kIdRole).toString();
        m_keys[id] = m_defaults.value(id);
        refreshKeyRow(item);
        onKeyItemChanged();
    });
    connect(resetAll, &QPushButton::clicked, this, [this] {
        m_keys = m_defaults;
        for (int i = 0; i < m_keyTree->topLevelItemCount(); ++i)
            refreshKeyRow(m_keyTree->topLevelItem(i));
        onKeyItemChanged();
        m_keyNote->setText(tr("All shortcuts restored to their defaults."));
    });
    onKeyItemChanged();
    return page;
}

void PreferencesDialog::updatePreview()
{
    const bool dark = m_theme->currentIndex() == 1 || (m_theme->currentIndex() == 0 && Theme::isDark());
    const QFont f = Theme::terminalFont(m_font->currentFont().family(), m_fontSize->value());
    m_preview->setFont(f);
    m_preview->setText(QStringLiteral("admin@core-sw1:~$ show interfaces status\n"
                                      "Gi1/0/1   uplink    connected   trunk  a-full a-1000\n"
                                      "0O 1lI| {}[]() <= => != ~ -> │ ─ ┌┐"));
    m_preview->setStyleSheet(QStringLiteral("QLabel { background: %1; color: %2; padding: 8px; border-radius: 4px; }")
                                 .arg(dark ? QStringLiteral("#16181D") : QStringLiteral("#FBFBFC"),
                                      dark ? QStringLiteral("#D4D7DD") : QStringLiteral("#24292F")));
}

void PreferencesDialog::refreshKeyRow(QTreeWidgetItem *item)
{
    const QString id = item->data(0, kIdRole).toString();
    const QList<QKeySequence> keys = m_keys.value(id);
    item->setText(1, keysText(keys));
    QFont f = item->font(0);
    f.setBold(keys != m_defaults.value(id));
    item->setFont(0, f);
    item->setFont(1, f);
}

void PreferencesDialog::filterKeys(const QString &text)
{
    for (int i = 0; i < m_keyTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_keyTree->topLevelItem(i);
        item->setHidden(!text.isEmpty() && !item->text(0).contains(text, Qt::CaseInsensitive)
                        && !item->text(1).contains(text, Qt::CaseInsensitive));
    }
}

QTreeWidgetItem *PreferencesDialog::currentKeyItem() const
{
    QTreeWidgetItem *item = m_keyTree->currentItem();
    return item && !item->isHidden() ? item : nullptr;
}

void PreferencesDialog::onKeyItemChanged()
{
    QTreeWidgetItem *item = currentKeyItem();
    m_keyEdit->setEnabled(item);
    m_keyClear->setEnabled(item);
    m_keyReset->setEnabled(item);
    m_updatingEdit = true;
    const QList<QKeySequence> keys = item ? m_keys.value(item->data(0, kIdRole).toString()) : QList<QKeySequence>();
    m_keyEdit->setKeySequence(keys.isEmpty() ? QKeySequence() : keys.first());
    m_updatingEdit = false;
    if (item) {
        const QString def = keysText(m_defaults.value(item->data(0, kIdRole).toString()));
        m_keyNote->setText(def.isEmpty() ? tr("No default shortcut.") : tr("Default: %1").arg(def));
    } else {
        m_keyNote->setText(tr("Select an action, then press the new key combination."));
    }
}

void PreferencesDialog::assignKey(const QKeySequence &seqIn)
{
    QTreeWidgetItem *item = currentKeyItem();
    if (!item)
        return;
    const QKeySequence seq = seqIn.isEmpty() ? QKeySequence() : QKeySequence(seqIn[0]);
    const QString id = item->data(0, kIdRole).toString();
    QStringList notes;
    if (!seq.isEmpty()) {
        // One key, one action: take it away from whoever had it.
        for (auto it = m_keys.begin(); it != m_keys.end(); ++it) {
            if (it.key() == id || !it.value().contains(seq))
                continue;
            it.value().removeAll(seq);
            notes << tr("Removed from \"%1\".").arg(m_titles.value(it.key()));
            for (int i = 0; i < m_keyTree->topLevelItemCount(); ++i)
                if (m_keyTree->topLevelItem(i)->data(0, kIdRole).toString() == it.key())
                    refreshKeyRow(m_keyTree->topLevelItem(i));
        }
        if (terminalKey(seq))
            notes << tr("Programs in the terminal will no longer receive %1.").arg(seq.toString(QKeySequence::NativeText));
    }
    m_keys[id] = seq.isEmpty() ? QList<QKeySequence>() : QList<QKeySequence>{seq};
    refreshKeyRow(item);
    m_keyNote->setText(notes.isEmpty() ? (seq.isEmpty() ? tr("Shortcut removed.") : tr("Shortcut set.")) : notes.join(QLatin1Char(' ')));
}

int PreferencesDialog::themeMode() const { return m_theme->currentIndex(); }
qreal PreferencesDialog::fontSize() const { return m_fontSize->value(); }

TerminalPrefs PreferencesDialog::prefs() const
{
    TerminalPrefs p = Theme::prefs();
    const QString family = m_font->currentFont().family();
    // Keep the built-in fallback list when the user did not change the font.
    if (family != QFontInfo(Theme::terminalFont()).family() || !p.fontFamily.isEmpty())
        p.fontFamily = family;
    p.cursorShape = m_cursorShape->currentIndex();
    p.cursorBlink = m_cursorBlink->isChecked();
    p.copyOnSelect = m_copyOnSelect->isChecked();
    p.rightClickPaste = m_rightClickPaste->isChecked();
    return p;
}

} // namespace termx
