/*
 * Copyright (c) 2012-2021 The Linux Foundation. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * DOC: wlan_cm_roam_fw_sync.c
 *
 * Implementation for the FW based roaming sync api interfaces.
 */
#include "qdf_types.h"
#include "wlan_objmgr_psoc_obj.h"
#include "wlan_objmgr_pdev_obj.h"
#include "wlan_objmgr_vdev_obj.h"
#include "wlan_cm_roam_i.h"
#include "wlan_blm_api.h"
#include "wlan_cm_roam_public_struct.h"
#include "wlan_utility.h"
#include "wlan_scan_api.h"
#include "wlan_crypto_global_api.h"
#include "wlan_cm_tgt_if_tx_api.h"
#include "wlan_cm_vdev_api.h"
#include "wlan_p2p_api.h"
#include "wlan_tdls_api.h"
#include "wlan_mlme_vdev_mgr_interface.h"
#include "wlan_pkt_capture_ucfg_api.h"
#include "cds_utils.h"
#ifdef FEATURE_CM_ENABLE
#include "connection_mgr/core/src/wlan_cm_roam.h"
#include "connection_mgr/core/src/wlan_cm_main.h"
#include "connection_mgr/core/src/wlan_cm_sm.h"

QDF_STATUS cm_fw_roam_sync_req(struct wlan_objmgr_psoc *psoc, uint8_t vdev_id,
			       uint8_t *event, uint32_t event_data_len)
{
	QDF_STATUS status;
	struct wlan_objmgr_vdev *vdev;

	vdev = wlan_objmgr_get_vdev_by_id_from_psoc(psoc, vdev_id,
						    WLAN_MLME_SB_ID);

	if (!vdev) {
		mlme_err("vdev object is NULL");
		return QDF_STATUS_E_NULL_VALUE;
	}

	status = cm_sm_deliver_event(vdev, WLAN_CM_SM_EV_ROAM_SYNC,
				     event_data_len, event);

	if (QDF_IS_STATUS_ERROR(status)) {
		mlme_err("EV ROAM SYNC REQ not handled");
		cm_fw_roam_abort_req(psoc, vdev_id);
		cm_roam_stop_req(psoc, vdev_id, REASON_ROAM_SYNCH_FAILED);
	}

	wlan_objmgr_vdev_release_ref(vdev, WLAN_MLME_SB_ID);

	return status;
}

QDF_STATUS
cm_fw_send_vdev_roam_event(struct cnx_mgr *cm_ctx, uint16_t data_len,
			   void *data)
{
	QDF_STATUS status;
	wlan_cm_id cm_id;
	struct wlan_objmgr_psoc *psoc;
	struct cm_roam_req *roam_req = NULL;

	roam_req = cm_get_first_roam_command(cm_ctx->vdev);
	if (!roam_req) {
		mlme_err("Failed to find roam req from list");
		cm_id = CM_ID_INVALID;
		status = QDF_STATUS_E_FAILURE;
		goto error;
	}

	cm_id = roam_req->cm_id;
	psoc = wlan_vdev_get_psoc(cm_ctx->vdev);
	if (!psoc) {
		mlme_err(CM_PREFIX_FMT "Failed to find psoc",
			 CM_PREFIX_REF(roam_req->req.vdev_id,
				       roam_req->cm_id));
		status = QDF_STATUS_E_FAILURE;
		goto error;
	}

	status = wlan_vdev_mlme_sm_deliver_evt(cm_ctx->vdev,
					       WLAN_VDEV_SM_EV_ROAM,
					       data_len,
					       data);

	if (QDF_IS_STATUS_ERROR(status))
		cm_roam_stop_req(psoc, roam_req->req.vdev_id,
				 REASON_ROAM_SYNCH_FAILED);

error:
	if (QDF_IS_STATUS_ERROR(status))
		cm_abort_fw_roam(cm_ctx, cm_id);

	return status;
}

