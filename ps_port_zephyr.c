/* SPDX-License-Identifier: LicenseRef-Pinscope-Device-1.0
 * Copyright (c) 2026 Pinscope. See LICENSE: for use with the Pinscope service only. */
/*
 * Pinscope platform layer for Zephyr: BSD sockets through the POSIX API,
 * settings for the few values kept across restarts, MCUboot for updates.
 * Internal, not in the public component. Boards so far: nrf7002dk (WiFi),
 * nucleo_f429zi (Ethernet).
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/linker/linker-defs.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/fatal.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/version.h>
#if __has_include(<app_version.h>)
#include <app_version.h> /* from the application's VERSION file */
#define PS_APP_VERSION APP_VERSION_STRING
#else
#define PS_APP_VERSION ""
#endif
#ifdef CONFIG_PINSCOPE_OTA
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>
#endif
#ifdef CONFIG_WIFI
#include <zephyr/net/wifi_mgmt.h>
#endif
#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS) && defined(CONFIG_COMMON_LIBC_MALLOC)
#include <zephyr/sys/sys_heap.h>
#define PS_HEAP_STATS 1
int malloc_runtime_stats_get(struct sys_memory_stats *stats);
#endif

#include "pinscope.h"
#include "ps_port.h"

#ifdef CONFIG_PINSCOPE_LOG_BACKEND
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(pinscope, LOG_LEVEL_INF);
#endif

static K_MUTEX_DEFINE(s_lock);
static K_THREAD_STACK_DEFINE(s_stack, CONFIG_PINSCOPE_STACK_SIZE);
static struct k_thread s_thread;
static bool s_capture;
static volatile bool s_net_up;
static char s_reset[12] = "other";

/* ---- crash: kept in RAM that a reset does not clear ---- */

#define CRASH_MAGIC 0x50534352u
#define CRASH_FRAMES 8
static __noinit struct {
    uint32_t magic;
    uint32_t reason;
    uint32_t count;           /* addresses in bt[] */
    uint32_t bt[CRASH_FRAMES]; /* pc, lr, then code addresses found on the stack */
    char     thread[16];
    char     build[17];       /* build ID of the image that crashed */
} s_crash;

/* ---- build ID: the GNU note the linker put in flash (build_id.ld) ---- */

extern const uint8_t __ps_build_id_start[], __ps_build_id_end[];

/* The first `chars` hex digits of the build ID, "" when the image has none.
 * The note is namesz, descsz, type, "GNU\0", then the ID itself. */
static void build_id_hex(char *out, size_t chars)
{
    const uint8_t *note = __ps_build_id_start;
    size_t have = (size_t)(__ps_build_id_end - __ps_build_id_start);
    out[0] = '\0';
    if (have < 16 + 8) {
        return;
    }
    uint32_t namesz, descsz;
    memcpy(&namesz, note, 4);
    memcpy(&descsz, note + 4, 4);
    const uint8_t *id = note + 12 + ((namesz + 3) & ~3u);
    for (size_t i = 0; i < descsz && 2 * i + 2 <= chars; i++) {
        snprintf(out + 2 * i, 3, "%02x", id[i]);
    }
}

static inline bool in_code(uint32_t address)
{
    return address >= (uint32_t)(uintptr_t)__text_region_start && address < (uint32_t)(uintptr_t)__text_region_end;
}

#if defined(CONFIG_PINSCOPE_CRASH_CAPTURE) && defined(CONFIG_XTENSA) && defined(CONFIG_XTENSA_ENABLE_BACKTRACE)
/* Zephyr's Xtensa unwinder (arch/xtensa/core/xtensa_backtrace.c); its header
 * is private to the architecture. */
struct xtensa_backtrace_frame_t {
    uint32_t pc;
    uint32_t sp;
    uint32_t next_pc;
    uint32_t mask;
    int cause;
};
void xtensa_backtrace_get_start(uint32_t *pc, uint32_t *sp, uint32_t *next_pc, int *interrupted_stack);
bool xtensa_backtrace_get_next_frame(struct xtensa_backtrace_frame_t *frame);
#endif

