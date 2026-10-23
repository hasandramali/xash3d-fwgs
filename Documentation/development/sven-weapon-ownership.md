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

Tests in `Test_RunWeaponListFix` cover stale pickup ammo, replenishment,
exhaustion, no-active transitions, alternate actions, bulk reset ordering,
expired resets and existing slot cycling. These are state-machine tests, not
a guarantee that every third-party server sends all ownership information.
In particular, a plugin silently removing an inactive weapon without weapon
data or any corresponding messages cannot be inferred exactly by the client.
