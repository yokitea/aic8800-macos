#include <CoreFoundation/CoreFoundation.h>
#include <DiskArbitration/DiskArbitration.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/storage/IOMedia.h>
#include <IOKit/usb/IOUSBLib.h>

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "aic8800_abi.h"
#include "aic8800_framing.h"
#include "aic8800_fwload_ctl.h"

#define AIC_FW_DEFAULT_DIR "/Users/rush/.gemini/antigravity/brain/3af3af06-ee65-4085-960c-f78f553e3493/scratch/aic8800d80/fw/aic8800D80"

#define AIC_BTMODE_BT_ONLY_COANT    0x05U
#define AIC_BTPORT_MB               0x01U
#define AIC_UART_BAUD_1_5M          1500000U
#define AIC_UART_FLOWCTRL_ENABLE    0x01U
#define AIC_TXPWR_LVL_8800D80       0x00006F2FU

static volatile sig_atomic_t g_stop_requested = 0;
static volatile sig_atomic_t g_sig_write_fd = -1;

static uint64_t mono_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0ULL;
    }
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

static void format_log_ts(char *buf, size_t cap) {
    struct timespec ts;
    struct tm tm_now;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0 &&
        localtime_r(&ts.tv_sec, &tm_now) != NULL) {
        snprintf(buf, cap, "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
                 tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                 tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec,
                 ts.tv_nsec / 1000000L);
    } else {
        snprintf(buf, cap, "0000-00-00 00:00:00.000");
    }
}

typedef struct {
    char     name[16];
    uint32_t type;
    uint32_t len;
    uint32_t *data;
} pt_section_t;

typedef struct {
    pt_section_t *sections;
    size_t        count;
    uint32_t      addr_adid;
    uint32_t      addr_patch;
    uint32_t      ext_patch_nb;
    uint32_t     *ext_patch_pairs;
} pt_table_t;

typedef struct {
    IOUSBDeviceInterface300    **dev;
    IOUSBInterfaceInterface300 **iface;
    uint8_t                      pipe_out;
    uint8_t                      pipe_in;
    uint16_t                     mps_out;
} usb_handle_t;

static int read_entire_file(const char *path, uint8_t **out_buf, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "error: fopen(%s): %s\n", path, strerror(errno));
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    long sz = ftell(fp);
    if (sz <= 0) {
        fclose(fp);
        return -1;
    }
    rewind(fp);
    uint8_t *buf = (uint8_t *)calloc(1U, (size_t)sz);
    if (!buf) {
        fclose(fp);
        return -1;
    }
    if (fread(buf, 1U, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    *out_buf = buf;
    *out_len = (size_t)sz;
    return 0;
}

static void pt_table_free(pt_table_t *tbl) {
    if (!tbl) {
        return;
    }
    for (size_t i = 0; i < tbl->count; i++) {
        free(tbl->sections[i].data);
    }
    free(tbl->sections);
    memset(tbl, 0, sizeof(*tbl));
}

static int pt_table_parse(const uint8_t *raw,
                          size_t len,
                          uint8_t chip_rev,
                          pt_table_t *out) {
    memset(out, 0, sizeof(*out));
    if (!raw || len < 16U || memcmp(raw, AICBT_PT_TAG_MAGIC, sizeof(AICBT_PT_TAG_MAGIC)) != 0) {
        fprintf(stderr, "error: invalid patch table magic (expected AICBT_PT_TAG)\n");
        return -1;
    }

    if (chip_rev == AIC_CHIP_REV_U01) {
        out->addr_adid = AIC_FW_RAM_ADID_ADDR_U01;
        out->addr_patch = AIC_FW_RAM_PATCH_ADDR_U01;
    } else {
        out->addr_adid = AIC_FW_RAM_ADID_ADDR_U02;
        out->addr_patch = AIC_FW_RAM_PATCH_ADDR_U02;
    }

    size_t pos = 16U;
    size_t cap = 0U;
    while (pos + 24U <= len) {
        if (out->count == cap) {
            size_t next_cap = cap ? (cap * 2U) : 8U;
            pt_section_t *grown = (pt_section_t *)realloc(out->sections, next_cap * sizeof(pt_section_t));
            if (!grown) {
                pt_table_free(out);
                return -1;
            }
            out->sections = grown;
            cap = next_cap;
        }
        pt_section_t *sec = &out->sections[out->count];
        memset(sec, 0, sizeof(*sec));
        memcpy(sec->name, &raw[pos], 16U);
        sec->type = aic_get_le32(&raw[pos + 16U]);
        sec->len = aic_get_le32(&raw[pos + 20U]);
        pos += 24U;

        if (sec->type >= 1000U || sec->len == 0U) {
            sec->len = 0U;
        } else {
            size_t bytes = (size_t)sec->len * 8U;
            if (pos + bytes > len) {
                fprintf(stderr, "error: truncated patch table section %.16s\n", sec->name);
                pt_table_free(out);
                return -1;
            }
            sec->data = (uint32_t *)calloc(bytes / sizeof(uint32_t), sizeof(uint32_t));
            if (!sec->data) {
                pt_table_free(out);
                return -1;
            }
            for (size_t i = 0; i < bytes / sizeof(uint32_t); i++) {
                sec->data[i] = aic_get_le32(&raw[pos + i * 4U]);
            }
            pos += bytes;
        }

        if (sec->type == AICBT_PT_INF && sec->len > 0U) {
            const uint32_t base_len = 4U;
            uint32_t memcpy_len = sec->len;
            if (sec->len > base_len) {
                memcpy_len = base_len + 1U;
                sec->len = base_len;
            }
            if (memcpy_len >= 1U) {
                out->addr_adid = sec->data[1];
            }
            if (memcpy_len >= 2U) {
                out->addr_patch = sec->data[3];
            }
            if (memcpy_len >= 5U) {
                out->ext_patch_nb = sec->data[9];
                out->ext_patch_pairs = sec->data + (memcpy_len * 2U);
            }
        }
        out->count++;
    }
    return 0;
}

static bool usb_vid_pid_present_exact(uint16_t vid, uint16_t pid) {
    CFMutableDictionaryRef match = IOServiceMatching(kIOUSBDeviceClassName);
    if (!match) {
        return false;
    }
    SInt32 v = (SInt32)vid;
    SInt32 p = (SInt32)pid;
    CFNumberRef cf_vid = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &v);
    CFNumberRef cf_pid = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &p);
    CFDictionarySetValue(match, CFSTR(kUSBVendorID), cf_vid);
    CFDictionarySetValue(match, CFSTR(kUSBProductID), cf_pid);
    CFRelease(cf_vid);
    CFRelease(cf_pid);

    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, match);
    if (svc != IO_OBJECT_NULL) {
        IOObjectRelease(svc);
        return true;
    }
    return false;
}

static bool usb_vid_pid_present(uint16_t vid, uint16_t pid) {
    if (usb_vid_pid_present_exact(vid, pid)) {
        return true;
    }
    if (vid == AIC_USB_VID_AICS_WIFI && pid == AIC_USB_PID_WIFI_BT_RUNTIME) {
        return usb_vid_pid_present_exact(AIC_USB_VID_CSR_SPOOF, pid);
    }
    return false;
}

static void usb_close(usb_handle_t *u) {
    if (u->iface) {
        (*u->iface)->USBInterfaceClose(u->iface);
        (*u->iface)->Release(u->iface);
        u->iface = NULL;
    }
    if (u->dev) {
        (*u->dev)->USBDeviceClose(u->dev);
        (*u->dev)->Release(u->dev);
        u->dev = NULL;
    }
}

