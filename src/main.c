/* hush - stop the PS5 recent-gameplay ring buffer
 *
 * The console continuously encodes the last minutes of gameplay and writes it
 * to a reserved area of the SSD. There is no setting to turn that off.
 *
 * SceAvCapture creates the recording encoder through libSceVenc. This payload
 * points every import slot of sceVencStartEncode at an existing
 * `xor eax,eax; ret` inside the capture daemon, so the call reports success
 * but nothing is ever encoded and nothing is written. Compositor, scan-in and
 * output switching are untouched, so starting and quitting games keeps
 * working. The patch lives in the process, so a reboot restores stock
 * behaviour.
 *
 * Copyright (C) 2026 disco_trooper
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/user.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

#define DIR "/data/hush"
#define CFG DIR "/config.ini"
#define LOG DIR "/log.txt"
#define STATE DIR "/state.txt"
#define LOCK DIR "/pid.lock"
#define LOG_MAX 65536

#define TARGET "SceAvCapture"
#define VENC_LIB "libSceVenc.sprx"
#define VENC_SYM "sceVencStartEncode"
#define MAIN_MOD "eboot.bin"

#define PID_MAX_SCAN 99999
#define MAX_SLOTS 32
#define GADGET_LEN 3

typedef struct notify_request {
    char useless1[45];
    char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

static struct {
    int pid;
    intptr_t base;
    intptr_t real;
    intptr_t gadget;
    intptr_t slots[MAX_SLOTS];
    int nslots;
} g;

static int g_enabled = 1;
static int g_verbose;
static int g_interval_s = 20;
static int g_rescan_s = 300;
static int g_check;
static int g_lock_held;
static int g_slot_total_prev;
static char g_target[64] = TARGET;
static int g_state = -1; /* -1 unknown, 0 stock, 1 patched */
static time_t g_last_notify;
static time_t g_last_scan;

static void notify(const char *msg) {
    notify_request_t req;
    time_t now = time(NULL);

    /* Rate limit so a flapping state cannot spam the screen. */
    if (now - g_last_notify < 5) {
        return;
    }
    g_last_notify = now;
    memset(&req, 0, sizeof(req));
    strncpy(req.message, msg, sizeof(req.message) - 1);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

/* Firmware versions are BCD: 0x13600007 is 13.60. */
static void fw_string(uint32_t raw, char *out, size_t len) {
    unsigned major = (((raw >> 28) & 0xF) * 10) + ((raw >> 24) & 0xF);
    unsigned minor = (((raw >> 20) & 0xF) * 10) + ((raw >> 16) & 0xF);

    snprintf(out, len, "%u.%02u", major, minor);
}

static void logmsg(const char *fmt, ...) {
    char body[448];
    char head[32];
    struct tm tm;
    struct timeval tv;
    struct stat st;
    va_list ap;
    int fd, n;

    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);
    n = snprintf(head, sizeof(head), "%04d-%02d-%02d %02d:%02d:%02d ",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                 tm.tm_min, tm.tm_sec);
    if (stat(LOG, &st) == 0 && st.st_size > LOG_MAX) {
        rename(LOG, LOG ".old");
    }
    fd = open(LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        return;
    }
    if (n > 0) {
        write(fd, head, n);
    }
    write(fd, body, strlen(body));
    write(fd, "\n", 1);
    close(fd);
}

/* ------------------------------------------------------------------ */
/* processes                                                           */

static int proc_info(int pid, struct kinfo_proc *out) {
    unsigned char raw[1400];
    int mib[4] = {CTL_KERN, KERN_PROC, 1, pid};
    size_t len = sizeof(raw);

    memset(raw, 0, sizeof(raw));
    if (sysctl(mib, 4, raw, &len, NULL, 0) != 0 || len < sizeof(*out)) {
        return -1;
    }
    memcpy(out, raw, sizeof(*out));
    if (out->ki_pid != pid) {
        return -1;
    }
    return 0;
}