#ifdef CONFIG_PINSCOPE_CRASH_CAPTURE
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
    s_crash.magic = CRASH_MAGIC;
    s_crash.reason = reason;
    s_crash.count = 0;
#ifdef CONFIG_ARM
    if (esf) {
        /* Thumb addresses have bit 0 set; the ELF symbols do not. */
        s_crash.bt[s_crash.count++] = esf->basic.pc & ~1u;
        s_crash.bt[s_crash.count++] = esf->basic.lr & ~1u;
        /* Then every word on the stack above the exception frame that points
         * into code: an approximate call chain, as on RISC-V ESP32s. */
        const uint32_t *sp = (const uint32_t *)esf + 8;
        uintptr_t top = (uintptr_t)sp + 256;
#ifdef CONFIG_THREAD_STACK_INFO
        const struct k_thread *t = k_current_get();
        top = MIN(top, t->stack_info.start + t->stack_info.size);
#endif
        for (; (uintptr_t)sp + 4 <= top && s_crash.count < CRASH_FRAMES; sp++) {
            if ((*sp & 1u) && in_code(*sp & ~1u)) {
                s_crash.bt[s_crash.count++] = *sp & ~1u;
            }
        }
    }
#elif defined(CONFIG_RISCV)
    if (esf) {
        s_crash.bt[s_crash.count++] = (uint32_t)esf->mepc;
        if (in_code((uint32_t)esf->ra)) {
            s_crash.bt[s_crash.count++] = (uint32_t)esf->ra;
        }
        /* The frame was pushed onto the stack that faulted: the words above
         * it that point into code give an approximate call chain. */
        const uint32_t *sp = (const uint32_t *)(esf + 1);
        uintptr_t top = (uintptr_t)sp + 256;
#ifdef CONFIG_THREAD_STACK_INFO
        const struct k_thread *t = k_current_get();
        if ((uintptr_t)sp >= t->stack_info.start && (uintptr_t)sp < t->stack_info.start + t->stack_info.size) {
            top = MIN(top, t->stack_info.start + t->stack_info.size);
        }
#endif
        for (; (uintptr_t)sp + 4 <= top && s_crash.count < CRASH_FRAMES; sp++) {
            if (!(*sp & 1u) && in_code(*sp)) {
                s_crash.bt[s_crash.count++] = *sp;
            }
        }
    }
#elif defined(CONFIG_XTENSA) && defined(CONFIG_XTENSA_ENABLE_BACKTRACE)
    /* Xtensa (ESP32, ESP32-S3): the windowed ABI unwinds exactly, with
     * Zephyr's own unwinder. Return addresses carry the window increment in
     * their top two bits and point after the call: map them back, minus 3. */
    if (esf) {
        struct xtensa_backtrace_frame_t f = { .mask = 0x40000000, .cause = 0 };
        xtensa_backtrace_get_start(&f.pc, &f.sp, &f.next_pc, (int *)esf);
        f.mask = f.pc & 0xc0000000;
        s_crash.bt[s_crash.count++] = f.pc;
        while (s_crash.count < CRASH_FRAMES && f.next_pc != 0 && xtensa_backtrace_get_next_frame(&f)) {
            uint32_t pc = (f.pc & 0x80000000) ? ((f.pc & 0x3fffffff) | f.mask) : f.pc;
            if (!in_code(pc - 3)) {
                break;
            }
            s_crash.bt[s_crash.count++] = pc - 3;
        }
    }
#else
    ARG_UNUSED(esf);
#endif
    const char *name = k_thread_name_get(k_current_get());
    snprintf(s_crash.thread, sizeof(s_crash.thread), "%s", name ? name : "?");
    build_id_hex(s_crash.build, 16);
    sys_reboot(SYS_REBOOT_COLD);
}
#endif

/* ---- network: connecting is tried only with an IPv4 address ---- */

static struct net_mgmt_event_callback s_net_cb;

static void net_event(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
    ARG_UNUSED(cb);
    ARG_UNUSED(iface);
    if (event == NET_EVENT_IPV4_ADDR_ADD) {
        s_net_up = true;
    } else if (event == NET_EVENT_IPV4_ADDR_DEL) {
        s_net_up = false;
    }
}