static int usb_open_device_ex(uint16_t vid, uint16_t pid, usb_handle_t *out, bool quiet) {
    memset(out, 0, sizeof(*out));
    CFMutableDictionaryRef match = IOServiceMatching(kIOUSBDeviceClassName);
    if (!match) {
        return -1;
    }
    SInt32 v = (SInt32)vid;
    SInt32 p = (SInt32)pid;
    CFNumberRef cf_vid = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &v);
    CFNumberRef cf_pid = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &p);
    CFDictionarySetValue(match, CFSTR(kUSBVendorID), cf_vid);
    CFDictionarySetValue(match, CFSTR(kUSBProductID), cf_pid);
    CFRelease(cf_vid);
    CFRelease(cf_pid);

    io_service_t dev_svc = IOServiceGetMatchingService(kIOMainPortDefault, match);
    if (dev_svc == IO_OBJECT_NULL) {
        return -1;
    }

    IOCFPlugInInterface **plug = NULL;
    SInt32 score = 0;
    kern_return_t kr = IOCreatePlugInInterfaceForService(
        dev_svc, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    IOObjectRelease(dev_svc);
    if (kr != kIOReturnSuccess || !plug) {
        return -1;
    }

    HRESULT hr = (*plug)->QueryInterface(
        plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID300), (LPVOID *)&out->dev);
    (*plug)->Release(plug);
    if (hr != S_OK || !out->dev) {
        return -1;
    }

    kr = (*out->dev)->USBDeviceOpen(out->dev);
    if (kr != kIOReturnSuccess && kr != kIOReturnExclusiveAccess) {
        usb_close(out);
        return -1;
    }

    UInt8 cfg = 0;
    if ((*out->dev)->GetConfiguration(out->dev, &cfg) == kIOReturnSuccess && cfg == 0) {
        IOUSBConfigurationDescriptorPtr cfg_desc = NULL;
        if ((*out->dev)->GetConfigurationDescriptorPtr(out->dev, 0, &cfg_desc) == kIOReturnSuccess && cfg_desc) {
            (*out->dev)->SetConfiguration(out->dev, cfg_desc->bConfigurationValue);
        }
    }

    IOUSBFindInterfaceRequest req = {
        kIOUSBFindInterfaceDontCare,
        kIOUSBFindInterfaceDontCare,
        kIOUSBFindInterfaceDontCare,
        kIOUSBFindInterfaceDontCare
    };
    io_iterator_t iter = IO_OBJECT_NULL;
    if ((*out->dev)->CreateInterfaceIterator(out->dev, &req, &iter) != kIOReturnSuccess) {
        usb_close(out);
        return -1;
    }

    io_service_t if_svc = IOIteratorNext(iter);
    IOObjectRelease(iter);
    if (if_svc == IO_OBJECT_NULL) {
        usb_close(out);
        return -1;
    }

    kr = IOCreatePlugInInterfaceForService(
        if_svc, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    IOObjectRelease(if_svc);
    if (kr != kIOReturnSuccess || !plug) {
        usb_close(out);
        return -1;
    }

    hr = (*plug)->QueryInterface(
        plug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID300), (LPVOID *)&out->iface);
    (*plug)->Release(plug);
    if (hr != S_OK || !out->iface) {
        usb_close(out);
        return -1;
    }

    kr = (*out->iface)->USBInterfaceOpenSeize(out->iface);
    if (kr != kIOReturnSuccess) {
        kr = (*out->iface)->USBInterfaceOpen(out->iface);
    }
    if (kr != kIOReturnSuccess) {
        if (!quiet) {
            fprintf(stderr,
                    "[fwload] usb_open_device(0x%04x:0x%04x) USBInterfaceOpen failed: 0x%08x%s\n",
                    vid, pid, (unsigned)kr,
                    (kr == kIOReturnExclusiveAccess) ? " (kIOReturnExclusiveAccess)" : "");
        }
        usb_close(out);
        return -1;
    }

    UInt8 num_ep = 0;
    (*out->iface)->GetNumEndpoints(out->iface, &num_ep);
    for (UInt8 ref = 1; ref <= num_ep; ref++) {
        UInt8 dir = 0, num = 0, xfer = 0, interval = 0;
        UInt16 mps = 0;
        if ((*out->iface)->GetPipeProperties(
                out->iface, ref, &dir, &num, &xfer, &mps, &interval) != kIOReturnSuccess) {
            continue;
        }
        if (xfer != kUSBBulk) {
            continue;
        }
        if (dir == kUSBOut && out->pipe_out == 0) {
            out->pipe_out = ref;
            out->mps_out = mps ? mps : AIC_USB_BULK_MPS_HS;
        } else if (dir == kUSBIn && out->pipe_in == 0) {
            out->pipe_in = ref;
        }
    }

    if (out->pipe_out == 0 || out->pipe_in == 0) {
        usb_close(out);
        return -1;
    }
    return 0;
}

static int usb_open_device_wait(uint16_t vid, uint16_t pid, usb_handle_t *out, uint32_t timeout_ms) {
    for (uint32_t elapsed = 0; !g_stop_requested; elapsed += 50U) {
        bool is_last = (elapsed + 50U > timeout_ms);
        if (usb_open_device_ex(vid, pid, out, !is_last) == 0) {
            return 0;
        }
        if (elapsed >= timeout_ms || !usb_vid_pid_present(vid, pid)) {
            break;
        }
        usleep(50000);
    }
    return -1;
}

typedef struct {
    uint16_t            expected_id;
    bool                got_match;
    aic_ipc_e2a_msg_t   msg;
} rx_wait_ctx_t;

static void on_rx_cfg_cb(uint8_t cfg_type, const uint8_t *payload, size_t payload_len, void *vctx) {
    rx_wait_ctx_t *ctx = (rx_wait_ctx_t *)vctx;
    if (cfg_type != AIC_USB_TYPE_CFG_CMD_RSP || payload_len < sizeof(aic_ipc_e2a_msg_hdr_t)) {
        return;
    }
    aic_ipc_e2a_msg_hdr_t hdr;
    memcpy(&hdr, payload, sizeof(hdr));
    if (hdr.pattern != AIC_IPC_MSGE2A_VALID_PAT) {
        return;
    }
    if (hdr.id == ctx->expected_id) {
        size_t cp = payload_len < sizeof(ctx->msg) ? payload_len : sizeof(ctx->msg);
        memset(&ctx->msg, 0, sizeof(ctx->msg));
        memcpy(&ctx->msg, payload, cp);
        ctx->got_match = true;
    }
}

static int send_dbg_cmd(usb_handle_t *u,
                        uint16_t req_id,
                        uint16_t cfm_id,
                        const void *req_param,
                        uint16_t req_len,
                        aic_ipc_e2a_msg_t *out_cfm) {
    uint8_t tx[AIC_USB_MSG_MAX_PKT_SIZE];
    size_t tx_len = 0;
    if (aic_encode_lmac_msg(tx, sizeof(tx), req_id, AIC_TASK_DBG, AIC_DRV_TASK_ID,
                            req_param, req_len, &tx_len) != 0) {
        return -1;
    }

    IOReturn kr = (*u->iface)->WritePipeTO(u->iface, u->pipe_out, tx, (UInt32)tx_len, 2000, 3000);
    if (kr != kIOReturnSuccess) {
        return -1;
    }
    if (aic_usb_needs_zlp((uint32_t)tx_len, u->mps_out)) {
        (*u->iface)->WritePipeTO(u->iface, u->pipe_out, tx, 0, 1000, 1000);
    }

    if (cfm_id == 0U) {
        return 0;
    }

    rx_wait_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.expected_id = cfm_id;
    uint8_t rx[AIC_CMD_BUF_MAX];

    for (int attempt = 0; attempt < 8; attempt++) {
        UInt32 rx_sz = sizeof(rx);
        kr = (*u->iface)->ReadPipeTO(u->iface, u->pipe_in, rx, &rx_sz, 2000, 3000);
        if (kr != kIOReturnSuccess || rx_sz == 0) {
            return -1;
        }
        (void)aic_rx_demux_usb_aggr(rx, rx_sz, NULL, on_rx_cfg_cb, &ctx);
        if (ctx.got_match) {
            if (out_cfm) {
                *out_cfm = ctx.msg;
            }
            return 0;
        }
    }
    return -1;
}

static int dbg_mem_read(usb_handle_t *u, uint32_t addr, uint32_t *out_val) {
    aic_dbg_mem_read_req_t req = { addr };
    aic_ipc_e2a_msg_t cfm;
    if (send_dbg_cmd(u, AIC_DBG_MEM_READ_REQ, AIC_DBG_MEM_READ_CFM, &req, sizeof(req), &cfm) != 0) {
        return -1;
    }
    aic_dbg_mem_read_cfm_t rd;
    memcpy(&rd, cfm.param, sizeof(rd));
    *out_val = rd.memdata;
    return 0;
}

static int dbg_mem_write(usb_handle_t *u, uint32_t addr, uint32_t val) {
    aic_dbg_mem_write_req_t req = { addr, val };
    return send_dbg_cmd(u, AIC_DBG_MEM_WRITE_REQ, AIC_DBG_MEM_WRITE_CFM, &req, sizeof(req), NULL);
}

static int upload_bin_file(usb_handle_t *u, uint32_t target_addr, const char *path) {
    uint8_t *buf = NULL;
    size_t len = 0;
    if (read_entire_file(path, &buf, &len) != 0) {
        return -1;
    }
    printf("[fwload] uploading %s -> 0x%08x (%zu bytes)\n", path, target_addr, len);

    for (size_t off = 0; off < len; off += 1024U) {
        size_t chunk = (len - off > 1024U) ? 1024U : (len - off);
        aic_dbg_mem_block_write_req_t req;
        memset(&req, 0, sizeof(req));
        req.memaddr = target_addr + (uint32_t)off;
        req.memsize = (uint32_t)chunk;
        memcpy(req.memdata, &buf[off], chunk);
        uint16_t param_len = (uint16_t)(8U + ((chunk + 3U) & ~3U));
        if (send_dbg_cmd(u, AIC_DBG_MEM_BLOCK_WRITE_REQ, AIC_DBG_MEM_BLOCK_WRITE_CFM,
                         &req, param_len, NULL) != 0) {
            free(buf);
            return -1;
        }
    }
    free(buf);
    return 0;
}

typedef struct {
    bool      done;
    DAReturn  status;
} da_op_ctx_t;