static int find_pid(const char *name) {
    struct kinfo_proc kp;
    int pid;

    for (pid = 1; pid <= PID_MAX_SCAN; pid++) {
        if (proc_info(pid, &kp) != 0) {
            continue;
        }
        if (strncmp(kp.ki_comm, name, COMMLEN) == 0) {
            return pid;
        }
    }
    logmsg("no process named %s up to pid %d", name, PID_MAX_SCAN);
    return -1;
}

/* ------------------------------------------------------------------ */
/* module layout                                                       */

/* The ELF header is not mapped at the module base on this platform, so the
 * code region is anchored on the module entry point instead. */
#define GADGET_WINDOW 0x80000
#define DATA_SCAN 0x800000

static intptr_t find_gadget(int pid, intptr_t base, intptr_t entry) {
    intptr_t lo = entry - GADGET_WINDOW / 2;
    intptr_t hi = entry + GADGET_WINDOW / 2;

    if (lo < base) {
        lo = base;
    }
    for (intptr_t addr = lo; addr + GADGET_LEN <= hi; addr += 0x1000) {
        unsigned char page[4096];
        size_t want = sizeof(page);

        if (addr + (intptr_t)want > hi) {
            want = (size_t)(hi - addr);
        }
        if (kernel_proc_copyout(pid, addr, page, want) != 0) {
            continue;
        }
        for (size_t i = 0; i + GADGET_LEN <= want; i++) {
            if (page[i] == 0x31 && page[i + 1] == 0xc0 && page[i + 2] == 0xc3) {
                return addr + (intptr_t)i;
            }
        }
    }
    return 0;
}

/* Counts every import slot holding want, filling up to max. */
static int find_slots(int pid, intptr_t base, intptr_t end, intptr_t want,
                      intptr_t *out, int max) {
    int found = 0;

    for (intptr_t addr = base; addr + 8 <= end; addr += 0x1000) {
        unsigned char page[4096];
        size_t len = (size_t)(end - addr);

        if (len > sizeof(page)) {
            len = sizeof(page);
        }
        if (kernel_proc_copyout(pid, addr, page, len) != 0) {
            continue;
        }
        for (size_t i = 0; i + 8 <= len; i += 8) {
            intptr_t v;

            memcpy(&v, page + i, 8);
            if (v != want) {
                continue;
            }
            if (found < max) {
                out[found] = addr + (intptr_t)i;
            }
            found++;
        }
    }
    return found;
}

/* ------------------------------------------------------------------ */
/* state file                                                          */

static int state_load(intptr_t *gadget, intptr_t *slots, intptr_t *slot_gadgets,
                      int max);

static int state_save(void) {
    intptr_t old_gadget = 0;
    intptr_t old_slots[MAX_SLOTS];
    intptr_t merged[MAX_SLOTS];
    int n_old, total = 0;
    char tmp[64];
    FILE *fp;

    for (int i = 0; i < g.nslots && total < MAX_SLOTS; i++) {
        merged[total++] = g.slots[i];
    }
    /* Keep slots a previous run redirected: a rescan only sees the ones that
     * still hold the real function, so without this the state file would lose
     * everything already patched. */
    n_old = state_load(&old_gadget, old_slots, NULL, MAX_SLOTS);
    for (int i = 0; i < n_old && total < MAX_SLOTS; i++) {
        int seen = 0;
        intptr_t cur = 0;

        for (int k = 0; k < total; k++) {
            if (merged[k] == old_slots[i]) {
                seen = 1;
                break;
            }
        }
        if (seen) {
            continue;
        }
        if (kernel_proc_copyout(g.pid, old_slots[i], &cur, 8) != 0) {
            continue;
        }
        if (cur != old_gadget) {
            continue;
        }
        merged[total++] = old_slots[i];
    }

    if (g_slot_total_prev > 0 && total > g_slot_total_prev) {
        logmsg("slot count grew %d -> %d", g_slot_total_prev, total);
    }
    g_slot_total_prev = total;

    snprintf(tmp, sizeof(tmp), STATE ".tmp");
    fp = fopen(tmp, "w");
    if (fp == NULL) {
        return -1;
    }
    fprintf(fp, "gadget=%#lx\n", (unsigned long)g.gadget);
    fprintf(fp, "base=%#lx\n", (unsigned long)g.base);
    fprintf(fp, "pid=%d\n", g.pid);
    for (int i = 0; i < total; i++) {
        fprintf(fp, "slot=%#lx %#lx\n", (unsigned long)merged[i],
                (unsigned long)g.gadget);
    }
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    if (rename(tmp, STATE) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int state_load(intptr_t *gadget, intptr_t *slots, intptr_t *slot_gadgets,
                      int max) {
    char line[128];
    FILE *fp = fopen(STATE, "r");
    int n = 0;

    if (fp == NULL) {
        return -1;
    }
    *gadget = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strncmp(line, "gadget=", 7) == 0) {
            *gadget = (intptr_t)strtoul(line + 7, NULL, 16);
        } else if (strncmp(line, "slot=", 5) == 0 && n < max) {
            char *end = NULL;
            intptr_t addr = (intptr_t)strtoul(line + 5, &end, 16);
            intptr_t gd = 0;

            if (end != NULL) {
                gd = (intptr_t)strtoul(end, NULL, 16);
            }
            slots[n] = addr;
            if (slot_gadgets != NULL) {
                slot_gadgets[n] = gd != 0 ? gd : *gadget;
            }
            n++;
        }
    }
    fclose(fp);
    return *gadget == 0 ? -1 : n;
}

