/*
 * aic8800_wifi.c - User-space Wi-Fi scan/connect/auto-daemon & WPA2-PSK CLI
 *
 * Connects to AIC8800WiFiUserClient (admin privilege required) via pure public
 * IOKit + CoreFoundation + CommonCrypto APIs.
 */

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "aic8800_abi.h"
#include "aic8800_wifi_ctl.h"
#include "aic8800_wpa2.h"

#define AIC_WIFI_CONF_DIR  "/Library/Application Support/AIC8800"
#define AIC_WIFI_CONF_PATH "/Library/Application Support/AIC8800/wifi.conf"

extern char **environ;

static volatile sig_atomic_t g_keep_running = 1;

static void on_sigterm(int sig) {
    (void)sig;
    g_keep_running = 0;
}

static int open_wifi_client(io_connect_t *out_conn, io_service_t *out_svc, bool quiet) {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                   IOServiceMatching("AIC8800WiFi"));
    if (svc == IO_OBJECT_NULL) {
        if (!quiet) {
            fprintf(stderr, "error: AIC8800WiFi service not found (is dongle plugged & kext loaded?)\n");
        }
        return -1;
    }

    io_connect_t conn = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), AIC_WIFI_USERCLIENT_TYPE, &conn);
    if (kr != KERN_SUCCESS) {
        if (!quiet) {
            fprintf(stderr, "error: IOServiceOpen failed: 0x%08x (try sudo?)\n", kr);
        }
        IOObjectRelease(svc);
        return -1;
    }

    *out_conn = conn;
    *out_svc  = svc;
    return 0;
}

static void get_bsd_name(io_service_t svc, char *buf, size_t cap) {
    snprintf(buf, cap, "unknown");
    CFTypeRef val = IORegistryEntrySearchCFProperty(svc,
                                                    kIOServicePlane,
                                                    CFSTR("BSD Name"),
                                                    kCFAllocatorDefault,
                                                    kIORegistryIterateRecursively);
    if (val) {
        if (CFGetTypeID(val) == CFStringGetTypeID()) {
            (void)CFStringGetCString((CFStringRef)val, buf, (CFIndex)cap, kCFStringEncodingUTF8);
        }
        CFRelease(val);
    }
}