/* Set just before a restart Pinscope asks for (an update, the dashboard): some
 * chips, the RP2040 among them, report a software reset as the reset pin. */
#define RESTART_MAGIC 0x50535257u
static __noinit uint32_t s_restart;

static void record_reset_cause(void)
{
    uint32_t cause = 0;
    bool asked = s_restart == RESTART_MAGIC;
    s_restart = 0;
    if (s_crash.magic == CRASH_MAGIC) {
        strcpy(s_reset, "panic");
    } else if (hwinfo_get_reset_cause(&cause) == 0) {
        strcpy(s_reset, (cause & RESET_WATCHDOG)            ? "wdt"
                        : (cause & RESET_BROWNOUT)          ? "brownout"
                        : (cause & RESET_POR)               ? "poweron"
                        : (asked || (cause & RESET_SOFTWARE)) ? "sw"
                        : (cause & RESET_PIN)               ? "ext"
                                                            : "other");
    } else if (asked) {
        strcpy(s_reset, "sw");
    }
    hwinfo_clear_reset_cause();
}

int ps_port_init(void)
{
    static bool inited;
    if (inited) {
        return 0;
    }
    inited = true;
    record_reset_cause();
    settings_subsys_init();
    net_mgmt_init_event_callback(&s_net_cb, net_event, NET_EVENT_IPV4_ADDR_ADD | NET_EVENT_IPV4_ADDR_DEL);
    net_mgmt_add_event_callback(&s_net_cb);
    struct net_if *iface = net_if_get_default();
    s_net_up = iface && net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED) != NULL;
    return 0;
}

uint32_t ps_port_millis(void) { return k_uptime_get_32(); }
void     ps_port_sleep(uint32_t ms) { k_msleep(ms); }

/* Session keys come from this: the cryptographic generator, never sys_rand. */
void ps_port_random(void *buf, size_t len)
{
    if (sys_csrand_get(buf, len) != 0) {
        k_panic();
    }
}

void ps_port_lock(void) { k_mutex_lock(&s_lock, K_FOREVER); }
void ps_port_unlock(void) { k_mutex_unlock(&s_lock); }

static void (*s_task_fn)(void *);

static void task_entry(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);
    s_task_fn(NULL);
}

/* The stack is set at build time (CONFIG_PINSCOPE_STACK_SIZE); the core's
 * wish for one is only checked against it. */
int ps_port_task_start(void (*fn)(void *), uint32_t stack, uint8_t priority)
{
    ARG_UNUSED(priority);
    if (stack > CONFIG_PINSCOPE_STACK_SIZE) {
        return -1;
    }
    s_task_fn = fn;
    k_tid_t tid = k_thread_create(&s_thread, s_stack, K_THREAD_STACK_SIZEOF(s_stack), task_entry,
                                  NULL, NULL, NULL, K_PRIO_PREEMPT(CONFIG_PINSCOPE_THREAD_PRIORITY), 0, K_NO_WAIT);
    k_thread_name_set(tid, "pinscope");
    return 0;
}

void ps_port_logf(int level, const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
#ifdef CONFIG_PINSCOPE_LOG_BACKEND
    /* Through Zephyr's log, which the Pinscope backend forwards. */
    if (level == PS_LOG_ERROR) {
        LOG_ERR("%s", line);
    } else if (level == PS_LOG_WARN) {
        LOG_WRN("%s", line);
    } else {
        LOG_INF("%s", line);
    }
#else
    printk("%c pinscope: %s\n", "EWI"[level], line);
    if (s_capture) {
        char out[176];
        int n = snprintf(out, sizeof(out), "%c pinscope: %s\n", "EWI"[level], line);
        pinscope_log(out, MIN((size_t)n, sizeof(out) - 1));
    }
#endif
}

bool ps_zephyr_log_capture(void) { return s_capture; }