/* ------------------------------------------------------------------ */
/* patch and revert                                                    */

static int resolve(int pid) {
    uint32_t lib_handle = 0, main_handle = 0;
    intptr_t entry;
    int n;

    if (kernel_dynlib_handle(pid, VENC_LIB, &lib_handle) != 0) {
        if (g_verbose || g_check) {
            logmsg("%s not loaded in pid %d", VENC_LIB, pid);
        }
        return -1;
    }
    g.real = kernel_dynlib_dlsym(pid, lib_handle, VENC_SYM);
    if (g.real == 0) {
        logmsg("cannot resolve %s", VENC_SYM);
        return -1;
    }
    if (kernel_dynlib_handle(pid, MAIN_MOD, &main_handle) != 0) {
        logmsg("cannot resolve %s in pid %d", MAIN_MOD, pid);
        return -1;
    }
    g.base = kernel_dynlib_mapbase_addr(pid, main_handle);
    if (g.base == 0) {
        logmsg("no base address for %s", MAIN_MOD);
        return -1;
    }
    entry = kernel_dynlib_entry_addr(pid, main_handle);
    if (entry == 0) {
        logmsg("no entry point for %s", MAIN_MOD);
        return -1;
    }
    g.gadget = find_gadget(pid, g.base, entry);
    if (g.gadget == 0) {
        logmsg("no ret0 gadget near entry %#lx", (unsigned long)entry);
        return -1;
    }
    n = find_slots(pid, g.base + 0x1000, g.base + DATA_SCAN, g.real, g.slots,
                   MAX_SLOTS);
    if (n > MAX_SLOTS) {
        logmsg("%d import slots found, tracking the first %d", n, MAX_SLOTS);
        n = MAX_SLOTS;
    }
    g.pid = pid;
    g.nslots = n;
    if (g_verbose) {
        logmsg("pid=%d base=%#lx gadget=%#lx slots=%d", pid,
               (unsigned long)g.base, (unsigned long)g.gadget, n);
    }
    return 0;
}

/* Verifies that no slot still holds want. */
static int no_slot_holds(int pid, intptr_t want) {
    intptr_t tmp[MAX_SLOTS + 1];

    return find_slots(pid, g.base + 0x1000, g.base + DATA_SCAN, want, tmp,
                      MAX_SLOTS) == 0;
}

