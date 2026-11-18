// SPDX-License-Identifier: GPL-3.0-or-later
#include "Theme.h"

#include <QApplication>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QPalette>
#include <QStyleFactory>
#include <QStyleHints>
#include <QWidget>

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#endif

namespace termx {

namespace {
Theme::Mode g_mode = Theme::Mode::System;
bool g_dark = true;
qreal g_fontSize = 11.0;
TerminalColors g_term;

TerminalColors darkColors()
{
    TerminalColors c;
    c.foreground = QColor(0xD4D7DD);
    c.background = QColor(0x16181D);
    c.cursor = QColor(0x6CB6FF);
    c.selection = QColor(0x2F4A6D);
    c.selectionText = QColor(0xFFFFFF);
    c.searchMatch = QColor(0x5A4A12);
    c.searchCurrent = QColor(0xC78A00);
    c.ansi = {0x1E2127, 0xE06C75, 0x98C379, 0xD19A66, 0x61AFEF, 0xC678DD, 0x56B6C2, 0xABB2BF,
              0x5C6370, 0xFF7A85, 0xB5E890, 0xF0C37A, 0x7CC4FF, 0xDE95F0, 0x6FD3E0, 0xF5F6F8};
    return c;
}

TerminalColors lightColors()
{
    TerminalColors c;
    c.foreground = QColor(0x24292F);
    c.background = QColor(0xFBFBFC);
    c.cursor = QColor(0x0969DA);
    c.selection = QColor(0xC8DDF7);
    c.selectionText = QColor(0x0B1A2B);
    c.searchMatch = QColor(0xFFF0A8);
    c.searchCurrent = QColor(0xFFC53D);
    c.ansi = {0x24292F, 0xCF222E, 0x116329, 0x9A6700, 0x0550AE, 0x8250DF, 0x1B7C83, 0x6E7781,
              0x57606A, 0xE5534B, 0x1A7F37, 0xBF8700, 0x218BFF, 0xA475F9, 0x3192AA, 0x8C959F};
    return c;
}

bool systemDark()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    return QGuiApplication::styleHints()->colorScheme() != Qt::ColorScheme::Light;
#else
    return true;
#endif
}

QPalette makePalette(bool dark)
{
    QPalette p;
    if (dark) {
        const QColor window(0x1F2228), base(0x16181D), alt(0x23262D), text(0xD4D7DD), button(0x2A2E36),
            highlight(0x2F6FDB), mid(0x3A3F48), disabled(0x6B717C);
        p.setColor(QPalette::Window, window);
        p.setColor(QPalette::WindowText, text);
        p.setColor(QPalette::Base, base);
        p.setColor(QPalette::AlternateBase, alt);
        p.setColor(QPalette::ToolTipBase, alt);
        p.setColor(QPalette::ToolTipText, text);
        p.setColor(QPalette::PlaceholderText, disabled);
        p.setColor(QPalette::Text, text);
        p.setColor(QPalette::Button, button);
        p.setColor(QPalette::ButtonText, text);
        p.setColor(QPalette::BrightText, Qt::white);
        p.setColor(QPalette::Highlight, highlight);
        p.setColor(QPalette::HighlightedText, Qt::white);
        p.setColor(QPalette::Link, QColor(0x6CB6FF));
        p.setColor(QPalette::Mid, mid);
        p.setColor(QPalette::Dark, QColor(0x121418));
        p.setColor(QPalette::Light, QColor(0x343943));
        p.setColor(QPalette::Disabled, QPalette::Text, disabled);
        p.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
        p.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    } else {
        const QColor window(0xF3F4F6), base(0xFFFFFF), alt(0xF6F8FA), text(0x1F2328), button(0xFFFFFF),
            highlight(0x0969DA), disabled(0x8C959F);
        p.setColor(QPalette::Window, window);
        p.setColor(QPalette::WindowText, text);
        p.setColor(QPalette::Base, base);
        p.setColor(QPalette::AlternateBase, alt);
        p.setColor(QPalette::ToolTipBase, base);
        p.setColor(QPalette::ToolTipText, text);
        p.setColor(QPalette::PlaceholderText, disabled);
        p.setColor(QPalette::Text, text);
        p.setColor(QPalette::Button, button);
        p.setColor(QPalette::ButtonText, text);
        p.setColor(QPalette::Highlight, highlight);
        p.setColor(QPalette::HighlightedText, Qt::white);
        p.setColor(QPalette::Link, highlight);
        p.setColor(QPalette::Mid, QColor(0xD0D7DE));
        p.setColor(QPalette::Disabled, QPalette::Text, disabled);
        p.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
        p.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    }
    return p;
}

QString styleSheet(bool dark)
{
    const QString border = dark ? QStringLiteral("#2C3038") : QStringLiteral("#D0D7DE");
    const QString tabBg = dark ? QStringLiteral("#1F2228") : QStringLiteral("#E8EAED");
    const QString tabSel = dark ? QStringLiteral("#16181D") : QStringLiteral("#FFFFFF");
    const QString tabText = dark ? QStringLiteral("#9AA1AC") : QStringLiteral("#57606A");
    const QString tabTextSel = dark ? QStringLiteral("#E6E8EB") : QStringLiteral("#1F2328");
    const QString accent = dark ? QStringLiteral("#4C8DFF") : QStringLiteral("#0969DA");
    return QStringLiteral(
               "QMainWindow::separator { background: %1; width: 1px; height: 1px; }"
               "QDockWidget { titlebar-close-icon: none; }"
               "QDockWidget::title { padding: 6px 8px; background: %2; }"
               "QTabWidget::pane { border: none; }"
               "QTabBar::tab { background: %2; color: %4; padding: 6px 14px; border: none;"
               "  border-right: 1px solid %1; min-width: 80px; }"
               "QTabBar::tab:selected { background: %3; color: %5; border-top: 2px solid %6; }"
               "QTabBar::tab:hover:!selected { color: %5; }"
               "QToolBar { border: none; spacing: 4px; padding: 2px; }"
               "QStatusBar { border-top: 1px solid %1; }"
               "QLineEdit, QComboBox, QSpinBox { padding: 3px 6px; }"
               "QTreeView, QListView { border: none; }")
        .arg(border, tabBg, tabSel, tabText, tabTextSel, accent);
}
} // namespace