void ps_port_device_id(char *buf, size_t len)
{
    uint8_t id[8];
    ssize_t n = hwinfo_get_device_id(id, sizeof(id));
    if (n <= 0) {
        /* No unique id from the hardware (no hwinfo driver): make one once and
         * keep it, so that two such boards stay two devices. */
        if (ps_port_kv_get("uid", buf, len) == 0 && buf[0]) {
            return;
        }
        uint8_t r[6];
        ps_port_random(r, sizeof(r));
        snprintf(buf, len, "%s-%02x%02x%02x%02x%02x%02x", CONFIG_BOARD, r[0], r[1], r[2], r[3], r[4], r[5]);
        ps_port_kv_set("uid", buf);
        return;
    }
    size_t at = 0;
    for (ssize_t i = 0; i < n && at + 3 <= len; i++) {
        at += (size_t)snprintf(buf + at, len - at, "%02x", id[i]);
    }
}

/* A new value on every start: the server counts a changed one as a restart. */
static uint32_t boot_id(void)
{
    static uint32_t id;
    if (!id) {
        ps_port_random(&id, sizeof(id));
        id |= 1;
    }
    return id;
}

/* The application's VERSION file if it has one, else the version MCUboot's
 * header gives the running image (imgtool --version), as the dashboard shows it. */
static const char *app_version(void)
{
    static char v[32];
    if (PS_APP_VERSION[0] || v[0]) {
        return PS_APP_VERSION[0] ? PS_APP_VERSION : v;
    }
#ifdef CONFIG_PINSCOPE_OTA
    struct mcuboot_img_header h;
    if (boot_read_bank_header(PARTITION_ID(slot0_partition), &h, sizeof(h)) == 0 && h.mcuboot_version == 1) {
        const struct mcuboot_img_sem_ver *sv = &h.h.v1.sem_ver;
        if (sv->major || sv->minor || sv->revision || sv->build_num) {
            int n = snprintf(v, sizeof(v), "%u.%u.%u", sv->major, sv->minor, sv->revision);
            if (sv->build_num && n > 0 && n < (int)sizeof(v)) {
                snprintf(v + n, sizeof(v) - n, "+%u", (unsigned)sv->build_num);
            }
        }
    }
#endif
    return v;
}

void ps_port_info(char *buf, size_t len)
{
    char build[17];
    build_id_hex(build, 16);
    snprintf(buf, len, "chip=%s;board=%s;fw=%.31s;zephyr=%s;reset=%s;elf=%s;boot=%08x", CONFIG_SOC, CONFIG_BOARD,
             app_version(), KERNEL_VERSION_STRING, s_reset, build, (unsigned)boot_id());
}

/* ---- small persistent store: the settings subsystem, under "pinscope/" ---- */

struct kv_read {
    char  *buf;
    size_t len;
    bool   found;
};

static int kv_loaded(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg, void *param)
{
    struct kv_read *out = param;
    if (key && key[0]) {
        return 0; /* a longer name under the same prefix */
    }
    if (len >= out->len) {
        return 0;
    }
    ssize_t n = read_cb(cb_arg, out->buf, len);
    if (n >= 0) {
        out->buf[n] = '\0';
        out->found = true;
    }
    return 0;
}

int ps_port_kv_get(const char *key, char *buf, size_t len)
{
    char name[48];
    struct kv_read out = { buf, len, false };
    snprintf(name, sizeof(name), "pinscope/%s", key);
    settings_load_subtree_direct(name, kv_loaded, &out);
    return out.found ? 0 : -1;
}

int ps_port_kv_set(const char *key, const char *value)
{
    char name[48];
    snprintf(name, sizeof(name), "pinscope/%s", key);
    return settings_save_one(name, value, strlen(value)) == 0 ? 0 : -1;
}

void ps_port_log_capture(bool on) { s_capture = on; }

int ps_port_crash_read(char *buf, size_t len)
{
    if (s_crash.magic != CRASH_MAGIC || s_crash.count > CRASH_FRAMES) {
        return 0;
    }
    s_crash.thread[sizeof(s_crash.thread) - 1] = '\0';
    s_crash.build[sizeof(s_crash.build) - 1] = '\0';
    int n = snprintf(buf, len, "crash\nreason=fatal_%u\ntask=%s\npc=0x%08x\napprox=1\nelf=%s\nbt=",
                     (unsigned)s_crash.reason, s_crash.thread, (unsigned)(s_crash.count ? s_crash.bt[0] : 0),
                     s_crash.build);
    for (uint32_t i = 0; i < s_crash.count && n > 0 && (size_t)n < len; i++) {
        n += snprintf(buf + n, len - (size_t)n, i ? " 0x%08x" : "0x%08x", (unsigned)s_crash.bt[i]);
    }
    if (n > 0 && (size_t)n < len - 1) {
        buf[n++] = '\n';
        buf[n] = '\0';
    }
    return n;
}