/* 0 verified patched, 1 nothing to patch yet, -1 error, -2 not verified. */
static int apply(int pid) {
    int written = 0, failed = 0;

    if (resolve(pid) != 0) {
        return -1;
    }
    if (g.nslots == 0) {
        /* Nothing holds the real function: either the daemon has not filled
         * its import table yet, or a previous run already redirected it. */
        intptr_t saved_gadget = 0;
        intptr_t saved_slots[MAX_SLOTS];
        int saved = state_load(&saved_gadget, saved_slots, NULL, MAX_SLOTS);

        /* Adopt slots that already point at our gadget: a previous run may have
         * patched them and the state file may be gone (fresh directory, wiped
         * /data, upgrade). Prefer the recorded gadget, fall back to the one we
         * just computed. */
        for (int pass = 0; pass < 2; pass++) {
            intptr_t want = pass == 0 ? saved_gadget : g.gadget;
            int n;

            if (want == 0) {
                continue;
            }
            n = find_slots(pid, g.base + 0x1000, g.base + DATA_SCAN, want,
                           g.slots, MAX_SLOTS);
            if (n == 0) {
                continue;
            }
            g.gadget = want;
            g.nslots = n > MAX_SLOTS ? MAX_SLOTS : n;
            if (state_save() == 0) {
                logmsg("already redirected in %d slot(s)", g.nslots);
                return 0;
            }
        }
        if (g_verbose) {
            logmsg("no import slot yet (state=%d)", saved);
        }
        return 1;
    }
    if (state_save() != 0) {
        logmsg("cannot write %s, refusing to patch", STATE);
        return -1;
    }
    for (int i = 0; i < g.nslots; i++) {
        intptr_t cur = 0;

        if (kernel_proc_copyin(pid, &g.gadget, g.slots[i], 8) != 0) {
            failed++;
            continue;
        }
        if (kernel_proc_copyout(pid, g.slots[i], &cur, 8) != 0 ||
            cur != g.gadget) {
            failed++;
            continue;
        }
        written++;
    }
    if (!no_slot_holds(pid, g.real)) {
        logmsg("patch not verified (%d written, %d failed)", written, failed);
        return -2;
    }
    logmsg("%s redirected in %d slot(s), %d failed", VENC_SYM, written, failed);
    return 0;
}

/* 0 verified stock, 1 nothing to do, -1 error, -2 not verified. */
static int revert(int pid) {
    intptr_t gadget = 0;
    intptr_t slots[MAX_SLOTS];
    intptr_t slot_gadgets[MAX_SLOTS];
    uint32_t lib_handle = 0;
    intptr_t real;
    int n, restored = 0, failed = 0;

    n = state_load(&gadget, slots, slot_gadgets, MAX_SLOTS);
    if (n < 0) {
        if (g_verbose) {
            logmsg("no %s to revert from", STATE);
        }
        return 1;
    }
    if (kernel_dynlib_handle(pid, VENC_LIB, &lib_handle) != 0) {
        return -1;
    }
    real = kernel_dynlib_dlsym(pid, lib_handle, VENC_SYM);
    if (real == 0) {
        return -1;
    }
    for (int i = 0; i < n; i++) {
        intptr_t cur = 0;

        if (kernel_proc_copyout(pid, slots[i], &cur, 8) != 0) {
            failed++;
            continue;
        }
        if (cur == real) {
            restored++;
            continue;
        }
        if (cur != slot_gadgets[i]) {
            logmsg("slot %#lx holds %#lx, not ours", (unsigned long)slots[i],
                   (unsigned long)cur);
            failed++;
            continue;
        }
        if (kernel_proc_copyin(pid, &real, slots[i], 8) != 0) {
            failed++;
            continue;
        }
        cur = 0;
        kernel_proc_copyout(pid, slots[i], &cur, 8);
        if (cur == real) {
            restored++;
        } else {
            failed++;
        }
    }
    if (failed != 0) {
        logmsg("revert incomplete (%d restored, %d failed)", restored, failed);
        return -2;
    }
    unlink(STATE);
    g_slot_total_prev = 0;
    logmsg("%s restored in %d slot(s)", VENC_SYM, restored);
    return 0;
}

/* ------------------------------------------------------------------ */
/* configuration and lock                                              */

static char *trim(char *s) {
    char *end;

    while (*s != '\0' && isspace((unsigned char)*s)) {
        s++;
    }
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return s;
}

