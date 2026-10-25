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