QDF_STATUS
cm_fw_roam_sync_start_ind(struct wlan_objmgr_vdev *vdev,
			  struct roam_offload_synch_ind *roam_synch_data)
{
	QDF_STATUS status;
	struct wlan_objmgr_pdev *pdev;
	struct qdf_mac_addr connected_bssid;
	uint8_t vdev_id;

	pdev = wlan_vdev_get_pdev(vdev);
	vdev_id = wlan_vdev_get_id(vdev);

	/*
	 * Get old bssid as, new AP is not updated yet and do cleanup
	 * for old bssid.
	 */
	wlan_mlme_get_bssid_vdev_id(pdev, vdev_id,
				    &connected_bssid);

	/* Update the BLM that the previous profile has disconnected */
	wlan_blm_update_bssid_connect_params(pdev,
					     connected_bssid,
					     BLM_AP_DISCONNECTED);
	if (IS_ROAM_REASON_STA_KICKOUT(roam_synch_data->roam_reason)) {
		struct reject_ap_info ap_info;

		ap_info.bssid = connected_bssid;
		ap_info.reject_ap_type = DRIVER_AVOID_TYPE;
		ap_info.reject_reason = REASON_STA_KICKOUT;
		ap_info.source = ADDED_BY_DRIVER;
		wlan_blm_add_bssid_to_reject_list(pdev, &ap_info);
	}

	cm_update_scan_mlme_on_roam(vdev, &connected_bssid,
				    SCAN_ENTRY_CON_STATE_NONE);

	status = wlan_cm_roam_state_change(pdev, vdev_id,
					   WLAN_ROAM_SYNCH_IN_PROG,
					   REASON_ROAM_HANDOFF_DONE);

	mlme_cm_osif_roam_sync_ind(vdev);

	return status;
}

void
cm_update_scan_mlme_on_roam(struct wlan_objmgr_vdev *vdev,
			    struct qdf_mac_addr *connected_bssid,
			    enum scan_entry_connection_state state)
{
	struct wlan_objmgr_pdev *pdev;
	struct bss_info bss_info;
	struct mlme_info mlme;
	struct wlan_channel *chan;
	QDF_STATUS status;

	pdev = wlan_vdev_get_pdev(vdev);
	if (!pdev) {
		mlme_err("failed to find pdev");
		return;
	}

	chan = wlan_vdev_get_active_channel(vdev);
	if (!chan) {
		mlme_err("failed to get active channel");
		return;
	}

	status = wlan_vdev_mlme_get_ssid(vdev, bss_info.ssid.ssid,
					 &bss_info.ssid.length);

	if (QDF_IS_STATUS_ERROR(status)) {
		mlme_err("failed to get ssid");
		return;
	}

	mlme.assoc_state = state;
	qdf_copy_macaddr(&bss_info.bssid, connected_bssid);

	bss_info.freq = chan->ch_freq;

	wlan_scan_update_mlme_by_bssinfo(pdev, &bss_info, &mlme);
}

#ifdef WLAN_FEATURE_FILS_SK
static QDF_STATUS
cm_fill_fils_ie(struct wlan_connect_rsp_ies *connect_ies,
		struct roam_offload_synch_ind *roam_synch_data)
{
	struct fils_connect_rsp_params *fils_ie;

	if (!roam_synch_data->hlp_data_len)
		return QDF_STATUS_SUCCESS;

	connect_ies->fils_ie = qdf_mem_malloc(sizeof(*fils_ie));
	if (!connect_ies->fils_ie)
		return QDF_STATUS_E_NOMEM;

	fils_ie = connect_ies->fils_ie;
	cds_copy_hlp_info(&roam_synch_data->dst_mac,
			  &roam_synch_data->src_mac,
			  roam_synch_data->hlp_data_len,
			  roam_synch_data->hlp_data,
			  &fils_ie->dst_mac,
			  &fils_ie->src_mac,
			  &fils_ie->hlp_data_len,
			  fils_ie->hlp_data);

	fils_ie->fils_seq_num = roam_synch_data->next_erp_seq_num;

	return QDF_STATUS_SUCCESS;
}
#else
static inline QDF_STATUS
cm_fill_fils_ie(struct wlan_connect_rsp_ies *connect_ies,
		struct roam_offload_synch_ind *roam_synch_data)
{
	return QDF_STATUS_SUCCESS;
}
#endif