static int trigger_dhcp(const char *bsd) {
    if (!bsd || strcmp(bsd, "unknown") == 0 || strncmp(bsd, "en", 2) != 0) {
        return -1;
    }
    char *argv_dhcp[] = { (char *)"ipconfig", (char *)"set", (char *)bsd, (char *)"DHCP", NULL };
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, "/usr/sbin/ipconfig", NULL, NULL, argv_dhcp, environ);
    if (rc != 0) {
        return -1;
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static kern_return_t uc_get_status(io_connect_t conn, aic_wifi_status_t *out) {
    size_t out_sz = sizeof(*out);
    return IOConnectCallStructMethod(conn, AIC_WIFI_UC_GET_STATUS, NULL, 0U, out, &out_sz);
}

static kern_return_t uc_start_scan(io_connect_t conn, const uint8_t *ssid, uint8_t ssid_len) {
    aic_wifi_scan_req_t req;
    memset(&req, 0, sizeof(req));
    req.ssid_len = ssid_len;
    if (ssid && ssid_len > 0U && ssid_len <= sizeof(req.ssid)) {
        memcpy(req.ssid, ssid, ssid_len);
    }
    return IOConnectCallStructMethod(conn, AIC_WIFI_UC_START_SCAN, &req, sizeof(req), NULL, NULL);
}

static kern_return_t uc_get_scan_results(io_connect_t conn, aic_wifi_scan_list_t *out) {
    size_t out_sz = sizeof(*out);
    return IOConnectCallStructMethod(conn, AIC_WIFI_UC_GET_SCAN_RESULTS, NULL, 0U, out, &out_sz);
}

static kern_return_t uc_connect(io_connect_t conn, const aic_wifi_connect_req_t *in_req, aic_wifi_connect_req_t *out_resp) {
    size_t out_sz = sizeof(*out_resp);
    return IOConnectCallStructMethod(conn, AIC_WIFI_UC_CONNECT, in_req, sizeof(*in_req), out_resp, &out_sz);
}

static kern_return_t uc_disconnect(io_connect_t conn, uint16_t reason) {
    aic_wifi_disconnect_req_t req = { reason, 0U };
    return IOConnectCallStructMethod(conn, AIC_WIFI_UC_DISCONNECT, &req, sizeof(req), NULL, NULL);
}

static kern_return_t uc_poll_eapol(io_connect_t conn, aic_wifi_eapol_frame_t *out) {
    size_t out_sz = sizeof(*out);
    return IOConnectCallStructMethod(conn, AIC_WIFI_UC_POLL_EAPOL, NULL, 0U, out, &out_sz);
}

static kern_return_t uc_send_eapol(io_connect_t conn, const uint8_t *eth, size_t len) {
    if (len < 14U || len > AIC_WIFI_EAPOL_MAX_FRAME) {
        return kIOReturnBadArgument;
    }
    aic_wifi_eapol_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.len       = (uint16_t)len;
    frame.has_frame = 1U;
    memcpy(frame.data, eth, len);
    return IOConnectCallStructMethod(conn, AIC_WIFI_UC_SEND_EAPOL, &frame, sizeof(frame), NULL, NULL);
}

static kern_return_t uc_add_key(io_connect_t conn,
                                uint8_t key_idx,
                                bool pairwise,
                                uint8_t cipher,
                                const uint8_t *key,
                                uint8_t key_len,
                                uint8_t *out_hw_idx) {
    aic_wifi_key_req_t req;
    aic_wifi_key_req_t resp;
    memset(&req, 0, sizeof(req));
    req.key_idx      = key_idx;
    req.pairwise     = pairwise ? 1U : 0U;
    req.cipher_suite = cipher;
    req.key_len      = key_len;
    memcpy(req.key, key, key_len);
    size_t out_sz = sizeof(resp);
    const kern_return_t kr = IOConnectCallStructMethod(conn, AIC_WIFI_UC_ADD_KEY, &req, sizeof(req), &resp, &out_sz);
    if (kr != KERN_SUCCESS) {
        return kr;
    }
    if (out_hw_idx) {
        *out_hw_idx = resp.out_hw_key_idx;
    }
    return (resp.out_status == 0U) ? KERN_SUCCESS : kIOReturnIOError;
}

static kern_return_t uc_set_port_link(io_connect_t conn, bool open_port, bool link_up) {
    aic_wifi_port_req_t req = { open_port ? 1U : 0U, link_up ? 1U : 0U, {0, 0} };
    return IOConnectCallStructMethod(conn, AIC_WIFI_UC_SET_PORT_LINK, &req, sizeof(req), NULL, NULL);
}

static int cmp_bss_rssi_desc(const void *a, const void *b) {
    const aic_wifi_scan_bss_t *ba = (const aic_wifi_scan_bss_t *)a;
    const aic_wifi_scan_bss_t *bb = (const aic_wifi_scan_bss_t *)b;
    if (ba->rssi > bb->rssi) return -1;
    if (ba->rssi < bb->rssi) return 1;
    return 0;
}

static void format_sec_str(uint32_t m, char *buf, size_t cap) {
    if ((m & AIC_SEC_WPA2_PSK) && (m & AIC_SEC_WPA3_SAE)) {
        snprintf(buf, cap, "WPA2/WPA3");
    } else if (m & AIC_SEC_WPA3_SAE) {
        snprintf(buf, cap, "WPA3-SAE");
    } else if ((m & AIC_SEC_WPA2_PSK) && (m & AIC_SEC_WPA_PSK)) {
        snprintf(buf, cap, "WPA/WPA2");
    } else if (m & AIC_SEC_WPA2_PSK) {
        snprintf(buf, cap, (m & AIC_SEC_SHA256) ? "WPA2-SHA256" : "WPA2-PSK");
    } else if (m & AIC_SEC_WPA_PSK) {
        snprintf(buf, cap, "WPA-PSK");
    } else if (m & AIC_SEC_8021X) {
        snprintf(buf, cap, "802.1X");
    } else if (m & AIC_SEC_PRIVACY) {
        snprintf(buf, cap, "WEP/ENC");
    } else {
        snprintf(buf, cap, "OPEN");
    }
}

static int run_scan_and_wait(io_connect_t conn,
                             const uint8_t *ssid,
                             uint8_t ssid_len,
                             aic_wifi_scan_list_t *out_list) {
    kern_return_t kr = uc_start_scan(conn, ssid, ssid_len);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "error: START_SCAN failed: 0x%08x\n", kr);
        return -1;
    }

    for (int i = 0; i < 60; ++i) {
        usleep(100000);
        kr = uc_get_scan_results(conn, out_list);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "error: GET_SCAN_RESULTS failed: 0x%08x\n", kr);
            return -1;
        }
        if (out_list->scan_done || !out_list->scanning) {
            break;
        }
    }

    if (out_list->count > 1U) {
        qsort(out_list->bss, out_list->count, sizeof(aic_wifi_scan_bss_t), cmp_bss_rssi_desc);
    }
    return 0;
}

