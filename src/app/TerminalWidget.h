// SPDX-License-Identifier: GPL-3.0-or-later
// Renders a TerminalSession and turns keyboard / mouse input into bytes.
#pragma once

#include "history/HistoryStore.h"
#include "terminal/TerminalSession.h"
#include "terminal/VtScreen.h"

#include <QFont>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <vector>

class QListWidget;

namespace termx {

class TerminalSession;
class HistoryStore;

class TerminalWidget : public QWidget {
    Q_OBJECT
public:
    TerminalWidget(TerminalSession *session, HistoryStore *history, QWidget *parent = nullptr);
    ~TerminalWidget() override;

    TerminalSession *session() const { return m_session; }

    // Scrolling: offset = lines above the live screen (0 = following output).
    int scrollOffset() const { return m_scrollOffset; }
    int maxScrollOffset() const;
    void setScrollOffset(int offset);
    void scrollToBottom() { setScrollOffset(0); }

    bool hasSelection() const { return m_selActive && !(m_selA.line == m_selB.line && m_selA.col == m_selB.col); }
    QString selectedText() const;
    void copySelection();
    void pasteClipboard();
    void pasteText(const QString &text);
    void selectAll();
    void clearSelection();

    // Search (returns false when nothing found).
    bool find(const QString &text, bool backwards, bool caseSensitive);
    void clearSearch();
    int searchMatchCountVisible() const;

    void applyAppearance();   // after theme / font changes
    QSize cellSize() const { return QSize(int(m_cw), int(m_ch)); }
    QSize sizeForCells(int cols, int rows) const;

Q_SIGNALS:
    void scrollChanged();
    void gridSizeChanged(int cols, int rows);
    void focused();
    void searchRequested();

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    bool event(QEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void inputMethodEvent(QInputMethodEvent *e) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery q) const override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void focusInEvent(QFocusEvent *e) override;
    void focusOutEvent(QFocusEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    bool focusNextPrevChild(bool) override { return false; }

private:
    struct Pos {
        qint64 line = 0;
        int col = 0;
        bool operator<(const Pos &o) const { return line < o.line || (line == o.line && col < o.col); }
        bool operator==(const Pos &o) const { return line == o.line && col == o.col; }
    };
    enum class SelMode { Char, Word, Line };

    void updateMetrics();
    void onScreenUpdated();
    void onScrollbackChanged();
    qint64 topLine() const;
    Pos posAt(const QPointF &p, bool round) const;
    bool isSelected(qint64 line, int col) const;
    void normalizedSelection(Pos &a, Pos &b) const;
    void extendSelection(const Pos &p);
    void wordBounds(const Pos &p, Pos &start, Pos &end) const;
    QString lineString(qint64 line, std::vector<int> *colOfChar) const;
    bool forwardMouse(QMouseEvent *e, bool pressed, bool release);
    int vtermMods(Qt::KeyboardModifiers m) const;
    bool handleSuggestionKey(QKeyEvent *e);
    void updateSuggestions();
    void hideSuggestions();
    void acceptSuggestion();
    void drawRow(QPainter &p, int row, qint64 absLine);
    QRect cursorRect() const;

    QPointer<TerminalSession> m_session;
    QPointer<HistoryStore> m_history;

    QFont m_font, m_fontBold, m_fontItalic, m_fontBoldItalic;
    qreal m_cw = 8;
    qreal m_ch = 16;
    qreal m_ascent = 12;

    int m_scrollOffset = 0;
    qint64 m_lastFirstLine = 0;

    bool m_selActive = false;
    bool m_selecting = false;
    SelMode m_selMode = SelMode::Char;
    Pos m_selA, m_selB;           // anchor, extent
    Pos m_wordA, m_wordB;         // initial word/line for double/triple-click drags
    QPointF m_pressPos;
    qint64 m_lastClickMs = 0;
    int m_clickCount = 0;
    QTimer m_autoScroll;
    int m_autoScrollDir = 0;
    int m_mouseButtonsDown = 0;

    QString m_searchText;
    bool m_searchCase = false;
    Pos m_searchHit{-1, 0};
    int m_searchLen = 0;

    QTimer m_blink;
    bool m_blinkOn = true;
    bool m_hasFocus = false;

    QListWidget *m_suggest = nullptr;
    QString m_suggestInput;
    QTimer m_suggestTimer;

    mutable std::vector<Cell> m_cells;
};

} // namespace termx
