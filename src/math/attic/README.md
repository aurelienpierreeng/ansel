# Archived, not built

Nothing here is compiled: these files appear in no CMake target and no file includes
them. They are kept for reference only.

They live in `attic/` rather than in `math/` proper because they would break the one
invariant that makes `math/` worth having — **no dependency on any higher layer**.
`thinplate.{c,h}` includes `chart/tonecurve.h` and `chart/deltaE.h` (layer 7), which is
exactly the kind of edge `math/` exists to exclude.

So the invariant check is written to skip this directory:

    grep -hoE '#include "(control|gui|develop|iop|libs|views|chart)/' src/math/*.h

Note `src/math/*.h`, not `src/math/**` — deliberate. If any of this is ever revived,
the chart/ dependency has to be inverted first (pass the tone curve and Delta-E as
plain data or callbacks), and only then can the file move up into `math/`.
