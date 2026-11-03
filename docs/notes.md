# Engineering notes

What follows is the record behind Hush: what was measured, what was tried, and
what does not work. It exists so nobody repeats the dead ends.

## The buffer is not in the filesystem

During a 19-minute session the free space on `/data` did not move by a single
byte, and `/user/temp/video_recording` stayed empty. The kernel log at clip-save
time names the real location:

```
[STORAGE:recording mapping] free:0xf6f6ef0000
  nsid1(used:0x9641100000 op:0x1e24d80000) nsid2(used:0x700b8000 garbage:0x7fa0000)
```

`nsid2` is a BFS logical partition on a separate namespace of the internal NVMe,
about 30 GB, written by the capture daemon and reported through
`bfsInternalSsdStorageNsid2*` counters.

## Write rate

A saved 15-second clip on 13.60 is 31.6 MB, about 17 Mbit/s. Assuming the ring
buffer is encoded at the same quality, an hour of play writes roughly 7–8 GB.
That figure is an inference from the clip bitrate, not a counter reading: no
readable interface for the BFS write counters was found on 13.60.

## There is no off switch

* No `regmgr` key controls recording; the settings database has no entry for it.
* `/user/system/video_recording/` and `/user/temp/video_recording/` stay empty.
* The per-app flag behind `getIsDisableRecording` (`ShellCore+0x124`) is only
  reported to `ShareLibrary` as a capability. It does not gate the writer.
* Recording data does not pass through the rec server's message dispatch: with
  hooks on the dispatch entry, 34 messages arrive at game launch and none
  afterwards, and the worker that maps the client ring is never called during
  play. The data reaches storage through a mapped buffer, so there is no
  per-block function to patch.

## Suspending the encoder works, and breaks the exit

`SIGSTOP` on `SceAvCapture` stops the writes: the Create menu loses its
durations and no clip can be saved. But the console needs the daemon to leave a
game. With the daemon suspended at quit time, `ShellCore` waits, times out, and
the system software error appears — the daemon dies and does not come back
without a reboot.

A 5 s stopped / 1 s running pulse survives game exit and cuts writes by about
83 %, but it is a compromise, not an off switch.

## Tooling reality on 13.60

| Primitive | Result |
| --- | --- |
| `kern.proc.all` | not exposed; use `kern.proc.1.<pid>` per pid |
| `ptrace(PT_ATTACH)` on a system daemon | `ESRCH` from a payload jail |
| `mdbg_copyout` | works, reads any process |
| `mdbg_copyin` | `EPERM`, even for the process itself |
| `kernel_proc_copyin` / `kernel_proc_copyout` | works, read and write |
| `kernel_mprotect` / `kernel_get_vmem_protection` | `-1` |
| ELF header at module base | not mapped; the page contains `0xCC` |
| `ps5debug-NG` on `SceShellCore` | `CMD_ERROR`: the debugger lives inside it |

The ELF program headers being absent is why Hush anchors the gadget search on
`kernel_dynlib_entry_addr` instead of parsing segments. That is a weaker bound
than a segment check and it is called out in the source.

## Two crashes worth knowing about

Both came from debugging, not from the finished payload.

1. Suspending `SceAvCapture` and quitting a game: the daemon was needed for the
   transition, the wait timed out, the daemon died.
2. Aborting a `ps5debug` session mid-attach left the debugger wedged; the
   console shut down.

Neither left anything behind: the changes were in process memory only, and a
power cycle restores the stock console.

## What Hush ended up doing

Point every import slot of `sceVencStartEncode` in `SceAvCapture` at an existing
`xor eax, eax; ret`. The call reports success, nothing is encoded, nothing is
written, and every other part of the capture daemon keeps working. Between one
and four slots on 13.60, depending on how far the daemon has initialised;
verified after each write, recorded before the first one, revertible.

## Measuring the writes: no counter exists on 13.60

The obvious number for a claim like "nothing is written" is the drive's own
`Data Units Written`. It is not reachable, and this is where each attempt stops.

| Attempt | Result |
| --- | --- |
| `/dev/nvme0` | does not exist; the internal drive is `/dev/ssd0`, `/dev/ssd0.user`, `/dev/ssd0.system` |
| `open("/dev/ssd0")` as a payload | `EPERM` at the payload's own authid `0x480000001000000e`, and at `0x4800000000000010`, `0x3800000000000010`, `0x4000000000000010` |
| `open("/dev/ssd0")` at authid **`0x4801000000000013`** | succeeds; `DIOCGSECTORSIZE` 512, `DIOCGMEDIASIZE` 2010944372736 (2 TB model) |
| NVMe admin command through it | `ENOTTY`; the driver is not FreeBSD `nvme(4)`, so there is no `GET LOG PAGE` passthrough |
| `sysctl kern.devstat.all` | missing; no per-device byte counters |
| `sysctl kern.bfs`, `vfs.bfs.stats`, `kern.ssd`, `dev.ssd.0` | all missing |
| `kern.geom.confxml` | exists, 35 kB, but it is geometry, not statistics |

