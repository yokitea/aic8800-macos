/*
 * aic8800_fwload_ctl.h - Control-plane helpers for aic8800_fwload daemon
 *
 * Pure POSIX/C11 POD state, CLI parser, firmware directory resolution,
 * single-instance advisory flock() locking, and event/telemetry accounting.
 * Safe for both user-space daemon runtime and ASan+UBSan unit tests.
 */

#ifndef AIC8800_FWLOAD_CTL_H
#define AIC8800_FWLOAD_CTL_H

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define AIC_FW_INSTALLED_DIR        "/Library/Application Support/AIC8800/fw/aic8800D80"
#define AIC_FW_REQUIRED_MARKER      "fw_patch_table_8800d80_u02.bin"
#define AIC_FWLOAD_DAEMON_LOCK_PATH "/var/run/aic8800_fwload.lock"
#define AIC_FWLOAD_MAX_WAKE_DELAY_MS 10000U

typedef enum {
    AIC_FWLOAD_MODE_ONCE = 0,
    AIC_FWLOAD_MODE_WATCH = 1,
    AIC_FWLOAD_MODE_DAEMON = 2,
} aic_fwload_mode_t;

typedef enum {
    AIC_FW_SRC_EXPLICIT = 0,
    AIC_FW_SRC_INSTALLED = 1,
    AIC_FW_SRC_DEV = 2,
} aic_fw_src_t;

typedef enum {
    AIC_FWLOAD_TRIGGER_STARTUP = 0,
    AIC_FWLOAD_TRIGGER_USB_MATCH = 1,
    AIC_FWLOAD_TRIGGER_WAKE = 2,
} aic_fwload_trigger_t;

typedef enum {
    AIC_CYCLE_LOADED_OK = 0,
    AIC_CYCLE_ALREADY_READY = 1,
    AIC_CYCLE_NO_DEVICE = 2,
    AIC_CYCLE_FAILED = 3,
} aic_cycle_outcome_t;

typedef struct {
    aic_fwload_mode_t mode;
    bool              enable_5g;
    bool              eject_only;
    bool              dry_run;
    bool              show_help;
    const char       *fw_dir_arg;
    const char       *lock_file_arg;
    uint32_t          post_wake_delay_ms;
    uint32_t          usb_settle_timeout_ms;
} aic_fwload_cfg_t;

typedef struct {
    int  fd;
    char path[512];
    bool held;
} aic_lock_handle_t;

typedef struct {
    uint64_t cycles_ok;
    uint64_t cycles_already_ready;
    uint64_t cycles_no_device;
    uint64_t cycles_failed;
    uint64_t usb_boot_matches;
    uint64_t stage2_matches;
    uint64_t stage2_removals;
    uint64_t wake_events;
    uint64_t sleep_events;
    uint64_t events_ignored_busy;
    uint64_t events_ignored_sleep;
    uint64_t last_stage0_eject_ms;
    uint64_t last_stage1_fw_ms;
    uint64_t last_stage2_enum_ms;
    uint64_t last_total_ms;
    uint64_t max_total_ms;
    bool     cycle_in_progress;
    bool     stage2_online;
    bool     sleep_in_progress;
} aic_fwload_telemetry_t;

static inline const uint16_t *aic_stage0_pids(size_t *out_count) {
    static const uint16_t kStage0Pids[] = {
        0x5721U, 0x5722U, 0x5723U, 0x5724U, 0x5725U, 0x5726U,
        0x5727U, 0x572AU, 0x572CU, 0x572FU, 0x5732U
    };
    if (out_count) {
        *out_count = sizeof(kStage0Pids) / sizeof(kStage0Pids[0]);
    }
    return kStage0Pids;
}

static inline bool aic_is_stage0_pid(uint16_t pid) {
    size_t count = 0;
    const uint16_t *pids = aic_stage0_pids(&count);
    for (size_t i = 0; i < count; i++) {
        if (pids[i] == pid) {
            return true;
        }
    }
    return false;
}

static inline void aic_fwload_cfg_init(aic_fwload_cfg_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->mode = AIC_FWLOAD_MODE_ONCE;
    cfg->enable_5g = true;
    cfg->post_wake_delay_ms = 1200U;
    cfg->usb_settle_timeout_ms = 1500U;
}