static int cmd_status(io_connect_t conn, io_service_t svc) {
    aic_wifi_status_t st;
    const kern_return_t kr = uc_get_status(conn, &st);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "error: GET_STATUS failed: 0x%08x\n", kr);
        return 1;
    }
    char bsd[32];
    get_bsd_name(svc, bsd, sizeof(bsd));

    char ssid_str[33] = {0};
    if (st.ssid_len > 0U && st.ssid_len <= 32U) {
        memcpy(ssid_str, st.ssid, st.ssid_len);
    }

    printf("Interface:   %s (VIF %u)\n", bsd, st.vif_idx);
    printf("MAC:         %02x:%02x:%02x:%02x:%02x:%02x\n",
           st.mac_addr[0], st.mac_addr[1], st.mac_addr[2],
           st.mac_addr[3], st.mac_addr[4], st.mac_addr[5]);
    printf("Firmware:    %s\n", (char *)st.fw_version);
    printf("Chip/LMAC:   0x%08x / 0x%08x (5GHz=%s)\n",
           st.chip_id, st.lmac_version, st.is_5g_supported ? "yes" : "no");
    printf("State:       lmac=%u associated=%u link_up=%u assoc_status=%u disc_reason=%u\n",
           st.lmac_ready, st.associated, st.link_up, st.assoc_status, st.disconnect_reason);
    if (st.associated) {
        printf("Connected:   SSID=\"%s\" BSSID=%02x:%02x:%02x:%02x:%02x:%02x freq=%u MHz (sta_idx=%u)\n",
               ssid_str,
               st.ap_bssid[0], st.ap_bssid[1], st.ap_bssid[2],
               st.ap_bssid[3], st.ap_bssid[4], st.ap_bssid[5],
               st.ap_center_freq, st.ap_sta_idx);
    }
    printf("Counters:    TX=%llu RX=%llu EAPOL_RX=%llu (queued=%u)\n",
           st.tx_packets, st.rx_packets, st.eapol_rx_total, st.eapol_rx_queued);
    return 0;
}

static int cmd_scan(io_connect_t conn) {
    aic_wifi_scan_list_t list;
    if (run_scan_and_wait(conn, NULL, 0U, &list) != 0) {
        return 1;
    }

    printf("Scan %s: found %u BSS\n", list.scan_done ? "complete" : "partial", list.count);
    printf("%-32s  %-17s  %-5s  %-4s  %s\n", "SSID", "BSSID", "FREQ", "RSSI", "SECURITY");
    for (uint16_t i = 0U; i < list.count; ++i) {
        const aic_wifi_scan_bss_t *b = &list.bss[i];
        char ssid_str[33] = "<hidden>";
        if (b->ssid_len > 0U && b->ssid_len <= 32U) {
            memcpy(ssid_str, b->ssid, b->ssid_len);
            ssid_str[b->ssid_len] = '\0';
        }
        char sec_str[20];
        format_sec_str(b->sec_mask, sec_str, sizeof(sec_str));
        printf("%-32s  %02x:%02x:%02x:%02x:%02x:%02x  %4u   %4d  %s\n",
               ssid_str,
               b->bssid[0], b->bssid[1], b->bssid[2], b->bssid[3], b->bssid[4], b->bssid[5],
               b->freq, (int)b->rssi, sec_str);
    }
    return 0;
}

