<p align="center">
  <img src="assets/icon.png" alt="Hush" width="132" height="132">
</p>

<h1 align="center">Hush</h1>

<p align="center"><em>The PlayStation 5 stops recording.</em></p>

<p align="center">
  <a href="https://github.com/disco-trooper/hush/releases"><img alt="release" src="https://img.shields.io/github/v/release/disco-trooper/hush?display_name=tag&sort=semver&color=c8a961" /></a>
  <a href="https://github.com/disco-trooper/hush/releases"><img alt="downloads" src="https://img.shields.io/github/downloads/disco-trooper/hush/total?color=c8a961" /></a>
  <a href="LICENSE"><img alt="license" src="https://img.shields.io/badge/license-GPL--3-c8a961" /></a>
  <img alt="firmware" src="https://img.shields.io/badge/PS5_firmware-7.61_·_13.20_·_13.40_·_13.60-brightgreen" />
  <img alt="payload" src="https://img.shields.io/badge/payload-ELF_·_Payload_Manager-lightgrey" />
</p>

---

Hush is a small payload for jailbroken consoles that turns off the console's
continuous "recent gameplay" recording. With nothing encoded, there is nothing
for the console to save.

## The problem

Every PS5 encodes the last 15 seconds to one hour of gameplay all the time.
Sony exposes the clip length, never an off switch. The data lands in a reserved
area of the internal NVMe, so it does not show up in any storage screen.

On a 13.60 console a saved 15-second clip is ~31.6 MB. Read as the buffer's
bitrate, that is **roughly 7–8 GB written per hour of play** — an inference from
the clip size; no readable storage counter was found.

## What it does

`SceAvCapture` creates its encoder through `libSceVenc`. Hush finds every import
slot of `sceVencStartEncode` in the running daemon and points it at an existing
`xor eax, eax; ret` in the daemon's own code. The call reports success and
encodes nothing.

* Every write is read back and verified, and the exact slots are recorded before
  the first one, so the change reverts precisely.
* Only process memory changes: no firmware, `/system`, `/preinst` or boot chain.
  A reboot restores the stock console; Hush's own files in `/data/hush` stay.
* `SceAvCapture` keeps running, so games start and quit normally.
* The Create menu still lists clip durations; saving answers **“Gameplay isn't
  being recorded”**, the console's own confirmation that the buffer is empty.

The worst realistic failure is a crashed capture daemon and a system software
error, cleared by a restart. There is no path from here to a brick.

## Install

1. Download `hush.elf` and `SHA256SUMS` from
   [Releases](https://github.com/disco-trooper/hush/releases) and check them:
   `shasum -a 256 -c SHA256SUMS` (macOS) or `sha256sum -c SHA256SUMS` (Linux).
2. Upload it to Payload Manager and load it, or add `local/hush` to the autoload
   list after ShadowMount+ so it runs on every boot.
3. Using Remote Play? Add `hold_s=30` to `/data/hush/config.ini` (see
   [Remote Play](#remote-play)) and reboot or reload Hush. The first run creates
   the file.
4. Start a game and try `Create → Save Recent Gameplay`: it should answer
   “Gameplay isn't being recorded”.

Any loader that runs an ELF works. Hush stays resident: it checks its slots every
20 seconds and rescans every 60.

## Remote Play

Remote Play uses the same encoder, so while the patch is in place a stream gets
no picture, and the console then stays on “still finishing the last Remote Play
session” until it is restarted.

`hold_s` decides how long the patch stays:

* **`hold_s=0`** (default): for as long as Hush runs. To stream, set `enabled=0`
  *before* the first stream attempt of that boot.
* **`hold_s=N`**: N seconds from the moment a game starts. The console builds its
  recording encoder in that window and never retries, so recording stays off for
  the whole game while Remote Play works after the window.

Tested with `hold_s=30` on 13.60 with Kingdom Come: Deliverance II, and reported
working on another console with *Sega Genesis/Mega Drive Classics* (`CUSA10828`,
a PS4 title, [#3](../../issues/3)).

With `hold_s=N`:

* **Don't connect during the window.** A stream started inside it gets stuck
  like above.
* **Load Hush before the game.** A game that is already running is skipped and
  covered from its next start.
* **If a game still records,** its recorder started after the window. Raise
  `hold_s` (60, then 120); the new value applies from the next game start. Check
  each new title once: after the window, *Save Recent Gameplay* must still refuse.
* Switching between `0` and a non-zero value needs a reload of Hush.

## Configuration

`/data/hush/config.ini`, written with defaults on the first run:

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `1` | `0` restores every slot, notifies “recording back on”, and exits |
| `mode` | `apply` | `check` reports what it sees and writes nothing |
| `hold_s` | `0` | seconds to hold the patch after a game starts; `0` = always |
| `interval_s` | `20` | how often known slots are verified |
| `rescan_s` | `60` | how often all slots are searched again |
| `verbose` | `0` | `1` logs more |

Next to it: `log.txt` (rotated at 64 KB), `state.txt` (the patched slots; present
exactly while the patch is in place) and `pid.lock`.

**Turn it off:** `enabled=0`. Within 20 seconds (one with `hold_s` set) the
slots are restored and Hush exits. A running game keeps refusing to save;
recording returns with the next game. **Uninstall:** turn it off, remove
`local/hush` from autoload, delete `/data/hush/`.

## Firmware

Nothing is tied to a firmware version: the process, the module and the function
are all resolved by name at runtime, and Hush does not check the version.

| Firmware | Status | Evidence |
|---|---|---|
| 7.61 | reported working | user report: “Gameplay isn't being recorded”, and the preview square stays blank while the timer runs |
| 13.20 | reported working | user report: “works”, with no detail about what was checked |
| 13.40 | reported working | user report: the same message, with a log showing four slots redirected |
| 13.60 | developed and tested here | four import slots tracked, revert path exercised, no spurious log lines over hours of uptime |

On a firmware not in the table, run `mode=check` first and open an issue with
your firmware version and `log.txt`. On a console where Hush never ran, create
the config yourself first, or the first run patches straight away:

```ini
enabled=1
mode=check
```

```
hush started; firmware=13.60 (0x13600007) enabled=1 mode=check
check: pid=60 base=0x2324000 function=0x83129e0e0 gadget=0x2327ca0 slots=4
check: slot[0]=0x234c4d8 points at 0x2327ca0 (redirected)
```

Check mode runs alongside a resident Hush, reports once and exits. Set
`mode=apply` to patch again.

## Build

```sh
PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk make
```

Builds are reproducible: the same compiler, linker and SDK give the release's
exact SHA-256. Releases are built with Homebrew clang and LLD 23.1.4. How it
works, the build fallback, and everything that did not work are in
[docs/notes.md](docs/notes.md).

## License

GPL-3.0. See [LICENSE](LICENSE).
