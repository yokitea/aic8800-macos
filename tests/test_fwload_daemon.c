/*
 * test_fwload_daemon.c - Phase 1 automation unit tests (ASan + UBSan)
 *
 * Verifies CLI argument parsing, firmware directory precedence, Stage 0 PID
 * recognition, re-entrant trigger/telemetry accounting, and cross-process
 * advisory flock() mutual exclusion via fork().
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "aic8800_abi.h"
#include "aic8800_fwload_ctl.h"

static void test_cli_parsing(void) {
    aic_fwload_cfg_t cfg;
    const char *bad = NULL;

    char *args_default[] = { (char *)"aic8800_fwload" };
    assert(aic_fwload_parse_args(1, args_default, &cfg, &bad) == 0);
    assert(cfg.mode == AIC_FWLOAD_MODE_ONCE);
    assert(cfg.enable_5g == true);
    assert(cfg.post_wake_delay_ms == 1200U);
    assert(cfg.usb_settle_timeout_ms == 1500U);
    assert(cfg.fw_dir_arg == NULL);

    char *args_daemon[] = {
        (char *)"aic8800_fwload",
        (char *)"--daemon",
        (char *)"--fw-dir", (char *)"/opt/fw",
        (char *)"--lock-file", (char *)"/var/run/test.lock",
        (char *)"--post-wake-delay-ms", (char *)"2500",
        (char *)"--usb-settle-timeout-ms", (char *)"800",
        (char *)"--no-5g"
    };
    assert(aic_fwload_parse_args(11, args_daemon, &cfg, &bad) == 0);
    assert(cfg.mode == AIC_FWLOAD_MODE_DAEMON);
    assert(strcmp(cfg.fw_dir_arg, "/opt/fw") == 0);
    assert(strcmp(cfg.lock_file_arg, "/var/run/test.lock") == 0);
    assert(cfg.post_wake_delay_ms == 2500U);
    assert(cfg.usb_settle_timeout_ms == 800U);
    assert(cfg.enable_5g == false);

    char *args_watch_dry[] = { (char *)"aic8800_fwload", (char *)"--watch", (char *)"--dry-run" };
    assert(aic_fwload_parse_args(3, args_watch_dry, &cfg, &bad) != 0);
    assert(strcmp(bad, "--dry-run") == 0);

    char *args_bad_wake[] = { (char *)"aic8800_fwload", (char *)"--post-wake-delay-ms", (char *)"99999" };
    assert(aic_fwload_parse_args(3, args_bad_wake, &cfg, &bad) != 0);
    assert(strcmp(bad, "--post-wake-delay-ms") == 0);

    char *args_help[] = { (char *)"aic8800_fwload", (char *)"--help" };
    assert(aic_fwload_parse_args(2, args_help, &cfg, &bad) == 0);
    assert(cfg.show_help == true);
}

static void test_stage0_pid_table(void) {
    assert(aic_is_stage0_pid(0x5721U));
    assert(aic_is_stage0_pid(0x572AU));
    assert(aic_is_stage0_pid(0x5732U));
    assert(!aic_is_stage0_pid(AIC_USB_PID_BOOTLOADER));
    assert(!aic_is_stage0_pid(AIC_USB_PID_WIFI_BT_RUNTIME));
    assert(!aic_is_stage0_pid(0x0000U));

    size_t count = 0;
    const uint16_t *pids = aic_stage0_pids(&count);
    assert(pids != NULL && count == 11U);
}

static void test_fw_dir_precedence(void) {
    const char *tmp_base = getenv("TMPDIR");
    if (!tmp_base || tmp_base[0] == '\0') {
        tmp_base = "/var/tmp";
    }
    char tmpl[512];
    size_t blen = strlen(tmp_base);
    while (blen > 1U && tmp_base[blen - 1U] == '/') blen--;
    snprintf(tmpl, sizeof(tmpl), "%.*s/aic_fw_test_XXXXXX", (int)blen, tmp_base);
    char *root = mkdtemp(tmpl);
    assert(root != NULL);

    char inst_dir[600], dev_dir[600], marker[700];
    snprintf(inst_dir, sizeof(inst_dir), "%s/installed", root);
    snprintf(dev_dir, sizeof(dev_dir), "%s/dev", root);
    assert(mkdir(inst_dir, 0755) == 0);
    assert(mkdir(dev_dir, 0755) == 0);

    char resolved[512];
    aic_fw_src_t src = AIC_FW_SRC_DEV;

    /* Neither has marker yet -> falls back to dev_dir */
    assert(aic_fwload_resolve_fw_dir(NULL, inst_dir, dev_dir, resolved, sizeof(resolved), &src) == 0);
    assert(src == AIC_FW_SRC_DEV);
    assert(strcmp(resolved, dev_dir) == 0);

    /* Create installed marker -> installed takes precedence over dev */
    snprintf(marker, sizeof(marker), "%s/%s", inst_dir, AIC_FW_REQUIRED_MARKER);
    FILE *fp = fopen(marker, "wb");
    assert(fp != NULL);
    fputs("AICBT_PT_TAG\0\0\0\0", fp);
    fclose(fp);

    assert(aic_fwload_resolve_fw_dir(NULL, inst_dir, dev_dir, resolved, sizeof(resolved), &src) == 0);
    assert(src == AIC_FW_SRC_INSTALLED);
    assert(strcmp(resolved, inst_dir) == 0);

    /* Explicit always overrides installed & dev */
    assert(aic_fwload_resolve_fw_dir("/custom/dir", inst_dir, dev_dir, resolved, sizeof(resolved), &src) == 0);
    assert(src == AIC_FW_SRC_EXPLICIT);
    assert(strcmp(resolved, "/custom/dir") == 0);

    unlink(marker);
    rmdir(inst_dir);
    rmdir(dev_dir);
    rmdir(root);
}