static QDF_STATUS
cm_populate_connect_ies(struct roam_offload_synch_ind *roam_synch_data,
			struct cm_vdev_join_rsp *rsp)
{
	struct wlan_connect_rsp_ies *connect_ies;
	uint8_t *bcn_probe_rsp_ptr;
	uint8_t *reassoc_rsp_ptr;
	uint8_t *reassoc_req_ptr;

	connect_ies = &rsp->connect_rsp.connect_ies;

	/* Beacon/Probe Rsp frame */
	if (roam_synch_data->beaconProbeRespLength) {
		connect_ies->bcn_probe_rsp.len =
			roam_synch_data->beaconProbeRespLength;
		bcn_probe_rsp_ptr = (uint8_t *)roam_synch_data +
					roam_synch_data->beaconProbeRespOffset;

		connect_ies->bcn_probe_rsp.ptr =
			qdf_mem_malloc(connect_ies->bcn_probe_rsp.len);
		if (!connect_ies->bcn_probe_rsp.ptr)
			return QDF_STATUS_E_NOMEM;
		qdf_mem_copy(connect_ies->bcn_probe_rsp.ptr, bcn_probe_rsp_ptr,
			     connect_ies->bcn_probe_rsp.len);
	}

	/* ReAssoc Rsp IE data */
	if (roam_synch_data->reassocRespLength >
	    sizeof(struct wlan_frame_hdr)) {
		connect_ies->assoc_rsp.len =
				roam_synch_data->reassocRespLength -
				sizeof(struct wlan_frame_hdr);
		reassoc_rsp_ptr = (uint8_t *)roam_synch_data +
				  roam_synch_data->reassocRespOffset +
				  sizeof(struct wlan_frame_hdr);
		connect_ies->assoc_rsp.ptr =
			qdf_mem_malloc(connect_ies->assoc_rsp.len);
		if (!connect_ies->assoc_rsp.ptr)
			return QDF_STATUS_E_NOMEM;

		qdf_mem_copy(connect_ies->assoc_rsp.ptr, reassoc_rsp_ptr,
			     connect_ies->assoc_rsp.len);
	}

	/* ReAssoc Req IE data */
	if (roam_synch_data->reassoc_req_length >
	    sizeof(struct wlan_frame_hdr)) {
		connect_ies->assoc_req.len =
				roam_synch_data->reassoc_req_length -
				sizeof(struct wlan_frame_hdr);
		reassoc_req_ptr = (uint8_t *)roam_synch_data +
				  roam_synch_data->reassoc_req_offset +
				  sizeof(struct wlan_frame_hdr);
		connect_ies->assoc_req.ptr =
			qdf_mem_malloc(connect_ies->assoc_req.len);
		if (!connect_ies->assoc_req.ptr)
			return QDF_STATUS_E_NOMEM;
		qdf_mem_copy(connect_ies->assoc_req.ptr, reassoc_req_ptr,
			     connect_ies->assoc_req.len);
	}
	rsp->connect_rsp.is_ft = roam_synch_data->is_ft_im_roam;

	cm_fill_fils_ie(connect_ies, roam_synch_data);

	return QDF_STATUS_SUCCESS;
}

#ifdef FEATURE_WLAN_ESE
static QDF_STATUS
cm_copy_tspec_ie(struct cm_vdev_join_rsp *rsp,
		 struct roam_offload_synch_ind *roam_synch_data)
{
	if (roam_synch_data->tspec_len) {
		rsp->tspec_ie.len = roam_synch_data->tspec_len;
		rsp->tspec_ie.ptr =
			qdf_mem_malloc(rsp->tspec_ie.len);
		if (!rsp->tspec_ie.ptr)
			return QDF_STATUS_E_NOMEM;

		qdf_mem_copy(rsp->tspec_ie.ptr,
			     roam_synch_data->ric_tspec_data +
			     roam_synch_data->ric_data_len,
			     rsp->tspec_ie.len);
	}

