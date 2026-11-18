// SPDX-License-Identifier: GPL-3.0-or-later
// Works out which command the user just ran, the way WindTerm does: by
// reading the command line from the screen rather than trusting keystrokes.
#pragma once

#include <QRegularExpression>
#include <QString>
#include <QVector>

namespace termx {

class VtScreen;

class CommandCapture {
public:
    CommandCapture();

    // Call for every key that reaches the host, BEFORE it is sent.
    // Returns the captured command when `isEnter` completes one.
    struct Result { bool valid = false; QString command; };
    Result onKey(const VtScreen &screen, bool isEnter, bool isPrintable, const QString &text, bool isEditKey);

    // Current input on the command line (prompt removed) for suggestions.
    QString currentInput(const VtScreen &screen) const;
    bool hasAnchor() const { return m_anchorLine >= 0; }

    void setExtraPromptPatterns(const QStringList &patterns);
    void reset();

    // --- helpers, public for tests ---
    static QString stripPrompt(const QString &line, const QVector<QRegularExpression> &patterns, bool *matched = nullptr);
    static bool looksLikeSecretPrompt(const QString &text);
    static bool shouldRecord(const QString &cmd);
    static QVector<QRegularExpression> defaultPromptPatterns();

private:
    QString readFromAnchor(const VtScreen &screen, bool *ok) const;

    qint64 m_anchorLine = -1;   // absolute line where the user started typing
    int m_anchorCol = 0;
    QString m_typed;            // keystroke fallback
    bool m_typedReliable = true;
    QVector<QRegularExpression> m_patterns;
};

} // namespace termx
