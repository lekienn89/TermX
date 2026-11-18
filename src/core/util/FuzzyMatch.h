// SPDX-License-Identifier: GPL-3.0-or-later
// Subsequence matching for the command palette: "spr" matches "Split Right".
#pragma once

#include <QString>

namespace termx {

// Returns -1 when `pattern` is not a (case-insensitive) subsequence of `text`,
// otherwise a score where higher is better: consecutive letters, word starts
// and an early first match score more.
int fuzzyScore(const QString &pattern, const QString &text);

} // namespace termx
