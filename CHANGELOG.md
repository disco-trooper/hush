# Changelog

## Unreleased

* **`hold_s`.** With `hold_s=N`, the patch is armed when a game process appears
  and lifted N seconds later instead of being held for the whole session. The
  recorder builds its encoder inside that window and never retries, so recording
  stays off while the encoder is free for Remote Play afterwards. Default `0`
  keeps the previous behaviour byte for byte. Verified on 13.60 with Kingdom
  Come: Deliverance II.
  * `config.ini` is re-read every second in hold mode: `enabled=0` restores and
    exits, `mode=check` reports without patching.
  * A game already running when Hush starts is skipped instead of being patched
    for nothing.
  * A failed release is logged and retried; the window is tied to the capture
    daemon's pid, and a replaced daemon is detected by name and state.
  * On start, slots that already point at the gadget are restored even when no
    `state.txt` covers them.
  * Games are found by a pid sweep for `eboot.bin` (`kern.proc.all` is not
    exposed): low pids every second, the full range every ten seconds, and the
    found pid is cached.
* **Faster rescan.** `rescan_s` defaults to 60 instead of 300, so import slots
  that appear after a game start are picked up sooner.

## 1.0.0

First release.

Hush stops the console's continuous recent-gameplay recording by pointing every
import slot of `sceVencStartEncode` inside `SceAvCapture` at an existing
`xor eax, eax; ret`. The call reports success, nothing is encoded, nothing is
written, and the rest of the capture path — compositor, scan-in, output
switching — keeps working, so games start and quit normally.

* **Symbol-based discovery.** The module and the function are resolved by name
  through the kernel dynlib API on every start, so nothing is tied to a fixed
  address and a reboot or a moved layout does not break it.
* **Verified writes.** Every slot is written, read back and compared; the run
  fails loudly if the end state is not what it claims.
* **Recorded before writing.** The gadget and the exact slot addresses are saved
  atomically before the first write, and `enabled=0` restores them precisely.
* **Resident daemon.** Known slots are verified every 20 seconds, a full rescan
  runs every five minutes and whenever the capture process is replaced, and a
  single-instance lock keeps two copies from fighting. When the capture daemon
  exposes more slots than before, the log records the growth (`slot count grew
  1 -> 4`) instead of silently extending the patch.
* **`mode=check`.** Reports the firmware version, module base, gadget and every
  import slot with its current target, then exits without writing a byte. It
  takes no lock, so it can run alongside the daemon, and `enabled=0` still wins.
* **Firmware in the log.** Every start records the version, decoded and raw.
* **Reproducible builds.** `-Wl,--build-id=none`, so a rebuild with the same
  compiler, linker and SDK gives a byte-identical binary and the same SHA-256.
* **No firmware writes.** Everything lives in process memory and in
  `/data/hush/`; a power cycle restores the stock console.

Tested on firmware 13.60.