void ps_port_crash_clear(void) { s_crash.magic = 0; }

/* ---- updates: MCUboot's second slot, booted on trial ---- */

#ifdef CONFIG_PINSCOPE_OTA
static struct flash_img_context s_img;
static bool s_img_open;

bool ps_port_ota_supported(void) { return true; }

int ps_port_ota_begin(uint32_t size)
{
    if (size > PARTITION_SIZE(slot1_partition)) {
        return -1;
    }
    s_img_open = flash_img_init_id(&s_img, PARTITION_ID(slot1_partition)) == 0;
    return s_img_open ? 0 : -1;
}

int ps_port_ota_write(const void *data, size_t len)
{
    return (s_img_open && flash_img_buffered_write(&s_img, data, len, false) == 0) ? 0 : -1;
}

/* The image is tested once: MCUboot boots it, and goes back unless it is confirmed. */
int ps_port_ota_end(void)
{
    if (!s_img_open || flash_img_buffered_write(&s_img, NULL, 0, true) != 0) {
        return -1;
    }
    s_img_open = false;
    return boot_request_upgrade(BOOT_UPGRADE_TEST) == 0 ? 0 : -1;
}

void ps_port_ota_abort(void) { s_img_open = false; }
void ps_port_restart(void);
static volatile bool s_confirmed;

void ps_port_ota_confirm(void)
{
    if (boot_write_img_confirmed() == 0) {
        s_confirmed = true;
    }
}
/* On trial: MCUboot booted this image as a test after an update, and goes back
 * unless it is confirmed. An image written with a cable is not on trial, even
 * unconfirmed: restarting it would not bring anything back. */
bool ps_port_ota_on_trial(void) { return mcuboot_swap_type() == BOOT_SWAP_TYPE_REVERT; }

/* A new image that hangs (a thread stuck, the agent never reaching the
 * server) must not stay: if it is still unconfirmed a minute after its trial,
 * restart from the timer interrupt, which runs whatever the threads do, and
 * MCUboot goes back to the previous image. Started at boot, before the
 * application, so it covers an image that never starts Pinscope at all. */
static void trial_guard_expired(struct k_timer *t)
{
    ARG_UNUSED(t);
    if (!s_confirmed) {
        ps_port_restart();
    }
}
K_TIMER_DEFINE(s_trial_guard, trial_guard_expired, NULL);

static int trial_guard_init(void)
{
    s_confirmed = !ps_port_ota_on_trial();
    if (!s_confirmed && CONFIG_PINSCOPE_OTA_TRIAL_S > 0) {
        k_timer_start(&s_trial_guard, K_SECONDS(CONFIG_PINSCOPE_OTA_TRIAL_S + 60), K_NO_WAIT);
    }
    return 0;
}
SYS_INIT(trial_guard_init, APPLICATION, 0);
#else
bool ps_port_ota_supported(void) { return false; }
int  ps_port_ota_begin(uint32_t size) { ARG_UNUSED(size); return -1; }
int  ps_port_ota_write(const void *data, size_t len) { ARG_UNUSED(data); ARG_UNUSED(len); return -1; }
int  ps_port_ota_end(void) { return -1; }
void ps_port_ota_abort(void) {}
void ps_port_ota_confirm(void) {}
bool ps_port_ota_on_trial(void) { return false; }
#endif

void ps_port_restart(void)
{
    s_restart = RESTART_MAGIC;
    sys_reboot(SYS_REBOOT_COLD);
}

/* ---- system metrics ---- */

uint16_t ps_port_default_metrics(void)
{
    return PINSCOPE_M_HEAP | PINSCOPE_M_UPTIME | PINSCOPE_M_RSSI | PINSCOPE_M_STACK;
}