static int perform_connect(io_connect_t conn,
                           io_service_t svc,
                           const char *ssid_arg,
                           const char *passphrase,
                           bool run_dhcp,
                           aic_wpa2_ctx_t *out_wpa_ctx,
                           bool *out_is_open) {
    const size_t ssid_len = strlen(ssid_arg);
    if (ssid_len == 0U || ssid_len > 32U) {
        fprintf(stderr, "error: invalid SSID length\n");
        return 1;
    }

    aic_wifi_status_t st;
    if (uc_get_status(conn, &st) != KERN_SUCCESS || !st.lmac_ready) {
        fprintf(stderr, "error: LMAC not ready\n");
        return 1;
    }

    printf("[1/4] Scanning for \"%s\"...\n", ssid_arg);
    aic_wifi_scan_list_t list;
    if (run_scan_and_wait(conn, (const uint8_t *)ssid_arg, (uint8_t)ssid_len, &list) != 0) {
        return 1;
    }

    const aic_wifi_scan_bss_t *target = NULL;
    for (uint16_t i = 0U; i < list.count; ++i) {
        if (list.bss[i].ssid_len == ssid_len &&
            memcmp(list.bss[i].ssid, ssid_arg, ssid_len) == 0) {
            target = &list.bss[i];
            break;
        }
    }
    if (!target) {
        fprintf(stderr, "error: AP \"%s\" not found in %u scan results\n", ssid_arg, list.count);
        return 1;
    }

    char sec_str[20];
    format_sec_str(target->sec_mask, sec_str, sizeof(sec_str));
    printf("      Found BSSID=%02x:%02x:%02x:%02x:%02x:%02x freq=%u MHz rssi=%d dBm sec=%s\n",
           target->bssid[0], target->bssid[1], target->bssid[2],
           target->bssid[3], target->bssid[4], target->bssid[5],
           target->freq, (int)target->rssi, sec_str);

    const bool is_open = (target->sec_mask & (AIC_SEC_PRIVACY | AIC_SEC_WPA_PSK | AIC_SEC_WPA2_PSK | AIC_SEC_WPA3_SAE | AIC_SEC_8021X)) == 0U;
    const bool has_wpa2 = (target->sec_mask & AIC_SEC_WPA2_PSK) != 0U;

    if (!is_open && !has_wpa2) {
        fprintf(stderr, "error: security mode '%s' is not supported (requires OPEN or WPA2-PSK/Mixed)\n",
                sec_str);
        return 1;
    }

    uint8_t pmk[32] = {0};
    uint8_t rsn_ie[22] = {0};
    size_t rsn_ie_len = 0U;
    if (!is_open) {
        if (!passphrase || strlen(passphrase) < 8U) {
            fprintf(stderr, "error: valid 8..63 char passphrase required for %s\n", sec_str);
            return 1;
        }
        if (aic_wpa2_derive_pmk(passphrase, strlen(passphrase),
                                target->ssid, target->ssid_len, pmk) != 0) {
            fprintf(stderr, "error: PBKDF2 passphrase derivation failed\n");
            return 1;
        }
        const uint8_t grp = (target->group_cipher != AIC_CIPHER_INVALID) ? target->group_cipher : AIC_CIPHER_CCMP;
        rsn_ie_len = aic_build_wpa2_rsn_ie(rsn_ie, grp, false);
    }

    printf("[2/4] Associating via SM_CONNECT_REQ...\n");
    aic_wifi_connect_req_t creq;
    aic_wifi_connect_req_t cresp;
    memset(&creq, 0, sizeof(creq));
    creq.ssid_len = target->ssid_len;
    memcpy(creq.ssid, target->ssid, target->ssid_len);
    memcpy(creq.bssid, target->bssid, AIC_MAC_ADDR_LEN);
    creq.band      = target->band;
    creq.freq      = target->freq;
    creq.auth_type = 0U;
    if (!is_open) {
        creq.flags               = AIC_CONTROL_PORT_HOST | AIC_WPA_WPA2_IN_USE;
        creq.ctrl_port_ethertype = htons(AIC_EAPOL_ETHERTYPE);
        creq.ie_len              = (uint16_t)rsn_ie_len;
        memcpy(creq.ie_buf, rsn_ie, rsn_ie_len);
    }

    kern_return_t kr = uc_connect(conn, &creq, &cresp);
    if (kr != KERN_SUCCESS || cresp.out_status != 0U) {
        fprintf(stderr, "error: SM_CONNECT_REQ failed kr=0x%08x fw_status=%u\n", kr, cresp.out_status);
        return 1;
    }

    bool assoc_ok = false;
    for (int i = 0; i < 60; ++i) {
        usleep(100000);
        if (uc_get_status(conn, &st) == KERN_SUCCESS) {
            if (st.associated) {
                assoc_ok = true;
                break;
            }
            if (st.assoc_status != 0xFFFFU && st.assoc_status != 0U) {
                fprintf(stderr, "error: association rejected status_code=%u\n", st.assoc_status);
                return 1;
            }
        }
    }
    if (!assoc_ok) {
        fprintf(stderr, "error: timed out waiting for SM_CONNECT_IND\n");
        return 1;
    }
    printf("      Associated! ap_sta_idx=%u bssid=%02x:%02x:%02x:%02x:%02x:%02x freq=%u\n",
           st.ap_sta_idx, st.ap_bssid[0], st.ap_bssid[1], st.ap_bssid[2],
           st.ap_bssid[3], st.ap_bssid[4], st.ap_bssid[5], st.ap_center_freq);

    aic_wpa2_ctx_t local_ctx;
    aic_wpa2_ctx_t *wpa_ctx = out_wpa_ctx ? out_wpa_ctx : &local_ctx;
    if (!is_open) {
        printf("[3/4] Running WPA2-PSK 4-Way Handshake...\n");
        const uint8_t pw = (target->pairwise_cipher != AIC_CIPHER_INVALID) ? target->pairwise_cipher : AIC_CIPHER_CCMP;
        const uint8_t grp = (target->group_cipher != AIC_CIPHER_INVALID) ? target->group_cipher : AIC_CIPHER_CCMP;
        aic_wpa2_ctx_init(wpa_ctx, st.mac_addr, st.ap_bssid, pmk, rsn_ie, (uint8_t)rsn_ie_len, pw, grp);

        bool keys_installed = false;
        for (int i = 0; i < 600 && !keys_installed; ++i) {
            aic_wifi_eapol_frame_t rx_frame;
            if (uc_poll_eapol(conn, &rx_frame) == KERN_SUCCESS && rx_frame.has_frame) {
                uint8_t reply_eth[AIC_WIFI_EAPOL_MAX_FRAME];
                size_t reply_len = 0U;
                bool keys_ready = false;
                bool gtk_rekey  = false;
                const int hrc = aic_wpa2_handle_rx_eapol(wpa_ctx,
                                                         rx_frame.data,
                                                         rx_frame.len,
                                                         reply_eth,
                                                         sizeof(reply_eth),
                                                         &reply_len,
                                                         &keys_ready,
                                                         &gtk_rekey);
                if (hrc < 0) {
                    fprintf(stderr, "error: EAPOL handshake/MIC failure\n");
                    (void)uc_disconnect(conn, 3U);
                    return 1;
                }
                if (hrc == 1 && reply_len > 0U) {
                    kr = uc_send_eapol(conn, reply_eth, reply_len);
                    if (kr != KERN_SUCCESS) {
                        fprintf(stderr, "error: SEND_EAPOL failed: 0x%08x\n", kr);
                        return 1;
                    }
                }
                if (keys_ready) {
                    uint8_t hw_idx = 0U;
                    kr = uc_add_key(conn, 0U, true, wpa_ctx->pairwise_cipher, &wpa_ctx->ptk[32], 16U, &hw_idx);
                    if (kr != KERN_SUCCESS) {
                        fprintf(stderr, "error: install PTK failed: 0x%08x\n", kr);
                        return 1;
                    }
                    printf("      Installed PTK (hw_key_idx=%u)\n", hw_idx);
                    if (wpa_ctx->has_gtk) {
                        kr = uc_add_key(conn, wpa_ctx->gtk_idx, false, wpa_ctx->group_cipher,
                                        wpa_ctx->gtk, wpa_ctx->gtk_len, &hw_idx);
                        if (kr != KERN_SUCCESS) {
                            fprintf(stderr, "error: install GTK failed: 0x%08x\n", kr);
                            return 1;
                        }
                        printf("      Installed GTK idx=%u (hw_key_idx=%u)\n", wpa_ctx->gtk_idx, hw_idx);
                    }
                    keys_installed = true;
                }
            } else {
                usleep(10000);
            }
        }

        if (!keys_installed) {
            fprintf(stderr, "error: timed out waiting for 4-way handshake\n");
            (void)uc_disconnect(conn, 3U);
            return 1;
        }
    } else {
        printf("[3/4] Open network (no 4-way handshake needed).\n");
    }

    char bsd[32];
    get_bsd_name(svc, bsd, sizeof(bsd));
    printf("[4/4] Opening LMAC control port & raising link on %s...\n", bsd);
    kr = uc_set_port_link(conn, true, true);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "error: SET_PORT_LINK failed: 0x%08x\n", kr);
        return 1;
    }

    if (run_dhcp) {
        const int drc = trigger_dhcp(bsd);
        printf("      DHCP trigger on %s: rc=%d\n", bsd, drc);
    }

    if (out_is_open) {
        *out_is_open = is_open;
    }
    return 0;
}