	return QDF_STATUS_SUCCESS;
}
#else
static inline QDF_STATUS
cm_copy_tspec_ie(struct cm_vdev_join_rsp *rsp,
		 struct roam_offload_synch_ind *roam_synch_data)
{
	return QDF_STATUS_SUCCESS;
}
#endif

static QDF_STATUS
cm_fill_roam_info(struct roam_offload_synch_ind *roam_synch_data,
		  struct cm_vdev_join_rsp *rsp, wlan_cm_id cm_id)
{
	struct wlan_roam_sync_info *roaming_info;
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	rsp->connect_rsp.roaming_info = qdf_mem_malloc(sizeof(*roaming_info));
	if (!rsp->connect_rsp.roaming_info)
			return QDF_STATUS_E_NOMEM;
	rsp->connect_rsp.vdev_id = roam_synch_data->roamed_vdev_id;
	qdf_copy_macaddr(&rsp->connect_rsp.bssid, &roam_synch_data->bssid);

	rsp->connect_rsp.is_reassoc = true;
	rsp->connect_rsp.connect_status = QDF_STATUS_SUCCESS;
	rsp->connect_rsp.cm_id = cm_id;
	rsp->connect_rsp.freq = roam_synch_data->chan_freq;
	rsp->nss = roam_synch_data->nss;

	if (roam_synch_data->ric_data_len) {
		rsp->ric_resp_ie.len = roam_synch_data->ric_data_len;
		rsp->ric_resp_ie.ptr =
			qdf_mem_malloc(rsp->ric_resp_ie.len);
		if (!rsp->ric_resp_ie.ptr)
			return QDF_STATUS_E_NOMEM;

		qdf_mem_copy(rsp->ric_resp_ie.ptr,
			     roam_synch_data->ric_tspec_data,
			     rsp->ric_resp_ie.len);
	}
	cm_copy_tspec_ie(rsp, roam_synch_data);

	status = cm_populate_connect_ies(roam_synch_data, rsp);
	if (QDF_IS_STATUS_ERROR(status))
		return status;

	roaming_info = rsp->connect_rsp.roaming_info;
	roaming_info->auth_status = roam_synch_data->auth_status;
	roaming_info->kck_len = roam_synch_data->kck_len;
	if (roaming_info->kck_len)
		qdf_mem_copy(roaming_info->kck, roam_synch_data->kck,
			     roam_synch_data->kck_len);
	roaming_info->kek_len = roam_synch_data->kek_len;
	if (roaming_info->kek_len)
		qdf_mem_copy(roaming_info->kek, roam_synch_data->kek,
			     roam_synch_data->kek_len);
	qdf_mem_copy(roaming_info->replay_ctr, roam_synch_data->replay_ctr,
		     REPLAY_CTR_LEN);
	roaming_info->roam_reason =
		roam_synch_data->roam_reason & ROAM_REASON_MASK;
	roaming_info->subnet_change_status =
			CM_GET_SUBNET_STATUS(roaming_info->roam_reason);
	roaming_info->pmk_len = roam_synch_data->pmk_len;
	if (roaming_info->pmk_len)
		qdf_mem_copy(roaming_info->pmk, roam_synch_data->pmk,
			     roaming_info->pmk_len);

	qdf_mem_copy(roaming_info->pmkid, roam_synch_data->pmkid,
		     PMKID_LEN);
	roaming_info->update_erp_next_seq_num =
			roam_synch_data->update_erp_next_seq_num;
	roaming_info->next_erp_seq_num = roam_synch_data->next_erp_seq_num;

	return status;
}