static inline int aic_fwload_parse_u32(const char *s, uint32_t max_val, uint32_t *out) {
    if (!s || s[0] == '\0') {
        return -1;
    }
    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v > (unsigned long)max_val) {
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

static inline int aic_fwload_parse_args(int argc,
                                        char *const *argv,
                                        aic_fwload_cfg_t *cfg,
                                        const char **out_bad_arg) {
    if (!cfg) {
        return -1;
    }
    aic_fwload_cfg_init(cfg);
    if (out_bad_arg) {
        *out_bad_arg = NULL;
    }

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--fw-dir") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                if (out_bad_arg) *out_bad_arg = arg;
                return -1;
            }
            cfg->fw_dir_arg = argv[++i];
        } else if (strcmp(arg, "--lock-file") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                if (out_bad_arg) *out_bad_arg = arg;
                return -1;
            }
            cfg->lock_file_arg = argv[++i];
        } else if (strcmp(arg, "--post-wake-delay-ms") == 0) {
            uint32_t val = 0;
            if (i + 1 >= argc ||
                aic_fwload_parse_u32(argv[i + 1], AIC_FWLOAD_MAX_WAKE_DELAY_MS, &val) != 0) {
                if (out_bad_arg) *out_bad_arg = arg;
                return -1;
            }
            cfg->post_wake_delay_ms = val;
            i++;
        } else if (strcmp(arg, "--usb-settle-timeout-ms") == 0) {
            uint32_t val = 0;
            if (i + 1 >= argc || aic_fwload_parse_u32(argv[i + 1], 10000U, &val) != 0) {
                if (out_bad_arg) *out_bad_arg = arg;
                return -1;
            }
            cfg->usb_settle_timeout_ms = val;
            i++;
        } else if (strcmp(arg, "--no-5g") == 0) {
            cfg->enable_5g = false;
        } else if (strcmp(arg, "--eject-only") == 0) {
            cfg->eject_only = true;
        } else if (strcmp(arg, "--dry-run") == 0) {
            cfg->dry_run = true;
        } else if (strcmp(arg, "--once") == 0) {
            cfg->mode = AIC_FWLOAD_MODE_ONCE;
        } else if (strcmp(arg, "--watch") == 0) {
            cfg->mode = AIC_FWLOAD_MODE_WATCH;
        } else if (strcmp(arg, "--daemon") == 0) {
            cfg->mode = AIC_FWLOAD_MODE_DAEMON;
        } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            cfg->show_help = true;
            return 0;
        } else {
            if (out_bad_arg) *out_bad_arg = arg;
            return -1;
        }
    }

    if ((cfg->mode == AIC_FWLOAD_MODE_WATCH || cfg->mode == AIC_FWLOAD_MODE_DAEMON) &&
        (cfg->dry_run || cfg->eject_only)) {
        if (out_bad_arg) {
            *out_bad_arg = cfg->dry_run ? "--dry-run" : "--eject-only";
        }
        return -1;
    }
    return 0;
}

static inline bool aic_fw_dir_has_marker(const char *dir) {
    if (!dir || dir[0] == '\0') {
        return false;
    }
    char probe[1024];
    int n = snprintf(probe, sizeof(probe), "%s/%s", dir, AIC_FW_REQUIRED_MARKER);
    if (n <= 0 || (size_t)n >= sizeof(probe)) {
        return false;
    }
    return access(probe, R_OK) == 0;
}

static inline int aic_fwload_resolve_fw_dir(const char *explicit_dir,
                                            const char *installed_dir,
                                            const char *dev_dir,
                                            char *out_dir,
                                            size_t out_cap,
                                            aic_fw_src_t *out_src) {
    if (!out_dir || out_cap == 0) {
        return -1;
    }

    const char *chosen = NULL;
    aic_fw_src_t src = AIC_FW_SRC_DEV;

    if (explicit_dir && explicit_dir[0] != '\0') {
        chosen = explicit_dir;
        src = AIC_FW_SRC_EXPLICIT;
    } else if (aic_fw_dir_has_marker(installed_dir)) {
        chosen = installed_dir;
        src = AIC_FW_SRC_INSTALLED;
    } else if (dev_dir && dev_dir[0] != '\0') {
        chosen = dev_dir;
        src = AIC_FW_SRC_DEV;
    } else {
        return -1;
    }

    size_t len = strlen(chosen);
    if (len == 0 || len >= out_cap) {
        return -1;
    }
    memcpy(out_dir, chosen, len + 1U);
    if (out_src) {
        *out_src = src;
    }
    return 0;
}

