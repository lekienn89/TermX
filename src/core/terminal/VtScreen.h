// SPDX-License-Identifier: GPL-3.0-or-later
// Terminal state: libvterm screen + TermX's own compact scrollback buffer.
// Lives on the GUI thread; it is fed in bounded slices by TerminalSession.
#pragma once

#include <QString>

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

struct VTerm;
struct VTermScreen;
struct VTermState;

namespace termx {

enum CellAttr : uint16_t {
    AttrBold = 1 << 0,
    AttrItalic = 1 << 1,
    AttrUnderline = 1 << 2,
    AttrDoubleUnderline = 1 << 3,
    AttrCurlyUnderline = 1 << 4,
    AttrReverse = 1 << 5,
    AttrStrike = 1 << 6,
    AttrBlink = 1 << 7,
    AttrConceal = 1 << 8,
    AttrDefaultFg = 1 << 9,   // fg is the theme's default foreground
    AttrDefaultBg = 1 << 10,  // bg is the theme's default background
};

struct Cell {
    char32_t ch = U' ';                 // 0 = right half of a double-width char
    std::array<char32_t, 2> comb{{0, 0}};  // combining marks
    uint32_t fg = 0;                    // 0xRRGGBB
    uint32_t bg = 0;
    uint16_t attrs = AttrDefaultFg | AttrDefaultBg;
    uint8_t width = 1;

    bool sameStyle(const Cell &o) const { return fg == o.fg && bg == o.bg && attrs == o.attrs; }
};

struct CursorInfo {
    int row = 0;
    int col = 0;
    bool visible = true;
    bool blink = true;
    enum Shape { Block, Underline, Bar } shape = Block;
};

class VtScreen {
public:
    VtScreen(int rows, int cols, int scrollbackLimit);
    ~VtScreen();
    VtScreen(const VtScreen &) = delete;
    VtScreen &operator=(const VtScreen &) = delete;

    // --- input from the host ---
    void feed(const char *bytes, size_t len);

    // --- geometry ---
    void resize(int rows, int cols);
    int rows() const { return m_rows; }
    int cols() const { return m_cols; }

    // --- lines ---
    // Absolute line numbers never change while a line exists: scrollback
    // lines have numbers below firstScreenLine(), the visible screen starts
    // at firstScreenLine().
    qint64 firstScreenLine() const { return m_pushedTotal; }
    qint64 firstAvailableLine() const { return m_pushedTotal - qint64(m_scrollback.size()); }
    int scrollbackSize() const { return int(m_scrollback.size()); }
    void setScrollbackLimit(int lines);
    void clearScrollback();

    // Fills exactly cols() cells for the given absolute line.
    void line(qint64 absLine, std::vector<Cell> &out) const;
    QString lineText(qint64 absLine, int fromCol = 0, int toCol = -1) const;
    bool lineContinuesNext(qint64 absLine) const;  // soft-wrapped into the next line

    // --- state ---
    CursorInfo cursor() const { return m_cursor; }
    bool altScreen() const { return m_altScreen; }
    int mouseMode() const { return m_mouseMode; }   // VTERM_PROP_MOUSE_*
    bool reverseVideo() const { return m_reverse; }
    QString title() const { return m_title; }

    // Shell integration (OSC 133): absolute position where the user's command starts.
    struct Mark { qint64 line = -1; int col = 0; };
    Mark commandStart() const { return m_cmdStart; }
    void clearCommandStart() { m_cmdStart = Mark(); }

    // --- input from the user (produces output via onOutput) ---
    void keyboardKey(int vtermKey, int mods);
    void keyboardChar(uint32_t ch, int mods);
    void startPaste();
    void endPaste();
    void mouseMove(int row, int col, int mods);
    void mouseButton(int button, bool pressed, int mods);
    void focusIn();
    void focusOut();

    void reset();
    void setPalette(uint32_t defaultFg, uint32_t defaultBg, const std::array<uint32_t, 16> &ansi);

    bool takeDirty() { const bool d = m_dirty; m_dirty = false; return d; }

    std::function<void(const char *, size_t)> onOutput;
    std::function<void()> onBell;
    std::function<void(const QString &)> onTitle;
    std::function<void()> onScrollbackChanged;

    // libvterm callbacks (public for the C trampolines).
    int cbDamage();
    int cbMoveCursor(int row, int col, int visible);
    int cbSetTermProp(int prop, const void *val);
    int cbBell();
    int cbPushLine(int cols, const void *cells);
    int cbPopLine(int cols, void *cells);
    int cbClearScrollback();
    int cbOsc(int command, const void *frag);

private:
    struct Run { uint16_t start; uint16_t attrs; uint32_t fg; uint32_t bg; };
    struct StoredLine {
        std::u32string chars;   // one entry per column, 0 = wide continuation
        std::vector<Run> runs;
        std::vector<std::pair<uint16_t, std::array<char32_t, 2>>> comb;
    };

    void convertCell(const void *vtermCell, Cell &out) const;
    void storedToCells(const StoredLine &s, std::vector<Cell> &out) const;
    void trimScrollback();

    VTerm *m_vt = nullptr;
    VTermScreen *m_screen = nullptr;
    VTermState *m_state = nullptr;
    int m_rows;
    int m_cols;
    int m_limit;
    std::deque<StoredLine> m_scrollback;
    qint64 m_pushedTotal = 0;
    CursorInfo m_cursor;
    bool m_altScreen = false;
    bool m_reverse = false;
    int m_mouseMode = 0;
    bool m_dirty = true;
    QString m_title;
    std::string m_titleBuf;
    std::string m_oscBuf;
    Mark m_cmdStart;
};

} // namespace termx
