# Map-transition HUD and local flashlight

## AntiRush countdown: corrected message path

The previous HudText-only regression did not exercise the message path used by
the reported multiplayer waiting countdown. The public AntiRush plugin calls
`PrintKeyBindingStringAll` every second with `Level changing in N seconds.`
and sends an empty string to clear it:
https://github.com/wootguy/AntiRush/blob/master/AntiRush.as

The supplied Windows server confirms the complete path: the AngelScript
registration at `0x10312AF4` binds the all-player wrapper `0x103118A0`, which
calls `0x10265B50`. That function sends one NUL-terminated PrintKB string to
each player, using message global `0x104FA3D0` registered at `0x1021CAC5`.

The supplied client PrintKB handler (`0x1005B890`) converts bindings, calls
engine pointer `0x101F8A14` at `0x1005B8C0`, and frees the conversion. This
pointer is CenterPrint, not ConsolePrint: it is six 32-bit callback slots after
TextMessageGet (`0x101F89FC`), and is also called by the TextMsg center branch
at `0x1005B738`. Our PrintKB handler incorrectly called ConsolePrint, explaining
why changing HudText refresh did not restore this countdown on screen.

PrintKB now uses CenterPrint. The engine also clears pending center text on
an empty message, so countdown cancellation/end does not leave stale text
until scr_centertime expires. No countdown text, 30-second duration, plugin
name, map name, or trigger detection is hardcoded in the implementation.

## Original-client comparison

The supplied `steam-refs/sven/client.dll` hooks `HudText` at `0x10043546`
through `0x10044750`, which calls the message handler at `0x10044510`.
That handler resolves a title or constructs the shared custom message, then
searches all 16 slots for its pointer at `0x100445CF`. An existing entry is
removed at `0x100445ED` before insertion, allowing its display time to restart.
This applies to raw text as well as named title channels.

Our previous early refresh covered only resolved title channels. Raw text
overwrote the shared custom buffer and then compared equal to the existing
entry, returning without refreshing its time. Repeated countdown updates could
therefore age out while the server was still sending them. Both message types
now refresh an existing entry, including when all slots are occupied. Unknown
text is also bounded to the custom buffer size.

`TextMsg` is separately hooked at `0x1005B296`; its center-print branch calls
the engine callback at `0x1005B738`. The original `TimeEnd` handler at
`0x10031AD0` updates the scoreboard's end time through `0x1006DC20`; it is not
evidence of a fixed 30-second intermission HUD timer. No client-side map-change
timer or guessed countdown text is synthesized.

Engine center prints now start on the presentation clock (`cl.time`) rather
than the last received server timestamp. HUD and center text render after
screen fading during intermission, and center text follows the client HUD.

## `r_dynamic 0` flashlight

Only the local player's flashlight submits a bake request. The request carries
a world-space beam endpoint, not a collision hit. This is renderer API 23;
engine and renderer libraries must be rebuilt together.

The GL renderer intersects the beam with actual world polygons and visible
solid brush models, including their entity transforms. It chooses the nearest
intersection before painting a 256-unit neighborhood. This avoids a nearby
NPC/player collision box determining which wall gets lit and removes reliance
on the movement physics entity list for distant brush geometry. Beam length is
131072 units, independent of the ordinary dynamic flashlight's 2000-unit range.
World traversal uses only the world submodel's faces; inline brush faces are
tested in their own coordinates.

Painted surfaces use a per-map byte mask. Their lightmaps are written directly
to fullbright, even if the original lightstyle is zero or samples are absent.
Original BSP samples and lightstyles are not modified. Repeated requests skip
already-painted surfaces, and geometry scans remain limited to four per second.
The mask is cleared on new maps and renderer shutdown. Surfaces remain lit
with `r_dynamic 0` after the flashlight is switched off, preserving the existing
session-painting behavior; switching dynamic lighting back on restores affected
lightmaps from their unchanged BSP samples and releases the mask.

The software renderer retains its existing paint implementation and resolves
the beam through its trace callback. The null renderer remains a no-op.

## Validation

- A regression using the actual PrintKB handler fails before the callback fix
  because center output stays empty. It passes after the fix, including
  30-to-1 countdown updates, binding substitution, literal percent characters,
  malformed-message rejection and the empty-string clear message.
- The actual engine center-print function is checked for presentation-clock
  refresh and empty/null cancellation with valid, invalid and absent fonts.
- The old `MessageAdd` reproduces the raw-text refresh failure; the replacement
  sustains 31 countdown updates, named channels, full queues and bounded text.
- Extracted engine draw cases verify that intermission fading precedes HUD and
  center text.
- Actual GL ray, bake and lightmap functions pass ASan/UBSan tests for distant
  walls/ceilings, large coordinates, plane-back faces, nearest brush occlusion,
  rotated/translated brush models, misses, repeated requests, throttling,
  clock/map resets, zero lightstyles and unchanged BSP samples.
- The local-only request branch is tested independently of dynamic lighting.
- The HLSDK client builds, and changed engine/GL/software sources pass syntax
  checks. Real-device/map rendering still needs confirmation; no fresh
  `engine.log` was available with this report.
