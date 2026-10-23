# Shock Rifle and key-binding messages

## Exhausted inventory entry

The reference `server.dll` Shock Rifle item-info routine at `0x102434D0`
sets weapon ID 28 and flags `0x19` (`SELECTONEMPTY | LIMITINWORLD |
EXHAUSTIBLE`). Its ammo-consumption path at `0x10245628` clamps exhausted
primary ammo to zero and calls the detonation/removal routine at `0x10245FF0`.
That routine removes the item through the player's virtual method at offset
`0x94` (`0x102462BE`), implemented at `0x10200800`.

`SELECTONEMPTY` also describes satchel remotes, so neither zero reserve nor
inactivity alone is sufficient ownership-removal evidence. Without predicted
weapon data, WeaponListFix correlates a recent attack with a server-confirmed
active-ID change and fresh depletion, while respecting manual selections.
All known reserves and both observed clip values must be empty. A cached
zero from before a pickup is not depletion evidence. The fix uses advertised
flags and ammo indices, not a classname or hardcoded weapon ID. Unknown ammo,
remaining ammunition and authoritative predicted weapon-data ownership retain
their guards. `SRDetonate` remains a
visual effect only: its broadcast payload has no owner ID, so another player's
or NPC's explosion cannot remove a local inventory entry.

See `sven-weapon-ownership.md` for the subsequent pickup/strip regression fix.

## Verified reference behavior

Addresses below refer to the supplied `steam-refs/sven/client.dll`, image base
`0x10000000`. The message registrations identify actual handlers, not names
inferred from strings alone.

| Message | Registration | Handler | Payload |
| --- | --- | --- | --- |
| `SRDetonate` | `0x100308B2` | `0x1002DE60` | Three signed 32-bit coordinates at 1/8 unit, charge byte: 13 bytes |
| `SRPrimed` | `0x100308C2` | `0x1002EC60` | Player byte, IEEE-754 start time: 5 bytes |
| `SRPrimedOff` | `0x100308D5` | `0x1002ECA0` | Player byte: 1 byte |
| `PrintKB` | `0x1005B2A3` | `0x1005B890` | NUL-terminated text |

The Shock Rifle messages mask the player byte with `0x1f`, as the reference
does. No weapon classname or custom-weapon list is used.

`SRPrimed` creates the `shrifle_primed_effect` managed effect through
`0x10015250`. Initialization at `0x100123F0` stores the start time and a five-second
charge interval. Update at `0x100122D0` modifies the viewmodel, not the player's
world model: `kRenderFxGlowShell`, shell strength up to 20, RGB up to
`(50, 50, 220)`. `0x10012440` removes it and restores no render effect,
render amount 255 and zero render color.

The port updates the charge from `HUD_CreateEntities`, including with `cl_lw 0`.
State is bounded to 32 entries, duplicate messages replace rather than allocate
effects, HUD initialization clears it, and video initialization retains it.
Stop/reset does not overwrite an unrelated replacement viewmodel/effect.
Unlike the unchecked reference, malformed/nonfinite start times are rejected
and the charge fraction is clamped to zero before the start time.

For charge byte `c`, `SRDetonate` uses:

- Radius `32 + 2*c`, light RGB `(85, 200, 255)`, decay 500, lifetime one second.
- `weapons/shock_discharge2.wav`, volume 1, attenuation 0.8, pitch 100.
- `3 + c/20` lightning beams, using `sprites/lgtning.spr`, lifetime 0.25,
  width `radius/200 + 0.5`, random amplitude in `[1, 1 + c/10]`, RGB `(96, 180, 255)`.
- Random beam endpoints within the radius; vertical spread is halved.
- `sprites/blueflare1.spr`, scale `radius/40`, glow/no-dissipation rendering,
  opacity 0.75 and lifetime 0.15.
- A zero-direction streak splash, color 72, count `40*(3 + c/20)`, speed
  `radius*4`, velocity range -400 through 400.

The implementation routes these through the existing portable engine effect
and sound APIs instead of transplanting the reference's internal effect/audio
manager objects. Missing sprite models or exhausted effect pools do not cause
unbounded retries or a disconnect.

`PrintKB` calls the reference key converter at `0x10039C40`, prints the result
through the engine console API, then frees it. Bound actions become bracketed
key names; unbound actions retain their original text. It is not a HUD or chat
message. The port uses the existing shared `KB_ConvertString` implementation,
with output bounds, allocation failure handling and unsigned-character
classification added. This also protects menu text using the same converter.
Console text remains literal, including `%` characters, and is never a command.

## Additional checks and remaining scope

`READ_FLOAT` previously accessed four bytes without checking the remaining
payload. It now follows the other primitive readers' bounds checks and retains
byte-wise decoding, so unaligned ARMv7 message buffers are safe.

A subsequent pass implements the five additional names identified here:
`CbElec`, `ToxicCloud`, `GargSplash`, `RampSprite` and `ShieldRic`. See
`sven-glow-effects.md` for the reference evidence, bounded portable effects and
the legacy `GargSplash` payload discrepancy. They are not registered as no-ops.
This still does not establish complete reference-client parity. `ScreenShake`
and `ScreenFade` are explicitly handled by the engine and do not require
client hooks.

## Validation

The native client library builds successfully. A temporary ASan/UBSan harness
compiles the actual effect and text handler sources, primitive readers and
shared key-conversion implementation. It checks message sizes, NULL and
unaligned buffers, NaN start times, the five-second charge ramp, saturation,
stop/reset, video initialization, missing entities, replacement viewmodels,
all 256 discharge strengths, missing sprite models and exhausted light pools.
It also checks bound/unbound actions, literal console formatting, conversion
expansion beyond 4096 bytes and truncated float inputs.

No live server or physical ARMv7/iOS test was performed. The reference's private
effect-manager scheduling and audio mixer internals are not copied; visible
timing should still be checked in-game with the Shock Rifle secondary attack.