static int save_profile(const char *ssid, const char *pass) {
    const size_t slen = strlen(ssid);
    if (slen == 0U || slen > 32U || strchr(ssid, '\n') || strchr(ssid, '\r')) {
        fprintf(stderr, "error: invalid SSID\n");
        return 1;
    }
    if (pass && (strlen(pass) < 8U || strlen(pass) > 63U || strchr(pass, '\n') || strchr(pass, '\r'))) {
        fprintf(stderr, "error: passphrase must be 8..63 chars\n");
        return 1;
    }

    (void)mkdir(AIC_WIFI_CONF_DIR, 0755);
    const int fd = open(AIC_WIFI_CONF_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        fprintf(stderr, "error: failed to write %s: %s (try sudo?)\n", AIC_WIFI_CONF_PATH, strerror(errno));
        return 1;
    }
    (void)fchmod(fd, 0600);

    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        close(fd);
        return 1;
    }
    fprintf(fp, "ssid=%s\n", ssid);
    if (pass) {
        fprintf(fp, "psk=%s\n", pass);
    }
    fclose(fp);
    printf("Saved Wi-Fi profile to %s (mode 0600)\n", AIC_WIFI_CONF_PATH);
    return 0;
}

static bool load_profile(char *ssid_out, size_t ssid_cap, char *pass_out, size_t pass_cap, bool *has_pass) {
    *has_pass = false;
    memset(ssid_out, 0, ssid_cap);
    memset(pass_out, 0, pass_cap);

    FILE *fp = fopen(AIC_WIFI_CONF_PATH, "r");
    if (!fp) {
        return false;
    }
    char line[128];
    while (fgets(line, sizeof(line), fp) != NULL) {
        size_t l = strlen(line);
        while (l > 0U && (line[l - 1U] == '\n' || line[l - 1U] == '\r')) {
            line[--l] = '\0';
        }
        if (strncmp(line, "ssid=", 5U) == 0) {
            snprintf(ssid_out, ssid_cap, "%s", line + 5U);
        } else if (strncmp(line, "psk=", 4U) == 0) {
            snprintf(pass_out, pass_cap, "%s", line + 4U);
            *has_pass = (pass_out[0] != '\0');
        }
    }
    fclose(fp);
    return (ssid_out[0] != '\0');
}

