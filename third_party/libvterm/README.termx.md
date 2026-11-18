Vendored copy of libvterm 0.3.3 (MIT, Paul Evans).
The encoding tables (`src/encoding/*.inc`) and `src/fullwidth.inc` are the
pre-generated ones shipped in the release tarball, so no Perl is needed.

Local changes (marked `TermX:` in the source), found by fuzzing:
- `src/parser.c`: a CSI sequence with more than 16 `;`-separated arguments
  wrote past the end of `args[]` (heap overflow). Extra arguments are now dropped.
- `src/parser.c`: very long numeric CSI arguments and OSC command numbers
  overflowed a signed integer; they are now clamped.
- `src/screen.c`: reflow on resize walked to row -1 when the first row carried
  a continuation flag, reading outside the screen buffer. The walk now stops at row 0.

- `src/parser.c`: an OSC/DCS string with ESC followed by a C0 control at the end
  of a read made the fragment length wrap to SIZE_MAX (huge over-read).
- `src/screen.c`: resize called `abort()` when it could not place the cursor;
  the cursor is now clamped into the new screen instead.

- `src/state.c`: REP (`CSI n b`) with no previously printed character looped
  forever (advance width 0). It is now ignored in that case.
- `src/state.c`: UTF-8 encoded C1 controls (U+0080..U+009F) have width -1 and
  moved the cursor left of column 0, which then corrupted tab stops, erase and
  scroll. They now take one cell.
- `src/state.c`: on resize only the bottom/right scroll margins were clamped,
  leaving inverted regions; DECSLRM accepted a zero-width region when the right
  margin was omitted. Both are reset to the full screen now.
- `src/state.c`: DECRQSS used a signed left shift on bytes >= 0x80 (UB).

Inputs that triggered these are kept in `fuzz/regressions/` and replayed by ctest.
