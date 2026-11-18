// SPDX-License-Identifier: GPL-3.0-or-later
#include "TerminalWidget.h"

#include "Theme.h"
#include "history/HistoryStore.h"
#include "terminal/TerminalSession.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QFontMetricsF>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QStyleHints>

#include <vterm_keycodes.h>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace termx {

namespace {

constexpr int kMaxPasteBytes = 4 * 1024 * 1024;

QColor rgb(uint32_t v) { return QColor::fromRgb(QRgb(0xFF000000u | v)); }

bool isWordChar(char32_t c)
{
    if (c == 0)
        return true;   // wide-char continuation belongs to the word
    if (c > 0x7f)
        return QChar::isLetterOrNumber(c);
    return std::isalnum(int(c)) || c == U'_' || c == U'-' || c == U'.' || c == U'/' || c == U':' || c == U'@'
        || c == U'~' || c == U'%' || c == U'+' || c == U'=' || c == U'?' || c == U'&' || c == U'#';
}

} // namespace

TerminalWidget::TerminalWidget(TerminalSession *session, HistoryStore *history, QWidget *parent)
    : QWidget(parent), m_session(session), m_history(history)
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setMouseTracking(true);
    setCursor(Qt::IBeamCursor);
    setMinimumSize(80, 40);

    updateMetrics();
    m_lastFirstLine = m_session->screen().firstScreenLine();

    connect(m_session, &TerminalSession::screenUpdated, this, &TerminalWidget::onScreenUpdated);
    connect(m_session, &TerminalSession::scrollbackChanged, this, &TerminalWidget::onScrollbackChanged);
    connect(m_session, &TerminalSession::inputChanged, this, [this] { m_suggestTimer.start(); });

    m_blink.setInterval(QApplication::cursorFlashTime() > 0 ? QApplication::cursorFlashTime() / 2 : 530);
    connect(&m_blink, &QTimer::timeout, this, [this] {
        m_blinkOn = !m_blinkOn;
        update(cursorRect());
    });

    m_autoScroll.setInterval(50);
    connect(&m_autoScroll, &QTimer::timeout, this, [this] {
        if (!m_selecting || m_autoScrollDir == 0)
            return;
        setScrollOffset(m_scrollOffset + (m_autoScrollDir < 0 ? 1 : -1));
        const QPointF local = mapFromGlobal(QCursor::pos());
        extendSelection(posAt(local, true));
    });

    m_suggest = new QListWidget(this);
    m_suggest->setFocusPolicy(Qt::NoFocus);
    m_suggest->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_suggest->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_suggest->setUniformItemSizes(true);
    m_suggest->hide();
    connect(m_suggest, &QListWidget::itemClicked, this, [this] { acceptSuggestion(); });
    m_suggestTimer.setSingleShot(true);
    m_suggestTimer.setInterval(70);
    connect(&m_suggestTimer, &QTimer::timeout, this, &TerminalWidget::updateSuggestions);

    applyAppearance();
}

TerminalWidget::~TerminalWidget() = default;

void TerminalWidget::updateMetrics()
{
    m_font = Theme::terminalFont();
    m_fontBold = m_font;
    m_fontBold.setBold(true);
    m_fontItalic = m_font;
    m_fontItalic.setItalic(true);
    m_fontBoldItalic = m_fontBold;
    m_fontBoldItalic.setItalic(true);
    const QFontMetricsF fm(m_font, this);
    m_cw = std::max<qreal>(1.0, fm.horizontalAdvance(QLatin1Char('M')));
    m_ch = std::max<qreal>(1.0, std::ceil(fm.height()));
    m_ascent = fm.ascent();
}

void TerminalWidget::applyAppearance()
{
    updateMetrics();
    const TerminalColors &tc = Theme::terminal();
    if (m_session)
        m_session->setPalette(tc.foreground.rgb() & 0xFFFFFF, tc.background.rgb() & 0xFFFFFF, tc.ansi);
    QPalette pal = m_suggest->palette();
    m_suggest->setFont(m_font);
    m_suggest->setStyleSheet(QStringLiteral("QListWidget { border: 1px solid %1; background: %2; color: %3; }"
                                            "QListWidget::item { padding: 2px 6px; }"
                                            "QListWidget::item:selected { background: %4; color: %5; }")
                                 .arg(Theme::accent().name(), (Theme::isDark() ? QColor(0x23262D) : QColor(0xFFFFFF)).name(),
                                      tc.foreground.name(), tc.selection.name(), tc.selectionText.name()));
    Q_UNUSED(pal);
    QResizeEvent ev(size(), size());
    resizeEvent(&ev);
    update();
}

QSize TerminalWidget::sizeForCells(int cols, int rows) const
{
    return QSize(int(std::ceil(cols * m_cw)), int(std::ceil(rows * m_ch)));
}

// ---------------------------------------------------------------- scrolling

int TerminalWidget::maxScrollOffset() const
{
    const VtScreen &s = m_session->screen();
    return int(s.firstScreenLine() - s.firstAvailableLine());
}

void TerminalWidget::setScrollOffset(int offset)
{
    offset = std::clamp(offset, 0, maxScrollOffset());
    if (offset == m_scrollOffset)
        return;
    m_scrollOffset = offset;
    hideSuggestions();
    update();
    Q_EMIT scrollChanged();
}