static void da_op_cb(DADiskRef disk, DADissenterRef dissenter, void *context) {
    (void)disk;
    da_op_ctx_t *ctx = (da_op_ctx_t *)context;
    ctx->status = dissenter ? DADissenterGetStatus(dissenter) : 0;
    ctx->done = true;
}

static bool read_registry_u16(io_registry_entry_t entry, CFStringRef key, uint16_t *out) {
    CFTypeRef val = IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0);
    if (!val) {
        return false;
    }
    bool ok = false;
    if (CFGetTypeID(val) == CFNumberGetTypeID()) {
        SInt32 num = 0;
        if (CFNumberGetValue((CFNumberRef)val, kCFNumberSInt32Type, &num)) {
            *out = (uint16_t)num;
            ok = true;
        }
    }
    CFRelease(val);
    return ok;
}

static bool media_belongs_to_usb_vid_pid(io_service_t media, uint16_t vid, uint16_t pid) {
    io_registry_entry_t cur = media;
    IOObjectRetain(cur);
    for (int depth = 0; depth < 32; depth++) {
        uint16_t found_vid = 0;
        uint16_t found_pid = 0;
        if ((IOObjectConformsTo(cur, "IOUSBHostDevice") ||
             IOObjectConformsTo(cur, kIOUSBDeviceClassName)) &&
            read_registry_u16(cur, CFSTR("idVendor"), &found_vid) &&
            read_registry_u16(cur, CFSTR("idProduct"), &found_pid)) {
            IOObjectRelease(cur);
            return (found_vid == vid && found_pid == pid);
        }
        io_registry_entry_t parent = IO_OBJECT_NULL;
        kern_return_t kr = IORegistryEntryGetParentEntry(cur, kIOServicePlane, &parent);
        IOObjectRelease(cur);
        if (kr != KERN_SUCCESS || parent == IO_OBJECT_NULL) {
            break;
        }
        cur = parent;
    }
    if (cur != IO_OBJECT_NULL) {
        IOObjectRelease(cur);
    }
    return false;
}

static void run_da_wait(const bool *done_flag, CFStringRef mode, int timeout_ms) {
    for (int elapsed = 0; !*done_flag && !g_stop_requested && elapsed < timeout_ms; elapsed += 50) {
        CFRunLoopRunInMode(mode, 0.05, true);
    }
}

static int eject_stage0_via_diskarbitration_ex(uint16_t vid,
                                               uint16_t pid,
                                               bool *out_found_disk) {
    if (out_found_disk) {
        *out_found_disk = false;
    }

    CFMutableDictionaryRef match = IOServiceMatching("IOMedia");
    if (!match) {
        return -1;
    }
    CFDictionarySetValue(match, CFSTR(kIOMediaWholeKey), kCFBooleanTrue);

    io_iterator_t iter = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter) != KERN_SUCCESS) {
        return -1;
    }

    DASessionRef session = DASessionCreate(kCFAllocatorDefault);
    if (!session) {
        IOObjectRelease(iter);
        return -1;
    }
    CFStringRef da_mode = CFSTR("com.rush.aic8800.dawait");
    DASessionScheduleWithRunLoop(session, CFRunLoopGetCurrent(), da_mode);

    bool found_disk = false;
    int rc = 0;
    io_service_t media = IO_OBJECT_NULL;
    while ((media = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
        if (!media_belongs_to_usb_vid_pid(media, vid, pid)) {
            IOObjectRelease(media);
            continue;
        }

        CFTypeRef bsd_cf = IORegistryEntryCreateCFProperty(
            media, CFSTR(kIOBSDNameKey), kCFAllocatorDefault, 0);
        IOObjectRelease(media);
        if (!bsd_cf || CFGetTypeID(bsd_cf) != CFStringGetTypeID()) {
            if (bsd_cf) {
                CFRelease(bsd_cf);
            }
            continue;
        }

        char bsd_name[64];
        bool ok = CFStringGetCString((CFStringRef)bsd_cf, bsd_name, sizeof(bsd_name), kCFStringEncodingUTF8);
        CFRelease(bsd_cf);
        if (!ok || bsd_name[0] == '\0') {
            continue;
        }

        DADiskRef disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, bsd_name);
        if (!disk) {
            rc = -1;
            continue;
        }

        found_disk = true;
        printf("[fwload] Stage 0 MSC held by IOUSBMassStorageDriver; ejecting /dev/%s via DiskArbitration\n",
               bsd_name);

        da_op_ctx_t unmount_ctx = { false, 0 };
        DADiskUnmount(disk,
                      kDADiskUnmountOptionWhole | kDADiskUnmountOptionForce,
                      da_op_cb,
                      &unmount_ctx);
        run_da_wait(&unmount_ctx.done, da_mode, 3000);
        if (unmount_ctx.done && unmount_ctx.status != 0) {
            fprintf(stderr,
                    "[fwload] note: DADiskUnmount(/dev/%s) status=0x%08x; proceeding with DADiskEject\n",
                    bsd_name, (unsigned)unmount_ctx.status);
        }

        da_op_ctx_t eject_ctx = { false, 0 };
        DADiskEject(disk, kDADiskEjectOptionDefault, da_op_cb, &eject_ctx);
        run_da_wait(&eject_ctx.done, da_mode, 5000);
        if (!eject_ctx.done || eject_ctx.status != 0) {
            fprintf(stderr,
                    "[fwload] error: DADiskEject(/dev/%s) failed (done=%d status=0x%08x)\n",
                    bsd_name, eject_ctx.done ? 1 : 0, (unsigned)eject_ctx.status);
            rc = -1;
        }
        CFRelease(disk);
    }

    DASessionUnscheduleFromRunLoop(session, CFRunLoopGetCurrent(), da_mode);
    CFRelease(session);
    IOObjectRelease(iter);
    if (out_found_disk) {
        *out_found_disk = found_disk;
    }
    return (found_disk && rc == 0) ? 0 : -1;
}

static bool any_stage0_present(uint16_t *out_pid) {
    size_t pid_count = 0;
    const uint16_t *pids = aic_stage0_pids(&pid_count);
    for (size_t i = 0; i < pid_count; i++) {
        if (usb_vid_pid_present(AIC_USB_VID_AICS_BOOT, pids[i])) {
            if (out_pid) {
                *out_pid = pids[i];
            }
            return true;
        }
    }
    return false;
}

static int eject_stage0_if_present(uint32_t settle_timeout_ms,
                                   bool *out_did_eject,
                                   uint64_t *out_eject_ms) {
    if (out_did_eject) {
        *out_did_eject = false;
    }
    if (out_eject_ms) {
        *out_eject_ms = 0ULL;
    }

    if (usb_vid_pid_present(AIC_USB_VID_AICS_BOOT, AIC_USB_PID_BOOTLOADER)) {
        return 0;
    }

    uint16_t stage0_pid = 0;
    if (!any_stage0_present(&stage0_pid)) {
        return 0;
    }

    uint64_t t_start = mono_ms();
    bool ejected = false;
    for (uint32_t waited = 0; !g_stop_requested; waited += 100U) {
        if (usb_vid_pid_present(AIC_USB_VID_AICS_BOOT, AIC_USB_PID_BOOTLOADER)) {
            if (out_did_eject) {
                *out_did_eject = true;
            }
            if (out_eject_ms) {
                *out_eject_ms = mono_ms() - t_start;
            }
            return 0;
        }

        bool is_last = (waited + 100U > settle_timeout_ms);
        usb_handle_t u;
        if (usb_open_device_ex(AIC_USB_VID_AICS_BOOT, stage0_pid, &u, !is_last) == 0) {
            uint8_t cbw[AIC_USB_BOT_CBW_LEN];
            aic_build_bot_eject_cbw(cbw, 0x41494330U);
            printf("[fwload] sending BOT SCSI eject CBW to Stage 0 (0x%04x:0x%04x)\n",
                   AIC_USB_VID_AICS_BOOT, stage0_pid);
            IOReturn wkr = (*u.iface)->WritePipeTO(u.iface, u.pipe_out, cbw, sizeof(cbw), 2000, 3000);
            if (wkr != kIOReturnSuccess) {
                fprintf(stderr,
                        "[fwload] error: BOT eject WritePipeTO failed for 0x%04x:0x%04x (ret=0x%08x)\n",
                        AIC_USB_VID_AICS_BOOT, stage0_pid, (unsigned)wkr);
                usb_close(&u);
                return -1;
            }
            uint8_t csw[AIC_USB_BOT_CSW_LEN];
            UInt32 csw_len = sizeof(csw);
            (*u.iface)->ReadPipeTO(u.iface, u.pipe_in, csw, &csw_len, 1000, 1500);
            usb_close(&u);
            ejected = true;
            break;
        }

        bool found_disk = false;
        if (eject_stage0_via_diskarbitration_ex(AIC_USB_VID_AICS_BOOT, stage0_pid, &found_disk) == 0) {
            ejected = true;
            break;
        }
        if (found_disk || is_last || !usb_vid_pid_present(AIC_USB_VID_AICS_BOOT, stage0_pid)) {
            fprintf(stderr,
                    "[fwload] error: Stage 0 MSC device (0x%04x:0x%04x) could neither be opened directly "
                    "nor ejected via DiskArbitration (ensure AIC8800MSCBlocker.kext or IOMedia is active)\n",
                    AIC_USB_VID_AICS_BOOT, stage0_pid);
            return -1;
        }
        usleep(100000);
    }

    if (!ejected || g_stop_requested) {
        return -1;
    }

    for (int wait_ms = 0; !g_stop_requested && wait_ms < 8000; wait_ms += 50) {
        if (usb_vid_pid_present(AIC_USB_VID_AICS_BOOT, AIC_USB_PID_BOOTLOADER)) {
            if (out_did_eject) {
                *out_did_eject = true;
            }
            if (out_eject_ms) {
                *out_eject_ms = mono_ms() - t_start;
            }
            return 0;
        }
        usleep(50000);
    }
    fprintf(stderr,
            "[fwload] error: Stage 0 (0x%04x:0x%04x) eject sent, but Stage 1 (0x%04x:0x%04x) "
            "did not enumerate within 8s\n",
            AIC_USB_VID_AICS_BOOT, stage0_pid,
            AIC_USB_VID_AICS_BOOT, AIC_USB_PID_BOOTLOADER);
    return -1;
}