static QDF_STATUS cm_process_roam_keys(struct wlan_objmgr_vdev *vdev,
				       struct cm_vdev_join_rsp *rsp,
				       wlan_cm_id cm_id)
{
	struct wlan_objmgr_psoc *psoc;
	struct wlan_objmgr_pdev *pdev;
	struct wlan_roam_sync_info *roaming_info;
	uint8_t vdev_id = wlan_vdev_get_id(vdev);
	struct cm_roam_values_copy config;
	uint8_t mdie_present;
	struct wlan_mlme_psoc_ext_obj *mlme_obj;
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	int32_t akm;

	pdev = wlan_vdev_get_pdev(vdev);
	if (!pdev) {
		mlme_err(CM_PREFIX_FMT "Failed to find pdev",
			 CM_PREFIX_REF(vdev_id, cm_id));
		status = QDF_STATUS_E_FAILURE;
		goto end;
	}
	psoc = wlan_pdev_get_psoc(pdev);
	if (!psoc) {
		mlme_err(CM_PREFIX_FMT "Failed to find psoc",
			 CM_PREFIX_REF(vdev_id, cm_id));
		status = QDF_STATUS_E_FAILURE;
		goto end;
	}
	mlme_obj = mlme_get_psoc_ext_obj(psoc);
	if (!mlme_obj) {
		mlme_err(CM_PREFIX_FMT "Failed to mlme psoc obj",
			 CM_PREFIX_REF(vdev_id, cm_id));
		status = QDF_STATUS_E_FAILURE;
		goto end;
	}

	roaming_info = rsp->connect_rsp.roaming_info;
	akm = wlan_crypto_get_param(vdev,
				    WLAN_CRYPTO_PARAM_KEY_MGMT);

	/*
	 * Encryption keys for new connection are obtained as follows:
	 * auth_status = CSR_ROAM_AUTH_STATUS_AUTHENTICATED
	 * Open - No keys required.
	 * Static WEP - Firmware copies keys from old AP to new AP.
	 * Fast roaming authentications e.g. PSK, FT, CCKM - firmware
	 *		supplicant obtains them through 4-way handshake.
	 *
	 * auth_status = CSR_ROAM_AUTH_STATUS_CONNECTED
	 * All other authentications - Host supplicant performs EAPOL
	 *	with AP after this point and sends new keys to the driver.
	 *	Driver starts wait_for_key timer for that purpose.
	 * Allow cm_lookup_pmkid_using_bssid() if akm is SAE/OWE since
	 * SAE/OWE roaming uses hybrid model and eapol is offloaded to
	 * supplicant unlike in WPA2 802.1x case, after 8 way handshake
	 * the __wlan_hdd_cfg80211_keymgmt_set_key ->sme_roam_set_psk_pmk()
	 * will get called after roam synch complete to update the
	 * session->psk_pmk, but in SAE/OWE roaming this sequence is not
	 * present and set_pmksa will come before roam synch indication &
	 * eapol. So the session->psk_pmk will be stale in PMKSA cached
	 * SAE/OWE roaming case.
	 */

	if (roaming_info->auth_status == ROAM_AUTH_STATUS_AUTHENTICATED ||
	    QDF_HAS_PARAM(akm, WLAN_CRYPTO_KEY_MGMT_SAE) ||
	    QDF_HAS_PARAM(akm, WLAN_CRYPTO_KEY_MGMT_OWE)) {
		struct wlan_crypto_pmksa *pmkid_cache;

		cm_csr_set_ss_none(vdev_id);
		/*
		 * If authStatus is AUTHENTICATED, then we have done successful
		 * 4 way handshake in FW using the cached PMKID.
		 * However, the session->psk_pmk has the PMK of the older AP
		 * as set_key is not received from supplicant.
		 * When any RSO command is sent for the current AP, the older
		 * AP's PMK is sent to the FW which leads to incorrect PMK and
		 * leads to 4 way handshake failure when roaming happens to
		 * this AP again.
		 * Check if a PMK cache exists for the roamed AP and update
		 * it into the session pmk.
		 */
		pmkid_cache = qdf_mem_malloc(sizeof(*pmkid_cache));
		if (!pmkid_cache) {
			status = QDF_STATUS_E_NOMEM;
			mlme_err(CM_PREFIX_FMT "Mem alloc failed",
				 CM_PREFIX_REF(vdev_id, cm_id));
			goto end;
		}
		wlan_vdev_get_bss_peer_mac(vdev, &pmkid_cache->bssid);
		mlme_debug(CM_PREFIX_FMT "Trying to find PMKID for "
			   QDF_MAC_ADDR_FMT " AKM Type:%d",
			   CM_PREFIX_REF(vdev_id, cm_id),
			   QDF_MAC_ADDR_REF(pmkid_cache->bssid.bytes), akm);

		wlan_cm_roam_cfg_get_value(psoc, vdev_id,
					   MOBILITY_DOMAIN, &config);
		mdie_present = config.bool_value;

		if (cm_lookup_pmkid_using_bssid(psoc,
						vdev_id,
						pmkid_cache)) {
			wlan_cm_set_psk_pmk(pdev, vdev_id,
					    pmkid_cache->pmk,
					    pmkid_cache->pmk_len);
			mlme_debug(CM_PREFIX_FMT "pmkid found for "
				   QDF_MAC_ADDR_FMT " len %d",
				   CM_PREFIX_REF(vdev_id, cm_id),
				   QDF_MAC_ADDR_REF(pmkid_cache->bssid.bytes),
				   pmkid_cache->pmk_len);
		} else {
			mlme_debug(CM_PREFIX_FMT "PMKID Not found in cache for "
				   QDF_MAC_ADDR_FMT,
				   CM_PREFIX_REF(vdev_id, cm_id),
				   QDF_MAC_ADDR_REF(pmkid_cache->bssid.bytes));
			/*
			 * In FT roam when the CSR lookup fails then the PMK
			 * details from the roam sync indication will be
			 * updated to Session/PMK cache. This will result in
			 * having multiple PMK cache entries for the same MDID,
			 * So do not add the PMKSA cache entry in all the
			 * FT-Roam cases.
			 */
			if (!cm_is_auth_type_11r(mlme_obj, vdev,
						 mdie_present) &&
				roaming_info->pmk_len) {
				qdf_mem_zero(pmkid_cache, sizeof(*pmkid_cache));
				wlan_cm_set_psk_pmk(pdev, vdev_id,
						    roaming_info->pmk,
						    roaming_info->pmk_len);
				wlan_vdev_get_bss_peer_mac(vdev,
							   &pmkid_cache->bssid);
				qdf_mem_copy(pmkid_cache->pmkid,
					     roaming_info->pmkid, PMKID_LEN);
				qdf_mem_copy(pmkid_cache->pmk,
					     roaming_info->pmk,
					     roaming_info->pmk_len);
				pmkid_cache->pmk_len = roaming_info->pmk_len;

				wlan_crypto_set_del_pmksa(vdev, pmkid_cache,
							  true);
			}
		}
		qdf_mem_zero(pmkid_cache, sizeof(*pmkid_cache));
		qdf_mem_free(pmkid_cache);
	} else {
		cm_update_wait_for_key_timer(vdev, vdev_id,
					     WAIT_FOR_KEY_TIMEOUT_PERIOD);
	}
end:
	return status;
}