qint64 TerminalWidget::topLine() const
{
    return m_session->screen().firstScreenLine() - m_scrollOffset;
}

void TerminalWidget::onScrollbackChanged()
{
    const qint64 first = m_session->screen().firstScreenLine();
    const qint64 delta = first - m_lastFirstLine;
    m_lastFirstLine = first;
    if (m_scrollOffset > 0 && delta != 0)
        m_scrollOffset = std::clamp(int(m_scrollOffset + delta), 0, maxScrollOffset());
    m_scrollOffset = std::min(m_scrollOffset, maxScrollOffset());
    Q_EMIT scrollChanged();
}

void TerminalWidget::onScreenUpdated()
{
    m_blinkOn = true;
    if (m_suggest->isVisible())
        m_suggestTimer.start();
    update();
}

// ---------------------------------------------------------------- geometry

void TerminalWidget::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    const int cols = std::max(2, int(width() / m_cw));
    const int rows = std::max(1, int(height() / m_ch));
    if (m_session && (cols != m_session->screen().cols() || rows != m_session->screen().rows())) {
        m_session->resize(cols, rows);
        m_lastFirstLine = m_session->screen().firstScreenLine();
        m_scrollOffset = std::min(m_scrollOffset, maxScrollOffset());
        Q_EMIT gridSizeChanged(cols, rows);
        Q_EMIT scrollChanged();
    }
    hideSuggestions();
}

QRect TerminalWidget::cursorRect() const
{
    const CursorInfo c = m_session->screen().cursor();
    const int row = c.row + m_scrollOffset;
    return QRectF(c.col * m_cw - 1, row * m_ch - 1, m_cw * 2 + 2, m_ch + 2).toAlignedRect();
}

// ---------------------------------------------------------------- painting

void TerminalWidget::paintEvent(QPaintEvent *e)
{
    QPainter p(this);
    const TerminalColors &tc = Theme::terminal();
    p.fillRect(e->rect(), tc.background);
    if (!m_session)
        return;
    const VtScreen &scr = m_session->screen();
    const qint64 top = topLine();
    const int firstRow = std::max(0, int(e->rect().top() / m_ch));
    const int lastRow = std::min(scr.rows() - 1, int(e->rect().bottom() / m_ch));
    for (int r = firstRow; r <= lastRow; ++r)
        drawRow(p, r, top + r);

    // Cursor.
    const CursorInfo c = scr.cursor();
    const int crow = c.row + m_scrollOffset;
    const bool showCursor = c.visible && crow >= 0 && crow < scr.rows()
                            && (!m_hasFocus || !c.blink || m_blinkOn || m_session->state() != TransportState::Connected);
    if (showCursor) {
        scr.line(scr.firstScreenLine() + c.row, m_cells);
        const int col = std::clamp(c.col, 0, scr.cols() - 1);
        const Cell cell = m_cells[size_t(col)];
        const qreal w = m_cw * (cell.width == 2 ? 2 : 1);
        const QRectF cr(col * m_cw, crow * m_ch, w, m_ch);
        QColor cursorColor = tc.cursor;
        if (!m_hasFocus) {
            p.setPen(QPen(cursorColor, 1));
            p.setBrush(Qt::NoBrush);
            p.drawRect(cr.adjusted(0.5, 0.5, -0.5, -0.5));
        } else if (c.shape == CursorInfo::Underline) {
            p.fillRect(QRectF(cr.left(), cr.bottom() - 2, w, 2), cursorColor);
        } else if (c.shape == CursorInfo::Bar) {
            p.fillRect(QRectF(cr.left(), cr.top(), 2, m_ch), cursorColor);
        } else {
            p.fillRect(cr, cursorColor);
            if (cell.ch && cell.ch != U' ') {
                p.setFont((cell.attrs & AttrBold) ? m_fontBold : m_font);
                p.setPen(tc.background);
                QString s = QString::fromUcs4(&cell.ch, 1);
                for (char32_t m : cell.comb)
                    if (m)
                        s += QString::fromUcs4(&m, 1);
                p.drawText(QPointF(cr.left(), cr.top() + m_ascent), s);
            }
        }
    }
}