static int run_dry_run(const char *fw_dir, bool enable_5g) {
    char pt_path[PATH_MAX];
    snprintf(pt_path, sizeof(pt_path), "%s/fw_patch_table_8800d80_u02.bin", fw_dir);
    uint8_t *raw = NULL;
    size_t len = 0;
    if (read_entire_file(pt_path, &raw, &len) != 0) {
        return 1;
    }
    pt_table_t tbl;
    if (pt_table_parse(raw, len, AIC_CHIP_REV_U02, &tbl) != 0) {
        free(raw);
        return 1;
    }
    free(raw);

    uint32_t pairs[3][2];
    aic_stage1_build_d80_patch_pairs(0U, enable_5g, pairs);
    printf("[dry-run] patch table sections=%zu addr_adid=0x%08x addr_patch=0x%08x ext_nb=%u\n",
           tbl.count, tbl.addr_adid, tbl.addr_patch, tbl.ext_patch_nb);
    printf("[dry-run] user_cfg offset=0x%04x value=0x%08x (5G=%s)\n",
           pairs[0][0], pairs[0][1], enable_5g ? "enabled" : "disabled");
    pt_table_free(&tbl);
    return 0;
}

typedef struct {
    uint64_t stage0_eject_ms;
    uint64_t stage1_fw_ms;
    uint64_t stage2_enum_ms;
    uint64_t total_ms;
} fwload_cycle_metrics_t;