static inline int aic_fwload_default_watch_lock_path(char *out_buf, size_t out_cap) {
    if (!out_buf || out_cap < 32U) {
        return -1;
    }
    const char *tmp = getenv("TMPDIR");
    if (!tmp || tmp[0] == '\0') {
        tmp = "/var/tmp";
    }
    size_t tlen = strlen(tmp);
    while (tlen > 1U && tmp[tlen - 1U] == '/') {
        tlen--;
    }
    int n = snprintf(out_buf, out_cap, "%.*s/aic8800_fwload.%lu.lock",
                     (int)tlen, tmp, (unsigned long)getuid());
    if (n <= 0 || (size_t)n >= out_cap) {
        return -1;
    }
    return 0;
}

static inline void aic_lock_init(aic_lock_handle_t *lk) {
    memset(lk, 0, sizeof(*lk));
    lk->fd = -1;
}

static inline int aic_lock_acquire(aic_lock_handle_t *lk,
                                   const char *path,
                                   pid_t *out_holder_pid) {
    if (!lk || !path || path[0] == '\0' || strlen(path) >= sizeof(lk->path)) {
        errno = EINVAL;
        return -1;
    }
    if (out_holder_pid) {
        *out_holder_pid = 0;
    }
    aic_lock_init(lk);

    int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (fd < 0) {
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int saved_err = errno;
        if (out_holder_pid) {
            char buf[32];
            memset(buf, 0, sizeof(buf));
            ssize_t r = pread(fd, buf, sizeof(buf) - 1U, 0);
            if (r > 0) {
                long pid_val = strtol(buf, NULL, 10);
                if (pid_val > 0) {
                    *out_holder_pid = (pid_t)pid_val;
                }
            }
        }
        close(fd);
        errno = saved_err;
        return -1;
    }

    if (ftruncate(fd, 0) == 0) {
        char pid_buf[32];
        int plen = snprintf(pid_buf, sizeof(pid_buf), "%ld\n", (long)getpid());
        if (plen > 0) {
            (void)pwrite(fd, pid_buf, (size_t)plen, 0);
        }
    }

    lk->fd = fd;
    memcpy(lk->path, path, strlen(path) + 1U);
    lk->held = true;
    return 0;
}

static inline void aic_lock_release(aic_lock_handle_t *lk) {
    if (!lk) {
        return;
    }
    if (lk->fd >= 0) {
        (void)flock(lk->fd, LOCK_UN);
        close(lk->fd);
        lk->fd = -1;
    }
    lk->held = false;
}

static inline void aic_telemetry_init(aic_fwload_telemetry_t *t) {
    memset(t, 0, sizeof(*t));
}

static inline bool aic_telemetry_begin_cycle(aic_fwload_telemetry_t *t,
                                             aic_fwload_trigger_t trigger) {
    if (!t) {
        return false;
    }
    if (t->sleep_in_progress && trigger != AIC_FWLOAD_TRIGGER_WAKE) {
        t->events_ignored_sleep++;
        return false;
    }
    if (t->cycle_in_progress) {
        t->events_ignored_busy++;
        return false;
    }
    t->cycle_in_progress = true;
    return true;
}

static inline void aic_telemetry_end_cycle(aic_fwload_telemetry_t *t,
                                           aic_cycle_outcome_t outcome,
                                           uint64_t stage0_eject_ms,
                                           uint64_t stage1_fw_ms,
                                           uint64_t stage2_enum_ms,
                                           uint64_t total_ms) {
    if (!t) {
        return;
    }
    t->cycle_in_progress = false;
    switch (outcome) {
    case AIC_CYCLE_LOADED_OK:
        t->cycles_ok++;
        t->last_stage0_eject_ms = stage0_eject_ms;
        t->last_stage1_fw_ms = stage1_fw_ms;
        t->last_stage2_enum_ms = stage2_enum_ms;
        t->last_total_ms = total_ms;
        if (total_ms > t->max_total_ms) {
            t->max_total_ms = total_ms;
        }
        t->stage2_online = true;
        break;
    case AIC_CYCLE_ALREADY_READY:
        t->cycles_already_ready++;
        t->stage2_online = true;
        break;
    case AIC_CYCLE_NO_DEVICE:
        t->cycles_no_device++;
        t->stage2_online = false;
        break;
    case AIC_CYCLE_FAILED:
    default:
        t->cycles_failed++;
        t->last_stage0_eject_ms = stage0_eject_ms;
        t->last_stage1_fw_ms = stage1_fw_ms;
        t->last_stage2_enum_ms = stage2_enum_ms;
        t->last_total_ms = total_ms;
        break;
    }
}

#endif /* AIC8800_FWLOAD_CTL_H */