void TerminalWidget::drawRow(QPainter &p, int row, qint64 absLine)
{
    const TerminalColors &tc = Theme::terminal();
    const VtScreen &scr = m_session->screen();
    scr.line(absLine, m_cells);
    const int cols = int(m_cells.size());
    const qreal y = row * m_ch;

    // Search highlights on this line.
    std::vector<std::pair<int, int>> hits;   // [colStart, colEnd)
    bool currentHitHere = false;
    std::pair<int, int> currentHit{0, 0};
    if (!m_searchText.isEmpty()) {
        std::vector<int> colOf;
        const QString text = lineString(absLine, &colOf);
        const Qt::CaseSensitivity cs = m_searchCase ? Qt::CaseSensitive : Qt::CaseInsensitive;
        for (int from = 0;;) {
            const int idx = int(text.indexOf(m_searchText, from, cs));
            if (idx < 0)
                break;
            const int endIdx = idx + int(m_searchText.size()) - 1;
            const int c0 = colOf[size_t(idx)];
            const int c1 = colOf[size_t(std::min<int>(endIdx, int(colOf.size()) - 1))] + 1;
            hits.emplace_back(c0, c1);
            if (absLine == m_searchHit.line && c0 == m_searchHit.col) {
                currentHitHere = true;
                currentHit = {c0, c1};
            }
            from = idx + 1;
        }
    }

    auto colorsFor = [&](const Cell &c, int col, QColor &fg, QColor &bg) {
        fg = (c.attrs & AttrDefaultFg) ? tc.foreground : rgb(c.fg);
        bg = (c.attrs & AttrDefaultBg) ? tc.background : rgb(c.bg);
        if (c.attrs & AttrReverse)
            std::swap(fg, bg);
        if (c.attrs & AttrConceal)
            fg = bg;
        if (isSelected(absLine, col)) {
            bg = tc.selection;
            fg = tc.selectionText;
            return;
        }
        for (const auto &h : hits) {
            if (col >= h.first && col < h.second) {
                const bool cur = currentHitHere && h == currentHit;
                bg = cur ? tc.searchCurrent : tc.searchMatch;
                if (cur)
                    fg = QColor(0x111111);
                break;
            }
        }
    };

    // Background pass.
    QColor fg, bg;
    int runStart = -1;
    QColor runBg;
    for (int col = 0; col <= cols; ++col) {
        bool same = false;
        if (col < cols) {
            colorsFor(m_cells[size_t(col)], col, fg, bg);
            same = runStart >= 0 && bg == runBg;
        }
        if (!same) {
            if (runStart >= 0 && runBg != tc.background)
                p.fillRect(QRectF(runStart * m_cw, y, (col - runStart) * m_cw, m_ch), runBg);
            runStart = col;
            runBg = bg;
        }
    }

    // Text pass.
    QString run;
    int runCol = 0;
    QColor runFg;
    uint16_t runAttrs = 0;
    auto fontFor = [&](uint16_t a) -> const QFont & {
        const bool b = a & AttrBold, i = a & AttrItalic;
        return b && i ? m_fontBoldItalic : b ? m_fontBold : i ? m_fontItalic : m_font;
    };
    auto decorate = [&](int col0, int ncols, const QColor &color, uint16_t a) {
        if (!(a & (AttrUnderline | AttrDoubleUnderline | AttrCurlyUnderline | AttrStrike)))
            return;
        p.setPen(QPen(color, 1));
        const qreal x0 = col0 * m_cw, x1 = (col0 + ncols) * m_cw;
        const qreal uy = y + std::min(m_ascent + 2, m_ch - 1.5);
        if (a & (AttrUnderline | AttrCurlyUnderline))
            p.drawLine(QPointF(x0, uy), QPointF(x1, uy));
        if (a & AttrDoubleUnderline) {
            p.drawLine(QPointF(x0, uy - 1), QPointF(x1, uy - 1));
            p.drawLine(QPointF(x0, uy + 1), QPointF(x1, uy + 1));
        }
        if (a & AttrStrike)
            p.drawLine(QPointF(x0, y + m_ch / 2), QPointF(x1, y + m_ch / 2));
    };
    auto flush = [&] {
        if (run.isEmpty())
            return;
        p.setFont(fontFor(runAttrs));
        p.setPen(runFg);
        p.drawText(QPointF(runCol * m_cw, y + m_ascent), run);
        decorate(runCol, int(run.size()), runFg, runAttrs);
        run.clear();
    };
    for (int col = 0; col < cols; ++col) {
        const Cell &c = m_cells[size_t(col)];
        if (c.ch == 0)
            continue;
        colorsFor(c, col, fg, bg);
        const uint16_t styleAttrs = c.attrs & (AttrBold | AttrItalic | AttrUnderline | AttrDoubleUnderline
                                               | AttrCurlyUnderline | AttrStrike);
        const bool simple = c.width == 1 && c.ch >= 0x20 && c.ch < 0x7f && !c.comb[0];
        if (simple) {
            if (!run.isEmpty() && (fg != runFg || styleAttrs != runAttrs))
                flush();
            if (run.isEmpty()) {
                runCol = col;
                runFg = fg;
                runAttrs = styleAttrs;
            }
            run += QChar(char16_t(c.ch));
            continue;
        }
        flush();
        if (c.ch == U' ')
            continue;
        QString s = QString::fromUcs4(&c.ch, 1);
        for (char32_t m : c.comb)
            if (m)
                s += QString::fromUcs4(&m, 1);
        const qreal w = m_cw * (c.width == 2 ? 2 : 1);
        p.save();
        p.setClipRect(QRectF(col * m_cw, y, w, m_ch));
        p.setFont(fontFor(styleAttrs));
        p.setPen(fg);
        p.drawText(QPointF(col * m_cw, y + m_ascent), s);
        p.restore();
        decorate(col, c.width == 2 ? 2 : 1, fg, styleAttrs);
    }
    flush();
}

// ---------------------------------------------------------------- selection

TerminalWidget::Pos TerminalWidget::posAt(const QPointF &pt, bool round) const
{
    const VtScreen &s = m_session->screen();
    const int row = std::clamp(int(std::floor(pt.y() / m_ch)), 0, s.rows() - 1);
    const qreal fx = pt.x() / m_cw;
    const int col = std::clamp(int(round ? std::floor(fx + 0.5) : std::floor(fx)), 0, s.cols());
    return Pos{topLine() + row, col};
}