QString Theme::modeName(Mode m)
{
    switch (m) {
    case Mode::Dark: return QStringLiteral("dark");
    case Mode::Light: return QStringLiteral("light");
    case Mode::System: break;
    }
    return QStringLiteral("system");
}

Theme::Mode Theme::modeFromName(const QString &s)
{
    if (s == QLatin1String("dark")) return Mode::Dark;
    if (s == QLatin1String("light")) return Mode::Light;
    return Mode::System;
}

void Theme::apply(Mode mode)
{
    g_mode = mode;
    g_dark = mode == Mode::Dark || (mode == Mode::System && systemDark());
    if (QStyle *s = QStyleFactory::create(QStringLiteral("Fusion")))
        QApplication::setStyle(s);
    QApplication::setPalette(makePalette(g_dark));
    qApp->setStyleSheet(styleSheet(g_dark));
    g_term = g_dark ? darkColors() : lightColors();
    for (QWidget *w : QApplication::topLevelWidgets())
        applyTitleBar(w);
}

bool Theme::isDark() { return g_dark; }
Theme::Mode Theme::mode() { return g_mode; }
const TerminalColors &Theme::terminal() { return g_term; }
QColor Theme::accent() { return g_dark ? QColor(0x4C8DFF) : QColor(0x0969DA); }

QFont Theme::terminalFont()
{
    QFont f;
    f.setFamilies({QStringLiteral("Cascadia Mono"), QStringLiteral("Cascadia Code"), QStringLiteral("Consolas"),
                   QStringLiteral("DejaVu Sans Mono"), QStringLiteral("Liberation Mono"), QStringLiteral("Courier New")});
    f.setStyleHint(QFont::TypeWriter, QFont::PreferDefault);
    f.setFixedPitch(true);
    f.setPointSizeF(g_fontSize);
    f.setKerning(false);
    return f;
}

void Theme::setTerminalFontSize(qreal pt) { g_fontSize = qBound(6.0, pt, 48.0); }
qreal Theme::terminalFontSize() { return g_fontSize; }

void Theme::applyTitleBar(QWidget *w)
{
#ifdef _WIN32
    if (!w || !w->isWindow() || !w->testAttribute(Qt::WA_WState_Created))
        return;
    const BOOL dark = g_dark ? TRUE : FALSE;
    HWND hwnd = reinterpret_cast<HWND>(w->winId());
    // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE (Win10 20H1+ / Win11); 19 on older Win10 builds.
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark))))
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
#else
    Q_UNUSED(w);
#endif
}

} // namespace termx
