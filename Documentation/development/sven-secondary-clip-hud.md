# M16 launcher and dual-Uzi ammo HUD

## Binary reference

The supplied `server.dll` generic weapon updater at `0x102AAC90` compares both
clip fields (`weapon+0x1F4` and `weapon+0x1F8`), and sends both through
`0x102026E0` as `CurWeapon: BYTE state, SHORT id, LONG clip1, LONG clip2`.
Ammo reserves are separate `AmmoX` updates, not the second CurWeapon LONG.

M16 initializes its second clip to zero at `0x101B249A`, loads one grenade
at `0x101B2D2C`, and clears it on firing at `0x101B2E33`. Thus a loaded
launcher and nine spare grenades must display `1 | 9`, including `1 | 0`
when the final grenade is loaded and the reserve has become empty.

Uzi's item info sets WeaponList flag `0x20` at `0x10273CCE`. Its mode switch
at `0x10274E70` keeps the entity classname `weapon_uzi`, and writes the
dual-mode byte at `weapon+0x210` at `0x10274F15`. The generic weapon updater
maps that byte to CurWeapon state bit `0x04` at `0x102AACD5..0x102AACE2`.
The second-Uzi pickup transfers its clip to the existing weapon's second
clip at `0x10273DE5` before enabling dual mode. A literal
`weapon_uziakimbo` HUD classname check therefore misses a normal conversion.

The supplied `client.dll` stores both clip LONGs and the state byte at
`WEAPON+0xA0/+0xA4/+0xA8` (`0x1000304D..0x1000305C`). HUD drawing tests
WeaponList flag `0x20` and state bit `0x04` at `0x100024BC..0x100024D0`.
It draws the off-hand magazine on an additional line using the primary ammo
icon; otherwise it draws secondary reserve and the second clip, if valid
(`0x10002573..0x100026DA`). No weapon-name or fixed-ID test is necessary.

## Port behavior

- CurWeapon retains its state byte and both clips, including inactive updates.
  Only exact eleven-byte packets are accepted; -1 remains the no-clip sentinel.
- The primary magazine/reserve display stays independent of the second clip.
- Dual-capable weapons show an extra magazine line only while dual mode is
  enabled, including an empty off-hand magazine. Reverting to single mode
  hides that line even if the server retains an old second-clip value.
- Other weapons with secondary ammo show loaded clip and reserve separately.
  A loaded grenade remains visible at zero reserve; an unavailable second
  clip shows only reserve. Ammo type zero is supported as a real index.
- A positive second clip counts as available ammunition for HUD selection.

This is presentation of server-provided data: no local reload prediction,
ammo fabrication, weapon-name exceptions or `cl_lw` changes are introduced.
