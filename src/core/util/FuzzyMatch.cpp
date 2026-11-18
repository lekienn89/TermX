// SPDX-License-Identifier: GPL-3.0-or-later
#include "FuzzyMatch.h"

namespace termx {

int fuzzyScore(const QString &patternIn, const QString &text)
{
    const QString pattern = patternIn.simplified().remove(QLatin1Char(' '));
    if (pattern.isEmpty())
        return 0;
    if (text.contains(patternIn.trimmed(), Qt::CaseInsensitive))
        return 1000 - int(text.indexOf(patternIn.trimmed(), 0, Qt::CaseInsensitive));   // plain substring wins
    int score = 0;
    int ti = 0;
    int prev = -2;
    for (QChar pc : pattern) {
        const QChar want = pc.toLower();
        bool found = false;
        for (; ti < text.size(); ++ti) {
            if (text[ti].toLower() != want)
                continue;
            const bool wordStart = ti == 0 || !text[ti - 1].isLetterOrNumber()
                                   || (text[ti].isUpper() && text[ti - 1].isLower());
            score += 1;
            if (ti == prev + 1)
                score += 5;
            if (wordStart)
                score += 8;
            if (ti < 3)
                score += 2;
            prev = ti;
            ++ti;
            found = true;
            break;
        }
        if (!found)
            return -1;
    }
    return score;
}

} // namespace termx