static aic_cycle_outcome_t run_fwload_cycle(const char *fw_dir,
                                            bool enable_5g,
                                            bool eject_only,
                                            uint32_t settle_timeout_ms,
                                            bool quiet_no_device,
                                            fwload_cycle_metrics_t *out_metrics) {
    fwload_cycle_metrics_t metrics;
    memset(&metrics, 0, sizeof(metrics));
    uint64_t t_cycle_start = mono_ms();

    if (usb_vid_pid_present(AIC_USB_VID_AICS_WIFI, AIC_USB_PID_WIFI_BT_RUNTIME)) {
        if (!quiet_no_device) {
            printf("[fwload] Stage 2 device (0x%04x:0x%04x) already enumerated\n",
                   AIC_USB_VID_AICS_WIFI, AIC_USB_PID_WIFI_BT_RUNTIME);
        }
        if (out_metrics) {
            *out_metrics = metrics;
        }
        return AIC_CYCLE_ALREADY_READY;
    }

    if (!any_stage0_present(NULL) &&
        !usb_vid_pid_present(AIC_USB_VID_AICS_BOOT, AIC_USB_PID_BOOTLOADER)) {
        if (!quiet_no_device) {
            fprintf(stderr, "error: Stage 1 device (0x%04x:0x%04x) not found\n",
                    AIC_USB_VID_AICS_BOOT, AIC_USB_PID_BOOTLOADER);
        }
        if (out_metrics) {
            *out_metrics = metrics;
        }
        return quiet_no_device ? AIC_CYCLE_NO_DEVICE : AIC_CYCLE_FAILED;
    }

    bool did_eject = false;
    if (eject_stage0_if_present(settle_timeout_ms, &did_eject, &metrics.stage0_eject_ms) != 0) {
        fprintf(stderr, "error: Stage 0 eject failed or Stage 1 bootloader did not appear\n");
        metrics.total_ms = mono_ms() - t_cycle_start;
        if (out_metrics) {
            *out_metrics = metrics;
        }
        return AIC_CYCLE_FAILED;
    }
    if (eject_only) {
        metrics.total_ms = mono_ms() - t_cycle_start;
        if (out_metrics) {
            *out_metrics = metrics;
        }
        return AIC_CYCLE_LOADED_OK;
    }

    if (g_stop_requested) {
        metrics.total_ms = mono_ms() - t_cycle_start;
        if (out_metrics) {
            *out_metrics = metrics;
        }
        return AIC_CYCLE_FAILED;
    }

    uint64_t t_stage1_start = mono_ms();
    usb_handle_t u;
    if (usb_open_device_wait(AIC_USB_VID_AICS_BOOT, AIC_USB_PID_BOOTLOADER, &u, 600U) != 0) {
        if (usb_vid_pid_present(AIC_USB_VID_AICS_BOOT, AIC_USB_PID_BOOTLOADER)) {
            fprintf(stderr, "error: Stage 1 device (0x%04x:0x%04x) present in IORegistry, but USB open failed\n",
                    AIC_USB_VID_AICS_BOOT, AIC_USB_PID_BOOTLOADER);
        } else {
            fprintf(stderr, "error: Stage 1 device (0x%04x:0x%04x) not found\n",
                    AIC_USB_VID_AICS_BOOT, AIC_USB_PID_BOOTLOADER);
        }
        metrics.total_ms = mono_ms() - t_cycle_start;
        if (out_metrics) {
            *out_metrics = metrics;
        }
        return AIC_CYCLE_FAILED;
    }

    uint32_t chip_reg = 0;
    if (dbg_mem_read(&u, AIC_SYS_CTRL_CHIP_ID_REG, &chip_reg) != 0) {
        usb_close(&u);
        metrics.total_ms = mono_ms() - t_cycle_start;
        if (out_metrics) *out_metrics = metrics;
        return AIC_CYCLE_FAILED;
    }
    uint8_t chip_rev = (uint8_t)(chip_reg >> 16U);
    bool is_chip_h = ((chip_rev & AIC_CHIP_ID_H_MASK) == AIC_CHIP_ID_H_MASK);
    bool mcu_cache = (((chip_reg >> 25U) & 1U) == 0U);
    printf("[fwload] chip_reg=0x%08x rev=0x%02x H=%d mcu_cache=%d\n",
           chip_reg, chip_rev, is_chip_h ? 1 : 0, mcu_cache ? 1 : 0);

    if (mcu_cache) {
        uint32_t cache_val = 0;
        if (dbg_mem_read(&u, AIC_SYS_CTRL_MCU_CFG_REG, &cache_val) != 0 ||
            dbg_mem_write(&u, AIC_SYS_CTRL_MCU_CFG_REG, cache_val | 1U) != 0) {
            usb_close(&u);
            metrics.total_ms = mono_ms() - t_cycle_start;
            if (out_metrics) *out_metrics = metrics;
            return AIC_CYCLE_FAILED;
        }
    }

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/fw_patch_table_8800d80_%s.bin",
             fw_dir, (chip_rev == AIC_CHIP_REV_U01) ? "u01" : "u02");
    uint8_t *pt_raw = NULL;
    size_t pt_len = 0;
    if (read_entire_file(path, &pt_raw, &pt_len) != 0) {
        usb_close(&u);
        metrics.total_ms = mono_ms() - t_cycle_start;
        if (out_metrics) *out_metrics = metrics;
        return AIC_CYCLE_FAILED;
    }
    pt_table_t tbl;
    int rc = pt_table_parse(pt_raw, pt_len, chip_rev, &tbl);
    free(pt_raw);
    if (rc != 0) {
        usb_close(&u);
        metrics.total_ms = mono_ms() - t_cycle_start;
        if (out_metrics) *out_metrics = metrics;
        return AIC_CYCLE_FAILED;
    }

    const char *rev_tag = (chip_rev == AIC_CHIP_REV_U01) ? "u01" : "u02";
    snprintf(path, sizeof(path), "%s/fw_adid_8800d80_%s.bin", fw_dir, rev_tag);
    if (upload_bin_file(&u, tbl.addr_adid, path) != 0) {
        pt_table_free(&tbl);
        usb_close(&u);
        metrics.total_ms = mono_ms() - t_cycle_start;
        if (out_metrics) *out_metrics = metrics;
        return AIC_CYCLE_FAILED;
    }
    snprintf(path, sizeof(path), "%s/fw_patch_8800d80_%s.bin", fw_dir, rev_tag);
    if (upload_bin_file(&u, tbl.addr_patch, path) != 0) {
        pt_table_free(&tbl);
        usb_close(&u);
        metrics.total_ms = mono_ms() - t_cycle_start;
        if (out_metrics) *out_metrics = metrics;
        return AIC_CYCLE_FAILED;
    }
    for (uint32_t i = 0; i < tbl.ext_patch_nb; i++) {
        uint32_t id = tbl.ext_patch_pairs[i * 2U];
        uint32_t addr = tbl.ext_patch_pairs[i * 2U + 1U];
        snprintf(path, sizeof(path), "%s/fw_patch_8800d80_%s_ext%u.bin", fw_dir, rev_tag, id);
        if (upload_bin_file(&u, addr, path) != 0) {
            pt_table_free(&tbl);
            usb_close(&u);
            metrics.total_ms = mono_ms() - t_cycle_start;
            if (out_metrics) *out_metrics = metrics;
            return AIC_CYCLE_FAILED;
        }
    }

    for (size_t i = 0; i < tbl.count; i++) {
        pt_section_t *sec = &tbl.sections[i];
        if (sec->type == AICBT_PT_VER) {
            continue;
        }
        if (sec->type == AICBT_PT_BTMODE && sec->len >= 9U) {
            sec->data[1] = 1U;
            sec->data[3] = UINT32_MAX;
            sec->data[5] = 0U;
            sec->data[7] = AIC_BTMODE_BT_ONLY_COANT;
            sec->data[9] = AIC_BTPORT_MB;
            sec->data[11] = AIC_UART_BAUD_1_5M;
            sec->data[13] = AIC_UART_FLOWCTRL_ENABLE;
            sec->data[15] = 0U;
            sec->data[17] = AIC_TXPWR_LVL_8800D80;
        }
        for (uint32_t p = 0; p < sec->len; p++) {
            if (dbg_mem_write(&u, sec->data[p * 2U], sec->data[p * 2U + 1U]) != 0) {
                pt_table_free(&tbl);
                usb_close(&u);
                metrics.total_ms = mono_ms() - t_cycle_start;
                if (out_metrics) *out_metrics = metrics;
                return AIC_CYCLE_FAILED;
            }
        }
        if (sec->type == AICBT_PT_PWRON) {
            usleep(100000);
        }
    }
    pt_table_free(&tbl);

    uint32_t fmac_base = (chip_rev == AIC_CHIP_REV_U01)
                             ? AIC_RAM_FMAC_FW_ADDR_U01
                             : AIC_RAM_FMAC_FW_ADDR_U02;
    const char *fmac_name = is_chip_h ? "fmacfw_8800d80_h_u02.bin" : "fmacfw_8800d80_u02.bin";
    snprintf(path, sizeof(path), "%s/%s", fw_dir, fmac_name);
    if (upload_bin_file(&u, fmac_base, path) != 0) {
        usb_close(&u);
        metrics.total_ms = mono_ms() - t_cycle_start;
        if (out_metrics) *out_metrics = metrics;
        return AIC_CYCLE_FAILED;
    }

    uint32_t rd_patch_addr = fmac_base + 0x0198U;
    uint32_t config_base = 0, aic_patch_str_base = 0, fw_ver = 0;
    if (dbg_mem_read(&u, rd_patch_addr, &config_base) != 0 ||
        dbg_mem_read(&u, rd_patch_addr + 8U, &aic_patch_str_base) != 0 ||
        dbg_mem_read(&u, fmac_base + 0x01CU, &fw_ver) != 0) {
        usb_close(&u);
        metrics.total_ms = mono_ms() - t_cycle_start;
        if (out_metrics) *out_metrics = metrics;
        return AIC_CYCLE_FAILED;
    }

    uint32_t patch_start = AIC_PATCH_DEFAULT_BUF_U02;
    if (fw_ver > 0x06090100U) {
        if (dbg_mem_read(&u, rd_patch_addr + 12U, &patch_start) != 0) {
            usb_close(&u);
            metrics.total_ms = mono_ms() - t_cycle_start;
            if (out_metrics) *out_metrics = metrics;
            return AIC_CYCLE_FAILED;
        }
    }

    uint32_t pairs[3][2];
    aic_stage1_build_d80_patch_pairs(config_base, enable_5g, pairs);
    if (dbg_mem_write(&u, aic_patch_str_base + (uint32_t)offsetof(aic_patch_t, magic_num), AIC_PATCH_MAGIC_NUM) != 0 ||
        dbg_mem_write(&u, aic_patch_str_base + (uint32_t)offsetof(aic_patch_t, magic_num_2), AIC_PATCH_MAGIC_NUM_2) != 0 ||
        dbg_mem_write(&u, aic_patch_str_base + (uint32_t)offsetof(aic_patch_t, pair_start), patch_start) != 0 ||
        dbg_mem_write(&u, aic_patch_str_base + (uint32_t)offsetof(aic_patch_t, pair_count), 3U) != 0) {
        usb_close(&u);
        metrics.total_ms = mono_ms() - t_cycle_start;
        if (out_metrics) *out_metrics = metrics;
        return AIC_CYCLE_FAILED;
    }
    for (uint32_t i = 0; i < 3U; i++) {
        if (dbg_mem_write(&u, patch_start + i * 8U, pairs[i][0]) != 0 ||
            dbg_mem_write(&u, patch_start + i * 8U + 4U, pairs[i][1]) != 0) {
            usb_close(&u);
            metrics.total_ms = mono_ms() - t_cycle_start;
            if (out_metrics) *out_metrics = metrics;
            return AIC_CYCLE_FAILED;
        }
    }
    for (uint32_t b = 0; b < 4U; b++) {
        if (dbg_mem_write(&u, aic_patch_str_base + (uint32_t)offsetof(aic_patch_t, block_size[b]), 0U) != 0) {
            usb_close(&u);
            metrics.total_ms = mono_ms() - t_cycle_start;
            if (out_metrics) *out_metrics = metrics;
            return AIC_CYCLE_FAILED;
        }
    }

    /* Match Linux rwnx_send_dbg_start_app_req(..., sync=0, cfm_id=0):
     * HOST_START_APP_AUTO jumps immediately and disconnects Stage 1 (0x8d80)
     * before a bulk IN CFM can complete cleanly, then re-enumerates Stage 2. */
    aic_dbg_start_app_req_t start_req = { fmac_base, AIC_HOST_START_APP_AUTO };
    if (send_dbg_cmd(&u, AIC_DBG_START_APP_REQ, 0U,
                     &start_req, sizeof(start_req), NULL) != 0) {
        fprintf(stderr, "[fwload] error: failed to send AIC_DBG_START_APP_REQ\n");
        usb_close(&u);
        metrics.total_ms = mono_ms() - t_cycle_start;
        if (out_metrics) *out_metrics = metrics;
        return AIC_CYCLE_FAILED;
    }
    usb_close(&u);
    metrics.stage1_fw_ms = mono_ms() - t_stage1_start;

    uint64_t t_enum_start = mono_ms();
    for (int wait_ms = 0; !g_stop_requested && wait_ms < 8000; wait_ms += 50) {
        if (usb_vid_pid_present(AIC_USB_VID_AICS_WIFI, AIC_USB_PID_WIFI_BT_RUNTIME)) {
            metrics.stage2_enum_ms = mono_ms() - t_enum_start;
            metrics.total_ms = mono_ms() - t_cycle_start;
            printf("[fwload] Stage 1 firmware started at 0x%08x (5G=%s); Stage 2 (0x%04x:0x%04x) active "
                   "in %llu ms (eject=%llu fw=%llu enum=%llu)\n",
                   fmac_base, enable_5g ? "enabled" : "disabled",
                   AIC_USB_VID_AICS_WIFI, AIC_USB_PID_WIFI_BT_RUNTIME,
                   (unsigned long long)metrics.total_ms,
                   (unsigned long long)metrics.stage0_eject_ms,
                   (unsigned long long)metrics.stage1_fw_ms,
                   (unsigned long long)metrics.stage2_enum_ms);
            if (out_metrics) {
                *out_metrics = metrics;
            }
            return AIC_CYCLE_LOADED_OK;
        }
        usleep(50000);
    }
    metrics.stage2_enum_ms = mono_ms() - t_enum_start;
    metrics.total_ms = mono_ms() - t_cycle_start;
    fprintf(stderr,
            "[fwload] error: Stage 1 firmware started at 0x%08x, but Stage 2 (0x%04x:0x%04x) "
            "did not enumerate within 8s\n",
            fmac_base, AIC_USB_VID_AICS_WIFI, AIC_USB_PID_WIFI_BT_RUNTIME);
    if (out_metrics) {
        *out_metrics = metrics;
    }
    return AIC_CYCLE_FAILED;
}