static void test_telemetry_state_machine(void) {
    aic_fwload_telemetry_t t;
    aic_telemetry_init(&t);

    assert(aic_telemetry_begin_cycle(&t, AIC_FWLOAD_TRIGGER_STARTUP) == true);
    /* Re-entrant arrival during active cycle must be rejected cleanly */
    assert(aic_telemetry_begin_cycle(&t, AIC_FWLOAD_TRIGGER_USB_MATCH) == false);
    assert(t.events_ignored_busy == 1U);

    aic_telemetry_end_cycle(&t, AIC_CYCLE_LOADED_OK, 120U, 410U, 320U, 850U);
    assert(t.cycle_in_progress == false);
    assert(t.cycles_ok == 1U);
    assert(t.stage2_online == true);
    assert(t.last_total_ms == 850U);
    assert(t.max_total_ms == 850U);

    /* USB match during active sleep is suppressed, wake clears sleep window */
    t.sleep_in_progress = true;
    assert(aic_telemetry_begin_cycle(&t, AIC_FWLOAD_TRIGGER_USB_MATCH) == false);
    assert(t.events_ignored_sleep == 1U);

    assert(aic_telemetry_begin_cycle(&t, AIC_FWLOAD_TRIGGER_WAKE) == true);
    aic_telemetry_end_cycle(&t, AIC_CYCLE_ALREADY_READY, 0U, 0U, 0U, 0U);
    assert(t.cycles_already_ready == 1U);
    assert(t.max_total_ms == 850U);
}

static void test_cross_process_flock(void) {
    const char *tmp_base = getenv("TMPDIR");
    if (!tmp_base || tmp_base[0] == '\0') {
        tmp_base = "/var/tmp";
    }
    char dir_tmpl[512];
    size_t blen = strlen(tmp_base);
    while (blen > 1U && tmp_base[blen - 1U] == '/') blen--;
    snprintf(dir_tmpl, sizeof(dir_tmpl), "%.*s/aic_lock_test_XXXXXX", (int)blen, tmp_base);
    char *dir = mkdtemp(dir_tmpl);
    assert(dir != NULL);

    char lock_path[600];
    snprintf(lock_path, sizeof(lock_path), "%s/daemon.lock", dir);

    aic_lock_handle_t parent_lock;
    pid_t holder = 0;
    assert(aic_lock_acquire(&parent_lock, lock_path, &holder) == 0);
    assert(parent_lock.held == true);

    /* Child process must fail to acquire lock held by parent and observe parent PID */
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        aic_lock_handle_t child_lock;
        pid_t observed_holder = 0;
        int rc = aic_lock_acquire(&child_lock, lock_path, &observed_holder);
        if (rc == 0) {
            aic_lock_release(&child_lock);
            _exit(2);
        }
        if (observed_holder != getppid()) {
            _exit(3);
        }
        _exit(0);
    }

    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* Release parent lock -> second child must now acquire cleanly */
    aic_lock_release(&parent_lock);
    assert(parent_lock.held == false);

    pid_t pid2 = fork();
    assert(pid2 >= 0);
    if (pid2 == 0) {
        aic_lock_handle_t child_lock;
        pid_t observed_holder = 0;
        if (aic_lock_acquire(&child_lock, lock_path, &observed_holder) != 0) {
            _exit(4);
        }
        aic_lock_release(&child_lock);
        _exit(0);
    }
    assert(waitpid(pid2, &status, 0) == pid2);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    unlink(lock_path);
    rmdir(dir);
}

int main(void) {
    test_cli_parsing();
    test_stage0_pid_table();
    test_fw_dir_precedence();
    test_telemetry_state_machine();
    test_cross_process_flock();
    printf("[PASS] test_fwload_daemon: all CLI/FW-dir/telemetry/fork-flock checks passed\n");
    return 0;
}