static void
cm_update_scan_db_on_roam_success(struct wlan_objmgr_vdev *vdev,
				  struct wlan_cm_connect_resp *resp,
				  struct roam_offload_synch_ind *roam_synch_data,
				  wlan_cm_id cm_id)
{
	struct cnx_mgr *cm_ctx;

	cm_ctx = cm_get_cm_ctx(vdev);
	if (!cm_ctx)
		return;

	cm_inform_bcn_probe(cm_ctx,
			    resp->connect_ies.bcn_probe_rsp.ptr,
			    resp->connect_ies.bcn_probe_rsp.len,
			    resp->freq,
			    roam_synch_data->rssi,
			    cm_id);

	cm_update_scan_mlme_on_roam(vdev, &resp->bssid,
				    SCAN_ENTRY_CON_STATE_ASSOC);
}

QDF_STATUS
cm_fw_roam_sync_propagation(struct wlan_objmgr_psoc *psoc, uint8_t vdev_id,
			    struct roam_offload_synch_ind *roam_synch_data)
{
	QDF_STATUS status;
	struct wlan_objmgr_vdev *vdev;
	struct cnx_mgr *cm_ctx;
	struct cm_roam_req *roam_req = NULL;
	struct cm_vdev_join_rsp *rsp = NULL;
	wlan_cm_id cm_id;
	struct wlan_objmgr_pdev *pdev;
	struct wlan_cm_connect_resp *connect_rsp;