typedef struct {
    aic_fwload_cfg_t       cfg;
    char                   fw_dir[PATH_MAX];
    aic_fwload_telemetry_t telemetry;
    aic_lock_handle_t      lock;
    CFRunLoopRef           run_loop;
    IONotificationPortRef  usb_notify_port;
    CFRunLoopSourceRef     usb_notify_src;
    io_iterator_t          boot_match_iter;
    io_iterator_t          stage2_match_iter;
    io_iterator_t          stage2_term_iter;
    io_connect_t           power_root;
    IONotificationPortRef  power_port;
    io_object_t            power_notifier;
    CFRunLoopSourceRef     power_src;
    CFRunLoopTimerRef      wake_timer;
    int                    sig_pipe[2];
    CFFileDescriptorRef    sig_cffd;
    CFRunLoopSourceRef     sig_src;
    bool                   initial_arming;
} daemon_ctx_t;

static void print_telemetry_line(const char *tag, const aic_fwload_telemetry_t *t) {
    char ts[32];
    format_log_ts(ts, sizeof(ts));
    printf("[%s][fwload] %s: stage2=%d ok=%llu ready=%llu nodev=%llu fail=%llu "
           "usb_boot=%llu s2_match=%llu s2_rm=%llu wakes=%llu sleeps=%llu "
           "ignored_busy=%llu last_ms=%llu max_ms=%llu\n",
           ts, tag,
           t->stage2_online ? 1 : 0,
           (unsigned long long)t->cycles_ok,
           (unsigned long long)t->cycles_already_ready,
           (unsigned long long)t->cycles_no_device,
           (unsigned long long)t->cycles_failed,
           (unsigned long long)t->usb_boot_matches,
           (unsigned long long)t->stage2_matches,
           (unsigned long long)t->stage2_removals,
           (unsigned long long)t->wake_events,
           (unsigned long long)t->sleep_events,
           (unsigned long long)t->events_ignored_busy,
           (unsigned long long)t->last_total_ms,
           (unsigned long long)t->max_total_ms);
}

static const char *trigger_name(aic_fwload_trigger_t tr) {
    switch (tr) {
    case AIC_FWLOAD_TRIGGER_STARTUP:   return "startup";
    case AIC_FWLOAD_TRIGGER_USB_MATCH: return "usb-match";
    case AIC_FWLOAD_TRIGGER_WAKE:      return "wake";
    default:                           return "unknown";
    }
}

static void dispatch_daemon_cycle(daemon_ctx_t *ctx, aic_fwload_trigger_t trigger) {
    if (!aic_telemetry_begin_cycle(&ctx->telemetry, trigger)) {
        return;
    }

    char ts[32];
    format_log_ts(ts, sizeof(ts));
    fwload_cycle_metrics_t m;
    aic_cycle_outcome_t outcome = run_fwload_cycle(ctx->fw_dir,
                                                   ctx->cfg.enable_5g,
                                                   false,
                                                   ctx->cfg.usb_settle_timeout_ms,
                                                   true,
                                                   &m);
    aic_telemetry_end_cycle(&ctx->telemetry, outcome,
                            m.stage0_eject_ms, m.stage1_fw_ms,
                            m.stage2_enum_ms, m.total_ms);

    if (outcome == AIC_CYCLE_LOADED_OK) {
        print_telemetry_line(trigger_name(trigger), &ctx->telemetry);
    } else if (outcome == AIC_CYCLE_ALREADY_READY) {
        if (trigger == AIC_FWLOAD_TRIGGER_STARTUP || trigger == AIC_FWLOAD_TRIGGER_WAKE) {
            printf("[%s][fwload] %s: Stage 2 (0x%04x:0x%04x) active\n",
                   ts, trigger_name(trigger),
                   AIC_USB_VID_AICS_WIFI, AIC_USB_PID_WIFI_BT_RUNTIME);
        }
    } else if (outcome == AIC_CYCLE_NO_DEVICE) {
        if (trigger == AIC_FWLOAD_TRIGGER_STARTUP || trigger == AIC_FWLOAD_TRIGGER_WAKE) {
            printf("[%s][fwload] %s: no AIC8800 USB device attached; waiting on IOKit notifications\n",
                   ts, trigger_name(trigger));
        }
    } else {
        fprintf(stderr, "[%s][fwload] %s: load cycle FAILED after %llu ms (fail_count=%llu)\n",
                ts, trigger_name(trigger),
                (unsigned long long)m.total_ms,
                (unsigned long long)ctx->telemetry.cycles_failed);
    }
}

static uint16_t read_usb_pid(io_service_t svc) {
    uint16_t pid = 0;
    CFTypeRef cf = IORegistryEntryCreateCFProperty(svc, CFSTR(kUSBProductID), kCFAllocatorDefault, 0);
    if (cf) {
        if (CFGetTypeID(cf) == CFNumberGetTypeID()) {
            SInt32 v = 0;
            if (CFNumberGetValue((CFNumberRef)cf, kCFNumberSInt32Type, &v) && v >= 0 && v <= 0xFFFF) {
                pid = (uint16_t)v;
            }
        }
        CFRelease(cf);
    }
    return pid;
}

static void on_boot_usb_matched(void *refcon, io_iterator_t iterator) {
    daemon_ctx_t *ctx = (daemon_ctx_t *)refcon;
    uint32_t relevant = 0;
    io_service_t svc = IO_OBJECT_NULL;
    while ((svc = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        uint16_t pid = read_usb_pid(svc);
        IOObjectRelease(svc);
        if (aic_is_stage0_pid(pid) || pid == AIC_USB_PID_BOOTLOADER) {
            relevant++;
            ctx->telemetry.usb_boot_matches++;
            if (!ctx->initial_arming) {
                char ts[32];
                format_log_ts(ts, sizeof(ts));
                printf("[%s][fwload] IOKit matched USB boot device (0x%04x:0x%04x)\n",
                       ts, AIC_USB_VID_AICS_BOOT, pid);
            }
        }
    }
    if (!ctx->initial_arming && relevant > 0U && !g_stop_requested) {
        dispatch_daemon_cycle(ctx, AIC_FWLOAD_TRIGGER_USB_MATCH);
    }
}

static void on_stage2_matched(void *refcon, io_iterator_t iterator) {
    daemon_ctx_t *ctx = (daemon_ctx_t *)refcon;
    uint32_t count = 0;
    io_service_t svc = IO_OBJECT_NULL;
    while ((svc = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        IOObjectRelease(svc);
        count++;
        ctx->telemetry.stage2_matches++;
    }
    if (count > 0U) {
        ctx->telemetry.stage2_online = true;
        if (!ctx->initial_arming) {
            char ts[32];
            format_log_ts(ts, sizeof(ts));
            printf("[%s][fwload] Stage 2 USB device online (0x%04x:0x%04x)\n",
                   ts, AIC_USB_VID_AICS_WIFI, AIC_USB_PID_WIFI_BT_RUNTIME);
        }
    }
}

static void on_stage2_terminated(void *refcon, io_iterator_t iterator) {
    daemon_ctx_t *ctx = (daemon_ctx_t *)refcon;
    uint32_t count = 0;
    io_service_t svc = IO_OBJECT_NULL;
    while ((svc = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        IOObjectRelease(svc);
        count++;
        ctx->telemetry.stage2_removals++;
    }
    if (!ctx->initial_arming && count > 0U) {
        bool still_present = usb_vid_pid_present(AIC_USB_VID_AICS_WIFI, AIC_USB_PID_WIFI_BT_RUNTIME);
        ctx->telemetry.stage2_online = still_present;
        char ts[32];
        format_log_ts(ts, sizeof(ts));
        printf("[%s][fwload] Stage 2 USB device removed (0x%04x:0x%04x, still_present=%d)\n",
               ts, AIC_USB_VID_AICS_WIFI, AIC_USB_PID_WIFI_BT_RUNTIME, still_present ? 1 : 0);
    }
}

static void on_post_wake_timer(CFRunLoopTimerRef timer, void *info) {
    (void)timer;
    daemon_ctx_t *ctx = (daemon_ctx_t *)info;
    if (ctx->wake_timer) {
        CFRunLoopTimerInvalidate(ctx->wake_timer);
        CFRelease(ctx->wake_timer);
        ctx->wake_timer = NULL;
    }
    if (!g_stop_requested) {
        dispatch_daemon_cycle(ctx, AIC_FWLOAD_TRIGGER_WAKE);
    }
}

static void schedule_post_wake_timer(daemon_ctx_t *ctx) {
    if (ctx->wake_timer) {
        CFRunLoopTimerInvalidate(ctx->wake_timer);
        CFRelease(ctx->wake_timer);
        ctx->wake_timer = NULL;
    }
    CFRunLoopTimerContext tctx = { 0, ctx, NULL, NULL, NULL };
    CFAbsoluteTime fire_at = CFAbsoluteTimeGetCurrent() +
                             (double)ctx->cfg.post_wake_delay_ms / 1000.0;
    ctx->wake_timer = CFRunLoopTimerCreate(kCFAllocatorDefault,
                                           fire_at,
                                           0.0,
                                           0,
                                           0,
                                           on_post_wake_timer,
                                           &tctx);
    if (ctx->wake_timer) {
        CFRunLoopAddTimer(ctx->run_loop, ctx->wake_timer, kCFRunLoopDefaultMode);
    }
}

static void on_system_power_event(void *refcon,
                                  io_service_t service,
                                  natural_t msg_type,
                                  void *msg_arg) {
    (void)service;
    daemon_ctx_t *ctx = (daemon_ctx_t *)refcon;
    char ts[32];

    switch (msg_type) {
    case kIOMessageCanSystemSleep:
        IOAllowPowerChange(ctx->power_root, (intptr_t)msg_arg);
        break;
    case kIOMessageSystemWillSleep:
        ctx->telemetry.sleep_events++;
        ctx->telemetry.sleep_in_progress = true;
        format_log_ts(ts, sizeof(ts));
        printf("[%s][fwload] system entering sleep (sleep #%llu)\n",
               ts, (unsigned long long)ctx->telemetry.sleep_events);
        IOAllowPowerChange(ctx->power_root, (intptr_t)msg_arg);
        break;
    case kIOMessageSystemWillNotSleep:
        ctx->telemetry.sleep_in_progress = false;
        break;
    case kIOMessageSystemWillPowerOn:
        ctx->telemetry.sleep_in_progress = false;
        break;
    case kIOMessageSystemHasPoweredOn:
        ctx->telemetry.sleep_in_progress = false;
        ctx->telemetry.wake_events++;
        format_log_ts(ts, sizeof(ts));
        printf("[%s][fwload] system wake powered on (wake #%llu); verifying USB state\n",
               ts, (unsigned long long)ctx->telemetry.wake_events);
        if (!usb_vid_pid_present(AIC_USB_VID_AICS_WIFI, AIC_USB_PID_WIFI_BT_RUNTIME)) {
            if (any_stage0_present(NULL) ||
                usb_vid_pid_present(AIC_USB_VID_AICS_BOOT, AIC_USB_PID_BOOTLOADER)) {
                dispatch_daemon_cycle(ctx, AIC_FWLOAD_TRIGGER_WAKE);
            }
            if (!ctx->telemetry.stage2_online && ctx->cfg.post_wake_delay_ms > 0U) {
                schedule_post_wake_timer(ctx);
            }
        } else {
            dispatch_daemon_cycle(ctx, AIC_FWLOAD_TRIGGER_WAKE);
        }
        break;
    default:
        break;
    }
}

static void handle_posix_signal(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_stop_requested = sig;
    }
    int fd = (int)g_sig_write_fd;
    if (fd >= 0) {
        char cmd = (sig == SIGUSR1) ? 'I' : 'S';
        (void)write(fd, &cmd, 1);
    }
}

static void on_signal_pipe_cb(CFFileDescriptorRef f, CFOptionFlags callBackTypes, void *info) {
    (void)callBackTypes;
    daemon_ctx_t *ctx = (daemon_ctx_t *)info;
    char buf[32];
    ssize_t n = read(ctx->sig_pipe[0], buf, sizeof(buf));
    bool want_stop = false;
    bool want_info = false;
    if (n > 0) {
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == 'S') {
                want_stop = true;
            } else if (buf[i] == 'I') {
                want_info = true;
            }
        }
    }
    if (want_info && !want_stop) {
        print_telemetry_line("status", &ctx->telemetry);
        CFFileDescriptorEnableCallBacks(f, kCFFileDescriptorReadCallBack);
        return;
    }
    if (want_stop || g_stop_requested) {
        char ts[32];
        format_log_ts(ts, sizeof(ts));
        printf("[%s][fwload] signal %d received; stopping event loop\n",
               ts, (int)g_stop_requested);
        CFRunLoopStop(ctx->run_loop);
    } else {
        CFFileDescriptorEnableCallBacks(f, kCFFileDescriptorReadCallBack);
    }
}

