// SPDX-License-Identifier: GPL-3.0-or-later
#include "VtScreen.h"

#include <vterm.h>

#include <algorithm>
#include <cstring>

namespace termx {

namespace {

VtScreen *self(void *user) { return static_cast<VtScreen *>(user); }

int cb_damage(VTermRect, void *user) { return self(user)->cbDamage(); }
int cb_moverect(VTermRect, VTermRect, void *user) { return self(user)->cbDamage(); }
int cb_movecursor(VTermPos pos, VTermPos, int visible, void *user) { return self(user)->cbMoveCursor(pos.row, pos.col, visible); }
int cb_settermprop(VTermProp prop, VTermValue *val, void *user) { return self(user)->cbSetTermProp(prop, val); }
int cb_bell(void *user) { return self(user)->cbBell(); }
int cb_sb_pushline(int cols, const VTermScreenCell *cells, void *user) { return self(user)->cbPushLine(cols, cells); }
int cb_sb_popline(int cols, VTermScreenCell *cells, void *user) { return self(user)->cbPopLine(cols, cells); }
int cb_sb_clear(void *user) { return self(user)->cbClearScrollback(); }
int cb_osc(int command, VTermStringFragment frag, void *user) { return self(user)->cbOsc(command, &frag); }
void cb_output(const char *s, size_t len, void *user)
{
    VtScreen *v = self(user);
    if (v->onOutput)
        v->onOutput(s, len);
}

const VTermScreenCallbacks kScreenCallbacks = {
    cb_damage, cb_moverect, cb_movecursor, cb_settermprop, cb_bell, nullptr /*resize*/,
    cb_sb_pushline, cb_sb_popline, cb_sb_clear,
};

const VTermStateFallbacks kFallbacks = {
    nullptr /*control*/, nullptr /*csi*/, cb_osc, nullptr /*dcs*/, nullptr /*apc*/, nullptr /*pm*/, nullptr /*sos*/,
};

uint32_t rgbOf(const VTermColor &c)
{
    return (uint32_t(c.rgb.red) << 16) | (uint32_t(c.rgb.green) << 8) | uint32_t(c.rgb.blue);
}

VTermModifier toMods(int m)
{
    return static_cast<VTermModifier>(m & VTERM_ALL_MODS_MASK);
}

constexpr int kMaxCols = 2000;
constexpr int kMaxRows = 1000;

} // namespace

VtScreen::VtScreen(int rows, int cols, int scrollbackLimit)
    : m_rows(std::clamp(rows, 1, kMaxRows)), m_cols(std::clamp(cols, 1, kMaxCols)),
      m_limit(std::max(0, scrollbackLimit))
{
    m_vt = vterm_new(m_rows, m_cols);
    vterm_set_utf8(m_vt, 1);
    vterm_output_set_callback(m_vt, cb_output, this);
    m_state = vterm_obtain_state(m_vt);
    m_screen = vterm_obtain_screen(m_vt);
    vterm_screen_set_callbacks(m_screen, &kScreenCallbacks, this);
    vterm_screen_set_unrecognised_fallbacks(m_screen, &kFallbacks, this);
    vterm_screen_enable_altscreen(m_screen, 1);
    vterm_screen_enable_reflow(m_screen, true);
    vterm_screen_set_damage_merge(m_screen, VTERM_DAMAGE_SCROLL);
    vterm_state_set_bold_highbright(m_state, 1);
    vterm_screen_reset(m_screen, 1);
}

VtScreen::~VtScreen()
{
    if (m_vt)
        vterm_free(m_vt);
}

void VtScreen::feed(const char *bytes, size_t len)
{
    if (!len)
        return;
    vterm_input_write(m_vt, bytes, len);
    vterm_screen_flush_damage(m_screen);
}

void VtScreen::resize(int rows, int cols)
{
    rows = std::clamp(rows, 1, kMaxRows);
    cols = std::clamp(cols, 1, kMaxCols);
    if (rows == m_rows && cols == m_cols)
        return;
    m_rows = rows;
    m_cols = cols;
    vterm_set_size(m_vt, rows, cols);
    vterm_screen_flush_damage(m_screen);
    VTermPos pos;
    vterm_state_get_cursorpos(m_state, &pos);
    m_cursor.row = std::clamp(pos.row, 0, rows - 1);
    m_cursor.col = std::clamp(pos.col, 0, cols - 1);
    m_dirty = true;
}

void VtScreen::setScrollbackLimit(int lines)
{
    m_limit = std::max(0, lines);
    trimScrollback();
}

void VtScreen::trimScrollback()
{
    bool changed = false;
    while (int(m_scrollback.size()) > m_limit) {
        m_scrollback.pop_front();
        changed = true;
    }
    if (changed && onScrollbackChanged)
        onScrollbackChanged();
}

void VtScreen::clearScrollback()
{
    m_scrollback.clear();
    m_dirty = true;
    if (onScrollbackChanged)
        onScrollbackChanged();
}

void VtScreen::convertCell(const void *vc, Cell &out) const
{
    const VTermScreenCell &c = *static_cast<const VTermScreenCell *>(vc);
    if (c.chars[0] == uint32_t(-1)) {
        out.ch = 0;
    } else if (c.chars[0] == 0) {
        out.ch = U' ';
    } else {
        out.ch = c.chars[0] > 0x10FFFF ? U'�' : char32_t(c.chars[0]);
    }
    out.comb = {{0, 0}};
    if (out.ch && c.chars[0]) {
        for (int i = 1; i < 3 && i < VTERM_MAX_CHARS_PER_CELL && c.chars[i]; ++i)
            out.comb[size_t(i - 1)] = c.chars[i] > 0x10FFFF ? 0 : char32_t(c.chars[i]);
    }
    out.width = c.width == 2 ? 2 : 1;

    uint16_t a = 0;
    if (c.attrs.bold) a |= AttrBold;
    if (c.attrs.italic) a |= AttrItalic;
    if (c.attrs.underline == VTERM_UNDERLINE_SINGLE) a |= AttrUnderline;
    if (c.attrs.underline == VTERM_UNDERLINE_DOUBLE) a |= AttrDoubleUnderline;
    if (c.attrs.underline == VTERM_UNDERLINE_CURLY) a |= AttrCurlyUnderline;
    if (c.attrs.reverse) a |= AttrReverse;
    if (c.attrs.strike) a |= AttrStrike;
    if (c.attrs.blink) a |= AttrBlink;
    if (c.attrs.conceal) a |= AttrConceal;

    VTermColor fg = c.fg;
    VTermColor bg = c.bg;
    if (VTERM_COLOR_IS_DEFAULT_FG(&fg)) {
        a |= AttrDefaultFg;
        out.fg = 0;
    } else {
        vterm_screen_convert_color_to_rgb(m_screen, &fg);
        out.fg = rgbOf(fg);
    }
    if (VTERM_COLOR_IS_DEFAULT_BG(&bg)) {
        a |= AttrDefaultBg;
        out.bg = 0;
    } else {
        vterm_screen_convert_color_to_rgb(m_screen, &bg);
        out.bg = rgbOf(bg);
    }
    out.attrs = a;
}

void VtScreen::storedToCells(const StoredLine &s, std::vector<Cell> &out) const
{
    out.assign(size_t(m_cols), Cell());
    const size_t n = std::min(s.chars.size(), size_t(m_cols));
    size_t run = 0;
    for (size_t i = 0; i < n; ++i) {
        while (run + 1 < s.runs.size() && s.runs[run + 1].start <= i)
            ++run;
        Cell &c = out[i];
        c.ch = s.chars[i];
        if (!s.runs.empty()) {
            c.attrs = s.runs[run].attrs;
            c.fg = s.runs[run].fg;
            c.bg = s.runs[run].bg;
        }
        c.width = (c.ch != 0 && i + 1 < s.chars.size() && s.chars[i + 1] == 0) ? 2 : 1;
    }
    for (const auto &cm : s.comb) {
        if (cm.first < out.size())
            out[cm.first].comb = cm.second;
    }
}

void VtScreen::line(qint64 absLine, std::vector<Cell> &out) const
{
    if (absLine >= m_pushedTotal) {
        const int row = int(absLine - m_pushedTotal);
        out.assign(size_t(m_cols), Cell());
        if (row < 0 || row >= m_rows)
            return;
        VTermScreenCell vc;
        for (int col = 0; col < m_cols; ++col) {
            if (vterm_screen_get_cell(m_screen, VTermPos{row, col}, &vc))
                convertCell(&vc, out[size_t(col)]);
        }
        return;
    }
    const qint64 idx = absLine - firstAvailableLine();
    if (idx < 0 || idx >= qint64(m_scrollback.size())) {
        out.assign(size_t(m_cols), Cell());
        return;
    }
    storedToCells(m_scrollback[size_t(idx)], out);
}

QString VtScreen::lineText(qint64 absLine, int fromCol, int toCol) const
{
    std::vector<Cell> cells;
    line(absLine, cells);
    if (toCol < 0 || toCol > int(cells.size()))
        toCol = int(cells.size());
    fromCol = std::clamp(fromCol, 0, toCol);
    QString s;
    s.reserve(toCol - fromCol);
    for (int i = fromCol; i < toCol; ++i) {
        const Cell &c = cells[size_t(i)];
        if (c.ch == 0)
            continue;
        s += QString::fromUcs4(&c.ch, 1);
        for (char32_t m : c.comb)
            if (m)
                s += QString::fromUcs4(&m, 1);
    }
    return s;
}

bool VtScreen::lineContinuesNext(qint64 absLine) const
{
    const qint64 next = absLine + 1;
    if (next < m_pushedTotal || next >= m_pushedTotal + m_rows)
        return false;   // scrollback lines do not keep wrap info
    const VTermLineInfo *info = vterm_state_get_lineinfo(m_state, int(next - m_pushedTotal));
    return info && info->continuation;
}

void VtScreen::keyboardKey(int key, int mods)
{
    vterm_keyboard_key(m_vt, static_cast<VTermKey>(key), toMods(mods));
}

void VtScreen::keyboardChar(uint32_t ch, int mods)
{
    vterm_keyboard_unichar(m_vt, ch, toMods(mods));
}

void VtScreen::startPaste() { vterm_keyboard_start_paste(m_vt); }
void VtScreen::endPaste() { vterm_keyboard_end_paste(m_vt); }

void VtScreen::mouseMove(int row, int col, int mods)
{
    vterm_mouse_move(m_vt, std::clamp(row, 0, m_rows - 1), std::clamp(col, 0, m_cols - 1), toMods(mods));
}

void VtScreen::mouseButton(int button, bool pressed, int mods)
{
    vterm_mouse_button(m_vt, button, pressed, toMods(mods));
}

void VtScreen::focusIn() { vterm_state_focus_in(m_state); }
void VtScreen::focusOut() { vterm_state_focus_out(m_state); }

void VtScreen::reset()
{
    vterm_screen_reset(m_screen, 1);
    m_dirty = true;
}

void VtScreen::setPalette(uint32_t defaultFg, uint32_t defaultBg, const std::array<uint32_t, 16> &ansi)
{
    auto mk = [](uint32_t rgb) {
        VTermColor c;
        vterm_color_rgb(&c, uint8_t(rgb >> 16), uint8_t(rgb >> 8), uint8_t(rgb));
        return c;
    };
    VTermColor fg = mk(defaultFg);
    VTermColor bg = mk(defaultBg);
    vterm_screen_set_default_colors(m_screen, &fg, &bg);
    for (int i = 0; i < 16; ++i) {
        VTermColor c = mk(ansi[size_t(i)]);
        vterm_state_set_palette_color(m_state, i, &c);
    }
    m_dirty = true;
}

// ---------------------------------------------------------------- callbacks

int VtScreen::cbDamage()
{
    m_dirty = true;
    return 1;
}

int VtScreen::cbMoveCursor(int row, int col, int visible)
{
    m_cursor.row = row;
    m_cursor.col = col;
    (void)visible;
    m_dirty = true;
    return 1;
}

int VtScreen::cbSetTermProp(int prop, const void *v)
{
    const VTermValue *val = static_cast<const VTermValue *>(v);
    switch (prop) {
    case VTERM_PROP_CURSORVISIBLE: m_cursor.visible = val->boolean; break;
    case VTERM_PROP_CURSORBLINK: m_cursor.blink = val->boolean; break;
    case VTERM_PROP_CURSORSHAPE:
        m_cursor.shape = val->number == VTERM_PROP_CURSORSHAPE_UNDERLINE ? CursorInfo::Underline
                       : val->number == VTERM_PROP_CURSORSHAPE_BAR_LEFT ? CursorInfo::Bar
                                                                        : CursorInfo::Block;
        break;
    case VTERM_PROP_ALTSCREEN: m_altScreen = val->boolean; break;
    case VTERM_PROP_REVERSE: m_reverse = val->boolean; break;
    case VTERM_PROP_MOUSE: m_mouseMode = val->number; break;
    case VTERM_PROP_TITLE: {
        const VTermStringFragment &f = val->string;
        if (f.initial)
            m_titleBuf.clear();
        if (m_titleBuf.size() < 1024 && f.str)
            m_titleBuf.append(f.str, std::min<size_t>(f.len, 1024 - m_titleBuf.size()));
        if (f.final) {
            m_title = QString::fromUtf8(m_titleBuf.data(), int(m_titleBuf.size())).left(256);
            if (onTitle)
                onTitle(m_title);
        }
        break;
    }
    default: break;
    }
    m_dirty = true;
    return 1;
}

int VtScreen::cbBell()
{
    if (onBell)
        onBell();
    return 1;
}

int VtScreen::cbPushLine(int cols, const void *cellsPtr)
{
    const auto *cells = static_cast<const VTermScreenCell *>(cellsPtr);
    StoredLine s;
    // Find the last cell worth keeping (trailing default blanks are dropped).
    int last = cols - 1;
    for (; last >= 0; --last) {
        const VTermScreenCell &c = cells[last];
        const bool blank = c.chars[0] == 0 || c.chars[0] == ' ';
        if (!blank || !VTERM_COLOR_IS_DEFAULT_BG(&c.bg) || c.attrs.reverse || c.attrs.underline)
            break;
    }
    const int n = last + 1;
    s.chars.reserve(size_t(n));
    Cell cell;
    Cell prev;
    for (int i = 0; i < n; ++i) {
        convertCell(&cells[i], cell);
        s.chars.push_back(cell.ch);
        if (cell.comb[0])
            s.comb.emplace_back(uint16_t(i), cell.comb);
        if (i == 0 || !cell.sameStyle(prev))
            s.runs.push_back(Run{uint16_t(i), cell.attrs, cell.fg, cell.bg});
        prev = cell;
    }
    s.chars.shrink_to_fit();
    m_scrollback.push_back(std::move(s));
    ++m_pushedTotal;
    if (int(m_scrollback.size()) > m_limit)
        m_scrollback.pop_front();
    m_dirty = true;
    if (onScrollbackChanged)
        onScrollbackChanged();
    return 1;
}

int VtScreen::cbPopLine(int cols, void *cellsPtr)
{
    if (m_scrollback.empty())
        return 0;
    auto *cells = static_cast<VTermScreenCell *>(cellsPtr);
    StoredLine s = std::move(m_scrollback.back());
    m_scrollback.pop_back();
    --m_pushedTotal;

    VTermColor defFg, defBg;
    vterm_state_get_default_colors(m_state, &defFg, &defBg);
    std::vector<Cell> tmp;
    const int savedCols = m_cols;
    m_cols = cols;               // storedToCells fills m_cols cells
    storedToCells(s, tmp);
    m_cols = savedCols;

    for (int i = 0; i < cols; ++i) {
        VTermScreenCell &dst = cells[i];
        std::memset(&dst, 0, sizeof(dst));
        const Cell &c = tmp[size_t(i)];
        if (c.ch == 0) {
            dst.chars[0] = 0;        // continuation is skipped by libvterm (width 2 on previous)
        } else if (c.ch != U' ') {
            dst.chars[0] = uint32_t(c.ch);
            dst.chars[1] = c.comb[0];
            dst.chars[2] = c.comb[0] ? c.comb[1] : 0;
        }
        dst.width = (c.width == 2 && i + 1 < cols) ? 2 : 1;
        dst.attrs.bold = (c.attrs & AttrBold) != 0;
        dst.attrs.italic = (c.attrs & AttrItalic) != 0;
        dst.attrs.underline = (c.attrs & AttrUnderline) ? VTERM_UNDERLINE_SINGLE
                            : (c.attrs & AttrDoubleUnderline) ? VTERM_UNDERLINE_DOUBLE
                            : (c.attrs & AttrCurlyUnderline) ? VTERM_UNDERLINE_CURLY : 0;
        dst.attrs.reverse = (c.attrs & AttrReverse) != 0;
        dst.attrs.strike = (c.attrs & AttrStrike) != 0;
        dst.attrs.blink = (c.attrs & AttrBlink) != 0;
        dst.attrs.conceal = (c.attrs & AttrConceal) != 0;
        if (c.attrs & AttrDefaultFg)
            dst.fg = defFg;
        else
            vterm_color_rgb(&dst.fg, uint8_t(c.fg >> 16), uint8_t(c.fg >> 8), uint8_t(c.fg));
        if (c.attrs & AttrDefaultBg)
            dst.bg = defBg;
        else
            vterm_color_rgb(&dst.bg, uint8_t(c.bg >> 16), uint8_t(c.bg >> 8), uint8_t(c.bg));
    }
    m_dirty = true;
    if (onScrollbackChanged)
        onScrollbackChanged();
    return 1;
}

int VtScreen::cbClearScrollback()
{
    clearScrollback();
    return 1;
}

int VtScreen::cbOsc(int command, const void *fragPtr)
{
    if (command != 133)
        return 0;
    const auto *f = static_cast<const VTermStringFragment *>(fragPtr);
    if (f->initial)
        m_oscBuf.clear();
    if (f->str && m_oscBuf.size() < 256)
        m_oscBuf.append(f->str, std::min<size_t>(f->len, 256 - m_oscBuf.size()));
    if (!f->final)
        return 1;
    if (!m_oscBuf.empty()) {
        VTermPos pos;
        vterm_state_get_cursorpos(m_state, &pos);
        switch (m_oscBuf[0]) {
        case 'A':   // prompt start
            m_cmdStart = Mark();
            break;
        case 'B':   // prompt end = command start
            m_cmdStart.line = m_pushedTotal + pos.row;
            m_cmdStart.col = pos.col;
            break;
        default:
            break;
        }
    }
    return 1;
}

} // namespace termx