void TerminalWidget::normalizedSelection(Pos &a, Pos &b) const
{
    a = m_selA;
    b = m_selB;
    if (b < a)
        std::swap(a, b);
}

bool TerminalWidget::isSelected(qint64 line, int col) const
{
    if (!m_selActive)
        return false;
    Pos a, b;
    normalizedSelection(a, b);
    const Pos p{line, col};
    return !(p < a) && p < b;
}

void TerminalWidget::wordBounds(const Pos &p, Pos &start, Pos &end) const
{
    m_session->screen().line(p.line, m_cells);
    const int cols = int(m_cells.size());
    int c = std::clamp(p.col, 0, cols - 1);
    const bool word = isWordChar(m_cells[size_t(c)].ch) && m_cells[size_t(c)].ch != U' ';
    int s = c, e = c;
    if (word) {
        while (s > 0 && isWordChar(m_cells[size_t(s - 1)].ch) && m_cells[size_t(s - 1)].ch != U' ')
            --s;
        while (e + 1 < cols && isWordChar(m_cells[size_t(e + 1)].ch) && m_cells[size_t(e + 1)].ch != U' ')
            ++e;
    }
    start = Pos{p.line, s};
    end = Pos{p.line, e + 1};
}

void TerminalWidget::extendSelection(const Pos &p)
{
    if (m_selMode == SelMode::Char) {
        m_selB = p;
    } else if (m_selMode == SelMode::Word) {
        Pos s, e;
        wordBounds(Pos{p.line, std::min(p.col, m_session->screen().cols() - 1)}, s, e);
        if (s < m_wordA) {
            m_selA = m_wordB;
            m_selB = s;
        } else {
            m_selA = m_wordA;
            m_selB = e;
        }
    } else {
        const int cols = m_session->screen().cols();
        if (p.line < m_wordA.line) {
            m_selA = m_wordB;
            m_selB = Pos{p.line, 0};
        } else {
            m_selA = m_wordA;
            m_selB = Pos{p.line, cols};
        }
    }
    m_selActive = true;
    update();
}

QString TerminalWidget::lineString(qint64 line, std::vector<int> *colOfChar) const
{
    m_session->screen().line(line, m_cells);
    QString s;
    if (colOfChar)
        colOfChar->clear();
    for (int col = 0; col < int(m_cells.size()); ++col) {
        const Cell &c = m_cells[size_t(col)];
        if (c.ch == 0)
            continue;
        const QString part = QString::fromUcs4(&c.ch, 1);
        s += part;
        if (colOfChar)
            for (int i = 0; i < part.size(); ++i)
                colOfChar->push_back(col);
        for (char32_t m : c.comb) {
            if (!m)
                continue;
            const QString mp = QString::fromUcs4(&m, 1);
            s += mp;
            if (colOfChar)
                for (int i = 0; i < mp.size(); ++i)
                    colOfChar->push_back(col);
        }
    }
    return s;
}

QString TerminalWidget::selectedText() const
{
    if (!hasSelection())
        return QString();
    Pos a, b;
    normalizedSelection(a, b);
    const VtScreen &s = m_session->screen();
    a.line = std::max(a.line, s.firstAvailableLine());
    QString out;
    for (qint64 l = a.line; l <= b.line; ++l) {
        const int from = l == a.line ? a.col : 0;
        const int to = l == b.line ? b.col : s.cols();
        QString part = s.lineText(l, from, to);
        const bool wraps = s.lineContinuesNext(l);
        if (!wraps) {
            while (part.endsWith(QLatin1Char(' ')))
                part.chop(1);
        }
        out += part;
        if (l != b.line && !wraps)
            out += QLatin1Char('\n');
    }
#ifdef _WIN32
    out.replace(QLatin1Char('\n'), QStringLiteral("\r\n"));
#endif
    return out;
}

void TerminalWidget::copySelection()
{
    const QString t = selectedText();
    if (!t.isEmpty())
        QApplication::clipboard()->setText(t);
}

void TerminalWidget::selectAll()
{
    const VtScreen &s = m_session->screen();
    m_selA = Pos{s.firstAvailableLine(), 0};
    m_selB = Pos{s.firstScreenLine() + s.rows() - 1, s.cols()};
    m_selActive = true;
    update();
}

void TerminalWidget::clearSelection()
{
    if (!m_selActive)
        return;
    m_selActive = false;
    update();
}

void TerminalWidget::pasteClipboard()
{
    pasteText(QApplication::clipboard()->text());
}

void TerminalWidget::pasteText(const QString &textIn)
{
    if (!m_session || textIn.isEmpty())
        return;
    if (m_session->state() != TransportState::Connected) {
        QApplication::beep();
        return;
    }
    QString text = textIn;
    if (text.toUtf8().size() > kMaxPasteBytes) {
        QMessageBox::warning(this, tr("Paste"), tr("The clipboard holds more than 4 MB of text. Paste was cancelled."));
        return;
    }
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("\r\n|\n|\r")));
    const bool multi = lines.size() > 1 && !(lines.size() == 2 && lines.last().isEmpty());
    if (multi && m_session->config().terminal.confirmMultilinePaste) {
        QMessageBox box(QMessageBox::Question, tr("Paste multiple lines"),
                        tr("Paste %n line(s) into %1?", nullptr, int(lines.size())).arg(m_session->config().displayName()),
                        QMessageBox::Cancel, this);
        QStringList preview = lines.mid(0, 12);
        if (lines.size() > 12)
            preview << QStringLiteral("...");
        box.setInformativeText(preview.join(QLatin1Char('\n')));
        QPushButton *ok = box.addButton(tr("Paste"), QMessageBox::AcceptRole);
        box.setDefaultButton(ok);
        box.exec();
        if (box.clickedButton() != ok || !m_session)
            return;
    }
    scrollToBottom();
    m_session->paste(text);
}