static void run_rekey_loop(io_connect_t conn, aic_wpa2_ctx_t *wpa_ctx, bool is_open) {
    while (g_keep_running) {
        aic_wifi_status_t st;
        if (uc_get_status(conn, &st) != KERN_SUCCESS || !st.lmac_ready || !st.associated) {
            fprintf(stderr, "notice: Wi-Fi link lost/disconnected (reason=%u)\n", st.disconnect_reason);
            break;
        }
        if (!is_open) {
            aic_wifi_eapol_frame_t rx_frame;
            if (uc_poll_eapol(conn, &rx_frame) == KERN_SUCCESS && rx_frame.has_frame) {
                uint8_t reply_eth[AIC_WIFI_EAPOL_MAX_FRAME];
                size_t reply_len = 0U;
                bool keys_ready = false;
                bool gtk_rekey  = false;
                if (aic_wpa2_handle_rx_eapol(wpa_ctx,
                                             rx_frame.data,
                                             rx_frame.len,
                                             reply_eth,
                                             sizeof(reply_eth),
                                             &reply_len,
                                             &keys_ready,
                                             &gtk_rekey) == 1) {
                    (void)uc_send_eapol(conn, reply_eth, reply_len);
                    if (gtk_rekey && wpa_ctx->has_gtk) {
                        uint8_t hw_idx = 0U;
                        (void)uc_add_key(conn, wpa_ctx->gtk_idx, false, wpa_ctx->group_cipher,
                                         wpa_ctx->gtk, wpa_ctx->gtk_len, &hw_idx);
                    }
                }
            }
        }
        usleep(250000);
    }
}

