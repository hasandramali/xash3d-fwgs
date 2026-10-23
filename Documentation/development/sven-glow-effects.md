# World glow occlusion and additional Sven effects

## Glow sprites

The GL sprite renderer explicitly disabled `GL_DEPTH_TEST` for `kRenderGlow`.
The software glow rasterizer similarly bypassed its per-pixel depth comparison.
The optional studio visibility ray was only a center test, so enabling it could
hide a whole large glow instead of covering its silhouette.

Both renderers now depth-test glow fragments without writing sprite depth.
Opaque players/NPCs and world geometry cover only overlapping pixels. The
rest of a large sprite remains visible, and an actor behind the sprite does
not cover it. Existing opaque-before-glow render ordering is retained. HUD
sprite drawing is not changed.

`r_traceglow` is retained as an optional legacy center trace against world
geometry only. Its default remains zero; default rendering no longer performs
the ray test. Studio models are excluded from that optional ray regardless of
the setting, since their silhouette is handled by the depth buffer.

The supplied reference `hw.dll` was inspected at sprite routine `0x01D532C0`.
Its legacy glow branch at `0x01D534A1` also disables depth testing. Therefore
this correction implements the requested fragment-level occlusion; it is not
presented as a literal copy of that legacy reference branch or proof of every
path used by the current official client. The software rasterizer regression
exercises real glow spans with partial/full/front/behind occluders and verifies
that drawing a glow does not overwrite the depth buffer. A GL-state harness
checks the actual render-mode switch and the world-only trace behavior.

## Additional message handlers

Reference addresses below are from the supplied `client.dll`, image base
`0x10000000`. All handlers validate their payload before creating effects.

### RampSprite

`0x1002E1A0` reads a model short, lifetime byte in tenths, three 32-bit fixed-point
coordinates, then a 16-bit optional-field mask. Its fields, in order, are:

| Flag | Field |
| --- | --- |
| `0x0001` | Roll byte, scaled by 360/255 |
| `0x0002` | Scale byte, scaled by 0.1 |
| `0x0004` | Opacity byte, scaled by 1/255 |
| `0x0008` | Packed render mode in upper three bits, render effect in lower five |
| `0x0010..0x0040` | Initial RGB bytes |
| `0x0080` | Initial animation frame byte |
| `0x0100` | Three fixed-point velocity coordinates |
| `0x0200` | Roll speed byte, scaled by 6 |
| `0x0400..0x4000` | Signed scale, opacity and RGB rates, scaled by 20/255 |
| `0x8000` | Animation-rate byte |

Defaults are scale/opacity one, additive rendering, white, no movement or
animation. The reference generic effect is initialized at `0x10012C60` and
updated at `0x10012DF0`. The portable version stores its callback parameters
inside the allocated temp entity, not a separate unbounded effect collection.
Scale/opacity/color/roll/frame are evaluated from elapsed time to avoid losing
fractional ramps at high FPS. Colors clamp rather than wrapping at byte overflow.
Missing/non-sprite model indices and exhausted pools are handled without a
disconnect. Truncated or trailing optional fields are rejected before allocation.

### ShieldRic

`0x1002E5A0` consumes three coordinates and calls `0x10015520`. This creates
`sprites/muz4.spr`: lifetime and initial scale 0.3, full initial opacity,
glow/no-dissipation mode, RGB `(100,100,255)`, scale rate 4 and opacity rate
`-10/3`. It uses the same bounded ramp callback; it is not a generic guessed
ricochet explosion.

### GargSplash

`0x1002E670` reads origin and direction, takes the absolute direction components
and calls `R_StreakSplash(origin,direction,6,20,50,0,400)`.
The supplied server registers only 12 bytes despite the client attempting
24 bytes of reads. Its checked coordinate reader at `0x100458A0` returns
`-1/8` on exhaustion; the absolute-value operation therefore supplies direction
`(0.125,0.125,0.125)` for the legacy 12-byte payload. The port reproduces that
fallback explicitly without an out-of-bounds read and also supports a complete
24-byte payload. No engine message-registration sizes were guessed or changed.

### Electric crowbar

`CbElec` at `0x1002EB90` reads one byte: five-bit player index and enable bit
`0x40`. Its managed effect at `0x10012130` adds world RGB `(80,100,220)` glow,
a radius-128 light, and `cbe_on`, `cbe_idle1`, `cbe_off` sounds under
`weapons/cbe/`. The first idle sound is scheduled one second after activation,
then every two seconds. The local viewmodel uses RGB `(50,75,220)`, zero shell
strength and `EF_DIMLIGHT` while enabled.

The portable implementation uses fixed player slots and a dedicated light key
namespace, rather than allocating effects on duplicate enable messages. Off/map
reset stops the hum and releases owned glow/light state. A retained light pointer
is cleared only if its key still matches; a slot reused for another effect is
not destroyed. No weapon-name checks or local weapon prediction are involved.

### ToxicCloud

`0x1002E730` consumes three coordinates, plays `doors/aliendoor1.wav` and
`ambience/disgusting.wav`, and starts a five-second type-11 emitter with a
50-puff limit and a 0.1-second emission interval.

The type-11 initialization at `0x10046E90` and update at `0x100480A0` were also
executed under a local x86 Unicorn harness with intercepted engine time/random
APIs and particle allocation. The captured native particle records confirm:

- `sprites/puff1.spr`, three initial puffs, origin offset +12 Z.
- Purple RGB `(c,0,c)` with `c` sampled from 150–255.
- Initial XY velocity within ±64 and Z velocity 64.
- World half-size initially one, growing at 270 units/second (256 for the
  second/third initial puffs), capped at 256.
- Five-second particle expiry; raw opacity 3500, fading by 1350/second until
  100, then by 85/second. Displayed opacity is capped at 255.
- Alternating 64-degree/second roll and short-lived vertical acceleration.

The portable emitter uses existing temp entities and their callback fields.
There are at most 16 emitting clouds, 50 allocation attempts per cloud, and no
catch-up bursts after a slow frame. The engine's existing temp-entity pool is
the final global bound. Empty/missing sprite bounds skip the visual rather than
substituting an unrelated model. Map initialization clears emitter state.
Motion jitter uses the portable frame timestep; the private particle manager's
per-puff world-light sampling and exact update ordering are not duplicated.

## Validation and scope

Native client builds and GL/software renderer syntax checks pass. ASan/UBSan
harnesses cover all optional RampSprite fields, 4,096 flag combinations, every
truncated prefix of a full message, model validation, exhausted pools, complete
animation frame wrapping, shield growth/fade, both GargSplash lengths, crowbar
sound/glow/light cleanup and toxic-cloud emission/fade limits. Previous Shock
Rifle/PrintKB regressions still pass. The changed software glow rasterizer also
cross-compiles to a freestanding Android ARMv7 object.

No actual Android/iOS GPU session was available. Transparent actors that do not
write depth are not treated as opaque silhouettes. These are working portable
handlers for the five previously reported effects, not a claim of complete
binary/rendering parity with every official-client effect or protocol feature.