// ---------------------------------------------------------------- search

bool TerminalWidget::find(const QString &text, bool backwards, bool caseSensitive)
{
    m_searchText = text;
    m_searchCase = caseSensitive;
    if (text.isEmpty()) {
        clearSearch();
        return false;
    }
    const VtScreen &s = m_session->screen();
    const qint64 first = s.firstAvailableLine();
    const qint64 last = s.firstScreenLine() + s.rows() - 1;
    const Qt::CaseSensitivity cs = caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;

    qint64 line = m_searchHit.line >= first ? m_searchHit.line : (backwards ? last : topLine());
    int startCol = m_searchHit.line >= first ? m_searchHit.col + (backwards ? -1 : 1) : (backwards ? s.cols() : 0);
    const qint64 total = last - first + 1;
    std::vector<int> colOf;
    for (qint64 step = 0; step <= total; ++step) {
        const QString str = lineString(line, &colOf);
        int idx = -1;
        if (backwards) {
            // last match starting before startCol
            int from = 0;
            for (;;) {
                const int i = int(str.indexOf(text, from, cs));
                if (i < 0 || colOf[size_t(i)] > startCol)
                    break;
                idx = i;
                from = i + 1;
            }
        } else {
            for (int from = 0;;) {
                const int i = int(str.indexOf(text, from, cs));
                if (i < 0)
                    break;
                if (colOf[size_t(i)] >= startCol) {
                    idx = i;
                    break;
                }
                from = i + 1;
            }
        }
        if (idx >= 0) {
            m_searchHit = Pos{line, colOf[size_t(idx)]};
            m_searchLen = int(text.size());
            // Bring it into view.
            const qint64 top = topLine();
            if (line < top || line >= top + s.rows()) {
                const qint64 wantTop = std::max(first, line - s.rows() / 2);
                setScrollOffset(int(s.firstScreenLine() - wantTop));
            }
            update();
            return true;
        }
        if (backwards) {
            line = line <= first ? last : line - 1;
            startCol = s.cols();
        } else {
            line = line >= last ? first : line + 1;
            startCol = 0;
        }
    }
    m_searchHit = Pos{-1, 0};
    update();
    return false;
}

void TerminalWidget::clearSearch()
{
    m_searchText.clear();
    m_searchHit = Pos{-1, 0};
    update();
}

// ---------------------------------------------------------------- keyboard

int TerminalWidget::vtermMods(Qt::KeyboardModifiers m) const
{
    int v = 0;
    if (m & Qt::ShiftModifier) v |= VTERM_MOD_SHIFT;
    if (m & Qt::AltModifier) v |= VTERM_MOD_ALT;
    if (m & Qt::ControlModifier) v |= VTERM_MOD_CTRL;
    return v;
}

bool TerminalWidget::event(QEvent *e)
{
    if (e->type() == QEvent::ShortcutOverride) {
        auto *ke = static_cast<QKeyEvent *>(e);
        const Qt::KeyboardModifiers m = ke->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier);
        const int k = ke->key();
        // Keys the terminal must receive even if an application shortcut uses them.
        const bool ctrlNoShift = (m & Qt::ControlModifier) && !(m & Qt::ShiftModifier) && !(m & Qt::AltModifier)
                                 && k != Qt::Key_Tab && k != Qt::Key_Backtab;
        const bool ciscoEscape = (m & Qt::ControlModifier) && (k == Qt::Key_6 || k == Qt::Key_AsciiCircum || k == Qt::Key_2
                                                               || k == Qt::Key_At || k == Qt::Key_Underscore || k == Qt::Key_Minus);
        const bool plain = m == Qt::NoModifier || m == Qt::ShiftModifier;
        if (ctrlNoShift || ciscoEscape || (m & Qt::AltModifier && !(m & Qt::ControlModifier))
            || (plain && (k == Qt::Key_Tab || k == Qt::Key_Backtab || k == Qt::Key_Escape || k == Qt::Key_Backspace))) {
            // Allow a few Ctrl+Shift app shortcuts to keep working.
            e->accept();
            return true;
        }
    }
    return QWidget::event(e);
}

bool TerminalWidget::handleSuggestionKey(QKeyEvent *e)
{
    if (!m_suggest->isVisible())
        return false;
    const auto m = e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier);
    switch (e->key()) {
    case Qt::Key_Right:
    case Qt::Key_End:
        if (m == Qt::NoModifier) {
            acceptSuggestion();
            return true;
        }
        break;
    case Qt::Key_Down:
    case Qt::Key_Up:
        if (m == Qt::AltModifier || m == Qt::ControlModifier) {
            const int n = m_suggest->count();
            int r = m_suggest->currentRow() + (e->key() == Qt::Key_Down ? 1 : -1);
            m_suggest->setCurrentRow((r + n) % n);
            return true;
        }
        break;
    case Qt::Key_Escape:
        hideSuggestions();
        return true;
    default:
        break;
    }
    return false;
}