static CFMutableDictionaryRef make_usb_match_dict(uint16_t vid, bool match_pid, uint16_t pid) {
    CFMutableDictionaryRef match = IOServiceMatching(kIOUSBDeviceClassName);
    if (!match) {
        return NULL;
    }
    SInt32 v = (SInt32)vid;
    CFNumberRef cf_vid = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &v);
    if (!cf_vid) {
        CFRelease(match);
        return NULL;
    }
    CFDictionarySetValue(match, CFSTR(kUSBVendorID), cf_vid);
    CFRelease(cf_vid);
    if (match_pid) {
        SInt32 p = (SInt32)pid;
        CFNumberRef cf_pid = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &p);
        if (!cf_pid) {
            CFRelease(match);
            return NULL;
        }
        CFDictionarySetValue(match, CFSTR(kUSBProductID), cf_pid);
        CFRelease(cf_pid);
    } else {
        /*
         * macOS IOUSBHostDevice::matchPropertyTable requires idProduct (or "*")
         * whenever idVendor is specified in the matching dictionary; without it,
         * wildcard notifications for Stage 0 (0xa69c:*) never fire.
         */
        CFDictionarySetValue(match, CFSTR(kUSBProductID), CFSTR("*"));
    }
    return match;
}

static void destroy_daemon_ctx(daemon_ctx_t *ctx) {
    g_sig_write_fd = -1;
    if (ctx->wake_timer) {
        CFRunLoopTimerInvalidate(ctx->wake_timer);
        CFRelease(ctx->wake_timer);
        ctx->wake_timer = NULL;
    }
    if (ctx->sig_src) {
        CFRunLoopRemoveSource(ctx->run_loop, ctx->sig_src, kCFRunLoopDefaultMode);
        CFRelease(ctx->sig_src);
        ctx->sig_src = NULL;
    }
    if (ctx->sig_cffd) {
        CFFileDescriptorInvalidate(ctx->sig_cffd);
        CFRelease(ctx->sig_cffd);
        ctx->sig_cffd = NULL;
    }
    if (ctx->sig_pipe[0] >= 0) {
        close(ctx->sig_pipe[0]);
        ctx->sig_pipe[0] = -1;
    }
    if (ctx->sig_pipe[1] >= 0) {
        close(ctx->sig_pipe[1]);
        ctx->sig_pipe[1] = -1;
    }

    if (ctx->power_src) {
        CFRunLoopRemoveSource(ctx->run_loop, ctx->power_src, kCFRunLoopDefaultMode);
        ctx->power_src = NULL;
    }
    if (ctx->power_notifier != IO_OBJECT_NULL) {
        IODeregisterForSystemPower(&ctx->power_notifier);
        ctx->power_notifier = IO_OBJECT_NULL;
    }
    if (ctx->power_port) {
        IONotificationPortDestroy(ctx->power_port);
        ctx->power_port = NULL;
    }
    if (ctx->power_root != IO_OBJECT_NULL) {
        IOServiceClose(ctx->power_root);
        ctx->power_root = IO_OBJECT_NULL;
    }

    if (ctx->boot_match_iter != IO_OBJECT_NULL) {
        IOObjectRelease(ctx->boot_match_iter);
        ctx->boot_match_iter = IO_OBJECT_NULL;
    }
    if (ctx->stage2_match_iter != IO_OBJECT_NULL) {
        IOObjectRelease(ctx->stage2_match_iter);
        ctx->stage2_match_iter = IO_OBJECT_NULL;
    }
    if (ctx->stage2_term_iter != IO_OBJECT_NULL) {
        IOObjectRelease(ctx->stage2_term_iter);
        ctx->stage2_term_iter = IO_OBJECT_NULL;
    }
    if (ctx->usb_notify_src) {
        CFRunLoopRemoveSource(ctx->run_loop, ctx->usb_notify_src, kCFRunLoopDefaultMode);
        ctx->usb_notify_src = NULL;
    }
    if (ctx->usb_notify_port) {
        IONotificationPortDestroy(ctx->usb_notify_port);
        ctx->usb_notify_port = NULL;
    }

    aic_lock_release(&ctx->lock);
}