void ps_port_sys_metrics(uint16_t mask)
{
    if (mask & PINSCOPE_M_UPTIME) {
        pinscope_metric("uptime_s", (double)(k_uptime_get() / 1000));
    }
#ifdef PS_HEAP_STATS
    struct sys_memory_stats heap;
    if ((mask & PINSCOPE_M_HEAP) && malloc_runtime_stats_get(&heap) == 0) {
        pinscope_metric("heap_free", (double)heap.free_bytes);
        pinscope_metric("heap_min", (double)(heap.free_bytes + heap.allocated_bytes - heap.max_allocated_bytes));
    }
#endif
#ifdef CONFIG_THREAD_STACK_INFO
    size_t unused;
    if ((mask & PINSCOPE_M_STACK) && k_thread_stack_space_get(&s_thread, &unused) == 0) {
        pinscope_metric("stack/pinscope", (double)unused);
    }
#endif
#ifdef CONFIG_WIFI
    struct wifi_iface_status status = { 0 };
    struct net_if *iface = net_if_get_default();
    if ((mask & PINSCOPE_M_RSSI) && iface &&
        net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &status, sizeof(status)) == 0 &&
        status.state >= WIFI_STATE_ASSOCIATED) {
        pinscope_metric("rssi", (double)status.rssi);
    }
#endif
}

/* ---- connections to local servers: plain non-blocking sockets ---- */

int ps_local_open(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        ps_port_logf(PS_LOG_WARN, "no socket for port %u (errno %d)", port, errno);
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    ps_set_nonblock(fd);

    /* The device's own address: the stack delivers it locally, with no
     * loopback interface that could become the default one. */
    struct sockaddr_in addr = { 0 };
    struct in_addr *own = net_if_ipv4_get_global_addr(net_if_get_default(), NET_ADDR_PREFERRED);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = own ? own->s_addr : htonl(0x7f000001);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 && !ps_sock_inprogress()) {
        ps_port_logf(PS_LOG_WARN, "cannot connect to port %u (errno %d)", port, errno);
        close(fd);
        return -1;
    }
    return fd;
}

int ps_local_state(int h)
{
    fd_set w;
    struct timeval tv = { 0, 0 };
    int err = 0;
    socklen_t len = sizeof(err);
    FD_ZERO(&w);
    FD_SET(h, &w);
    if (select(h + 1, NULL, &w, NULL, &tv) <= 0) {
        return 0;
    }
    getsockopt(h, SOL_SOCKET, SO_ERROR, &err, &len);
    return err == 0 ? 1 : -1;
}

int ps_local_read(int h, void *buf, size_t len)
{
    ssize_t n = recv(h, buf, len, 0);
    if (n > 0) {
        return (int)n;
    }
    return (n < 0 && ps_sock_wouldblock()) ? 0 : -1;
}

int ps_local_write(int h, const void *buf, size_t len)
{
    ssize_t n = send(h, buf, len, 0);
    if (n > 0) {
        return (int)n;
    }
    return (n < 0 && ps_sock_wouldblock()) ? 0 : -1;
}

int  ps_local_avail(int h) { ARG_UNUSED(h); return -1; }
void ps_local_want(int h, size_t bytes) { ARG_UNUSED(h); ARG_UNUSED(bytes); }
void ps_local_close(int h) { close(h); }

void ps_local_watch(int h, bool connecting, bool want_read, bool want_write,
                    fd_set *r, fd_set *w, fd_set *e, int *maxfd)
{
    if (connecting) {
        FD_SET(h, w);
        FD_SET(h, e);
    }
    if (want_read) {
        FD_SET(h, r);
    }
    if (want_write) {
        FD_SET(h, w);
    }
    if (h > *maxfd) {
        *maxfd = h;
    }
}

void ps_local_flush(void) {}
bool ps_local_ready(void) { return false; }
void ps_local_poll(const fd_set *readable) { ARG_UNUSED(readable); }

bool ps_port_network_ready(void) { return s_net_up; }

int ps_port_resolve(const char *host, uint8_t *scratch, size_t len, uint32_t *addr)
{
    struct addrinfo hints = { 0 }, *res = NULL;
    ARG_UNUSED(scratch);
    ARG_UNUSED(len);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
        return -1;
    }
    *addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return 0;
}
