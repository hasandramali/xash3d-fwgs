# Mobile push-to-talk

Android Settings exposes `Microphone` above `Use volume buttons`. The iOS
launch sheet exposes the same setting below `Immersive mode`. Both default to
enabled, but recording additionally requires the operating system's permission.
Denial keeps incoming voice available.

The launcher sets `XASH3D_MICROPHONE` before starting the engine. On mobile,
only the value `1` permits transmission; an absent or disabled value is
receive-only even if a config sets `voice_receiveonly 0`. No capture device is
opened at connection time. Hold `+voicerecord` to open and record; release it
(`-voicerecord`) to stop and close capture. Focus loss and disconnect also stop
recording. This does not add a touch button or change existing bindings.

## Native reference and wire format

The supplied `steam-refs/sven/hw.dll` (image base `0x01d00000`) confirms that
voice belongs to the engine, not the game client DLL:

- `0x01dcf070` starts recording through SteamUser020 virtual slot `0x1c`.
- The stop path ending at `0x01dcf25c` calls slot `0x20` at `0x01dcf235`.
- `0x01dcf260` queries available data through slot `0x24`, then calls `GetVoice`
  through slot `0x28` at `0x01dcf2fd`.

The local ReHLDS_Sven reference, `SV_ParseVoiceData` in
`rehlds/engine/sv_user.cpp`, consumes a 16-bit byte count and relays the Steam
payload according to server voice masks. It rejects payloads above 4096 bytes.

Our portable sender uses the existing libopus encoder at 24 kHz mono, with
480-sample frames. It emits the Steam container already supported by our
receiver: Steam ID, sample-rate control (11), Opus PLC chunk (6), per-frame
length/sequence pairs, and CRC32. Packets are capped at 1200 bytes and available
datagram space. Multi-byte fields use `memcpy` to avoid unaligned ARM access.
Unsent PCM remains buffered when the packet budget is exhausted.

This is a portable compatible-container implementation, not a port of Steam's
proprietary capture/encoder. Real libopus encode/container/decode tests and
receive-only regression tests pass with sanitizers. Microphone hardware,
platform audio routing and reception by original PC clients still require
device/server testing. Server voice enablement, mute masks and team/alltalk
rules remain authoritative; microphone permission does not override them.