static int run_daemon_mode(const aic_fwload_cfg_t *cfg, const char *fw_dir) {
    if (!aic_fw_dir_has_marker(fw_dir)) {
        fprintf(stderr,
                "[fwload] error: firmware directory '%s' missing %s\n",
                fw_dir, AIC_FW_REQUIRED_MARKER);
        return 1;
    }

    char lock_buf[512];
    const char *lock_path = cfg->lock_file_arg;
    if (!lock_path) {
        if (cfg->mode == AIC_FWLOAD_MODE_DAEMON) {
            lock_path = AIC_FWLOAD_DAEMON_LOCK_PATH;
        } else if (aic_fwload_default_watch_lock_path(lock_buf, sizeof(lock_buf)) == 0) {
            lock_path = lock_buf;
        } else {
            fprintf(stderr, "[fwload] error: could not determine watch lock path\n");
            return 1;
        }
    }

    daemon_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.cfg = *cfg;
    snprintf(ctx.fw_dir, sizeof(ctx.fw_dir), "%s", fw_dir);
    aic_telemetry_init(&ctx.telemetry);
    aic_lock_init(&ctx.lock);
    ctx.sig_pipe[0] = -1;
    ctx.sig_pipe[1] = -1;

    pid_t holder = 0;
    if (aic_lock_acquire(&ctx.lock, lock_path, &holder) != 0) {
        fprintf(stderr,
                "[fwload] error: another instance holds lock '%s' (pid=%ld, errno=%d)\n",
                lock_path, (long)holder, errno);
        return 1;
    }

    if (pipe(ctx.sig_pipe) != 0) {
        fprintf(stderr, "[fwload] error: pipe() failed (errno=%d)\n", errno);
        aic_lock_release(&ctx.lock);
        return 1;
    }
    for (int i = 0; i < 2; i++) {
        int flags = fcntl(ctx.sig_pipe[i], F_GETFL, 0);
        if (flags >= 0) {
            (void)fcntl(ctx.sig_pipe[i], F_SETFL, flags | O_NONBLOCK);
        }
        (void)fcntl(ctx.sig_pipe[i], F_SETFD, FD_CLOEXEC);
    }

    ctx.run_loop = CFRunLoopGetCurrent();

    CFFileDescriptorContext fctx = { 0, &ctx, NULL, NULL, NULL };
    ctx.sig_cffd = CFFileDescriptorCreate(kCFAllocatorDefault,
                                          ctx.sig_pipe[0],
                                          false,
                                          on_signal_pipe_cb,
                                          &fctx);
    if (!ctx.sig_cffd) {
        fprintf(stderr, "[fwload] error: CFFileDescriptorCreate failed\n");
        destroy_daemon_ctx(&ctx);
        return 1;
    }
    CFFileDescriptorEnableCallBacks(ctx.sig_cffd, kCFFileDescriptorReadCallBack);
    ctx.sig_src = CFFileDescriptorCreateRunLoopSource(kCFAllocatorDefault, ctx.sig_cffd, 0);
    CFRunLoopAddSource(ctx.run_loop, ctx.sig_src, kCFRunLoopDefaultMode);

    g_sig_write_fd = ctx.sig_pipe[1];
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_posix_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);

    ctx.usb_notify_port = IONotificationPortCreate(kIOMainPortDefault);
    if (!ctx.usb_notify_port) {
        fprintf(stderr, "[fwload] error: IONotificationPortCreate failed\n");
        destroy_daemon_ctx(&ctx);
        return 1;
    }
    ctx.usb_notify_src = IONotificationPortGetRunLoopSource(ctx.usb_notify_port);
    CFRunLoopAddSource(ctx.run_loop, ctx.usb_notify_src, kCFRunLoopDefaultMode);

    CFMutableDictionaryRef m_boot = make_usb_match_dict(AIC_USB_VID_AICS_BOOT, false, 0U);
    CFMutableDictionaryRef m_s2_match = make_usb_match_dict(AIC_USB_VID_AICS_WIFI, true, AIC_USB_PID_WIFI_BT_RUNTIME);
    CFMutableDictionaryRef m_s2_term = make_usb_match_dict(AIC_USB_VID_AICS_WIFI, true, AIC_USB_PID_WIFI_BT_RUNTIME);
    if (!m_boot || !m_s2_match || !m_s2_term) {
        if (m_boot) CFRelease(m_boot);
        if (m_s2_match) CFRelease(m_s2_match);
        if (m_s2_term) CFRelease(m_s2_term);
        fprintf(stderr, "[fwload] error: failed to allocate IOKit matching dictionaries\n");
        destroy_daemon_ctx(&ctx);
        return 1;
    }

    ctx.initial_arming = true;
    kern_return_t kr1 = IOServiceAddMatchingNotification(
        ctx.usb_notify_port, kIOMatchedNotification, m_boot,
        on_boot_usb_matched, &ctx, &ctx.boot_match_iter);
    kern_return_t kr2 = IOServiceAddMatchingNotification(
        ctx.usb_notify_port, kIOMatchedNotification, m_s2_match,
        on_stage2_matched, &ctx, &ctx.stage2_match_iter);
    kern_return_t kr3 = IOServiceAddMatchingNotification(
        ctx.usb_notify_port, kIOTerminatedNotification, m_s2_term,
        on_stage2_terminated, &ctx, &ctx.stage2_term_iter);
    if (kr1 != KERN_SUCCESS || kr2 != KERN_SUCCESS || kr3 != KERN_SUCCESS) {
        fprintf(stderr,
                "[fwload] error: IOServiceAddMatchingNotification failed (0x%x / 0x%x / 0x%x)\n",
                (unsigned)kr1, (unsigned)kr2, (unsigned)kr3);
        destroy_daemon_ctx(&ctx);
        return 1;
    }

    /* Drain initial iterators to arm notifications before first scan. */
    on_boot_usb_matched(&ctx, ctx.boot_match_iter);
    on_stage2_matched(&ctx, ctx.stage2_match_iter);
    on_stage2_terminated(&ctx, ctx.stage2_term_iter);
    ctx.initial_arming = false;

    ctx.power_root = IORegisterForSystemPower(
        &ctx, &ctx.power_port, on_system_power_event, &ctx.power_notifier);
    if (ctx.power_root == IO_OBJECT_NULL || !ctx.power_port || ctx.power_notifier == IO_OBJECT_NULL) {
        fprintf(stderr, "[fwload] error: IORegisterForSystemPower failed\n");
        destroy_daemon_ctx(&ctx);
        return 1;
    }
    ctx.power_src = IONotificationPortGetRunLoopSource(ctx.power_port);
    CFRunLoopAddSource(ctx.run_loop, ctx.power_src, kCFRunLoopDefaultMode);

    char ts[32];
    format_log_ts(ts, sizeof(ts));
    printf("[%s][fwload] %s started (pid=%ld 5G=%s wake_delay=%ums fw_dir=%s lock=%s)\n",
           ts,
           (cfg->mode == AIC_FWLOAD_MODE_DAEMON) ? "daemon" : "watch",
           (long)getpid(),
           cfg->enable_5g ? "enabled" : "disabled",
           cfg->post_wake_delay_ms,
           ctx.fw_dir,
           lock_path);

    dispatch_daemon_cycle(&ctx, AIC_FWLOAD_TRIGGER_STARTUP);
    if (!g_stop_requested) {
        CFRunLoopRun();
    }

    print_telemetry_line("shutdown", &ctx.telemetry);
    destroy_daemon_ctx(&ctx);
    return 0;
}

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n"
           "  --once                   Run a single load check and exit (default)\n"
           "  --watch                  Watch USB arrival & sleep/wake events in foreground\n"
           "  --daemon                 Run in launchd daemon mode (default lock %s)\n"
           "  --fw-dir DIR             Override firmware directory (auto-detects %s then dev path)\n"
           "  --lock-file PATH         Override single-instance flock path\n"
           "  --post-wake-delay-ms MS  Post-wake XHCI settle re-check delay (default 1200)\n"
           "  --usb-settle-timeout-ms MS Max Stage 0 interface/DiskArbitration settle wait (default 1500)\n"
           "  --no-5g                  Disable 5 GHz band patch\n"
           "  --eject-only             Only eject Stage 0 MSC into Stage 1 bootloader (--once)\n"
           "  --dry-run                Parse firmware patch table offline and exit (--once)\n"
           "  --help, -h               Show this help\n",
           prog, AIC_FWLOAD_DAEMON_LOCK_PATH, AIC_FW_INSTALLED_DIR);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    aic_fwload_cfg_t cfg;
    const char *bad_arg = NULL;
    if (aic_fwload_parse_args(argc, argv, &cfg, &bad_arg) != 0) {
        fprintf(stderr, "invalid or conflicting option: %s\n", bad_arg ? bad_arg : "(unknown)");
        return 2;
    }
    if (cfg.show_help) {
        print_usage(argv[0]);
        return 0;
    }

    char fw_dir[PATH_MAX];
    aic_fw_src_t fw_src = AIC_FW_SRC_DEV;
    if (aic_fwload_resolve_fw_dir(cfg.fw_dir_arg,
                                  AIC_FW_INSTALLED_DIR,
                                  AIC_FW_DEFAULT_DIR,
                                  fw_dir, sizeof(fw_dir), &fw_src) != 0) {
        fprintf(stderr, "[fwload] error: could not resolve firmware directory\n");
        return 1;
    }

    if (cfg.dry_run) {
        return run_dry_run(fw_dir, cfg.enable_5g);
    }

    if (cfg.mode == AIC_FWLOAD_MODE_WATCH || cfg.mode == AIC_FWLOAD_MODE_DAEMON) {
        return run_daemon_mode(&cfg, fw_dir);
    }

    aic_cycle_outcome_t outcome = run_fwload_cycle(
        fw_dir, cfg.enable_5g, cfg.eject_only, cfg.usb_settle_timeout_ms, false, NULL);
    return (outcome == AIC_CYCLE_LOADED_OK || outcome == AIC_CYCLE_ALREADY_READY) ? 0 : 1;
}

