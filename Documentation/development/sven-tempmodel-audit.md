# Sven projectile and temporary-model audit

The local `steam-refs/sven/hw.dll` was checked for the fading-model/projectile
report. Addresses below are virtual addresses in that reference build.

| Message | Native handler | Model index |
| --- | --- | --- |
| `TE_MODEL` (106) | `0x1d34b12` | signed 16-bit short |
| `TE_BREAKMODEL` (108) | `0x1d34c46` | signed 16-bit short |
| `TE_SPRITE_SPRAY` (110) | `0x1d3518c`, index read at `0x1d351e3` | signed 16-bit short |
| `TE_PROJECTILE` (119) | `0x1d33367`, index read at `0x1d333be` | signed 16-bit short |
| `TE_SPRAY` (120) | `0x1d33401`, index read at `0x1d33458` | signed 16-bit short |

The native short reader is at `0x1d3c310`. The coordinate reader at
`0x1d3d6a0` selects short or long coordinates according to the native protocol
mode. This fork uses Sven's long-coordinate mode. The checked handlers have
the same field order as `CL_ParseTempEntity`.

The native model resolver at `0x1d27ec0` directly indexes the precached model
table. The spray implementations (`0x1d36ef0`, `0x1d370c0`) do not substitute
a canonical projectile model or reject every studio model. A helicopter is
therefore not enough evidence to justify replacing models or silently dropping
effects on this client.

Local temporary-effect model indices are not narrowed to 8 bits or masked at
255/512. `CL_ModelHandle` validates against `MAX_MODELS` (4096 in the normal
build). Invalid indices resolve to null rather than wrapping. The 2048-entry
Sven temporary-entity pool drops a low-priority allocation when exhausted;
high-priority allocation can reclaim a low-priority entry and reinitializes
its model pointer. Increasing that pool would not repair a wrong model index.

## Focused diagnostics

`cl_trace_tempmodels 1` enables model-name/index logging for temporary models,
break-model effects, sprays and projectiles. It is off by default. Reproduce
the visual problem briefly, then use `cl_trace_tempmodels 0`.

`TEMP-MODEL` lines contain the effect function, `wire` message type, resolved
model name/type, index, model-table size and origin. `wire=-1` means the effect
was called outside the engine's temporary-entity packet parser, for example
from a client user-message handler. It does not prove the effect originated
without server input. This trace does not cover ordinary network entities.

If a packet's index resolves to a helicopter, compare that index with the
server's precache table before assigning blame. A server plugin retaining an
old index across maps is one possible cause, not a demonstrated diagnosis.
The existing gameplay log does not contain the offending effect payload.

## Validation

A temporary UndefinedBehaviorSanitizer harness uses the actual effect and
allocation functions and selected actual parser case bodies with the real
network bit reader. It verifies indices 255, 256, 511, 512, 1023, 1024 and 4095,
invalid-index rejection, complete consumption of the tested message layouts,
2048-slot exhaustion/reclaim and opt-in tracing. It places a different model
in low-byte-alias slots to detect accidental index truncation. These tests
do not identify which NPC or live server packet caused the reported visual.