static void write_default_config(void) {
    FILE *fp = fopen(CFG, "w");

    if (fp == NULL) {
        return;
    }
    fprintf(fp, "# hush\n");
    fprintf(fp, "# enabled=1 stops the recent-gameplay recording.\n");
    fprintf(fp, "# enabled=0 restores the stock behaviour and exits.\n");
    fprintf(fp, "enabled=1\n");
    fprintf(fp, "# mode=check reports what would be patched and changes nothing\n");
    fprintf(fp, "mode=apply\n");
    fprintf(fp, "verbose=0\n");
    fprintf(fp, "interval_s=%d\n", g_interval_s);
    fprintf(fp, "rescan_s=%d\n", g_rescan_s);
    fclose(fp);
}

static void load_config(void) {
    char line[512];
    FILE *fp = fopen(CFG, "r");

    if (fp == NULL) {
        write_default_config();
        return;
    }
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *eq, *key, *val;
        int value;

        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }
        eq = strchr(line, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        key = trim(line);
        val = trim(eq + 1);
        if (strcmp(key, "enabled") == 0) {
            value = atoi(val);
            if (strcmp(val, "0") == 0 || strcmp(val, "1") == 0) {
                g_enabled = value;
            } else {
                logmsg("unparsable enabled=%s, keeping %d", val, g_enabled);
            }
        } else if (strcmp(key, "verbose") == 0) {
            g_verbose = atoi(val) != 0;
        } else if (strcmp(key, "interval_s") == 0) {
            value = atoi(val);
            if (value >= 5 && value <= 3600) {
                g_interval_s = value;
            }
        } else if (strcmp(key, "rescan_s") == 0) {
            value = atoi(val);
            if (value >= 30 && value <= 86400) {
                g_rescan_s = value;
            }
        } else if (strcmp(key, "mode") == 0) {
            if (strcmp(val, "check") == 0) {
                g_check = 1;
            } else if (strcmp(val, "apply") == 0) {
                g_check = 0;
            } else {
                logmsg("unknown mode '%s', staying in %s mode", val,
                       g_check ? "check" : "apply");
            }
        } else if (strcmp(key, "target") == 0 && val[0] != '\0') {
            snprintf(g_target, sizeof(g_target), "%s", val);
        } else {
            logmsg("unknown config key '%s'", key);
        }
    }
    fclose(fp);
}

/* Takes the lock. Returns 0 when this process owns it. */
static int lock_acquire(void) {
    struct kinfo_proc kp;
    char buf[32] = {0};
    int fd, n, pid = 0;

    fd = open(LOCK, O_RDWR | O_CREAT | O_EXCL, 0644);
    if (fd < 0 && errno == EEXIST) {
        fd = open(LOCK, O_RDONLY);
        if (fd >= 0) {
            n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            pid = n > 0 ? (int)strtol(buf, NULL, 10) : 0;
        }
        /* A lock from another live payload instance blocks us. */
        if (pid > 1 && pid != getpid() && proc_info(pid, &kp) == 0 &&
            kp.ki_stat != 5 /* SZOMB */ &&
            strncmp(kp.ki_comm, "payload.elf", COMMLEN) == 0) {
            return 1;
        }
        unlink(LOCK);
        fd = open(LOCK, O_RDWR | O_CREAT | O_EXCL, 0644);
    }
    if (fd < 0) {
        return -1;
    }
    n = snprintf(buf, sizeof(buf), "%d\n", getpid());
    if (write(fd, buf, n) != n) {
        close(fd);
        unlink(LOCK);
        return -1;
    }
    fsync(fd);
    close(fd);
    g_lock_held = 1;
    return 0;
}

static void lock_release(void) {
    if (g_lock_held) {
        g_lock_held = 0;
        unlink(LOCK);
    }
}

/* ------------------------------------------------------------------ */