void TerminalWidget::keyPressEvent(QKeyEvent *e)
{
    if (!m_session)
        return;
    if (handleSuggestionKey(e))
        return;

    const Qt::KeyboardModifiers qm = e->modifiers();
    const bool shift = qm & Qt::ShiftModifier;
    const bool ctrl = qm & Qt::ControlModifier;
    const bool alt = qm & Qt::AltModifier;
    const int key = e->key();

    // Local shortcuts (also available from the menu).
    if (ctrl && shift && !alt) {
        switch (key) {
        case Qt::Key_C: copySelection(); return;
        case Qt::Key_V: pasteClipboard(); return;
        case Qt::Key_F: Q_EMIT searchRequested(); return;
        case Qt::Key_A: selectAll(); return;
        default: break;
        }
    }
    if (shift && !ctrl && !alt) {
        const int page = std::max(1, m_session->screen().rows() - 1);
        switch (key) {
        case Qt::Key_Insert: pasteClipboard(); return;
        case Qt::Key_PageUp: setScrollOffset(m_scrollOffset + page); return;
        case Qt::Key_PageDown: setScrollOffset(m_scrollOffset - page); return;
        case Qt::Key_Home: setScrollOffset(maxScrollOffset()); return;
        case Qt::Key_End: setScrollOffset(0); return;
        default: break;
        }
    }
    if (ctrl && !shift && !alt && key == Qt::Key_Insert) {
        copySelection();
        return;
    }

    const int mods = vtermMods(qm);
    int vkey = VTERM_KEY_NONE;
    switch (key) {
    case Qt::Key_Return: vkey = VTERM_KEY_ENTER; break;
    case Qt::Key_Enter: vkey = (qm & Qt::KeypadModifier) ? VTERM_KEY_KP_ENTER : VTERM_KEY_ENTER; break;
    case Qt::Key_Tab: vkey = VTERM_KEY_TAB; break;
    case Qt::Key_Backtab: vkey = VTERM_KEY_TAB; break;
    case Qt::Key_Escape: vkey = VTERM_KEY_ESCAPE; break;
    case Qt::Key_Up: vkey = VTERM_KEY_UP; break;
    case Qt::Key_Down: vkey = VTERM_KEY_DOWN; break;
    case Qt::Key_Left: vkey = VTERM_KEY_LEFT; break;
    case Qt::Key_Right: vkey = VTERM_KEY_RIGHT; break;
    case Qt::Key_Insert: vkey = VTERM_KEY_INS; break;
    case Qt::Key_Delete: vkey = VTERM_KEY_DEL; break;
    case Qt::Key_Home: vkey = VTERM_KEY_HOME; break;
    case Qt::Key_End: vkey = VTERM_KEY_END; break;
    case Qt::Key_PageUp: vkey = VTERM_KEY_PAGEUP; break;
    case Qt::Key_PageDown: vkey = VTERM_KEY_PAGEDOWN; break;
    case Qt::Key_Backspace:
        if (ctrl) {
            clearSelection();
            scrollToBottom();
            m_session->userRaw(QByteArray("\x08", 1));
            return;
        }
        vkey = VTERM_KEY_BACKSPACE;
        break;
    default:
        if (key >= Qt::Key_F1 && key <= Qt::Key_F12)
            vkey = VTERM_KEY_FUNCTION(key - Qt::Key_F1 + 1);
        break;
    }
    if (vkey != VTERM_KEY_NONE) {
        clearSelection();
        scrollToBottom();
        int m = mods;
        if (key == Qt::Key_Backtab)
            m |= VTERM_MOD_SHIFT;
        m_session->userKey(vkey, m);
        return;
    }

    // Ctrl combinations: compute the control byte ourselves so devices that
    // do not understand CSI-u (switches, routers) get classic ASCII codes.
    if (ctrl && !alt) {
        char code = -1;
        if (key >= Qt::Key_A && key <= Qt::Key_Z)
            code = char(key - Qt::Key_A + 1);
        else if (key == Qt::Key_Space || key == Qt::Key_2 || key == Qt::Key_At)
            code = 0x00;
        else if (key == Qt::Key_BracketLeft || key == Qt::Key_3)
            code = 0x1b;
        else if (key == Qt::Key_Backslash || key == Qt::Key_4)
            code = 0x1c;
        else if (key == Qt::Key_BracketRight || key == Qt::Key_5)
            code = 0x1d;
        else if (key == Qt::Key_6 || key == Qt::Key_AsciiCircum)
            code = 0x1e;   // Cisco escape: Ctrl+Shift+6
        else if (key == Qt::Key_Minus || key == Qt::Key_Underscore || key == Qt::Key_7 || key == Qt::Key_Slash)
            code = 0x1f;
        else if (key == Qt::Key_8 || key == Qt::Key_Question)
            code = 0x7f;
        if (code >= 0) {
            clearSelection();
            scrollToBottom();
            m_session->userRaw(QByteArray(1, code));
            return;
        }
    }

    QString text = e->text();
    if (!text.isEmpty()) {
        // Drop C0 controls produced by unhandled combos.
        QString printable;
        for (QChar ch : text)
            if (ch.unicode() >= 0x20 && ch.unicode() != 0x7f)
                printable += ch;
        if (!printable.isEmpty()) {
            clearSelection();
            scrollToBottom();
            // AltGr arrives as Ctrl+Alt on Windows: it is plain text.
            const int m = (alt && !ctrl) ? VTERM_MOD_ALT : 0;
            m_session->userText(printable, m);
            return;
        }
    }
    QWidget::keyPressEvent(e);
}