What does exist is `/dev/klog`, and it carries the BFS counters:

```
[STORAGE:recording mapping] free:0xf6f6ef0000
  nsid1(used:0x96811d0000 op:0x1e24d80000) nsid2(used:0x6fd000000 garbage:0x37a0000 grabbed:0x0)
```

`nsid2` is the recording partition. On 13.60 it is 27.95 GiB used of roughly
30 GB, and the `op` field does not advance between events, so this line is
occupancy, not throughput.

Measured with a watcher on `/dev/klog`:

* With Hush active, saving a clip produced **no `[STORAGE:recording mapping]`
  line at all** — the console answered “Gameplay isn't being recorded”.
* With Hush reverted and a fresh game session, saving produced the line, and
  `nsid2(used)` moved from `0x6fcff0000` to `0x6fd000000`: 64 KiB on a partition
  already 27.95 GiB full.

The ring is allocated, so new data displaces old data and `used` stays flat. A
byte-accurate write rate is therefore out of reach on this firmware, and the
7–8 GB/h figure in the README stays an inference from the clip bitrate.

## Reverting takes a new game session

Setting `enabled=0` restores the original function in every slot within twenty
seconds, but a game that was already running keeps refusing to save. All four
slots were restored mid-session and the console still answered “Gameplay isn't
being recorded” for that title until it was restarted. Recording returned with
the next game start.

## Remote Play shares the encoder

