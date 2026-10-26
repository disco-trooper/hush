<p align="center">
  <img src="assets/icon.png" alt="Hush" width="132" height="132">
</p>

<h1 align="center">Hush</h1>

<p align="center"><em>The PlayStation 5 stops recording.</em></p>

<p align="center">
  <a href="https://github.com/disco-trooper/hush/releases"><img alt="release" src="https://img.shields.io/github/v/release/disco-trooper/hush?display_name=tag&sort=semver&color=c8a961" /></a>
  <a href="https://github.com/disco-trooper/hush/releases"><img alt="downloads" src="https://img.shields.io/github/downloads/disco-trooper/hush/total?color=c8a961" /></a>
  <a href="LICENSE"><img alt="license" src="https://img.shields.io/badge/license-GPL--3-c8a961" /></a>
  <img alt="firmware" src="https://img.shields.io/badge/PS5_firmware-7.61_·_13.20_·_13.60-brightgreen" />
  <img alt="payload" src="https://img.shields.io/badge/payload-ELF_·_Payload_Manager-lightgrey" />
</p>

---

Hush is a small payload for jailbroken consoles that turns off the console's
continuous "recent gameplay" recording. With nothing encoded, there is nothing
for the console to save.

After downloading `hush.elf` and `SHA256SUMS` from [Releases](https://github.com/disco-trooper/hush/releases):

```
$ sha256sum -c SHA256SUMS      # Linux
$ shasum -a 256 -c SHA256SUMS  # macOS
hush.elf: OK
```

## The problem

Every PS5 encodes the last 15 seconds to one hour of gameplay all the time.
Sony exposes the clip length, never an off switch. The data lands in a reserved
area of the internal NVMe, so it does not show up in any storage screen.

Measured on a 13.60 console, a saved 15-second clip is ~31.6 MB. Read as the
buffer's bitrate, that implies **roughly 7–8 GB written per hour of play** — an
inference from the clip size, not a counter reading, because no readable
interface for the storage counters was found. Over a year of normal use it is
terabytes for footage nobody asked for.

## What Hush does

`SceAvCapture` creates its encoder through `libSceVenc`. Hush resolves that
library inside the running daemon, finds every import slot that holds
`sceVencStartEncode`, and points it at an existing `xor eax, eax; ret` in the
daemon's own code. The call reports success and encodes nothing.

* One pointer per slot. The daemon exposes between one and four of them on a
  13.60 console, depending on how far it has initialised; Hush tracks all of
  them.
* Every write is read back and verified.
* The exact slots and the target address are stored before the first write, so
  the change can be reverted precisely.
* A reboot restores stock behaviour because no firmware or system file is
  changed. Hush's own files under `/data/hush` do persist; they are
  not firmware state.

## What Hush does not do

* It does not modify firmware, `/system`, `/preinst` or the boot chain. The
  patched memory does not survive a power cycle; Hush's own files under
  `/data/hush` do.
* It does not stop `SceAvCapture`, so compositor, scan-in and output switching
  keep working.
* It does not remove the clip options from the Create menu. The menu still
  offers durations; saving answers **“Gameplay isn't being recorded”**. That
  message is the console's own confirmation that the buffer is empty.
* It is not a general framework, and it does not check your firmware version —
  it will attempt the patch wherever it is loaded. See
  [Firmware](#firmware) for what has been reported working.

## Requirements

* A jailbroken PS5 with a way to load payloads — Payload Manager, elfldr, or
  any loader that can run an ELF.

## Firmware

Nothing is hardcoded to a firmware version: the capture process is found by
name, the encoder function is resolved at runtime, and the gadget is located
inside the daemon's own code. A different layout therefore does not break the
lookup by itself.

| Firmware | Status | Evidence |
|---|---|---|
| 13.60 | developed and tested here | four import slots tracked, revert path exercised, no spurious log lines over hours of uptime |
| 13.20 | reported working | user report: the Create menu answers “Gameplay isn't being recorded” |
| 7.61 | reported working | user report: the same message, and the preview square stays blank while the timer runs |

The bottom two rows are user reports, not my own measurements. What they show is
that resolving and patching works on older firmware as well. What is still
unverified there is the rest: whether the call's success semantics and the slot
layout match. If they do not, the capture daemon can crash and the console shows
a system software error until you restart it. A reboot is enough — nothing is
written to firmware.

If you run a firmware that is not in the table, please try `mode=check` first (it
writes nothing) and open an issue with your firmware version and `log.txt`.

## Install

1. Download `hush.elf` from [Releases](https://github.com/disco-trooper/hush/releases) and check it against
   `SHA256SUMS`.
2. Upload it to Payload Manager and load it, or add `local/hush` to the autoload
   list after ShadowMount+ so it runs on every boot.
3. Start a game. The Create menu should answer “Gameplay isn't being recorded”.

Hush stays resident. It verifies the slots it knows every 20 seconds and repairs
them if they moved; a full rescan, which is what discovers slots created later,
runs every five minutes and whenever the capture process is replaced.

## Turn it back on

Edit `/data/hush/config.ini`:

```ini
enabled=0
```

Within about twenty seconds Hush restores the original function in every slot it
changed, notifies “recording back on”, and exits. A reboot does the same thing
less politely.

To uninstall completely: let it revert as above (or reboot), remove the
`local/hush` entry from Payload Manager's autoload list or from whichever loader
you used, and delete `/data/hush/` including `log.txt.old`. Removing the
autoload entry matters — otherwise Hush starts again on every boot.

## Files on the console

```
/data/hush/
├── config.ini    enabled, verbose, mode, interval_s, rescan_s
├── log.txt       what happened and when, rotated at 64 KB
├── state.txt     target address + exact slots, written before any change
└── pid.lock      single-instance guard
```

## Check before you change anything

Set `mode=check` in `config.ini` and load Hush. It resolves the process, reports
the firmware version, the module base, the gadget and every import slot it can
see — with what each slot currently points at — and then exits without writing a
single byte. This is the safe way to contribute data from a firmware nobody has
tested:

```ini
enabled=1
mode=check
```

```
hush started; firmware=13.60 (0x13600007) enabled=1 mode=check
check: pid=60 base=0x2324000 function=0x83129e0e0 gadget=0x2327ca0 slots=4
check: slot[0]=0x234c4d8 points at 0x2327ca0 (redirected)
```

On a console where Hush has never run there is no config file yet, and the first
run writes one with `mode=apply` and patches immediately. To inspect a fresh
console without changing it, create `/data/hush/config.ini` yourself first:

```ini
enabled=1
mode=check
```

Check mode does not take the single-instance lock, so it can run while the
resident daemon is loaded; it reports once and exits. A resident daemon ignores
`mode=check` and keeps maintaining the patch. `enabled=0` still wins: if
the master switch is off, Hush reverts instead of reporting.

Set `mode=apply` to go back to patching.

## Verify it yourself

* `Create → Save Recent Gameplay` answers “Gameplay isn't being recorded”.
* After pressing Save Recent Gameplay, no new clip appears under
  `/user/av_contents/video/` — `.webm` by default, or `.mp4` if you changed
  Settings → Captures and Broadcasts → Captures → Video Clip Format. Note that a
  plain session writes nothing there either way; the clip only appears when a
  save succeeds, which is what makes this check meaningful.
* `log.txt` shows `sceVencStartEncode redirected in N slot(s), 0 failed`, where N
  is how many slots the last full scan found. It is one to four on 13.60 and can
  grow while the capture daemon initialises.

## Safety

The only thing Hush changes is a function pointer inside the capture daemon.
That write is verified, recorded, and reversible; a reboot always restores the
stock console. The realistic failure modes are a crashed capture daemon and the
system software error that follows, both cleared by a restart. There is no path
from here to a brick.

## How it works, if you care

```
find pid           kern.proc.1.<pid> lookups (kern.proc.all is not exposed)
resolve module     kernel_dynlib_handle("eboot.bin") + mapbase
resolve function   kernel_dynlib_dlsym("sceVencStartEncode")
find the gadget    first `31 C0 C3` within ±256 KB of the module entry point
find the slots     every 8-byte word in the first 8 MB of the module that
                   holds the function
write              kernel_proc_copyin, read back, compare
record             gadget + slot addresses, atomically, before writing
```

`mdbg_copyin` returns `EPERM` for this and `ptrace` cannot reach system daemons
from a payload jail, so the kernel process-copy calls are the only workable
primitive. Those details, and everything that did not work, are in
[docs/notes.md](docs/notes.md).

## Build

```sh
PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk make
```

The Makefile requires `$PS5_PAYLOAD_SDK/bin/prospero-clang` and stops with an
error when it is missing. If the wrapper exists but its bundled toolchain check
fails, the build retries with `clang` against the SDK sysroot, exporting
`SCE_PROSPERO_SDK_DIR` and putting the SDK's `bin` first on `PATH` — so that is
normally the SDK's own clang, not a system one.

Builds are reproducible with the same toolchain. The PS5 toolchain normally
links with `--build-id=uuid`, which made every build differ byte for byte, so the
Makefile passes `-Wl,--build-id=none`; building this source twice with the same
compiler, linker and SDK gives byte-identical output and the same SHA-256 as the
release asset.

The release was built with Homebrew clang and LLD **23.1.4** against the
`ps5-payload-sdk` snapshot in the repository's development setup. A different
clang, lld or SDK produces different code — the compiler identity is embedded in
`.comment` — so compare hashes only against a build made with those versions.

## License

GPL-3.0. See [LICENSE](LICENSE).
