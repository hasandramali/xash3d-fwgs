# Sven network limits and snapshot storage

## Reference observations

These addresses refer to the local `steam-refs/sven/hw.dll` reference build,
not every Sven release. They are virtual addresses, not file offsets.

| Observation | Reference |
| --- | --- |
| Packet entity output is capped at 1024 states | `0x1d21e90` and `0x1d21f7b` compare against `0x400` |
| Temporary entity pool has 2048 entries | Allocator overflow paths at `0x1d38db0` and `0x1d38e40` use `0x800` |
| `rate` default is 50000 | Cvar structure at `0x1edf214` |
| `cl_updaterate` default is 60 | Cvar structure at `0x1edf228` |
| `cl_cmdrate` default is 200 | Cvar structure at `0x1edf14c` |
| Legacy `-heapsize` uses kilobytes without a suffix | Conversion at `0x1dbc4fb`; subsequent heap-size check against 512 MiB at `0x1dbc51f` |

## Snapshot storage

The engine previously reserved 256 states per history slot. With 64 slots,
that retains only about 16 complete 1000-entity snapshots. Delta validation
also reserved only 256 output states, allowing a larger output snapshot to
overwrite its own source snapshot while decoding.

For Sven, history capacity now grows from 256 through 512 to 1024 states per
slot when needed. Growth remaps retained states by their absolute indices
and invalidates snapshots whose data had already been overwritten. Validation
reserves the declared output count. The decoder checks that count before
appending states and rejects snapshots whose decoded count does not match.

At 340 bytes per state, a 64-slot history uses approximately 5.31 MiB initially
and 21.25 MiB at maximum capacity, excluding allocator metadata. During the
largest growth step, both allocations temporarily occupy about 31.88 MiB.
Capacity is not repeatedly grown and shrunk during normal snapshot processing.

Model index zero and nonsolid state do not establish packet corruption.
Active-play entities with those properties remain in snapshot history rather
than being discarded or triggering an automatic reconnect. The independent
signon baseline checks remain enabled.

## Limits that should not be blindly doubled

Entity/model/sound index widths and delta sequence widths are wire protocol
properties. Movement structures such as `playermove_t` also share an ABI with
the client library. Increasing those constants locally is not a connection
optimization and can break compatibility.

Increasing temporary entity capacity above the reference's 2048 entries can
increase collision, update and rendering work. It does not guarantee lower
latency. Socket buffers already request 512 KiB in each direction; the OS may
cap the granted capacity. Incoming packets are drained in a receive loop.

This engine's memory pools allocate dynamically through `Q_malloc`; they are
not the reference engine's fixed startup heap. Adding a legacy `-heapsize`
reservation would not make the snapshot history or network transport faster.

The reference rate defaults above are comparison data, not forced settings.
Existing user settings and server rate restrictions remain untouched. Actual
latency still depends on server load, rendering time, packet loss, bandwidth
and the network route; these changes do not establish lower latency than the
original Sven client.

## Validation

Temporary harnesses using the actual snapshot functions and network bit reader
exercise history growth and wraparound, preservation of retained states,
non-resurrection of overwritten snapshots, output reservations, 16-bit delta
sequence wraparound, the 63/64-frame age boundary, 1000-entity full/delta
snapshots, models without geometry, signon baseline rejection, and malformed
or truncated entity counts. These checks run with UndefinedBehaviorSanitizer.
They do not replace an Android/iOS crowded-server performance test.