void TerminalWidget::inputMethodEvent(QInputMethodEvent *e)
{
    if (!e->commitString().isEmpty() && m_session) {
        clearSelection();
        scrollToBottom();
        m_session->userText(e->commitString(), 0);
    }
    e->accept();
}

QVariant TerminalWidget::inputMethodQuery(Qt::InputMethodQuery q) const
{
    switch (q) {
    case Qt::ImCursorRectangle:
    case Qt::ImAnchorRectangle:
        return QVariant(cursorRect());
    case Qt::ImFont:
        return QVariant(m_font);
    case Qt::ImEnabled:
        return true;
    default:
        return QWidget::inputMethodQuery(q);
    }
}

// ---------------------------------------------------------------- suggestions

void TerminalWidget::hideSuggestions()
{
    if (m_suggest && m_suggest->isVisible())
        m_suggest->hide();
}

void TerminalWidget::updateSuggestions()
{
    if (!m_session || !m_history || !m_session->config().historyEnabled || m_scrollOffset != 0 || !hasFocus()
        || m_session->state() != TransportState::Connected || m_session->screen().altScreen()) {
        hideSuggestions();
        return;
    }
    const QString input = m_session->currentInput();
    if (input.trimmed().size() < 2) {
        hideSuggestions();
        return;
    }
    const auto items = m_history->suggest(input, m_session->config().historyHost(), 8);
    if (items.isEmpty()) {
        hideSuggestions();
        return;
    }
    m_suggestInput = input;
    m_suggest->clear();
    for (const HistoryEntry &h : items) {
        auto *it = new QListWidgetItem(h.text, m_suggest);
        it->setToolTip(tr("%1 · used %n time(s)", nullptr, h.useCount).arg(h.host));
    }
    m_suggest->setCurrentRow(0);

    const QFontMetrics fm(m_font);
    int w = 120;
    for (const HistoryEntry &h : items)
        w = std::max(w, fm.horizontalAdvance(h.text) + 24);
    w = std::min(w, width() - 8);
    const int rowH = fm.height() + 4;
    const int h = rowH * int(items.size()) + 4;
    const CursorInfo c = m_session->screen().cursor();
    const int x = std::clamp(int((c.col - input.size()) * m_cw), 0, std::max(0, width() - w));
    int y = int((c.row + 1) * m_ch) + 2;
    if (y + h > height())
        y = std::max(0, int(c.row * m_ch) - h - 2);
    m_suggest->setGeometry(x, y, w, h);
    m_suggest->show();
    m_suggest->raise();
}

void TerminalWidget::acceptSuggestion()
{
    QListWidgetItem *it = m_suggest->currentItem();
    if (!it || !m_session) {
        hideSuggestions();
        return;
    }
    const QString full = it->text();
    const QString input = m_session->currentInput();
    hideSuggestions();
    if (full.startsWith(input) && full.size() > input.size())
        m_session->acceptSuggestion(full.mid(input.size()));
    setFocus();
}

// ---------------------------------------------------------------- mouse

bool TerminalWidget::forwardMouse(QMouseEvent *e, bool pressed, bool release)
{
    const VtScreen &s = m_session->screen();
    if (s.mouseMode() == 0 || (e->modifiers() & Qt::ShiftModifier) || m_scrollOffset != 0
        || m_session->state() != TransportState::Connected)
        return false;
    const int row = std::clamp(int(e->position().y() / m_ch), 0, s.rows() - 1);
    const int col = std::clamp(int(e->position().x() / m_cw), 0, s.cols() - 1);
    const int mods = vtermMods(e->modifiers());
    m_session->screen().mouseMove(row, col, mods);
    int button = 0;
    switch (e->button()) {
    case Qt::LeftButton: button = 1; break;
    case Qt::MiddleButton: button = 2; break;
    case Qt::RightButton: button = 3; break;
    default: break;
    }
    if (button && (pressed || release))
        m_session->screen().mouseButton(button, pressed, mods);
    return true;
}

