# `src/colorprofiles/` — ICC colour profiles

LittleCMS2 work: building, loading and describing ICC profiles. Not pixel processing — a
profile is a description of a colour space, and applying one to an image is somebody else's
job.

Currently one file:

| file          | what it does                                                |
|---------------|-------------------------------------------------------------|
| `printprof.c` | resolves the printer/soft-proof profile for an output device |

It is deliberately a module rather than a home for one file. Two more belong here and are not
yet moved:

- `common/colorspaces.c` — 218 LCMS2 calls, the largest of the three
- `pixel/iop_profile.c` — 7, the pipeline-facing half of profile handling

Merging them is the point: all three wrap the same library, and the split between them today
follows where the code happened to be written rather than what it does. That merge is a change
of its own — `colorspaces.c` in particular carries state and a good deal of history — so it is
not attempted alongside the module's creation.

`printprof.c` is stateless (measured: `tools/statelessness_audit.py --dir src/colorprofiles`).
The other two are not, so this directory will not be stateless once they arrive; that is
expected and is why the guarantee lives in `src/system` and `src/math`, not here.
