#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "lmac_msg.h"
#include "ipc_shared.h"
#include "rwnx_rx.h"
#include "aic8800_abi.h"

#define CHECK_SIZE(linux_t, aic_t) \
    _Static_assert(sizeof(linux_t) == sizeof(aic_t), "sizeof mismatch: " #linux_t " vs " #aic_t)

#define CHECK_OFF(linux_t, l_mem, aic_t, a_mem) \
    _Static_assert(offsetof(linux_t, l_mem) == offsetof(aic_t, a_mem), \
                   "offsetof mismatch: " #linux_t "." #l_mem " vs " #aic_t "." #a_mem)

#define CHECK_CONST(a, b) \
    _Static_assert((uint64_t)(a) == (uint64_t)(b), "constant mismatch: " #a " vs " #b)

/* Core MAC / crypto / HT / VHT / HE structs */
CHECK_SIZE(struct mac_addr, aic_mac_addr_t);
CHECK_SIZE(struct mac_ssid, aic_mac_ssid_t);
CHECK_SIZE(struct mac_chan_def, aic_mac_chan_def_t);
CHECK_SIZE(struct mac_chan_op, aic_mac_chan_op_t);
CHECK_SIZE(struct mac_sec_key, aic_mac_sec_key_t);
CHECK_SIZE(struct mac_htcapability, aic_mac_htcapability_t);
CHECK_SIZE(struct mac_vhtcapability, aic_mac_vhtcapability_t);
CHECK_SIZE(struct mac_hecapability, aic_mac_hecapability_t);

/* TX host descriptors */
CHECK_SIZE(struct hostdesc, aic_hostdesc_t);
CHECK_SIZE(struct txdesc_api, aic_txdesc_api_t);
CHECK_SIZE(struct txdesc_host, aic_txdesc_host_t);
CHECK_OFF(struct hostdesc, packet_len, aic_hostdesc_t, packet_len);
CHECK_OFF(struct hostdesc, eth_dest_addr, aic_hostdesc_t, eth_dest_addr);
CHECK_OFF(struct hostdesc, eth_src_addr, aic_hostdesc_t, eth_src_addr);
CHECK_OFF(struct hostdesc, ethertype, aic_hostdesc_t, ethertype);
CHECK_OFF(struct hostdesc, ac, aic_hostdesc_t, ac);
CHECK_OFF(struct hostdesc, tid, aic_hostdesc_t, tid);
CHECK_OFF(struct hostdesc, vif_idx, aic_hostdesc_t, vif_idx);
CHECK_OFF(struct hostdesc, staid, aic_hostdesc_t, staid);
CHECK_OFF(struct hostdesc, flags, aic_hostdesc_t, flags);

/* RX PHY vector & hardware RX header */
CHECK_SIZE(struct rx_leg_vect, aic_rx_leg_vect_t);
CHECK_SIZE(struct rx_ht_vect, aic_rx_ht_vect_t);
CHECK_SIZE(struct rx_vht_vect, aic_rx_vht_vect_t);
CHECK_SIZE(struct rx_he_vect, aic_rx_he_vect_t);
CHECK_SIZE(struct rx_vector_1, aic_rx_vector_1_t);
CHECK_SIZE(struct rx_vector_2, aic_rx_vector_2_t);
CHECK_SIZE(struct phy_channel_info_desc, aic_phy_channel_info_desc_t);
CHECK_SIZE(struct hw_vect, aic_hw_vect_t);
CHECK_OFF(struct hw_vect, rx_vect2, aic_hw_vect_t, rx_vect2);

CHECK_SIZE(struct hw_rxhdr, aic_hw_rxhdr_t);
CHECK_OFF(struct hw_rxhdr, hwvect, aic_hw_rxhdr_t, hwvect);
CHECK_OFF(struct hw_rxhdr, phy_info, aic_hw_rxhdr_t, phy_info);
CHECK_OFF(struct hw_rxhdr, pattern, aic_hw_rxhdr_t, pattern);

/* LMAC / IPC control messages */
CHECK_SIZE(struct lmac_msg, aic_lmac_msg_hdr_t);
CHECK_OFF(struct lmac_msg, id, aic_lmac_msg_hdr_t, id);
CHECK_OFF(struct lmac_msg, dest_id, aic_lmac_msg_hdr_t, dest_id);
CHECK_OFF(struct lmac_msg, src_id, aic_lmac_msg_hdr_t, src_id);
CHECK_OFF(struct lmac_msg, param_len, aic_lmac_msg_hdr_t, param_len);

CHECK_SIZE(struct ipc_e2a_msg, aic_ipc_e2a_msg_t);
CHECK_OFF(struct ipc_e2a_msg, pattern, aic_ipc_e2a_msg_t, pattern);
CHECK_OFF(struct ipc_e2a_msg, param, aic_ipc_e2a_msg_t, param);

/* MM / SCANU / ME / SM request & indication structs */
CHECK_SIZE(struct phy_cfg_tag, aic_phy_cfg_tag_t);
CHECK_SIZE(struct mm_start_req, aic_mm_start_req_t);
CHECK_SIZE(struct mm_set_channel_req, aic_mm_set_channel_req_t);
CHECK_SIZE(struct mm_set_channel_cfm, aic_mm_set_channel_cfm_t);
CHECK_SIZE(struct mm_add_if_req, aic_mm_add_if_req_t);
CHECK_SIZE(struct mm_add_if_cfm, aic_mm_add_if_cfm_t);
CHECK_SIZE(struct mm_remove_if_req, aic_mm_remove_if_req_t);
CHECK_SIZE(struct mm_set_vif_state_req, aic_mm_set_vif_state_req_t);
CHECK_SIZE(struct mm_version_cfm, aic_mm_version_cfm_t);
CHECK_SIZE(struct mm_key_add_req, aic_mm_key_add_req_t);
CHECK_SIZE(struct mm_key_add_cfm, aic_mm_key_add_cfm_t);
CHECK_SIZE(struct mm_key_del_req, aic_mm_key_del_req_t);
CHECK_SIZE(struct mm_set_ps_mode_req, aic_mm_set_ps_mode_req_t);
CHECK_SIZE(struct mm_set_agg_disable_req, aic_mm_set_agg_disable_req_t);
CHECK_SIZE(struct mm_set_coex_req, aic_mm_set_coex_req_t);

CHECK_SIZE(struct scanu_start_req, aic_scanu_start_req_t);
CHECK_SIZE(struct scanu_start_cfm, aic_scanu_start_cfm_t);
CHECK_SIZE(struct scanu_result_ind, aic_scanu_result_ind_hdr_t);
CHECK_OFF(struct scanu_result_ind, rssi, aic_scanu_result_ind_hdr_t, rssi);
_Static_assert(offsetof(struct scanu_result_ind, payload) == sizeof(aic_scanu_result_ind_hdr_t),
               "scanu_result_ind payload offset vs hdr size");
CHECK_SIZE(struct me_config_req, aic_me_config_req_t);
CHECK_SIZE(struct me_chan_config_req, aic_me_chan_config_req_t);
CHECK_SIZE(struct me_set_control_port_req, aic_me_set_control_port_req_t);
CHECK_SIZE(struct me_rc_set_rate_req, aic_me_rc_set_rate_req_t);

CHECK_SIZE(struct sm_connect_req, aic_sm_connect_req_t);
CHECK_SIZE(struct sm_connect_cfm, aic_sm_connect_cfm_t);
CHECK_SIZE(struct sm_connect_ind, aic_sm_connect_ind_t);
CHECK_SIZE(struct sm_disconnect_req, aic_sm_disconnect_req_t);
CHECK_SIZE(struct sm_disconnect_ind, aic_sm_disconnect_ind_t);
CHECK_SIZE(struct sm_external_auth_required_ind, aic_sm_external_auth_required_ind_t);
CHECK_SIZE(struct sm_external_auth_required_rsp, aic_sm_external_auth_required_rsp_t);

/* Enum / Task / Decrypt constant checks */
CHECK_CONST(DRV_TASK_ID, AIC_DRV_TASK_ID);
CHECK_CONST(IPC_MSGE2A_VALID_PATTERN, AIC_IPC_MSGE2A_VALID_PAT);
CHECK_CONST(TASK_MM, AIC_TASK_MM);
CHECK_CONST(TASK_DBG, AIC_TASK_DBG);
CHECK_CONST(TASK_SCANU, AIC_TASK_SCANU);
CHECK_CONST(TASK_ME, AIC_TASK_ME);
CHECK_CONST(TASK_SM, AIC_TASK_SM);

CHECK_CONST(MM_RESET_REQ, AIC_MM_RESET_REQ);
CHECK_CONST(MM_START_REQ, AIC_MM_START_REQ);
CHECK_CONST(MM_ADD_IF_REQ, AIC_MM_ADD_IF_REQ);
CHECK_CONST(MM_SET_CHANNEL_REQ, AIC_MM_SET_CHANNEL_REQ);
CHECK_CONST(MM_KEY_ADD_REQ, AIC_MM_KEY_ADD_REQ);
CHECK_CONST(MM_SET_COEX_REQ, AIC_MM_SET_COEX_REQ);

CHECK_CONST(RWNX_RX_HD_DECR_UNENC, AIC_RX_HD_DECR_UNENC);
CHECK_CONST(RWNX_RX_HD_DECR_WEP, AIC_RX_HD_DECR_WEP);
CHECK_CONST(RWNX_RX_HD_DECR_TKIP, AIC_RX_HD_DECR_TKIP);
CHECK_CONST(RWNX_RX_HD_DECR_CCMP128, AIC_RX_HD_DECR_CCMP128);
CHECK_CONST(RWNX_RX_HD_DECR_WAPI, AIC_RX_HD_DECR_WAPI);

CHECK_CONST(USB_TYPE_DATA, AIC_USB_TYPE_DATA);
CHECK_CONST(USB_TYPE_CFG, AIC_USB_TYPE_CFG);
CHECK_CONST(USB_TYPE_CFG_CMD_RSP, AIC_USB_TYPE_CFG_CMD_RSP);
CHECK_CONST(USB_TYPE_CFG_DATA_CFM, AIC_USB_TYPE_CFG_DATA_CFM);
CHECK_CONST(USB_TYPE_CFG_PRINT, AIC_USB_TYPE_CFG_PRINT);

CHECK_CONST(MM_SET_RF_CONFIG_REQ, AIC_MM_SET_RF_CONFIG_REQ);
CHECK_CONST(MM_SET_RF_CONFIG_CFM, AIC_MM_SET_RF_CONFIG_CFM);
CHECK_CONST(MM_SET_RF_CALIB_REQ, AIC_MM_SET_RF_CALIB_REQ);
CHECK_CONST(MM_SET_RF_CALIB_CFM, AIC_MM_SET_RF_CALIB_CFM);
CHECK_CONST(MM_SET_MU_EDCA_REQ, AIC_MM_SET_MU_EDCA_REQ);
CHECK_CONST(MM_SET_MU_EDCA_CFM, AIC_MM_SET_MU_EDCA_CFM);
CHECK_CONST(MM_SET_UORA_REQ, AIC_MM_SET_UORA_REQ);
CHECK_CONST(MM_SET_UORA_CFM, AIC_MM_SET_UORA_CFM);
CHECK_CONST(MM_SET_TXOP_RTS_THRES_REQ, AIC_MM_SET_TXOP_RTS_THRES_REQ);
CHECK_CONST(MM_SET_TXOP_RTS_THRES_CFM, AIC_MM_SET_TXOP_RTS_THRES_CFM);
CHECK_CONST(MM_SET_BSS_COLOR_REQ, AIC_MM_SET_BSS_COLOR_REQ);
CHECK_CONST(MM_SET_BSS_COLOR_CFM, AIC_MM_SET_BSS_COLOR_CFM);
CHECK_CONST(MM_GET_MAC_ADDR_REQ, AIC_MM_GET_MAC_ADDR_REQ);
CHECK_CONST(MM_GET_MAC_ADDR_CFM, AIC_MM_GET_MAC_ADDR_CFM);
CHECK_CONST(MM_GET_STA_INFO_REQ, AIC_MM_GET_STA_INFO_REQ);
CHECK_CONST(MM_GET_STA_INFO_CFM, AIC_MM_GET_STA_INFO_CFM);
CHECK_CONST(MM_SET_TXPWR_IDX_LVL_REQ, AIC_MM_SET_TXPWR_IDX_LVL_REQ);
CHECK_CONST(MM_SET_TXPWR_IDX_LVL_CFM, AIC_MM_SET_TXPWR_IDX_LVL_CFM);
CHECK_CONST(MM_SET_TXPWR_OFST_REQ, AIC_MM_SET_TXPWR_OFST_REQ);
CHECK_CONST(MM_SET_TXPWR_OFST_CFM, AIC_MM_SET_TXPWR_OFST_CFM);
CHECK_CONST(MM_SET_STACK_START_REQ, AIC_MM_SET_STACK_START_REQ);
CHECK_CONST(MM_SET_STACK_START_CFM, AIC_MM_SET_STACK_START_CFM);
CHECK_CONST(MM_APM_STALOSS_IND, AIC_MM_APM_STALOSS_IND);
CHECK_CONST(MM_SET_VENDOR_HWCONFIG_REQ, AIC_MM_SET_VENDOR_HWCONFIG_REQ);
CHECK_CONST(MM_SET_VENDOR_HWCONFIG_CFM, AIC_MM_SET_VENDOR_HWCONFIG_CFM);
CHECK_CONST(MM_GET_FW_VERSION_REQ, AIC_MM_GET_FW_VERSION_REQ);
CHECK_CONST(MM_GET_FW_VERSION_CFM, AIC_MM_GET_FW_VERSION_CFM);
CHECK_CONST(MM_SET_RESUME_RESTORE_REQ, AIC_MM_SET_RESUME_RESTORE_REQ);
CHECK_CONST(MM_SET_RESUME_RESTORE_CFM, AIC_MM_SET_RESUME_RESTORE_CFM);
CHECK_CONST(MM_GET_WIFI_DISABLE_REQ, AIC_MM_GET_WIFI_DISABLE_REQ);
CHECK_CONST(MM_GET_WIFI_DISABLE_CFM, AIC_MM_GET_WIFI_DISABLE_CFM);
CHECK_CONST(MM_CFG_RSSI_CFM, AIC_MM_CFG_RSSI_CFM);
CHECK_CONST(MM_SET_VENDOR_SWCONFIG_REQ, AIC_MM_SET_VENDOR_SWCONFIG_REQ);
CHECK_CONST(MM_SET_VENDOR_SWCONFIG_CFM, AIC_MM_SET_VENDOR_SWCONFIG_CFM);
CHECK_CONST(MM_SET_TXPWR_LVL_ADJ_REQ, AIC_MM_SET_TXPWR_LVL_ADJ_REQ);
CHECK_CONST(MM_SET_TXPWR_LVL_ADJ_CFM, AIC_MM_SET_TXPWR_LVL_ADJ_CFM);
CHECK_CONST(MM_RADAR_DETECT_IND, AIC_MM_RADAR_DETECT_IND);
CHECK_CONST(MM_SET_APF_PROG_REQ, AIC_MM_SET_APF_PROG_REQ);
CHECK_CONST(MM_SET_APF_PROG_CFM, AIC_MM_SET_APF_PROG_CFM);
CHECK_CONST(MM_GET_APF_PROG_REQ, AIC_MM_GET_APF_PROG_REQ);
CHECK_CONST(MM_GET_APF_PROG_CFM, AIC_MM_GET_APF_PROG_CFM);
CHECK_CONST(MM_SET_TXPWR_PER_STA_REQ, AIC_MM_SET_TXPWR_PER_STA_REQ);
CHECK_CONST(MM_SET_TXPWR_PER_STA_CFM, AIC_MM_SET_TXPWR_PER_STA_CFM);
CHECK_CONST(MM_GET_STATISTIC_REQ, AIC_MM_GET_STATISTIC_REQ);
CHECK_CONST(MM_GET_STATISTIC_CFM, AIC_MM_GET_STATISTIC_CFM);
CHECK_CONST(MM_VENDOR_SWCONFIG_IND, AIC_MM_VENDOR_SWCONFIG_IND);
CHECK_CONST(MM_FW_PANIC_IND, AIC_MM_FW_PANIC_IND);
CHECK_CONST(MM_FW_ASSERT_IND, AIC_MM_FW_ASSERT_IND);
CHECK_CONST(MM_MAX, AIC_MM_MAX);

CHECK_SIZE(struct mm_set_rf_calib_req, aic_mm_set_rf_calib_req_t);
CHECK_OFF(struct mm_set_rf_calib_req, cal_cfg_24g, aic_mm_set_rf_calib_req_t, cal_cfg_24g);
CHECK_OFF(struct mm_set_rf_calib_req, cal_cfg_5g, aic_mm_set_rf_calib_req_t, cal_cfg_5g);
CHECK_OFF(struct mm_set_rf_calib_req, param_alpha, aic_mm_set_rf_calib_req_t, param_alpha);
CHECK_OFF(struct mm_set_rf_calib_req, bt_calib_en, aic_mm_set_rf_calib_req_t, bt_calib_en);
CHECK_OFF(struct mm_set_rf_calib_req, bt_calib_param, aic_mm_set_rf_calib_req_t, bt_calib_param);
CHECK_OFF(struct mm_set_rf_calib_req, xtal_cap, aic_mm_set_rf_calib_req_t, xtal_cap);
CHECK_OFF(struct mm_set_rf_calib_req, xtal_cap_fine, aic_mm_set_rf_calib_req_t, xtal_cap_fine);

CHECK_SIZE(struct mm_set_rf_calib_cfm, aic_mm_set_rf_calib_cfm_t);
CHECK_OFF(struct mm_set_rf_calib_cfm, rxgain_24g_addr, aic_mm_set_rf_calib_cfm_t, rxgain_24g_addr);
CHECK_OFF(struct mm_set_rf_calib_cfm, txgain_5g_addr, aic_mm_set_rf_calib_cfm_t, txgain_5g_addr);

CHECK_SIZE(struct mm_get_mac_addr_req, aic_mm_get_mac_addr_req_t);
CHECK_OFF(struct mm_get_mac_addr_req, get, aic_mm_get_mac_addr_req_t, get);
CHECK_SIZE(struct mm_get_mac_addr_cfm, aic_mm_get_mac_addr_cfm_t);
CHECK_OFF(struct mm_get_mac_addr_cfm, mac_addr, aic_mm_get_mac_addr_cfm_t, mac_addr);

CHECK_SIZE(txpwr_lvl_conf_t, aic_txpwr_lvl_conf_t);
CHECK_SIZE(txpwr_lvl_conf_v2_t, aic_txpwr_lvl_conf_v2_t);
CHECK_SIZE(txpwr_lvl_conf_v3_t, aic_txpwr_lvl_conf_v3_t);
CHECK_OFF(txpwr_lvl_conf_v3_t, enable, aic_txpwr_lvl_conf_v3_t, enable);
CHECK_OFF(txpwr_lvl_conf_v3_t, pwrlvl_11b_11ag_2g4, aic_txpwr_lvl_conf_v3_t, pwrlvl_11b_11ag_2g4);
CHECK_OFF(txpwr_lvl_conf_v3_t, pwrlvl_11n_11ac_2g4, aic_txpwr_lvl_conf_v3_t, pwrlvl_11n_11ac_2g4);
CHECK_OFF(txpwr_lvl_conf_v3_t, pwrlvl_11ax_2g4, aic_txpwr_lvl_conf_v3_t, pwrlvl_11ax_2g4);
CHECK_OFF(txpwr_lvl_conf_v3_t, pwrlvl_11a_5g, aic_txpwr_lvl_conf_v3_t, pwrlvl_11a_5g);
CHECK_OFF(txpwr_lvl_conf_v3_t, pwrlvl_11n_11ac_5g, aic_txpwr_lvl_conf_v3_t, pwrlvl_11n_11ac_5g);
CHECK_OFF(txpwr_lvl_conf_v3_t, pwrlvl_11ax_5g, aic_txpwr_lvl_conf_v3_t, pwrlvl_11ax_5g);
CHECK_SIZE(txpwr_lvl_conf_v4_t, aic_txpwr_lvl_conf_v4_t);
CHECK_OFF(txpwr_lvl_conf_v4_t, pwrlvl_11ax_6g, aic_txpwr_lvl_conf_v4_t, pwrlvl_11ax_6g);
CHECK_SIZE(struct mm_set_txpwr_lvl_req, aic_mm_set_txpwr_lvl_req_t);

CHECK_SIZE(txpwr_lvl_adj_conf_t, aic_txpwr_lvl_adj_conf_t);
CHECK_SIZE(txpwr_lvl_adj_conf_v2_t, aic_txpwr_lvl_adj_conf_v2_t);
CHECK_SIZE(struct mm_set_txpwr_lvl_adj_req, aic_mm_set_txpwr_lvl_adj_req_t);

CHECK_SIZE(txpwr_ofst_conf_t, aic_txpwr_ofst_conf_t);
CHECK_SIZE(txpwr_ofst2x_conf_t, aic_txpwr_ofst2x_conf_t);
CHECK_SIZE(txpwr_ofst2x_conf_v2_t, aic_txpwr_ofst2x_conf_v2_t);
CHECK_SIZE(txpwr_ofst2x_conf_v3_t, aic_txpwr_ofst2x_conf_v3_t);
CHECK_SIZE(struct mm_set_txpwr_ofst_req, aic_mm_set_txpwr_ofst_req_t);

CHECK_SIZE(struct mm_set_stack_start_req, aic_mm_set_stack_start_req_t);
CHECK_OFF(struct mm_set_stack_start_req, is_stack_start, aic_mm_set_stack_start_req_t, is_stack_start);
CHECK_OFF(struct mm_set_stack_start_req, efuse_valid, aic_mm_set_stack_start_req_t, efuse_valid);
CHECK_OFF(struct mm_set_stack_start_req, set_vendor_info, aic_mm_set_stack_start_req_t, set_vendor_info);
CHECK_OFF(struct mm_set_stack_start_req, fwtrace_redir, aic_mm_set_stack_start_req_t, fwtrace_redir);
CHECK_SIZE(struct mm_set_stack_start_cfm, aic_mm_set_stack_start_cfm_t);
CHECK_OFF(struct mm_set_stack_start_cfm, is_5g_support, aic_mm_set_stack_start_cfm_t, is_5g_support);
CHECK_OFF(struct mm_set_stack_start_cfm, vendor_info, aic_mm_set_stack_start_cfm_t, vendor_info);

CHECK_SIZE(struct mm_get_fw_version_cfm, aic_mm_get_fw_version_cfm_t);
CHECK_OFF(struct mm_get_fw_version_cfm, fw_version_len, aic_mm_get_fw_version_cfm_t, fw_version_len);
CHECK_OFF(struct mm_get_fw_version_cfm, fw_version, aic_mm_get_fw_version_cfm_t, fw_version);

int main(void) {
    printf("ABI parity verified at compile time: 100%% struct sizeof/offsetof/const identity.\n");
    return 0;
}