	vdev = wlan_objmgr_get_vdev_by_id_from_psoc(psoc, vdev_id,
						    WLAN_MLME_SB_ID);

	if (!vdev) {
		mlme_err("vdev object is NULL");
		return QDF_STATUS_E_NULL_VALUE;
	}

	pdev = wlan_vdev_get_pdev(vdev);
	if (!pdev) {
		status = QDF_STATUS_E_FAILURE;
		goto rel_ref;
	}

	cm_ctx = cm_get_cm_ctx(vdev);
	if (!cm_ctx) {
		status = QDF_STATUS_E_FAILURE;
		goto rel_ref;
	}

	roam_req = cm_get_first_roam_command(vdev);
	if (!roam_req) {
		mlme_err("Failed to find roam req from list");
		cm_id = CM_ID_INVALID;
		status = QDF_STATUS_E_FAILURE;
		goto error;
	}

	cm_id = roam_req->cm_id;
	rsp = qdf_mem_malloc(sizeof(struct cm_vdev_join_rsp));
	if (!rsp) {
		status = QDF_STATUS_E_NOMEM;
		goto error;
	}
	status = cm_fill_roam_info(roam_synch_data, rsp, cm_id);
	if (QDF_IS_STATUS_ERROR(status)) {
		mlme_err(CM_PREFIX_FMT " fail to prepare rsp",
			 CM_PREFIX_REF(vdev_id, cm_id));
		goto error;
	}

	connect_rsp = &rsp->connect_rsp;
	cm_update_scan_db_on_roam_success(vdev, connect_rsp,
					  roam_synch_data, cm_id);

	cm_csr_roam_sync_rsp(vdev, rsp);
	cm_process_roam_keys(vdev, rsp, cm_id);

	mlme_cm_osif_connect_complete(vdev, connect_rsp);
	cm_if_mgr_inform_connect_complete(cm_ctx->vdev,
					  connect_rsp->connect_status);
	cm_inform_blm_connect_complete(cm_ctx->vdev, connect_rsp);
	cm_connect_info(vdev, true, &connect_rsp->bssid, &connect_rsp->ssid,
			connect_rsp->freq);
	wlan_tdls_notify_sta_connect(vdev_id,
				     mlme_get_tdls_chan_switch_prohibited(vdev),
				     mlme_get_tdls_prohibited(vdev), vdev);
	wlan_p2p_status_connect(vdev);

	if (!cm_csr_is_ss_wait_for_key(vdev_id)) {
		mlme_debug(CM_PREFIX_FMT "WLAN link up with AP = "
			   QDF_MAC_ADDR_FMT,
			   CM_PREFIX_REF(vdev_id, cm_id),
			   QDF_MAC_ADDR_REF(connect_rsp->bssid.bytes));
		cm_roam_start_init_on_connect(pdev, vdev_id);
	}

	wlan_cm_tgt_send_roam_sync_complete_cmd(psoc, vdev_id);
	status = cm_sm_deliver_event_sync(cm_ctx, WLAN_CM_SM_EV_ROAM_DONE,
					  sizeof(*roam_synch_data),
					  roam_synch_data);
	if (QDF_IS_STATUS_ERROR(status)) {
		mlme_err(CM_PREFIX_FMT " fail to post WLAN_CM_SM_EV_ROAM_DONE",
			 CM_PREFIX_REF(vdev_id, cm_id));
		goto error;
	}
	mlme_cm_osif_roam_complete(vdev);
	mlme_debug(CM_PREFIX_FMT, CM_PREFIX_REF(vdev_id, cm_id));
	cm_remove_cmd(cm_ctx, &cm_id);
	status = QDF_STATUS_SUCCESS;
error:
	if (rsp)
		wlan_cm_free_connect_rsp(rsp);