Reported on 13.60 ([#3](https://github.com/disco-trooper/hush/issues/3)): with
Hush loaded, Remote Play shows a black picture and the streaming client reports
that the previous session is still finishing. Without Hush the same setup
streams fine. The user bisected the autoload list one payload at a time and Hush
was the first addition that changed the result; the PS5Upload maintainer closed
[the upstream report](https://github.com/phantomptr/ps5upload/issues/561) as a
conflict with Hush rather than something to fix there.

The mechanism is the one the patch already implies. `SceAvCapture` provides the
encoder for local recording and for network streaming alike. Once the entry
returns success without encoding anything, the stream has no frames — and the
session state machine never completes either, which is where "still finishing"
comes from. So the two features are mutually exclusive today. Nothing in this
is specific to 13.60: Hush resolves and patches the same entry on every
firmware, so the conflict should follow it. Only 13.60 has been reported.

Reverting with `enabled=0` restores the slots, but a stream that is already
stuck may need the console restarted.

Open direction, unverified: patch by call site instead of by slot. The four
import slots are distinct addresses inside the daemon, and the slot that appears
when a recording starts may not be the one the streaming path uses. A hook that
checks its caller would allow recording to stay dead while streaming lives.
Nobody has measured which slot belongs to which feature yet.

## The pause is done by the client, and it works

Verified on 13.60 on 2026-11-01. PS5Upload 10.4 recognises the payload on its own,
and starting a Remote Play stream offers `Pause Hush while I play`. What the
console log showed:

```
17:22:16 sceVencStartEncode restored in 4 slot(s)
17:22:16 disabled in config, exiting
17:24:21 sceVencStartEncode restored in 4 slot(s)   <- stream start
17:24:21 disabled in config, exiting
17:24:42 hush started; enabled=1 ... redirected in 4 slot(s)   <- stream end
```

So the picture comes back within a second of the pause and recording protection
returns about twenty seconds after the stream stops. Nothing inside Hush had to
change: the client writes `enabled=0`, waits for the slots, streams, then writes
`enabled=1` and reloads the payload.

That leaves the conflict for clients that do not know about Hush — Sony's own PS
Remote Play app, chiaki-ng, capture tooling. For those the answer is still
`enabled=0`, or a Hush-side fix that tells the recording path apart from the
streaming path. The cheap version of that is still untried: patching only the
slots the recorder uses, which needs the four slots mapped to their callers.

## What decides whether the patch sticks: timing, not the slot

Measured on 13.60 on 2026-11-01:

| When Hush is loaded | Recording |
| --- | --- |
| before the game starts (autoload, or restart the game afterwards) | blocked |
| while the game is already running | keeps working |

`redirected in 4 slot(s)` in the log therefore does not prove the patch is
effective. The capture daemon resolves the encoder once when it creates it and
keeps that pointer; rewriting the import table afterwards changes nothing. A
game restart re-creates the encoder, and the patch applies again.

## Selective patching does not separate recording from streaming

The idea was that one of the four import slots belongs to the recorder and the
others to Remote Play. It does not hold. With the full set patched, recording
stops and a game stream hangs on "Starting the stream". With only the slot at
`base+0x284d8` patched — the one that once looked like the recorder — recording
still worked, on three separate boots.

So the encoder really is shared, and no subset of the four slots gives
"recording off, streaming on". That needs a different target: the path that
consumes encoded frames and writes them into the recording area, which has not
been located yet.

## The first slot set is not the final one

At boot `SceAvCapture` exposes one import slot; three more appear later. With a
five minute rescan a recording could start and finish inside that window, which
is why a clip saved minutes after loading Hush could still exist. The default
rescan is now 60 s.

## The recorder does not retry, and that makes Remote Play possible

Measured on 13.60 on 2026-11-03. The console builds the recording encoder when a
game starts and keeps that pointer for the whole session. Two consequences:

* Patching the import slot while a game is already running changes nothing: the
  encoder exists, the call never goes through the slot again.
* Patching for the first seconds of a game is enough. Once encoder creation has
  failed, the recorder does not try again for that session, even after the slot
  is restored.

That second point is what `hold_s` uses. The patch is armed when a game process
appears, held for `hold_s` seconds, then lifted. The run with the final build:

```
13:29:54 sceVencStartEncode redirected in 4 slot(s), 0 failed
13:29:54 hold: game 116 started, armed for 30s
13:30:24 sceVencStartEncode restored in 4 slot(s)
13:30:24 hold: released, Remote Play is free
```

After the release, saving a clip still answered “Gameplay isn't being recorded”,
and a Remote Play stream of the running game had its picture. So the encoder is
free for streaming while the recorder stays dead.

Limits: a stream started inside the window fails and can leave the session
reporting “still finishing”; a title that builds a second encoder later in the
session would record again (none seen so far); and Hush loaded while a game runs
cannot block that game retroactively, so it skips it.

## Finding the game: no kern.proc.all

The first hold build found `eboot.bin` by asking about every pid from 1 to
99999, once a second. It worked — the 12:49 run armed — but cost about 100,000
sysctl calls a second. A review proposed reading the whole process table with
one `KERN_PROC_ALL` sysctl instead. On 13.60 that node returns `ENOENT` — the
table at the top of this file already said so — and that build never saw a game:
Kingdom Come: Deliverance II ran at pid 108 and nothing armed.

The detector now uses per-pid `KERN_PROC_PID` lookups only: the last game's pid
first, then pids 1–2048 every second and the rest every ten seconds. Game pids
seen here were 104, 108, 116, 125 and 132, so a game is found within a second
and a running one costs a single lookup a second.

Two details keep that cheap detector from misfiring:

* A pid is only accepted together with its `ki_start` time. A new game that
  lands on a previously used pid is therefore a new game, not the one already
  handled.
* "The game closed" is declared only after three consecutive sweeps come back
  empty. One missed lookup, or the ten-second gap on pids above 2048, would
  otherwise re-arm the patch inside a running game, which costs nothing for
  recording but blocks streams for another window.

## Hold mode: what the reviews caught

Two code reviews of the first hold build found problems that the happy path
hid; each fix was checked on the console on 2026-11-03.

* `enabled=0` and `mode=check` were read once at start, so the master switch did
  nothing and check mode patched. The config is now re-read every second.
* A run killed inside the window left patched slots with no `state.txt`, and the
  next run could not restore them. On start Hush now scans for slots that point
  at its gadget and restores them; the test cleared three such slots.
* With a game already running at load, the first build armed anyway: no effect
  on recording, but streams failed for the window. That game is now skipped.
* A failed release used to be dropped silently; it is now logged and retried, on
  the timed release and on the `enabled=0` exit alike.

## One slot is the recorder's, and it is the only one with code behind it

Of the four import slots, only `base+0x29b78` is reached by code — a single
`jmpq *0x...(%rip)` stub at `base+0x1aad0`. The other three only appear in data
and are filled at runtime. Patching just the slot behind that stub is enough to
stop recording; patching any other single slot is not.

This also closes the idea of separating recording from streaming by choosing
slots: both features call the encoder through the same single call site
(`base+0x2e4b`), so no subset of slots gives “recording off, streaming on”.