int main(void) {
    int pid, rc;

    if (mkdir(DIR, 0755) != 0 && errno != EEXIST) {
        return 1;
    }
    load_config();

    /* Check mode only reads, so it runs alongside a resident daemon. */
    if (!g_check) {
        rc = lock_acquire();
        if (rc != 0) {
            return rc == 1 ? 0 : 1;
        }
    }

    {
        char fw[16];
        uint32_t raw = kernel_get_fw_version();

        fw_string(raw, fw, sizeof(fw));
        logmsg("hush started; firmware=%s (%#x) enabled=%d mode=%s interval=%ds "
               "rescan=%ds",
               fw, raw, g_enabled, g_check ? "check" : "apply", g_interval_s,
               g_rescan_s);
    }

    for (;;) {
        load_config();
        pid = find_pid(g_target);
        if (pid <= 1) {
            if (g_check && g_enabled && !g_lock_held) {
                logmsg("check: %s is not running", g_target);
                notify("hush: check found no capture process");
                lock_release();
                return 1;
            }
            if (g_state != 2) {
                logmsg("%s not running, waiting", g_target);
                g_state = 2;
            }
            sleep((unsigned)g_interval_s);
            continue;
        }

        if (g_check && g_enabled && !g_lock_held) {
            logmsg("mode=check: reporting once and exiting");
            if (resolve(pid) != 0) {
                logmsg("check: cannot resolve the capture process");
                notify("hush: check failed, see log");
                lock_release();
                return 1;
            }
            {
                intptr_t addrs[MAX_SLOTS];
                int n = 0, extra, base_n;

                for (int i = 0; i < g.nslots && n < MAX_SLOTS; i++) {
                    addrs[n++] = g.slots[i];
                }
                base_n = n;
                extra = find_slots(pid, g.base + 0x1000, g.base + DATA_SCAN,
                                   g.gadget, addrs + base_n, MAX_SLOTS - base_n);
                for (int e = 0; e < extra && n < MAX_SLOTS; e++) {
                    intptr_t cand = addrs[base_n + e];
                    int dup = 0;

                    for (int k = 0; k < n; k++) {
                        if (addrs[k] == cand) {
                            dup = 1;
                            break;
                        }
                    }
                    if (!dup) {
                        g.slots[n] = cand;
                        n++;
                    }
                }
                logmsg("check: pid=%d base=%#lx function=%#lx gadget=%#lx "
                       "slots=%d",
                       pid, (unsigned long)g.base, (unsigned long)g.real,
                       (unsigned long)g.gadget, n);
                for (int i = 0; i < n; i++) {
                    intptr_t cur = 0;

                    if (kernel_proc_copyout(pid, g.slots[i], &cur, 8) != 0) {
                        logmsg("check: slot[%d]=%#lx unreadable (errno=%d)", i,
                               (unsigned long)g.slots[i], errno);
                        continue;
                    }
                    logmsg("check: slot[%d]=%#lx points at %#lx (%s)", i,
                           (unsigned long)g.slots[i], (unsigned long)cur,
                           cur == g.real     ? "original"
                           : cur == g.gadget ? "redirected"
                                             : "unknown");
                }
            }
            notify("hush: check written to log.txt");
            lock_release();
            return 0;
        }

        if (!g_enabled) {
            rc = revert(pid);
            if (rc == 0 || rc == 1) {
                notify("hush: recording back on");
                logmsg("disabled in config, exiting");
                lock_release();
                return 0;
            }
            logmsg("revert not verified (rc=%d), retrying", rc);
            sleep((unsigned)g_interval_s);
            continue;
        }

        if (pid != g.pid || g.nslots == 0 ||
            time(NULL) - g_last_scan >= g_rescan_s) {
            rc = apply(pid);
            g_last_scan = time(NULL);
        } else {
            /* Cheap path: verify and repair the slots we already know. */
            int bad = 0;

            for (int i = 0; i < g.nslots; i++) {
                intptr_t cur = 0;

                if (kernel_proc_copyout(pid, g.slots[i], &cur, 8) != 0 ||
                    cur != g.gadget) {
                    bad++;
                }
            }
            rc = bad == 0 ? 0 : apply(pid);
        }

        if (rc == 0) {
            if (g_state != 1) {
                notify("hush: active");
                g_state = 1;
            }
        } else if (rc == 1) {
            /* Nothing to patch yet; the daemon has not filled its import
             * table. Keep trying without claiming a failure. */
        } else {
            if (g_state != 2) {
                logmsg("patch failed, will retry");
                notify("hush: failed, still retrying");
                g_state = 2;
            }
        }
        sleep((unsigned)g_interval_s);
    }
}