void TerminalWidget::mousePressEvent(QMouseEvent *e)
{
    setFocus();
    if (forwardMouse(e, true, false))
        return;
    if (e->button() == Qt::MiddleButton) {
        pasteClipboard();
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    hideSuggestions();

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool sameSpot = (e->position() - m_pressPos).manhattanLength() < 6;
    m_clickCount = (sameSpot && now - m_lastClickMs < QApplication::doubleClickInterval()) ? m_clickCount + 1 : 1;
    if (m_clickCount > 3)
        m_clickCount = 1;
    m_lastClickMs = now;
    m_pressPos = e->position();

    const Pos p = posAt(e->position(), true);
    if ((e->modifiers() & Qt::ShiftModifier) && m_selActive) {
        m_selecting = true;
        extendSelection(p);
        return;
    }
    if (m_clickCount == 1) {
        m_selMode = SelMode::Char;
        m_selA = m_selB = p;
        m_selActive = false;
        update();
    } else if (m_clickCount == 2) {
        m_selMode = SelMode::Word;
        Pos s, en;
        wordBounds(posAt(e->position(), false), s, en);
        m_wordA = m_selA = s;
        m_wordB = m_selB = en;
        m_selActive = true;
        update();
    } else {
        m_selMode = SelMode::Line;
        const Pos lp = posAt(e->position(), false);
        m_wordA = m_selA = Pos{lp.line, 0};
        m_wordB = m_selB = Pos{lp.line, m_session->screen().cols()};
        m_selActive = true;
        update();
    }
    m_selecting = true;
}

void TerminalWidget::mouseDoubleClickEvent(QMouseEvent *e)
{
    // Handled by click counting in mousePressEvent.
    mousePressEvent(e);
}

void TerminalWidget::mouseMoveEvent(QMouseEvent *e)
{
    if (m_session->screen().mouseMode() >= 2 && !(e->modifiers() & Qt::ShiftModifier) && !m_selecting) {
        if (m_session->screen().mouseMode() == 3 || e->buttons() != Qt::NoButton)
            forwardMouse(e, false, false);
        return;
    }
    if (!m_selecting)
        return;
    if (e->position().y() < 0)
        m_autoScrollDir = -1;
    else if (e->position().y() > height())
        m_autoScrollDir = 1;
    else
        m_autoScrollDir = 0;
    if (m_autoScrollDir != 0 && !m_autoScroll.isActive())
        m_autoScroll.start();
    else if (m_autoScrollDir == 0)
        m_autoScroll.stop();
    extendSelection(posAt(e->position(), true));
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent *e)
{
    if (!m_selecting) {
        forwardMouse(e, false, true);
        return;
    }
    m_selecting = false;
    m_autoScroll.stop();
    if (hasSelection())
        copySelection();   // copy on select (PuTTY / TeraTerm behaviour)
    else
        m_selActive = false;
    update();
}

void TerminalWidget::wheelEvent(QWheelEvent *e)
{
    const int steps = e->angleDelta().y() / 40;   // ~3 lines per notch
    if (steps == 0)
        return;
    const VtScreen &s = m_session->screen();
    if (s.altScreen() && m_session->state() == TransportState::Connected && !(e->modifiers() & Qt::ShiftModifier)) {
        if (s.mouseMode() != 0) {
            const QPointF pt = e->position();
            m_session->screen().mouseMove(std::clamp(int(pt.y() / m_ch), 0, s.rows() - 1),
                                          std::clamp(int(pt.x() / m_cw), 0, s.cols() - 1), 0);
            for (int i = 0; i < std::abs(steps); ++i)
                m_session->screen().mouseButton(steps > 0 ? 4 : 5, true, 0);
        } else {
            for (int i = 0; i < std::abs(steps); ++i)
                m_session->userKey(steps > 0 ? VTERM_KEY_UP : VTERM_KEY_DOWN, 0);
        }
        return;
    }
    setScrollOffset(m_scrollOffset + steps);
}

void TerminalWidget::focusInEvent(QFocusEvent *e)
{
    QWidget::focusInEvent(e);
    m_hasFocus = true;
    m_blinkOn = true;
    m_blink.start();
    if (m_session)
        m_session->focusChanged(true);
    update();
    Q_EMIT focused();
}

void TerminalWidget::focusOutEvent(QFocusEvent *e)
{
    QWidget::focusOutEvent(e);
    m_hasFocus = false;
    m_blink.stop();
    hideSuggestions();
    if (m_session)
        m_session->focusChanged(false);
    update();
}

void TerminalWidget::contextMenuEvent(QContextMenuEvent *e)
{
    if (m_session->screen().mouseMode() != 0 && !(e->modifiers() & Qt::ShiftModifier) && m_scrollOffset == 0)
        return;
    QMenu menu(this);
    QAction *copy = menu.addAction(tr("Copy"), this, &TerminalWidget::copySelection);
    copy->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+C")));
    copy->setEnabled(hasSelection());
    QAction *paste = menu.addAction(tr("Paste"), this, &TerminalWidget::pasteClipboard);
    paste->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+V")));
    paste->setEnabled(m_session->state() == TransportState::Connected && !QApplication::clipboard()->text().isEmpty());
    menu.addAction(tr("Select All"), this, &TerminalWidget::selectAll)->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+A")));
    menu.addSeparator();
    menu.addAction(tr("Find..."), this, [this] { Q_EMIT searchRequested(); })->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")));
    menu.addAction(tr("Clear Scrollback"), this, [this] {
        m_session->screen().clearScrollback();
        setScrollOffset(0);
        update();
    });
    if (m_session->supportsBreak()) {
        menu.addSeparator();
        menu.addAction(tr("Send Break"), m_session, &TerminalSession::sendBreak);
    }
    menu.addSeparator();
    if (m_session->state() == TransportState::Connected)
        menu.addAction(tr("Disconnect"), m_session, &TerminalSession::disconnectByUser);
    else
        menu.addAction(tr("Reconnect"), m_session, &TerminalSession::restart);
    menu.exec(e->globalPos());
}

int TerminalWidget::searchMatchCountVisible() const
{
    return m_searchHit.line >= 0 ? 1 : 0;
}

} // namespace termx
