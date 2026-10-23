# Custom crosshairs, voice continuity and dynamic lighting

## Evidence from the supplied logs and assets

Both reports identify an older `f5fc0c7-dirty` Android build. `engine.log`
records failed server transfers for `sprites/cs16/640hud1.spr`, `640hud2.spr`
and `640hud16.spr`. Missing server files cannot be reconstructed from a sprite
record or silently replaced with an unrelated weapon's crosshair.

The supplied CS weapon records use `CustWeapon` sprite-list directories and
root-relative sprite names such as `cs16/crosshairs`. The reference client DLL
at `0x10003DA1` constructs `sprites/<directory>/<record>.txt`; at `0x10003DF0`
it constructs `sprites/<sprite-name>.spr` from the selected record. These are
two distinct namespaces, not paths to concatenate indiscriminately. The
client loader no longer prefixes an already-qualified missing sprite with the
weapon-list directory.

The supplied crosshair sprite is a valid 256x256 masked sprite. Its records
request a rectangle beginning at x=256; the engine already clamps that invalid
rectangle to the available frame, so changing individual CS weapon records
or adding weapon-name special cases is not necessary.

An actual engine-side retry issue was found: deferred HUD sprite models could
produce nonzero `SPR_Load` handles without render data, causing the weapon
loader's success tracking to stop retrying. HUD sprite loading now returns zero
until data is available, while retaining the queued resource. Cached models
without data are retried instead of being marked present after a failed load.

`crosshair 2` additionally draws a centered, outlined logical-pixel HUD dot after
the client HUD. It does not depend on a weapon or weapon sprite. Values zero
and one retain their previous behavior. Background maps, loading screens and
intermission do not show the dot.

## Voice

The voice report contains a map reconnect and several repeated record-start
commands, but no capture timestamps or outgoing voice packet trace. It does
not establish a single device-specific cause of the reported interruption.
The following verified failure paths are addressed:

- Repeating `+voicerecord` no longer closes a healthy running capture device.
- Repeating an identical voice initialization no longer destroys an active
  encoder/capture session. Signon still resets codecs/decoders for the new map.
  A real codec/quality change resumes an ongoing
  recording only while the game is active and focused.
- Voice encoding uses the space remaining in the actual outgoing packet,
  including movement data and already-queued unreliable messages. Previously,
  voice could consume PCM and increment frame sequences before the entire
  datagram was discarded for not fitting the final packet.
- SDL2 GoldSrc capture retains the newest eight 20 ms frames instead of
  retaining old audio and discarding fresh speech during render/network stalls.
  Discarded PCM is frame-aligned. iOS conversion drains through the same bounded
  queue without changing its native-format-to-24-kHz conversion.
- An active, focused SDL2 session receiving no PCM for two seconds reopens its
  input device. Capture timestamps are read under the audio-device lock, including
  on ARMv7. Backgrounding, receive-only mode and explicit stop still close capture;
  the watchdog cannot reopen it in those states.
- Opus Custom encoding also respects frame-header overhead and leaves raw PCM
  and gain state intact when the packet budget is too small.

## Dynamic lights and particle pressure

The dynamic-light cache no longer invalidates merely because a live light's
expiration time, key or decay metadata changed. Active-light membership and
all output-affecting parameters remain represented, including light indices.
Each surface stops scanning lights after its highest set bit. Per-column
distance is calculated once per light instead of once per luxel row.

For fading map lights, the lightmap builder now reuses a bounded RGB contribution
table for integer luxel distances when the surface is large enough to amortize
its construction. Small surfaces and very large light radii keep the direct
calculation. The table occupies 12 KiB of static renderer memory, not the ARMv7
stack; no NEON or ARM64-only instructions are required. Rows outside the light
are skipped. Zero-intensity secondary lightstyles skip their sample pass.

The dynamic-atlas signature now includes only lights and styles referenced by
its surface chain, plus the current brush-model transform. Unrelated fading
lights no longer invalidate it. A chain spanning multiple atlas uploads cannot
be reused: only the final atlas page survives in the GPU texture.

Lightstyle interpolation runs at its original cadence; the experimental
update-rate cvar has been removed. Animated baked-lightmap changes are queued
in CPU atlas shadows and uploaded as one dirty rectangle per atlas before its
draw, instead of binding and uploading once per changed surface. Both classic
and VBO draws flush their atlas before use. Byte-identical rows are not marked
dirty, including fades too small to change the final gamma-corrected texels.
Shadows are capped at 16 MiB; pages
outside that budget retain immediate uploads. Rebuilds, map changes and renderer
shutdown release the shadows. Flashlight painting updates the shadow as well.
Dynamic point-light atlases also omit unused right-hand columns when at least
a quarter of the atlas width is empty, in both the classic and VBO paths.

Single-style, non-water surfaces without dynamic point lights use a shared
256-entry sample-to-output table per style and frame, avoiding the intermediate
RGB accumulator and repeated scaling/gamma conversion. Multi-style surfaces,
water and point lights retain the general builder. These paths preserve light
values and timing rather than skipping fade frames. Device FPS still needs to
be measured on the affected map; CPU/upload tests are not a GPU benchmark.

The October 23 log completes a roughly 16-minute session without a capture,
codec or packet-overflow error and exits through the menu normally. It does
report unimplemented `SRPrimed`, `SRPrimedOff`, `SRDetonate` and `PrintKB` client
handlers, missing server content and two rejected Ogg files. These are separate
from lighting. The subsequent Shock Rifle/PrintKB port addresses those four
message handlers; see `sven-shockrifle-printkb.md`. Missing content and rejected
Ogg files are not fixed or hidden by either change.

Particle/tracer allocation attempts to reclaim expired objects before reporting
an exhausted pool. Reclaim walks occur at most once per frame's clock value.
Map resets clear stale callbacks as well as list links. Genuine exhaustion
remains bounded by the existing limit; only additional cosmetic particles are
skipped. Its diagnostic is a developer-level warning throttled to five seconds,
not a fatal engine/network error or an unbounded allocation request.

The unsupported-operation SDL message in the old log is already filtered by the
current mobile SDL logging code. Missing sound assets and unsupported WAV formats
remain separate content/decoder limitations; they are not evidence that outgoing
microphone packets stopped.

## Validation

ASan/UBSan harnesses exercise deferred/loaded sprite handles, the independent
HUD dot, all 26 supplied CS weapon lists with crosshair records, newest-audio
queue behavior, real Opus wire roundtrips, repeated record/init calls, packet
budgets, capture-stall recovery, focus privacy and expired particle reclamation.
Five hundred randomized light configurations produce bit-identical luxels before
and after optimization, including transformed brush models. Real SDL converters
retain the expected pitch/duration for native 16/24/44.1/48-kHz inputs.

The fading-light follow-up expands the exact-light comparison to 5,000 cases,
including fractional radii. Another 10,000 cases compare unlimited lightstyle
updates with the previous code. Rate-limited tests verify clock continuity,
pause behavior and immediate static-style changes; cache tests cover unrelated
lights/styles, light index 31, expiry and brush transforms. Both light calculation
and lightstyle routines cross-compile as freestanding Android ARMv7 objects.
This is not a full ARMv7 APK build or device test. An ARM64 CPU-only microbenchmark
showed about 28% less light-addition time for a 64×64-luxel, four-light example;
that number does not predict whole-frame FPS or GPU upload cost.

The client library builds and engine/renderer syntax checks pass. FPS gains and
long-session microphone stability still require testing on the affected devices;
no live device or server session was available for this audit.
