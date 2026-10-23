# Sven weapon ownership reconciliation

## Reference checks

The supplied Windows binaries confirm two distinct information paths:

- `server.dll` `GetEntityAPI2` at `0x100FC170` supplies the callback table at
  `0x104E0C70`. Entry 42 is `GetWeaponData` at `0x1012D200`. It clears 256
  records of 88 bytes and fills each owned weapon's record at its ID, writing
  the ID at `0x1012D2F5`. Missing records are ownership removals.
- `client.dll` inventory reconciliation at `0x10003BE0` tests that record's
  first integer at `0x10003C14` and adds or removes its slot entry. It is called
  at `0x10015BF1`. Ammo and `SELECTONEMPTY` control selection, not ownership.
- The supplied ReHLDS Sven server's `SV_WriteClientdataToMessage` only sends
  those weapon records when `host_client->lw` is enabled. WeaponListFix must
  therefore retain a separate fallback when using `cl_lw 0`; no complete
  inventory mask is available from ordinary pickup/ammo messages alone.
  The supplied `hw.dll` also gates `GetWeaponData` at `0x01DA4516` before
  calling the callback through `0x095D22E8` at `0x01DA4549`.
- `server.dll`'s no-active-weapon sender at `0x10202750` emits
  `CurWeapon(state=0, id=-1, clip=0, ammo=0)`. A negative ID is not itself a
  complete inventory snapshot and must not unconditionally erase every weapon.
- `player_weaponstrip` invokes the full strip at `0x101FC210` from
  `0x1021DB6B`. The strip zeroes all 64 ammo reserves at `0x101FC3CD`.
  The ammo sender at `0x101FCEE0` visits 64 indices but sends only changed
  counts. Its no-active sender is called at `0x101FCFBD`. The fallback
  correlates at least two distinct positive-to-zero reserves in consecutive
  ammo updates with a recent
  no-active message within one second, in either order, before clearing old
  ownership. An initial zero, a lone ammo depletion or a lone no-active
  message is insufficient. This is a conservative inference, not a dedicated
  server-side strip notification; one-ammo/melee-only and selective strips
  cannot be reconstructed exactly from these messages when prediction data
  is unavailable.

## Regression fixes

Exhaustion now needs positive ammo observed during the current pickup lifetime
followed by depletion, or a positive-to-empty CurWeapon update. New pickups
clear old depletion, clip/reserve observations and matching drop suppression.
This prevents a newly picked grenade from being pruned by the previous one's
cached zero before its new AmmoX message arrives. A refill cancels depletion.
Active weapons, positive clips, unknown reserves, nonempty secondary reserves
and non-exhaustible weapons retain their guards. Prediction snapshots, when
available, override these depletion heuristics.

`ITEM_FLAG_SELECTONEMPTY` is bit 0, not bit 2 (`NOAUTORELOAD`). It does not
override `EXHAUSTIBLE` and it is not evidence of possession.

An outstanding ammo-less alternate action now accepts the server's negative
no-active ID as a transition. Previously that branch discarded the pending
action without removing the thrown weapon. Rejected/expired actions and
authoritative weapon snapshots do not trigger this inference.

A pending classname selection also cannot rename an unknown weapon just
because the server repeats the same active ID: that can be a rejected switch
or an older ammo update. Naming waits for an actual active-ID transition.

## Ammo-only reacquisition regression

Waiting exclusively for `WeapPickup` leaves a previously dropped/exhausted
throwable permanently hidden when the server treats the next touch as an ammo
transfer to an existing item. The regression test now runs the real command
and user-message handlers, rather than simulating a call to the pickup helper:
`WeaponList -> WeapPickup -> AmmoX -> CurWeapon -> drop -> AmmoX(0) ->
AmmoPickup -> AmmoX(positive) -> inactive CurWeapon`. Before this fix, three
assertions fail: ownership is not restored, drop suppression remains armed,
and the item cannot return to its slot.

Reference anchors from the supplied Windows binaries:

- `client.dll` `AmmoPickup` handler `0x100048A0` reads a BYTE ammo index and a
  LONG quantity. The local HUD handler also needs that LONG, not the inherited
  Half-Life BYTE quantity. The packet is five bytes; malformed packets and
  `INT_MIN` quantities are rejected before history's absolute-value operation.
- `server.dll` registers `AmmoPickup` into `0x104FA334`. The grant path at
  `0x1020240E` writes the ammo index and LONG added count (`0x10202455`).
- The weapon duplicate dispatcher at `0x102A95D0` chooses ammo extraction
  through virtual offsets `0x234`/`0x238`. The latter is `0x102A9850`, which
  transfers ammo through the player's virtual grant method and does not emit
  `WeapPickup`. The primary extraction method `0x102A9620` likewise grants ammo.
- The separate weapon-pickup notifier at `0x102A9910` reads message ID
  `0x104FA2DC` and writes a SHORT weapon ID. `client.dll` reads that SHORT
  at `0x100048F0`. Ammo and weapon-pickup notifications are distinct events.

The fallback now marks only drop/exhaustion removals as ammo-recoverable.
An actual positive AmmoPickup or an increasing positive AmmoX can restore a
previously known EXHAUSTIBLE item with a nonpositive observed clip, a unique primary-ammo mapping
and no secondary ammo. This also cancels old drop suppression. It does not
register new weapon names or treat every ammo update as a new weapon.
Ambiguous shared pools, ordinary firearms, strip/reset removals, nonpositive
quantities and authoritative weapon-data snapshots do not use this recovery.
Explicit WeapPickup still works for ambiguous pools. No `cl_lw` or wire
userinfo setting is changed.

Tests in `Test_RunWeaponListFix` cover stale pickup ammo, replenishment,
exhaustion, no-active transitions, alternate actions, bulk reset ordering,
expired resets and existing slot cycling. These are state-machine tests, not
a guarantee that every third-party server sends all ownership information.
In particular, a plugin silently removing an inactive weapon without weapon
data or any corresponding messages cannot be inferred exactly by the client.
