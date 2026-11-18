// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QColor>
#include <QFont>
#include <QString>

#include <array>
#include <cstdint>

class QApplication;
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

class Theme {
public:
    enum class Mode { System, Dark, Light };

    static void apply(Mode mode);
    static bool isDark();
    static Mode mode();
    static const TerminalColors &terminal();
    static QColor accent();
    static QFont terminalFont();
    static void setTerminalFontSize(qreal pt);
    static qreal terminalFontSize();
    static void applyTitleBar(QWidget *topLevel);   // dark title bar on Windows 10/11

    static QString modeName(Mode m);
    static Mode modeFromName(const QString &s);
};

} // namespace termx
