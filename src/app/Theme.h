// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QColor>
#include <QFont>
#include <QString>

#include <array>
#include <cstdint>

class QApplication;
class QSettings;
class QWidget;

namespace termx {

struct TerminalColors {
    QColor foreground;
    QColor background;
    QColor cursor;
    QColor selection;
    QColor selectionText;
    QColor searchMatch;
    QColor searchCurrent;
    std::array<uint32_t, 16> ansi{};
};

// User preferences that affect how every terminal looks and behaves.
struct TerminalPrefs {
    enum CursorShape { CursorDefault = 0, CursorBlock, CursorUnderline, CursorBar };
    QString fontFamily;            // empty = built-in monospace fallback list
    int cursorShape = CursorDefault;   // CursorDefault lets the remote program decide
    bool cursorBlink = true;
    bool copyOnSelect = true;
    bool rightClickPaste = false;  // PuTTY style; Shift+right-click still opens the menu
};

class Theme {
public:
    enum class Mode { System, Dark, Light };

    static void apply(Mode mode);
    static bool isDark();
    static Mode mode();
    static const TerminalColors &terminal();
    static QColor accent();
    static QFont terminalFont();
    static QFont terminalFont(const QString &family, qreal pt);
    static void setTerminalFontSize(qreal pt);
    static qreal terminalFontSize();
    static void applyTitleBar(QWidget *topLevel);   // dark title bar on Windows 10/11

    static TerminalPrefs &prefs();
    static void loadPrefs(const QSettings &s);
    static void savePrefs(QSettings &s);

    static QString modeName(Mode m);
    static Mode modeFromName(const QString &s);
};

} // namespace termx
