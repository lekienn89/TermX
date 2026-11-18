// SPDX-License-Identifier: GPL-3.0-or-later
#include "CommandCapture.h"
#include "terminal/VtScreen.h"

namespace termx {

namespace {
constexpr int kMaxCommandLength = 4096;
}

QVector<QRegularExpression> CommandCapture::defaultPromptPatterns()
{
    static const QVector<QRegularExpression> p = [] {
        QVector<QRegularExpression> v;
        const char *list[] = {
            R"(^PS [^>]*> ?)",                       // PowerShell
            R"(^[A-Za-z]:\\[^>]*> ?)",               // cmd.exe
            R"(^\[[^\]]+\][$#] ?)",                  // [user@host dir]$ (RHEL)
            R"(^<[^<>\s]+> ?)",                      // <HUAWEI> / <H3C>
            R"(^\[[^\]\s]+\] ?)",                    // [HUAWEI-GigabitEthernet0/0/1]
            R"(^[^\s#$>%]*[\w)\]~][#$>%] ?)",        // user@host:~$  Router#  sw(config-if)#  user@sw>
            R"(^\S+@\S+[$#>%] ?)",
        };
        for (const char *s : list)
            v.push_back(QRegularExpression(QString::fromLatin1(s)));
        return v;
    }();
    return p;
}

CommandCapture::CommandCapture() : m_patterns(defaultPromptPatterns()) {}

void CommandCapture::setExtraPromptPatterns(const QStringList &patterns)
{
    m_patterns.clear();
    for (const QString &p : patterns) {
        QRegularExpression re(p.startsWith(QLatin1Char('^')) ? p : QLatin1Char('^') + p);
        if (re.isValid())
            m_patterns.push_back(re);
    }
    m_patterns += defaultPromptPatterns();
}

void CommandCapture::reset()
{
    m_anchorLine = -1;
    m_anchorCol = 0;
    m_typed.clear();
    m_typedReliable = true;
}

QString CommandCapture::stripPrompt(const QString &line, const QVector<QRegularExpression> &patterns, bool *matched)
{
    for (const QRegularExpression &re : patterns) {
        const QRegularExpressionMatch m = re.match(line);
        if (m.hasMatch() && m.capturedLength() > 0) {
            if (matched)
                *matched = true;
            return line.mid(m.capturedLength());
        }
    }
    if (matched)
        *matched = false;
    return line;
}

bool CommandCapture::looksLikeSecretPrompt(const QString &text)
{
    static const QRegularExpression re(
        QStringLiteral(R"(\b(password|passphrase|passcode|secret|pin|token|verification code|otp)\b[^:]*:\s*$)"),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(text).hasMatch();
}

bool CommandCapture::shouldRecord(const QString &cmd)
{
    if (cmd.isEmpty() || cmd.size() > kMaxCommandLength)
        return false;
    static const QRegularExpression secret(
        QStringLiteral(R"((password|passwd|pwd|token|secret|api[_-]?key)\s*[=:]\s*\S|--password[\s=]\S|-p\s*'[^']+'|sshpass\s+-p)"),
        QRegularExpression::CaseInsensitiveOption);
    if (secret.match(cmd).hasMatch())
        return false;
    for (QChar c : cmd) {
        if (c.unicode() < 0x20 && c != QLatin1Char('\t'))
            return false;
    }
    return true;
}

QString CommandCapture::readFromAnchor(const VtScreen &screen, bool *ok) const
{
    *ok = false;
    const qint64 cursorLine = screen.firstScreenLine() + screen.cursor().row;
    if (m_anchorLine < 0 || cursorLine < m_anchorLine || cursorLine - m_anchorLine > 50)
        return QString();
    for (qint64 l = m_anchorLine; l < cursorLine; ++l) {
        if (!screen.lineContinuesNext(l))
            return QString();   // output appeared in between: anchor is stale
    }
    *ok = true;
    QString text = screen.lineText(m_anchorLine, m_anchorCol);
    for (qint64 l = m_anchorLine + 1; l <= cursorLine; ++l)
        text += screen.lineText(l);
    return text;
}

QString CommandCapture::currentInput(const VtScreen &screen) const
{
    if (screen.altScreen())
        return QString();
    const CursorInfo cur = screen.cursor();
    const qint64 cursorLine = screen.firstScreenLine() + cur.row;
    if (m_anchorLine < 0 || cursorLine != m_anchorLine)
        return QString();
    if (cur.col < m_anchorCol)
        return QString();
    // Only suggest when the cursor is at the end of the input.
    if (!screen.lineText(cursorLine, cur.col).trimmed().isEmpty())
        return QString();
    return screen.lineText(cursorLine, m_anchorCol, cur.col);
}

CommandCapture::Result CommandCapture::onKey(const VtScreen &screen, bool isEnter, bool isPrintable,
                                             const QString &text, bool isEditKey)
{
    Result r;
    if (screen.altScreen()) {
        reset();
        return r;
    }
    const CursorInfo cur = screen.cursor();
    const qint64 cursorLine = screen.firstScreenLine() + cur.row;

    if (!isEnter) {
        if (text == QLatin1String("\x03") || text == QLatin1String("\x15")) {   // Ctrl+C, Ctrl+U
            reset();
            return r;
        }
        if (m_anchorLine < 0 && (isPrintable || isEditKey)) {
            const auto mark = screen.commandStart();
            if (mark.line == cursorLine && mark.col <= cur.col) {
                m_anchorLine = mark.line;
                m_anchorCol = mark.col;
            } else {
                m_anchorLine = cursorLine;
                m_anchorCol = cur.col;
            }
            m_typed.clear();
            m_typedReliable = true;
        }
        if (isPrintable) {
            if (m_typed.size() < kMaxCommandLength)
                m_typed += text;
        } else if (text == QLatin1String("\x7f") || text == QLatin1String("\b")) {
            if (!m_typed.isEmpty())
                m_typed.chop(1);
        } else if (isEditKey) {
            m_typedReliable = false;
        }
        return r;
    }

    // ---- Enter ----
    QString prompt;
    QString screenText;
    bool anchored = false;
    if (m_anchorLine >= 0) {
        screenText = readFromAnchor(screen, &anchored);
        if (anchored)
            prompt = screen.lineText(m_anchorLine, 0, m_anchorCol);
    }
    if (!anchored) {
        // No usable anchor (e.g. shell history recall with arrow keys only):
        // read the cursor line and strip the prompt with regexes.
        const QString line = screen.lineText(cursorLine);
        bool matched = false;
        screenText = stripPrompt(line, m_patterns, &matched);
        if (!matched)
            screenText.clear();
        prompt = line.left(line.size() - screenText.size());
    }

    const QString typed = m_typed;
    const bool typedReliable = m_typedReliable;
    reset();

    if (looksLikeSecretPrompt(prompt))
        return r;
    QString cmd = screenText.trimmed();
    if (cmd.isEmpty() && !typed.trimmed().isEmpty())
        return r;   // typed but nothing echoed: echo is off (password)
    if (typedReliable && !typed.isEmpty() && typed.startsWith(QLatin1Char(' ')))
        return r;   // leading space = do not record (bash HISTCONTROL convention)
    if (typedReliable && typed.trimmed().size() > cmd.size() && typed.trimmed().startsWith(cmd))
        cmd = typed.trimmed();   // echo had not caught up yet
    if (!shouldRecord(cmd))
        return r;
    r.valid = true;
    r.command = cmd;
    return r;
}

} // namespace termx