static int run_daemon(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    signal(SIGINT, on_sigterm);
    signal(SIGTERM, on_sigterm);

    printf("aic8800_wifi daemon starting (profile=%s)\n", AIC_WIFI_CONF_PATH);

    while (g_keep_running) {
        char ssid[36] = {0};
        char pass[72] = {0};
        bool has_pass = false;
        if (!load_profile(ssid, sizeof(ssid), pass, sizeof(pass), &has_pass)) {
            sleep(3);
            continue;
        }

        io_connect_t conn = IO_OBJECT_NULL;
        io_service_t svc  = IO_OBJECT_NULL;
        if (open_wifi_client(&conn, &svc, true) != 0) {
            sleep(2);
            continue;
        }

        aic_wifi_status_t st;
        if (uc_get_status(conn, &st) != KERN_SUCCESS || !st.lmac_ready) {
            IOServiceClose(conn);
            IOObjectRelease(svc);
            sleep(1);
            continue;
        }

        if (!st.associated || !st.link_up) {
            aic_wpa2_ctx_t wpa_ctx;
            bool is_open = false;
            if (perform_connect(conn, svc, ssid, has_pass ? pass : NULL, true, &wpa_ctx, &is_open) == 0) {
                run_rekey_loop(conn, &wpa_ctx, is_open);
            } else {
                sleep(3);
            }
        } else {
            sleep(2);
        }

        IOServiceClose(conn);
        IOObjectRelease(svc);
        if (g_keep_running) {
            sleep(1);
        }
    }
    printf("aic8800_wifi daemon stopping.\n");
    return 0;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "Usage:\n"
            "  sudo %s status\n"
            "  sudo %s scan\n"
            "  sudo %s connect <SSID> [passphrase] [--no-dhcp] [--stay]\n"
            "  sudo %s save <SSID> [passphrase]    (save auto-connect profile to wifi.conf)\n"
            "  sudo %s disconnect\n"
            "  sudo %s --daemon\n",
            prog, prog, prog, prog, prog, prog);
}

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage(argv[0]);
        return (argc < 2) ? 2 : 0;
    }

    if (strcmp(argv[1], "--daemon") == 0) {
        return run_daemon();
    }
    if (strcmp(argv[1], "save") == 0 && argc >= 3) {
        const char *pass = (argc >= 4) ? argv[3] : NULL;
        return save_profile(argv[2], pass);
    }

    io_connect_t conn = IO_OBJECT_NULL;
    io_service_t svc  = IO_OBJECT_NULL;
    if (open_wifi_client(&conn, &svc, false) != 0) {
        return 1;
    }

    int rc = 0;
    if (strcmp(argv[1], "status") == 0) {
        rc = cmd_status(conn, svc);
    } else if (strcmp(argv[1], "scan") == 0) {
        rc = cmd_scan(conn);
    } else if (strcmp(argv[1], "connect") == 0 && argc >= 3) {
        const char *ssid = argv[2];
        const char *pass = NULL;
        bool stay = false;
        bool run_dhcp = true;
        for (int i = 3; i < argc; ++i) {
            if (strcmp(argv[i], "--stay") == 0) {
                stay = true;
            } else if (strcmp(argv[i], "--no-dhcp") == 0) {
                run_dhcp = false;
            } else if (!pass) {
                pass = argv[i];
            }
        }
        aic_wpa2_ctx_t wpa_ctx;
        bool is_open = false;
        rc = perform_connect(conn, svc, ssid, pass, run_dhcp, &wpa_ctx, &is_open);
        if (rc == 0 && stay) {
            signal(SIGINT, on_sigterm);
            signal(SIGTERM, on_sigterm);
            printf("Supplicant staying resident for GTK rekeying (Ctrl+C to exit)...\n");
            run_rekey_loop(conn, &wpa_ctx, is_open);
        }
    } else if (strcmp(argv[1], "disconnect") == 0) {
        const kern_return_t kr = uc_disconnect(conn, 3U);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "error: DISCONNECT failed: 0x%08x\n", kr);
            rc = 1;
        } else {
            printf("Disconnected.\n");
        }
    } else {
        print_usage(argv[0]);
        rc = 2;
    }

    IOServiceClose(conn);
    IOObjectRelease(svc);
    return rc;
}
