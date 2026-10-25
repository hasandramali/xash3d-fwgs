# Screen shake and recoil timing

Two independent visual effects are involved: the engine handles the server's
six-byte ScreenShake message, while weapon events such as EV_FirePython set
the client-side punch angle through V_PunchAxis. Changing one path does not
change the other. Server-authoritative punch angles, aiming, movement
prediction, command rates and game/render FPS limits are not modified.

## 30 Hz visual updates

Both local effects now advance on the client presentation clock in 1/30-second
steps. Extra render frames reuse the latest result instead of advancing the
effect again. Multiple refdef passes at the same time likewise cannot decay
the effect more than once. Missed rendering intervals are caught up by the
elapsed step count rather than stretching the effect's duration. Below 30 FPS,
updates occur at the available render cadence while accounting for missed
steps.

For event recoil, the original 30 FPS decay recurrence is
`length_next = max(0, length * (1 - 0.5/30) - 10/30)`.
The equivalent bounded-work expression for N elapsed steps is
`max(0, (length + 20) * pow(1 - 0.5/30, N) - 20)`.
This preserves the reference 30 FPS recovery curve without a long catch-up
loop after an Android/iOS backgrounding or rendering interruption. View
calculation updates the punch before applying it, keeping same-time camera
passes consistent. New impulses start at their event time.

For ScreenShake, the existing frequency, duration, randomized offsets and
sine-wave shape are sampled on a 30 Hz clock. Amplitude decay uses the same
30 FPS multiplier for the elapsed number of steps. Negative decay multipliers
are clamped to zero. Expiration still uses the server-supplied deadline, not
a count of rendered frames. Expired amplitude cannot strengthen a later,
weaker shake. New server messages begin a fresh sampling interval.

Small backward clock corrections do not erase an active effect; map-sized
clock rewinds clear it. Invalid recoil axes/nonfinite impulses are rejected.
Neither implementation allocates memory or changes a public client/renderer
ABI; the two additional engine shake timestamps are internal state.

## Validation

Temporary ASan/UBSan tests compile the actual ScreenShake parser/calculator
and actual punch-update functions. They compare 15, 30, 60, 100, 144, 200 and
300 FPS runs, including long map clocks, same-time duplicate calls, missing
render intervals, expiration, weaker follow-up shakes, clock corrections and
rewinds, invalid network timings and invalid punch values. The client library
builds; engine syntax and ARM helper compile checks are separate from a
physical Android/iOS gameplay test, which still needs confirmation.
