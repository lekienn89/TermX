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

Inputs that triggered these are kept in `fuzz/regressions/` and replayed by ctest.