	if (QDF_IS_STATUS_ERROR(status)) {
		cm_roam_stop_req(psoc, vdev_id, REASON_ROAM_SYNCH_FAILED);
		cm_abort_fw_roam(cm_ctx, cm_id);
	}
rel_ref:
	wlan_objmgr_vdev_release_ref(vdev, WLAN_MLME_SB_ID);

	return status;
}

QDF_STATUS cm_fw_roam_complete(struct cnx_mgr *cm_ctx, void *data)
{
	struct roam_offload_synch_ind *roam_synch_data;
	struct wlan_objmgr_pdev *pdev;
	struct wlan_objmgr_psoc *psoc;
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	uint8_t vdev_id;

	roam_synch_data = (struct roam_offload_synch_ind *)data;
	vdev_id = wlan_vdev_get_id(cm_ctx->vdev);

	pdev = wlan_vdev_get_pdev(cm_ctx->vdev);
	if (!pdev) {
		mlme_err("Failed to find pdev");
		status = QDF_STATUS_E_FAILURE;
		goto end;
	}

	psoc = wlan_pdev_get_psoc(pdev);
	if (!pdev) {
		mlme_err("Failed to find psoc");
		status = QDF_STATUS_E_FAILURE;
		goto end;
	}

	/* Handle one race condition that if candidate is already
	 *selected & FW has gone ahead with roaming or about to go
	 * ahead when set_band comes, it will be complicated for FW
	 * to stop the current roaming. Instead, host will check the
	 * roam sync to make sure the new AP is not on disable freq
	 * or disconnect the AP.
	 */
	if (wlan_reg_is_disable_for_freq(pdev, roam_synch_data->chan_freq)) {
		cm_disconnect(psoc, vdev_id, CM_ROAM_DISCONNECT,
			      REASON_OPER_CHANNEL_BAND_CHANGE, NULL);
		status = QDF_STATUS_E_FAILURE;
		goto end;
	}

	/*
	 * Following operations need to be done once roam sync
	 * completion is sent to FW, hence called here:
	 * 1) Firmware has already updated DBS policy. Update connection
	 *	  table in the host driver.
	 * 2) Force SCC switch if needed
	 */
	/* first update connection info from wma interface */
	policy_mgr_update_connection_info(psoc, vdev_id);
	/* then update remaining parameters from roam sync ctx */
	policy_mgr_hw_mode_transition_cb(
		roam_synch_data->hw_mode_trans_ind.old_hw_mode_index,
		roam_synch_data->hw_mode_trans_ind.new_hw_mode_index,
		roam_synch_data->hw_mode_trans_ind.num_vdev_mac_entries,
		roam_synch_data->hw_mode_trans_ind.vdev_mac_map,
		psoc);

	cm_check_and_set_sae_single_pmk_cap(psoc, vdev_id);

	if (ucfg_pkt_capture_get_pktcap_mode(psoc))
		ucfg_pkt_capture_record_channel(cm_ctx->vdev);

	if (WLAN_REG_IS_5GHZ_CH_FREQ(roam_synch_data->chan_freq)) {
		wlan_cm_set_disable_hi_rssi(pdev,
					    vdev_id, true);
		mlme_debug("Disabling HI_RSSI, AP freq=%d rssi %d",
			   roam_synch_data->chan_freq, roam_synch_data->rssi);
	} else {
		wlan_cm_set_disable_hi_rssi(pdev,
					    vdev_id, false);
	}

	if (roam_synch_data->auth_status == ROAM_AUTH_STATUS_AUTHENTICATED)
		wlan_cm_roam_state_change(pdev, vdev_id,
					  WLAN_ROAM_RSO_ENABLED,
					  REASON_CONNECT);
	else
		/*
		 * STA is just in associated state here, RSO
		 * enable will be sent once EAP & EAPOL will be done by
		 * user-space and after set key response
		 * is received.
		 */
		wlan_cm_roam_state_change(pdev, vdev_id,
					  WLAN_ROAM_INIT,
					  REASON_CONNECT);
end:
	return status;
}
#endif // FEATURE_CM_ENABLE
