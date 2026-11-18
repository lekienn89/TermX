// SPDX-License-Identifier: GPL-3.0-or-later
// Snippets dock: saved commands you send to the current terminal with a double-click.
#pragma once

#include <QDialog>
#include <QWidget>

class QLineEdit;
class QPlainTextEdit;
class QTreeWidget;

namespace termx {

class SnippetStore;
struct Snippet;

class SnippetDialog : public QDialog {
    Q_OBJECT
public:
    SnippetDialog(const Snippet &s, QWidget *parent = nullptr);
    Snippet snippet() const;

private:
    QString m_id;
    QLineEdit *m_name;
    QLineEdit *m_group;
    QPlainTextEdit *m_text;
};

class SnippetPanel : public QWidget {
    Q_OBJECT
public:
    SnippetPanel(SnippetStore *store, QWidget *parent = nullptr);
    void focusFilter();
    void addSnippet(const QString &initialText = QString());

Q_SIGNALS:
    void runRequested(const QString &text);
    void pasteRequested(const QString &text);   // insert without pressing Enter

protected:
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    void rebuild();
    QString currentId() const;
    void editCurrent();
    void removeCurrent();

    SnippetStore *m_store;
    QLineEdit *m_filter;
    QTreeWidget *m_tree;
};

} // namespace termx
