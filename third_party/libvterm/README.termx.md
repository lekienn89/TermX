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

Inputs that triggered these are kept in `fuzz/regressions/` and replayed by ctest.
