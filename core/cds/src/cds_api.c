/*
 * Copyright (c) 2014-2015 The Linux Foundation. All rights reserved.
 *
 * Previously licensed under the ISC license by Qualcomm Atheros, Inc.
 *
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
 * This file was originally distributed by Qualcomm Atheros, Inc.
 * under proprietary terms before Copyright ownership was assigned
 * to the Linux Foundation.
 */

/**
 * DOC: cds_api.c
 *
 * Connectivity driver services APIs
 */

#include <cds_mq.h>
#include "cds_sched.h"
#include <cds_api.h>
#include "sir_types.h"
#include "sir_api.h"
#include "sir_mac_prot_def.h"
#include "sme_api.h"
#include "mac_init_api.h"
#include "wlan_qct_sys.h"
#include "wlan_hdd_misc.h"
#include "i_cds_packet.h"
#include "cds_reg_service.h"
#include "wma_types.h"
#include "wlan_hdd_main.h"
#include <linux/vmalloc.h>
#ifdef CONFIG_CNSS
#include <net/cnss.h>
#endif

#include "sap_api.h"
#include "cdf_trace.h"
#include "bmi.h"
#include "ol_fw.h"
#include "ol_if_athvar.h"
#include "hif.h"

#include "cds_utils.h"
#include "wlan_logging_sock_svc.h"
#include "wma.h"

#include "wlan_hdd_ipa.h"
/* Preprocessor Definitions and Constants */

/* Maximum number of cds message queue get wrapper failures to cause panic */
#define CDS_WRAPPER_MAX_FAIL_COUNT (CDS_CORE_MAX_MESSAGES * 3)

/* Data definitions */
static cds_context_type g_cds_context;
static p_cds_contextType gp_cds_context;
static struct __cdf_device g_cdf_ctx;

/* Debug variable to detect MC thread stuck */
static atomic_t cds_wrapper_empty_count;

static uint8_t cds_multicast_logging;

void cds_sys_probe_thread_cback(void *pUserData);

/**
 * cds_alloc_global_context() - allocate CDS global context
 * @p_cds_context: A pointer to where to store the CDS Context
 *
 * cds_alloc_global_context() function allocates the CDS global Context,
 * but does not initialize all the members. This overal initialization will
 * happen at cds_open().
 *
 * Return: CDF status
 */
CDF_STATUS cds_alloc_global_context(v_CONTEXT_t *p_cds_context)
{
	if (p_cds_context == NULL)
		return CDF_STATUS_E_FAILURE;

	/* allocate the CDS Context */
	*p_cds_context = NULL;
	gp_cds_context = &g_cds_context;

	cdf_mem_zero(gp_cds_context, sizeof(cds_context_type));
	*p_cds_context = gp_cds_context;

	gp_cds_context->cdf_ctx = &g_cdf_ctx;
	cdf_mem_zero(&g_cdf_ctx, sizeof(g_cdf_ctx));

	/* initialize the spinlock */
	cdf_trace_spin_lock_init();
	/* it is the right time to initialize MTRACE structures */
#if defined(TRACE_RECORD)
	cdf_trace_init();
#endif

	cdf_dp_trace_init();
	return CDF_STATUS_SUCCESS;
} /* cds_alloc_global_context() */

/**
 * cds_free_global_context() - free CDS global context
 * @p_cds_context: A pointer to where the CDS Context was stored
 *
 * cds_free_global_context() function frees the CDS Context.
 *
 * Return: CDF status
 */
CDF_STATUS cds_free_global_context(v_CONTEXT_t *p_cds_context)
{
	CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_INFO,
		  "%s: De-allocating the CDS Context", __func__);

	if ((p_cds_context == NULL) || (*p_cds_context == NULL)) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: vOS Context is Null", __func__);
		return CDF_STATUS_E_FAILURE;
	}

	if (gp_cds_context != *p_cds_context) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: Context mismatch", __func__);
		return CDF_STATUS_E_FAILURE;
	}
	gp_cds_context->cdf_ctx = NULL;
	*p_cds_context = gp_cds_context = NULL;

	return CDF_STATUS_SUCCESS;
} /* cds_free_global_context() */

#ifdef WLAN_FEATURE_NAN
/**
 * cds_set_nan_enable() - set nan enable flag in mac open param
 * @wma_handle: Pointer to mac open param
 * @hdd_ctx: Pointer to hdd context
 *
 * Return: none
 */
static void cds_set_nan_enable(tMacOpenParameters *param,
					hdd_context_t *hdd_ctx)
{
	param->is_nan_enabled = hdd_ctx->config->enable_nan_support;
}
#else
static void cds_set_nan_enable(tMacOpenParameters *param,
					hdd_context_t *pHddCtx)
{
}
#endif

/**
 * cds_open() - open the CDS Module
 * @p_cds_context: A pointer to where the CDS Context was stored
 * @hddContextSize: Size of the HDD context to allocate.
 *
 * cds_open() function opens the CDS Scheduler
 * Upon successful initialization:
 * - All CDS submodules should have been initialized
 *
 * - The CDS scheduler should have opened
 *
 * - All the WLAN SW components should have been opened. This includes
 * SYS, MAC, SME, WMA and TL.
 *
 * Return: CDF status
 */
CDF_STATUS cds_open(v_CONTEXT_t *p_cds_context, uint32_t hddContextSize)
{
	CDF_STATUS cdf_status = CDF_STATUS_SUCCESS;
	int iter = 0;
	tSirRetStatus sirStatus = eSIR_SUCCESS;
	tMacOpenParameters mac_openParms;
	cdf_device_t cdf_ctx;
	HTC_INIT_INFO htcInfo;
	struct ol_softc *scn;
	void *HTCHandle;
	hdd_context_t *pHddCtx;

	CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_INFO_HIGH,
		  "%s: Opening CDS", __func__);

	if (NULL == gp_cds_context) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_FATAL,
			  "%s: Trying to open CDS without a PreOpen", __func__);
		CDF_ASSERT(0);
		return CDF_STATUS_E_FAILURE;
	}

	/* Initialize the timer module */
	cdf_timer_module_init();

	/* Initialize bug reporting structure */
	cds_init_log_completion();

	/* Initialize the probe event */
	if (cdf_event_init(&gp_cds_context->ProbeEvent) != CDF_STATUS_SUCCESS) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_FATAL,
			  "%s: Unable to init probeEvent", __func__);
		CDF_ASSERT(0);
		return CDF_STATUS_E_FAILURE;
	}
	if (cdf_event_init(&(gp_cds_context->wmaCompleteEvent)) !=
	    CDF_STATUS_SUCCESS) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_FATAL,
			  "%s: Unable to init wmaCompleteEvent", __func__);
		CDF_ASSERT(0);
		goto err_probe_event;
	}

	/* Initialize the free message queue */
	cdf_status = cds_mq_init(&gp_cds_context->freeVosMq);
	if (!CDF_IS_STATUS_SUCCESS(cdf_status)) {
		/* Critical Error ...  Cannot proceed further */
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_FATAL,
			  "%s: Failed to initialize CDS free message queue",
			  __func__);
		CDF_ASSERT(0);
		goto err_wma_complete_event;
	}

	for (iter = 0; iter < CDS_CORE_MAX_MESSAGES; iter++) {
		(gp_cds_context->aMsgWrappers[iter]).pVosMsg =
			&(gp_cds_context->aMsgBuffers[iter]);
		INIT_LIST_HEAD(&gp_cds_context->aMsgWrappers[iter].msgNode);
		cds_mq_put(&gp_cds_context->freeVosMq,
			   &(gp_cds_context->aMsgWrappers[iter]));
	}

	/* Now Open the CDS Scheduler */
	cdf_status = cds_sched_open(gp_cds_context, &gp_cds_context->cdf_sched,
				    sizeof(cds_sched_context));

	if (!CDF_IS_STATUS_SUCCESS(cdf_status)) {
		/* Critical Error ...  Cannot proceed further */
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_FATAL,
			  "%s: Failed to open CDS Scheduler", __func__);
		CDF_ASSERT(0);
		goto err_msg_queue;
	}

	pHddCtx = (hdd_context_t *) (gp_cds_context->pHDDContext);
	if ((NULL == pHddCtx) || (NULL == pHddCtx->config)) {
		/* Critical Error ...  Cannot proceed further */
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_FATAL,
			  "%s: Hdd Context is Null", __func__);
		CDF_ASSERT(0);
		goto err_sched_close;
	}

	scn = cds_get_context(CDF_MODULE_ID_HIF);
	if (!scn) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_FATAL,
			  "%s: scn is null!", __func__);
		goto err_sched_close;
	}
	scn->enableuartprint = pHddCtx->config->enablefwprint;
	scn->enablefwlog = pHddCtx->config->enablefwlog;
	scn->max_no_of_peers = pHddCtx->config->maxNumberOfPeers;
#ifdef WLAN_FEATURE_LPSS
	scn->enablelpasssupport = pHddCtx->config->enablelpasssupport;
#endif
	scn->enable_ramdump_collection =
				pHddCtx->config->is_ramdump_enabled;
	scn->enable_self_recovery = pHddCtx->config->enableSelfRecovery;

	/* Initialize BMI and Download firmware */
	cdf_status = bmi_download_firmware(scn);
	if (cdf_status != CDF_STATUS_SUCCESS) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_FATAL,
			  "BMI FIALED status:%d", cdf_status);
		goto err_bmi_close;
	}

	htcInfo.pContext = gp_cds_context->pHIFContext;
	htcInfo.TargetFailure = ol_target_failure;
	htcInfo.TargetSendSuspendComplete = wma_target_suspend_acknowledge;
	cdf_ctx = cds_get_context(CDF_MODULE_ID_CDF_DEVICE);

	/* Create HTC */
	gp_cds_context->htc_ctx =
		htc_create(htcInfo.pContext, &htcInfo, cdf_ctx);
	if (!gp_cds_context->htc_ctx) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_FATAL,
			  "%s: Failed to Create HTC", __func__);
		goto err_bmi_close;
	}

	if (bmi_done(scn)) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_FATAL,
			  "%s: Failed to complete BMI phase", __func__);
		goto err_htc_close;
	}

	/*
	** Need to open WMA first because it calls WDI_Init, which calls wpalOpen
	** The reason that is needed becasue cds_packet_open need to use PAL APIs
	*/

	/*Open the WMA module */
	cdf_mem_set(&mac_openParms, sizeof(mac_openParms), 0);
	/* UMA is supported in hardware for performing the
	** frame translation 802.11 <-> 802.3
	*/
	mac_openParms.frameTransRequired = 1;
	mac_openParms.driverType = eDRIVER_TYPE_PRODUCTION;
	mac_openParms.powersaveOffloadEnabled =
		pHddCtx->config->enablePowersaveOffload;
	mac_openParms.staDynamicDtim = pHddCtx->config->enableDynamicDTIM;
	mac_openParms.staModDtim = pHddCtx->config->enableModulatedDTIM;
	mac_openParms.staMaxLIModDtim = pHddCtx->config->fMaxLIModulatedDTIM;
	mac_openParms.wowEnable = pHddCtx->config->wowEnable;
	mac_openParms.maxWoWFilters = pHddCtx->config->maxWoWFilters;
	/* Here olIniInfo is used to store ini status of arp offload
	 * ns offload and others. Currently 1st bit is used for arp
	 * off load and 2nd bit for ns offload currently, rest bits are unused
	 */
	if (pHddCtx->config->fhostArpOffload)
		mac_openParms.olIniInfo = mac_openParms.olIniInfo | 0x1;
	if (pHddCtx->config->fhostNSOffload)
		mac_openParms.olIniInfo = mac_openParms.olIniInfo | 0x2;
	/*
	 * Copy the DFS Phyerr Filtering Offload status.
	 * This parameter reflects the value of the
	 * dfsPhyerrFilterOffload flag  as set in the ini.
	 */
	mac_openParms.dfsPhyerrFilterOffload =
		pHddCtx->config->fDfsPhyerrFilterOffload;
	if (pHddCtx->config->ssdp)
		mac_openParms.ssdp = pHddCtx->config->ssdp;
#ifdef FEATURE_WLAN_RA_FILTERING
	mac_openParms.RArateLimitInterval =
		pHddCtx->config->RArateLimitInterval;
	mac_openParms.IsRArateLimitEnabled =
		pHddCtx->config->IsRArateLimitEnabled;
#endif

	mac_openParms.apMaxOffloadPeers = pHddCtx->config->apMaxOffloadPeers;

	mac_openParms.apMaxOffloadReorderBuffs =
		pHddCtx->config->apMaxOffloadReorderBuffs;

	mac_openParms.apDisableIntraBssFwd =
		pHddCtx->config->apDisableIntraBssFwd;

	mac_openParms.dfsRadarPriMultiplier =
		pHddCtx->config->dfsRadarPriMultiplier;
	mac_openParms.reorderOffload = pHddCtx->config->reorderOffloadSupport;

	/* IPA micro controller data path offload resource config item */
	mac_openParms.ucOffloadEnabled = hdd_ipa_uc_is_enabled(pHddCtx);
	mac_openParms.ucTxBufCount = pHddCtx->config->IpaUcTxBufCount;
	mac_openParms.ucTxBufSize = pHddCtx->config->IpaUcTxBufSize;
	mac_openParms.ucRxIndRingCount = pHddCtx->config->IpaUcRxIndRingCount;
	mac_openParms.ucTxPartitionBase = pHddCtx->config->IpaUcTxPartitionBase;
	mac_openParms.max_scan = pHddCtx->config->max_scan_count;

	mac_openParms.ip_tcp_udp_checksum_offload =
			pHddCtx->config->enable_ip_tcp_udp_checksum_offload;
	mac_openParms.enable_rxthread = pHddCtx->config->enableRxThread;
	mac_openParms.ce_classify_enabled =
				pHddCtx->config->ce_classify_enabled;

#ifdef QCA_LL_TX_FLOW_CONTROL_V2
	mac_openParms.tx_flow_stop_queue_th =
				pHddCtx->config->TxFlowStopQueueThreshold;
	mac_openParms.tx_flow_start_queue_offset =
				pHddCtx->config->TxFlowStartQueueOffset;
#endif
	cds_set_nan_enable(&mac_openParms, pHddCtx);

	mac_openParms.tx_chain_mask_cck = pHddCtx->config->tx_chain_mask_cck;
	mac_openParms.self_gen_frm_pwr = pHddCtx->config->self_gen_frm_pwr;

	cdf_status = wma_open(gp_cds_context,
			      hdd_update_tgt_cfg,
			      hdd_dfs_indicate_radar, &mac_openParms);

	if (!CDF_IS_STATUS_SUCCESS(cdf_status)) {
		/* Critical Error ...  Cannot proceed further */
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_FATAL,
			  "%s: Failed to open WMA module", __func__);
		CDF_ASSERT(0);
		goto err_htc_close;
	}

	/* Number of peers limit differs in each chip version. If peer max
	 * limit configured in ini exceeds more than supported, WMA adjusts
	 * and keeps correct limit in mac_openParms.maxStation. So, make sure
	 * config entry pHddCtx->config->maxNumberOfPeers has adjusted value
	 */
	pHddCtx->config->maxNumberOfPeers = mac_openParms.maxStation;
	HTCHandle = cds_get_context(CDF_MODULE_ID_HTC);
	if (!HTCHandle) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_FATAL,
			  "%s: HTCHandle is null!", __func__);
		goto err_wma_close;
	}
	if (htc_wait_target(HTCHandle)) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_FATAL,
			  "%s: Failed to complete BMI phase", __func__);
		goto err_wma_close;
	}

	/* Now proceed to open the MAC */

	/* UMA is supported in hardware for performing the
	 * frame translation 802.11 <-> 802.3
	 */
	mac_openParms.frameTransRequired = 1;

	sirStatus =
		mac_open(&(gp_cds_context->pMACContext), gp_cds_context->pHDDContext,
			 &mac_openParms);

	if (eSIR_SUCCESS != sirStatus) {
		/* Critical Error ...  Cannot proceed further */
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_FATAL,
			  "%s: Failed to open MAC", __func__);
		CDF_ASSERT(0);
		goto err_wma_close;
	}

	/* Now proceed to open the SME */
	cdf_status = sme_open(gp_cds_context->pMACContext);
	if (!CDF_IS_STATUS_SUCCESS(cdf_status)) {
		/* Critical Error ...  Cannot proceed further */
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_FATAL,
			  "%s: Failed to open SME", __func__);
		CDF_ASSERT(0);
		goto err_mac_close;
	}

	gp_cds_context->pdev_txrx_ctx =
		ol_txrx_pdev_alloc(gp_cds_context->cfg_ctx,
				    gp_cds_context->htc_ctx,
				    gp_cds_context->cdf_ctx);
	if (!gp_cds_context->pdev_txrx_ctx) {
		/* Critical Error ...  Cannot proceed further */
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_FATAL,
			  "%s: Failed to open TXRX", __func__);
		CDF_ASSERT(0);
		goto err_sme_close;
	}

	CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_INFO_HIGH,
		  "%s: CDS successfully Opened", __func__);

	*p_cds_context = gp_cds_context;

	return CDF_STATUS_SUCCESS;

err_sme_close:
	sme_close(gp_cds_context->pMACContext);

err_mac_close:
	mac_close(gp_cds_context->pMACContext);

err_wma_close:
	wma_close(gp_cds_context);

	wma_wmi_service_close(gp_cds_context);

err_htc_close:
	if (gp_cds_context->htc_ctx) {
		htc_destroy(gp_cds_context->htc_ctx);
		gp_cds_context->htc_ctx = NULL;
	}

err_bmi_close:
	bmi_cleanup(scn);

err_sched_close:
	cds_sched_close(gp_cds_context);

err_msg_queue:
	cds_mq_deinit(&gp_cds_context->freeVosMq);

err_wma_complete_event:
	cdf_event_destroy(&gp_cds_context->wmaCompleteEvent);

err_probe_event:
	cdf_event_destroy(&gp_cds_context->ProbeEvent);

	return CDF_STATUS_E_FAILURE;
} /* cds_open() */

/**
 * cds_pre_enable() - pre enable cds
 * @cds_context: CDS context
 *
 * Return: CDF status
 */
CDF_STATUS cds_pre_enable(v_CONTEXT_t cds_context)
{
	CDF_STATUS cdf_status = CDF_STATUS_SUCCESS;
	p_cds_contextType p_cds_context = (p_cds_contextType) cds_context;
	void *scn;
	CDF_TRACE(CDF_MODULE_ID_SYS, CDF_TRACE_LEVEL_INFO, "cds prestart");

	if (gp_cds_context != p_cds_context) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: Context mismatch", __func__);
		CDF_ASSERT(0);
		return CDF_STATUS_E_INVAL;
	}

	if (p_cds_context->pMACContext == NULL) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: MAC NULL context", __func__);
		CDF_ASSERT(0);
		return CDF_STATUS_E_INVAL;
	}

	if (p_cds_context->pWMAContext == NULL) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: WMA NULL context", __func__);
		CDF_ASSERT(0);
		return CDF_STATUS_E_INVAL;
	}

	scn = cds_get_context(CDF_MODULE_ID_HIF);
	if (!scn) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_FATAL,
			  "%s: scn is null!", __func__);
		return CDF_STATUS_E_FAILURE;
	}

	/* Reset wma wait event */
	cdf_event_reset(&gp_cds_context->wmaCompleteEvent);

	/*call WMA pre start */
	cdf_status = wma_pre_start(gp_cds_context);
	if (!CDF_IS_STATUS_SUCCESS(cdf_status)) {
		CDF_TRACE(CDF_MODULE_ID_SYS, CDF_TRACE_LEVEL_FATAL,
			  "Failed to WMA prestart");
		CDF_ASSERT(0);
		return CDF_STATUS_E_FAILURE;
	}

	/* Need to update time out of complete */
	cdf_status = cdf_wait_single_event(&gp_cds_context->wmaCompleteEvent,
					   CDS_WMA_TIMEOUT);
	if (cdf_status != CDF_STATUS_SUCCESS) {
		if (cdf_status == CDF_STATUS_E_TIMEOUT) {
			CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
				  "%s: Timeout occurred before WMA complete",
				  __func__);
		} else {
			CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
				  "%s: wma_pre_start reporting other error",
				  __func__);
		}
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: Test MC thread by posting a probe message to SYS",
			  __func__);
		wlan_sys_probe();

		CDF_ASSERT(0);
		return CDF_STATUS_E_FAILURE;
	}

	cdf_status = htc_start(gp_cds_context->htc_ctx);
	if (!CDF_IS_STATUS_SUCCESS(cdf_status)) {
		CDF_TRACE(CDF_MODULE_ID_SYS, CDF_TRACE_LEVEL_FATAL,
			  "Failed to Start HTC");
		CDF_ASSERT(0);
		return CDF_STATUS_E_FAILURE;
	}
	cdf_status = wma_wait_for_ready_event(gp_cds_context->pWMAContext);
	if (!CDF_IS_STATUS_SUCCESS(cdf_status)) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_FATAL,
			  "Failed to get ready event from target firmware");
		htc_set_target_to_sleep(scn);
		htc_stop(gp_cds_context->htc_ctx);
		CDF_ASSERT(0);
		return CDF_STATUS_E_FAILURE;
	}

	if (ol_txrx_pdev_attach(gp_cds_context->pdev_txrx_ctx)) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_FATAL,
			"Failed to attach pdev");
		htc_set_target_to_sleep(scn);
		htc_stop(gp_cds_context->htc_ctx);
		CDF_ASSERT(0);
		return CDF_STATUS_E_FAILURE;
	}

	htc_set_target_to_sleep(scn);

	return CDF_STATUS_SUCCESS;
}

/**
 * cds_enable() - start/enable cds module
 * @cds_context: CDS context
 *
 * Return: CDF status
 */
CDF_STATUS cds_enable(v_CONTEXT_t cds_context)
{
	CDF_STATUS cdf_status = CDF_STATUS_SUCCESS;
	tSirRetStatus sirStatus = eSIR_SUCCESS;
	p_cds_contextType p_cds_context = (p_cds_contextType) cds_context;
	tHalMacStartParameters halStartParams;

	CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_INFO,
		  "%s: Starting Libra SW", __func__);

	/* We support only one instance for now ... */
	if (gp_cds_context != p_cds_context) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: mismatch in context", __func__);
		return CDF_STATUS_E_FAILURE;
	}

	if ((p_cds_context->pWMAContext == NULL) ||
	    (p_cds_context->pMACContext == NULL)) {
		if (p_cds_context->pWMAContext == NULL)
			CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
				  "%s: WMA NULL context", __func__);
		else
			CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
				  "%s: MAC NULL context", __func__);

		return CDF_STATUS_E_FAILURE;
	}

	/* Start the wma */
	cdf_status = wma_start(p_cds_context);
	if (cdf_status != CDF_STATUS_SUCCESS) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: Failed to start wma", __func__);
		return CDF_STATUS_E_FAILURE;
	}
	CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_INFO,
		  "%s: wma correctly started", __func__);

	/* Start the MAC */
	cdf_mem_zero(&halStartParams,
		     sizeof(tHalMacStartParameters));

	/* Start the MAC */
	sirStatus =
		mac_start(p_cds_context->pMACContext, &halStartParams);

	if (eSIR_SUCCESS != sirStatus) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_FATAL,
			  "%s: Failed to start MAC", __func__);
		goto err_wma_stop;
	}

	CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_INFO,
		  "%s: MAC correctly started", __func__);

	/* START SME */
	cdf_status = sme_start(p_cds_context->pMACContext);

	if (!CDF_IS_STATUS_SUCCESS(cdf_status)) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_FATAL,
			  "%s: Failed to start SME", __func__);
		goto err_mac_stop;
	}

	CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_INFO,
		  "%s: SME correctly started", __func__);

	if (ol_txrx_pdev_attach_target
		       (p_cds_context->pdev_txrx_ctx) != A_OK) {
	   CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_FATAL,
				"%s: Failed attach target", __func__);
	   goto err_sme_stop;
	}

	CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_INFO,
		  "TL correctly started");
	CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_INFO,
		  "%s: CDS Start is successful!!", __func__);

	return CDF_STATUS_SUCCESS;

err_sme_stop:
	sme_stop(p_cds_context->pMACContext, HAL_STOP_TYPE_SYS_RESET);

err_mac_stop:
	mac_stop(p_cds_context->pMACContext, HAL_STOP_TYPE_SYS_RESET);

err_wma_stop:
	cdf_event_reset(&(gp_cds_context->wmaCompleteEvent));
	cdf_status = wma_stop(p_cds_context, HAL_STOP_TYPE_RF_KILL);
	if (!CDF_IS_STATUS_SUCCESS(cdf_status)) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: Failed to stop wma", __func__);
		CDF_ASSERT(CDF_IS_STATUS_SUCCESS(cdf_status));
		wma_setneedshutdown(cds_context);
	} else {
		cdf_status =
			cdf_wait_single_event(&(gp_cds_context->wmaCompleteEvent),
					      CDS_WMA_TIMEOUT);
		if (cdf_status != CDF_STATUS_SUCCESS) {
			if (cdf_status == CDF_STATUS_E_TIMEOUT) {
				CDF_TRACE(CDF_MODULE_ID_CDF,
					  CDF_TRACE_LEVEL_FATAL,
					  "%s: Timeout occurred before WMA_stop complete",
					  __func__);
			} else {
				CDF_TRACE(CDF_MODULE_ID_CDF,
					  CDF_TRACE_LEVEL_FATAL,
					  "%s: WMA_stop reporting other error",
					  __func__);
			}
			CDF_ASSERT(0);
			wma_setneedshutdown(cds_context);
		}
	}

	return CDF_STATUS_E_FAILURE;
} /* cds_enable() */

/**
 * cds_disable() - stop/disable cds module
 * @cds_context: CDS context
 *
 * Return: CDF status
 */
CDF_STATUS cds_disable(v_CONTEXT_t cds_context)
{
	CDF_STATUS cdf_status;

	/* wma_stop is called before the SYS so that the processing of target
	 * pending responses will not be handled during uninitialization of
	 * WLAN driver
	 */
	cdf_event_reset(&(gp_cds_context->wmaCompleteEvent));

	cdf_status = wma_stop(cds_context, HAL_STOP_TYPE_RF_KILL);

	if (!CDF_IS_STATUS_SUCCESS(cdf_status)) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: Failed to stop wma", __func__);
		CDF_ASSERT(CDF_IS_STATUS_SUCCESS(cdf_status));
		wma_setneedshutdown(cds_context);
	}

	hif_disable_isr(((cds_context_type *) cds_context)->pHIFContext);
	hif_reset_soc(((cds_context_type *) cds_context)->pHIFContext);

	/* SYS STOP will stop SME and MAC */
	cdf_status = sys_stop(cds_context);
	if (!CDF_IS_STATUS_SUCCESS(cdf_status)) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: Failed to stop SYS", __func__);
		CDF_ASSERT(CDF_IS_STATUS_SUCCESS(cdf_status));
	}

	return CDF_STATUS_SUCCESS;
}

/**
 * cds_close() - close cds module
 * @cds_context: CDS context
 *
 * Return: CDF status
 */
CDF_STATUS cds_close(v_CONTEXT_t cds_context)
{
	CDF_STATUS cdf_status;

	cdf_status = wma_wmi_work_close(cds_context);
	if (!CDF_IS_STATUS_SUCCESS(cdf_status)) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
		 "%s: Failed to close wma_wmi_work", __func__);
		CDF_ASSERT(0);
	}

	if (gp_cds_context->htc_ctx) {
		htc_stop(gp_cds_context->htc_ctx);
		htc_destroy(gp_cds_context->htc_ctx);
		gp_cds_context->htc_ctx = NULL;
	}

	ol_txrx_pdev_detach(gp_cds_context->pdev_txrx_ctx, 1);
	cds_free_context(cds_context, CDF_MODULE_ID_TXRX,
			 gp_cds_context->pdev_txrx_ctx);

	cdf_status = sme_close(((p_cds_contextType) cds_context)->pMACContext);
	if (!CDF_IS_STATUS_SUCCESS(cdf_status)) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: Failed to close SME", __func__);
		CDF_ASSERT(CDF_IS_STATUS_SUCCESS(cdf_status));
	}

	cdf_status = mac_close(((p_cds_contextType) cds_context)->pMACContext);
	if (!CDF_IS_STATUS_SUCCESS(cdf_status)) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: Failed to close MAC", __func__);
		CDF_ASSERT(CDF_IS_STATUS_SUCCESS(cdf_status));
	}

	((p_cds_contextType) cds_context)->pMACContext = NULL;

	if (true == wma_needshutdown(cds_context)) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
				  "%s: Failed to shutdown wma", __func__);
	} else {
		cdf_status = wma_close(cds_context);
		if (!CDF_IS_STATUS_SUCCESS(cdf_status)) {
			CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
				  "%s: Failed to close wma", __func__);
			CDF_ASSERT(CDF_IS_STATUS_SUCCESS(cdf_status));
		}
	}

	cdf_status = wma_wmi_service_close(cds_context);
	if (!CDF_IS_STATUS_SUCCESS(cdf_status)) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: Failed to close wma_wmi_service", __func__);
		CDF_ASSERT(CDF_IS_STATUS_SUCCESS(cdf_status));
	}

	cds_mq_deinit(&((p_cds_contextType) cds_context)->freeVosMq);

	cdf_status = cdf_event_destroy(&gp_cds_context->wmaCompleteEvent);
	if (!CDF_IS_STATUS_SUCCESS(cdf_status)) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: failed to destroy wmaCompleteEvent", __func__);
		CDF_ASSERT(CDF_IS_STATUS_SUCCESS(cdf_status));
	}

	cdf_status = cdf_event_destroy(&gp_cds_context->ProbeEvent);
	if (!CDF_IS_STATUS_SUCCESS(cdf_status)) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: failed to destroy ProbeEvent", __func__);
		CDF_ASSERT(CDF_IS_STATUS_SUCCESS(cdf_status));
	}

	cds_deinit_log_completion();
	return CDF_STATUS_SUCCESS;
}

/**
 * cds_get_context() - get context data area
 *
 * @moduleId: ID of the module who's context data is being retrived.
 *
 * Each module in the system has a context / data area that is allocated
 * and managed by CDS.  This API allows any user to get a pointer to its
 * allocated context data area from the CDS global context.
 *
 * Return: pointer to the context data area of the module ID
 *	   specified, or NULL if the context data is not allocated for
 *	   the module ID specified
 */
void *cds_get_context(CDF_MODULE_ID moduleId)
{
	void *pModContext = NULL;

	if (gp_cds_context == NULL) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: cds context pointer is null", __func__);
		return NULL;
	}

	switch (moduleId) {
#ifndef WLAN_FEATURE_MBSSID
	case CDF_MODULE_ID_SAP:
	{
		pModContext = gp_cds_context->pSAPContext;
		break;
	}
#endif

	case CDF_MODULE_ID_HDD:
	{
		pModContext = gp_cds_context->pHDDContext;
		break;
	}

	case CDF_MODULE_ID_SME:
	case CDF_MODULE_ID_PE:
	{
		/* In all these cases, we just return the MAC Context */
		pModContext = gp_cds_context->pMACContext;
		break;
	}

	case CDF_MODULE_ID_WMA:
	{
		/* For wma module */
		pModContext = gp_cds_context->pWMAContext;
		break;
	}

	case CDF_MODULE_ID_CDF:
	{
		/* For SYS this is CDS itself */
		pModContext = gp_cds_context;
		break;
	}

	case CDF_MODULE_ID_HIF:
	{
		pModContext = gp_cds_context->pHIFContext;
		break;
	}

	case CDF_MODULE_ID_HTC:
	{
		pModContext = gp_cds_context->htc_ctx;
		break;
	}

	case CDF_MODULE_ID_CDF_DEVICE:
	{
		pModContext = gp_cds_context->cdf_ctx;
		break;
	}

	case CDF_MODULE_ID_TXRX:
	{
		pModContext = gp_cds_context->pdev_txrx_ctx;
		break;
	}

	case CDF_MODULE_ID_CFG:
	{
		pModContext = gp_cds_context->cfg_ctx;
		break;
	}

	default:
	{
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: Module ID %i does not have its context maintained by CDS",
			  __func__, moduleId);
		CDF_ASSERT(0);
		return NULL;
	}
	}

	if (pModContext == NULL) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: Module ID %i context is Null", __func__,
			  moduleId);
	}

	return pModContext;
} /* cds_get_context() */

/**
 * cds_get_global_context() - get CDS global Context
 *
 * This API allows any user to get the CDS Global Context pointer from a
 * module context data area.
 *
 * Return: pointer to the CDS global context, NULL if the function is
 *	   unable to retreive the CDS context.
 */
v_CONTEXT_t cds_get_global_context(void)
{
	if (gp_cds_context == NULL) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: global cds context is NULL", __func__);
	}

	return gp_cds_context;
} /* cds_get_global_context() */

/**
 * cds_is_logp_in_progress() - check if ssr/self recovery is going on
 *
 * Return: true if ssr/self recvoery is going on else false
 */
uint8_t cds_is_logp_in_progress(void)
{
	if (gp_cds_context == NULL) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: global cds context is NULL", __func__);
		return 1;
	}

	return gp_cds_context->isLogpInProgress;
}

/**
 * cds_set_logp_in_progress() - set ssr/self recovery in progress
 * @value: value to set
 *
 * Return: none
 */
void cds_set_logp_in_progress(uint8_t value)
{
	hdd_context_t *pHddCtx = NULL;

	if (gp_cds_context == NULL) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: global cds context is NULL", __func__);
		return;
	}
	gp_cds_context->isLogpInProgress = value;

	/* HDD uses it's own context variable to check if SSR in progress,
	 * instead of modifying all HDD APIs set the HDD context variable
	 * here
	 */
	pHddCtx = cds_get_context(CDF_MODULE_ID_HDD);
	if (!pHddCtx) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_FATAL,
			  "%s: HDD context is Null", __func__);
		return;
	}
	pHddCtx->isLogpInProgress = value;
}

/**
 * cds_is_load_unload_in_progress() - check if driver load/unload in progress
 *
 * Return: true if load/unload is going on else false
 */
uint8_t cds_is_load_unload_in_progress(void)
{
	if (gp_cds_context == NULL) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: global cds context is NULL", __func__);
		return 0;
	}

	return gp_cds_context->isLoadUnloadInProgress;
}

/**
 * cds_is_unload_in_progress() - check if driver unload in
 * progress
 *
 * Return: true if unload is going on else false
 */
uint8_t cds_is_unload_in_progress(void)
{
	hdd_context_t *hdd_ctx = NULL;
	if (gp_cds_context == NULL) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: global cds context is NULL", __func__);
		return 0;
	}
	hdd_ctx = cds_get_context(CDF_MODULE_ID_HDD);

	if (hdd_ctx == NULL) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: HDD context is NULL", __func__);
		return 0;
	}

	return hdd_ctx->isUnloadInProgress;
}

/**
 * cds_set_load_unload_in_progress() - set load/unload in progress
 * @value: value to set
 *
 * Return: none
 */
void cds_set_load_unload_in_progress(uint8_t value)
{
	if (gp_cds_context == NULL) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: global cds context is NULL", __func__);
		return;
	}
	gp_cds_context->isLoadUnloadInProgress = value;

#ifdef CONFIG_CNSS
	if (value)
		cnss_set_driver_status(CNSS_LOAD_UNLOAD);
	else
		cnss_set_driver_status(CNSS_INITIALIZED);
#endif
}

/**
 * cds_alloc_context() - allocate a context within the CDS global Context
 * @p_cds_context: pointer to the global Vos context
 * @moduleId: module ID who's context area is being allocated.
 * @ppModuleContext: pointer to location where the pointer to the
 *	allocated context is returned. Note this output pointer
 *	is valid only if the API returns CDF_STATUS_SUCCESS
 * @param size: size of the context area to be allocated.
 *
 * This API allows any user to allocate a user context area within the
 * CDS Global Context.
 *
 * Return: CDF status
 */
CDF_STATUS cds_alloc_context(void *p_cds_context, CDF_MODULE_ID moduleID,
			     void **ppModuleContext, uint32_t size)
{
	void **pGpModContext = NULL;

	if (p_cds_context == NULL) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: cds context is null", __func__);
		return CDF_STATUS_E_FAILURE;
	}

	if ((gp_cds_context != p_cds_context) || (ppModuleContext == NULL)) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: context mismatch or null param passed",
			  __func__);
		return CDF_STATUS_E_FAILURE;
	}

	switch (moduleID) {

#ifndef WLAN_FEATURE_MBSSID
	case CDF_MODULE_ID_SAP:
	{
		pGpModContext = &(gp_cds_context->pSAPContext);
		break;
	}
#endif

	case CDF_MODULE_ID_WMA:
	{
		pGpModContext = &(gp_cds_context->pWMAContext);
		break;
	}

	case CDF_MODULE_ID_HIF:
	{
		pGpModContext = &(gp_cds_context->pHIFContext);
		break;
	}

	case CDF_MODULE_ID_EPPING:
	{
		pGpModContext = &(gp_cds_context->epping_ctx);
		break;
	}
	case CDF_MODULE_ID_SME:
	case CDF_MODULE_ID_PE:
	case CDF_MODULE_ID_HDD:
	case CDF_MODULE_ID_HDD_SOFTAP:
	default:
	{
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: Module ID %i "
			  "does not have its context allocated by CDS",
			  __func__, moduleID);
		CDF_ASSERT(0);
		return CDF_STATUS_E_INVAL;
	}
	}

	if (NULL != *pGpModContext) {
		/* Context has already been allocated!
		 * Prevent double allocation
		 */
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: Module ID %i context has already been allocated",
			  __func__, moduleID);
		return CDF_STATUS_E_EXISTS;
	}

	/* Dynamically allocate the context for module */

	*ppModuleContext = cdf_mem_malloc(size);

	if (*ppModuleContext == NULL) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: Failed to " "allocate Context for module ID %i",
			  __func__, moduleID);
		CDF_ASSERT(0);
		return CDF_STATUS_E_NOMEM;
	}

	if (moduleID == CDF_MODULE_ID_TLSHIM)
		cdf_mem_zero(*ppModuleContext, size);

	*pGpModContext = *ppModuleContext;

	return CDF_STATUS_SUCCESS;
} /* cds_alloc_context() */

/**
 * cds_free_context() - free an allocated context within the
 *			CDS global Context
 * @p_cds_context: pointer to the global Vos context
 * @moduleId: module ID who's context area is being free
 * @pModuleContext: pointer to module context area to be free'd.
 *
 *  This API allows a user to free the user context area within the
 *  CDS Global Context.
 *
 * Return: CDF status
 */
CDF_STATUS cds_free_context(void *p_cds_context, CDF_MODULE_ID moduleID,
			    void *pModuleContext)
{
	void **pGpModContext = NULL;

	if ((p_cds_context == NULL) || (gp_cds_context != p_cds_context) ||
	    (pModuleContext == NULL)) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: Null params or context mismatch", __func__);
		return CDF_STATUS_E_FAILURE;
	}

	switch (moduleID) {
#ifndef WLAN_FEATURE_MBSSID
	case CDF_MODULE_ID_SAP:
	{
		pGpModContext = &(gp_cds_context->pSAPContext);
		break;
	}
#endif

	case CDF_MODULE_ID_WMA:
	{
		pGpModContext = &(gp_cds_context->pWMAContext);
		break;
	}

	case CDF_MODULE_ID_HIF:
	{
		pGpModContext = &(gp_cds_context->pHIFContext);
		break;
	}

	case CDF_MODULE_ID_EPPING:
	{
		pGpModContext = &(gp_cds_context->epping_ctx);
		break;
	}

	case CDF_MODULE_ID_TXRX:
	{
		pGpModContext = &(gp_cds_context->pdev_txrx_ctx);
		break;
	}

	case CDF_MODULE_ID_HDD:
	case CDF_MODULE_ID_SME:
	case CDF_MODULE_ID_PE:
	case CDF_MODULE_ID_HDD_SOFTAP:
	default:
	{
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: Module ID %i "
			  "does not have its context allocated by CDS",
			  __func__, moduleID);
		CDF_ASSERT(0);
		return CDF_STATUS_E_INVAL;
	}
	}

	if (NULL == *pGpModContext) {
		/* Context has not been allocated or freed already! */
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: Module ID %i "
			  "context has not been allocated or freed already",
			  __func__, moduleID);
		return CDF_STATUS_E_FAILURE;
	}

	if (*pGpModContext != pModuleContext) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: pGpModContext != pModuleContext", __func__);
		return CDF_STATUS_E_FAILURE;
	}

	if (pModuleContext != NULL)
		cdf_mem_free(pModuleContext);

	*pGpModContext = NULL;

	return CDF_STATUS_SUCCESS;
} /* cds_free_context() */

/**
 * cds_mq_post_message() - post a message to a message queue
 * @msgQueueId: identifies the message queue upon which the message
 *	will be posted.
 * @message: a pointer to a message buffer. Memory for this message
 *	buffer is allocated by the caller and free'd by the CDF after the
 *	message is posted to the message queue.  If the consumer of the
 *	message needs anything in this message, it needs to copy the contents
 *	before returning from the message queue handler.
 *
 * Return: CDF status
 */
CDF_STATUS cds_mq_post_message(CDS_MQ_ID msgQueueId, cds_msg_t *pMsg)
{
	p_cds_mq_type pTargetMq = NULL;
	p_cds_msg_wrapper pMsgWrapper = NULL;
	uint32_t debug_count = 0;

	if ((gp_cds_context == NULL) || (pMsg == NULL)) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: Null params or global cds context is null",
			  __func__);
		CDF_ASSERT(0);
		return CDF_STATUS_E_FAILURE;
	}

	switch (msgQueueId) {
	/* Message Queue ID for messages bound for SME */
	case CDS_MQ_ID_SME:
	{
		pTargetMq = &(gp_cds_context->cdf_sched.smeMcMq);
		break;
	}

	/* Message Queue ID for messages bound for PE */
	case CDS_MQ_ID_PE:
	{
		pTargetMq = &(gp_cds_context->cdf_sched.peMcMq);
		break;
	}

	/* Message Queue ID for messages bound for wma */
	case CDS_MQ_ID_WMA:
	{
		pTargetMq = &(gp_cds_context->cdf_sched.wmaMcMq);
		break;
	}

	/* Message Queue ID for messages bound for the SYS module */
	case CDS_MQ_ID_SYS:
	{
		pTargetMq = &(gp_cds_context->cdf_sched.sysMcMq);
		break;
	}

	default:
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  ("%s: Trying to queue msg into unknown MC Msg queue ID %d"),
			  __func__, msgQueueId);

		return CDF_STATUS_E_FAILURE;
	}

	CDF_ASSERT(NULL != pTargetMq);
	if (pTargetMq == NULL) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: pTargetMq == NULL", __func__);
		return CDF_STATUS_E_FAILURE;
	}

	/* Try and get a free Msg wrapper */
	pMsgWrapper = cds_mq_get(&gp_cds_context->freeVosMq);

	if (NULL == pMsgWrapper) {
		debug_count = atomic_inc_return(&cds_wrapper_empty_count);
		if (1 == debug_count)
			CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
				"%s: CDS Core run out of message wrapper %d",
				__func__, debug_count);

		if (CDS_WRAPPER_MAX_FAIL_COUNT == debug_count)
			CDF_BUG(0);

		return CDF_STATUS_E_RESOURCES;
	}

	atomic_set(&cds_wrapper_empty_count, 0);

	/* Copy the message now */
	cdf_mem_copy((void *)pMsgWrapper->pVosMsg,
		     (void *)pMsg, sizeof(cds_msg_t));

	cds_mq_put(pTargetMq, pMsgWrapper);

	set_bit(MC_POST_EVENT_MASK, &gp_cds_context->cdf_sched.mcEventFlag);
	wake_up_interruptible(&gp_cds_context->cdf_sched.mcWaitQueue);

	return CDF_STATUS_SUCCESS;
} /* cds_mq_post_message() */

/**
 * cds_sys_probe_thread_cback() -  probe mc thread callback
 * @pUserData: pointer to user data
 *
 * Return: none
 */
void cds_sys_probe_thread_cback(void *pUserData)
{
	if (gp_cds_context != pUserData) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: gp_cds_context != pUserData", __func__);
		return;
	}

	if (cdf_event_set(&gp_cds_context->ProbeEvent) != CDF_STATUS_SUCCESS) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: cdf_event_set failed", __func__);
		return;
	}
} /* cds_sys_probe_thread_cback() */

/**
 * cds_wma_complete_cback() - wma complete callback
 * @pUserData: pointer to user data
 *
 * Return: none
 */
void cds_wma_complete_cback(void *pUserData)
{
	if (gp_cds_context != pUserData) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: gp_cds_context != pUserData", __func__);
		return;
	}

	if (cdf_event_set(&gp_cds_context->wmaCompleteEvent) !=
	    CDF_STATUS_SUCCESS) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: cdf_event_set failed", __func__);
		return;
	}
} /* cds_wma_complete_cback() */

/**
 * cds_core_return_msg() - return core message
 * @pVContext: pointer to cds context
 * @pMsgWrapper: pointer to message wrapper
 *
 * Return: none
 */
void cds_core_return_msg(void *pVContext, p_cds_msg_wrapper pMsgWrapper)
{
	p_cds_contextType p_cds_context = (p_cds_contextType) pVContext;

	CDF_ASSERT(gp_cds_context == p_cds_context);

	if (gp_cds_context != p_cds_context) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: gp_cds_context != p_cds_context", __func__);
		return;
	}

	CDF_ASSERT(NULL != pMsgWrapper);

	if (pMsgWrapper == NULL) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: pMsgWrapper == NULL in function", __func__);
		return;
	}

	/*
	** Return the message on the free message queue
	*/
	INIT_LIST_HEAD(&pMsgWrapper->msgNode);
	cds_mq_put(&p_cds_context->freeVosMq, pMsgWrapper);
} /* cds_core_return_msg() */


/**
 * cds_shutdown() - shutdown CDS
 * @cds_context: global cds context
 *
 * Return: CDF status
 */
CDF_STATUS cds_shutdown(v_CONTEXT_t cds_context)
{
	CDF_STATUS cdf_status;
	tpAniSirGlobal pmac = (((p_cds_contextType)cds_context)->pMACContext);

	ol_txrx_pdev_detach(gp_cds_context->pdev_txrx_ctx, 1);
	cds_free_context(cds_context, CDF_MODULE_ID_TXRX,
			 gp_cds_context->pdev_txrx_ctx);

	cdf_status = sme_close(((p_cds_contextType) cds_context)->pMACContext);
	if (!CDF_IS_STATUS_SUCCESS(cdf_status)) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: Failed to close SME", __func__);
		CDF_ASSERT(CDF_IS_STATUS_SUCCESS(cdf_status));
	}
	/*
	 * CAC timer will be initiated and started only when SAP starts on
	 * DFS channel and it will be stopped and destroyed immediately once the
	 * radar detected or timedout. So as per design CAC timer should be
	 * destroyed after stop
	 */
	if (pmac->sap.SapDfsInfo.is_dfs_cac_timer_running) {
		cdf_mc_timer_stop(&pmac->sap.SapDfsInfo.sap_dfs_cac_timer);
		pmac->sap.SapDfsInfo.is_dfs_cac_timer_running = 0;
		cdf_mc_timer_destroy(&pmac->sap.SapDfsInfo.sap_dfs_cac_timer);
	}

	cdf_status = mac_close(((p_cds_contextType) cds_context)->pMACContext);
	if (!CDF_IS_STATUS_SUCCESS(cdf_status)) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: Failed to close MAC", __func__);
		CDF_ASSERT(CDF_IS_STATUS_SUCCESS(cdf_status));
	}

	((p_cds_contextType) cds_context)->pMACContext = NULL;

	if (false == wma_needshutdown(cds_context)) {

		cdf_status = wma_close(cds_context);
		if (!CDF_IS_STATUS_SUCCESS(cdf_status)) {
			CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
				  "%s: Failed to close wma!", __func__);
			CDF_ASSERT(CDF_IS_STATUS_SUCCESS(cdf_status));
		}
	}

	cdf_status = wma_wmi_work_close(cds_context);
	if (!CDF_IS_STATUS_SUCCESS(cdf_status)) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
		"%s: Failed to close wma_wmi_work!", __func__);
		CDF_ASSERT(0);
	}

	if (gp_cds_context->htc_ctx) {
		htc_stop(gp_cds_context->htc_ctx);
		htc_destroy(gp_cds_context->htc_ctx);
		gp_cds_context->htc_ctx = NULL;
	}

	cdf_status = wma_wmi_service_close(cds_context);
	if (!CDF_IS_STATUS_SUCCESS(cdf_status)) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: Failed to close wma_wmi_service!", __func__);
		CDF_ASSERT(CDF_IS_STATUS_SUCCESS(cdf_status));
	}

	cds_mq_deinit(&((p_cds_contextType) cds_context)->freeVosMq);

	cdf_status = cdf_event_destroy(&gp_cds_context->wmaCompleteEvent);
	if (!CDF_IS_STATUS_SUCCESS(cdf_status)) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: failed to destroy wmaCompleteEvent", __func__);
		CDF_ASSERT(CDF_IS_STATUS_SUCCESS(cdf_status));
	}

	cdf_status = cdf_event_destroy(&gp_cds_context->ProbeEvent);
	if (!CDF_IS_STATUS_SUCCESS(cdf_status)) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: failed to destroy ProbeEvent", __func__);
		CDF_ASSERT(CDF_IS_STATUS_SUCCESS(cdf_status));
	}

	return CDF_STATUS_SUCCESS;
}

/**
 * cds_get_vdev_types() - get vdev type
 * @mode: mode
 * @type: type
 * @sub_type: sub_type
 *
 * Return: WMI vdev type
 */
CDF_STATUS cds_get_vdev_types(tCDF_CON_MODE mode, uint32_t *type,
			      uint32_t *sub_type)
{
	CDF_STATUS status = CDF_STATUS_SUCCESS;
	*type = 0;
	*sub_type = 0;

	switch (mode) {
	case CDF_STA_MODE:
		*type = WMI_VDEV_TYPE_STA;
		break;
	case CDF_SAP_MODE:
		*type = WMI_VDEV_TYPE_AP;
		break;
	case CDF_P2P_DEVICE_MODE:
		*type = WMI_VDEV_TYPE_AP;
		*sub_type = WMI_UNIFIED_VDEV_SUBTYPE_P2P_DEVICE;
		break;
	case CDF_P2P_CLIENT_MODE:
		*type = WMI_VDEV_TYPE_STA;
		*sub_type = WMI_UNIFIED_VDEV_SUBTYPE_P2P_CLIENT;
		break;
	case CDF_P2P_GO_MODE:
		*type = WMI_VDEV_TYPE_AP;
		*sub_type = WMI_UNIFIED_VDEV_SUBTYPE_P2P_GO;
		break;
	case CDF_OCB_MODE:
		*type = WMI_VDEV_TYPE_OCB;
		break;
	default:
		hddLog(CDF_TRACE_LEVEL_ERROR, "Invalid device mode %d", mode);
		status = CDF_STATUS_E_INVAL;
		break;
	}
	return status;
}

/**
 * cds_flush_work() - flush pending works
 * @work: pointer to work
 *
 * Return: none
 */
void cds_flush_work(void *work)
{
#if defined (CONFIG_CNSS)
	cnss_flush_work(work);
#elif defined (WLAN_OPEN_SOURCE)
	cancel_work_sync(work);
#endif
}

/**
 * cds_flush_delayed_work() - flush delayed works
 * @dwork: pointer to delayed work
 *
 * Return: none
 */
void cds_flush_delayed_work(void *dwork)
{
#if defined (CONFIG_CNSS)
	cnss_flush_delayed_work(dwork);
#elif defined (WLAN_OPEN_SOURCE)
	cancel_delayed_work_sync(dwork);
#endif
}

/**
 * cds_is_packet_log_enabled() - check if packet log is enabled
 *
 * Return: true if packet log is enabled else false
 */
bool cds_is_packet_log_enabled(void)
{
	hdd_context_t *pHddCtx;

	pHddCtx = (hdd_context_t *) (gp_cds_context->pHDDContext);
	if ((NULL == pHddCtx) || (NULL == pHddCtx->config)) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_FATAL,
			  "%s: Hdd Context is Null", __func__);
		return false;
	}

	return pHddCtx->config->enablePacketLog;
}

/**
 * cds_trigger_recovery() - trigger self recovery
 *
 * Return: none
 */
void cds_trigger_recovery(void)
{
	tp_wma_handle wma_handle = cds_get_context(CDF_MODULE_ID_WMA);
	CDF_STATUS status = CDF_STATUS_SUCCESS;

	if (!wma_handle) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			"WMA context is invald!");
		return;
	}

	wma_crash_inject(wma_handle, RECOVERY_SIM_SELF_RECOVERY, 0);

	status = cdf_wait_single_event(&wma_handle->recovery_event,
		WMA_CRASH_INJECT_TIMEOUT);

	if (CDF_STATUS_SUCCESS != status) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			"CRASH_INJECT command is timed out!");
 #ifdef CONFIG_CNSS
		if (cds_is_logp_in_progress()) {
			CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
				"LOGP is in progress, ignore!");
			return;
		}
		cds_set_logp_in_progress(true);
		cnss_schedule_recovery_work();
 #endif

		return;
	}
}

/**
 * cds_get_monotonic_boottime() - Get kernel boot time.
 *
 * Return: Time in microseconds
 */

uint64_t cds_get_monotonic_boottime(void)
{
#ifdef CONFIG_CNSS
	struct timespec ts;

	cnss_get_monotonic_boottime(&ts);
	return ((uint64_t) ts.tv_sec * 1000000) + (ts.tv_nsec / 1000);
#else
	return ((uint64_t)cdf_system_ticks_to_msecs(cdf_system_ticks()) *
			 1000);
#endif
}

/**
 * cds_set_wakelock_logging() - Logging of wakelock enabled/disabled
 * @value: Boolean value
 *
 * This function is used to set the flag which will indicate whether
 * logging of wakelock is enabled or not
 *
 * Return: None
 */
void cds_set_wakelock_logging(bool value)
{
	p_cds_contextType p_cds_context;

	p_cds_context = cds_get_global_context();
	if (!p_cds_context) {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
				"cds context is Invald");
		return;
	}
	p_cds_context->is_wakelock_log_enabled = value;
}

/**
 * cds_is_wakelock_enabled() - Check if logging of wakelock is enabled/disabled
 * @value: Boolean value
 *
 * This function is used to check whether logging of wakelock is enabled or not
 *
 * Return: true if logging of wakelock is enabled
 */
bool cds_is_wakelock_enabled(void)
{
	p_cds_contextType p_cds_context;

	p_cds_context = cds_get_global_context();
	if (!p_cds_context) {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
				"cds context is Invald");
		return false;
	}
	return p_cds_context->is_wakelock_log_enabled;
}

/**
 * cds_set_ring_log_level() - Sets the log level of a particular ring
 * @ring_id: ring_id
 * @log_levelvalue: Log level specificed
 *
 * This function converts HLOS values to driver log levels and sets the log
 * level of a particular ring accordingly.
 *
 * Return: None
 */
void cds_set_ring_log_level(uint32_t ring_id, uint32_t log_level)
{
	p_cds_contextType p_cds_context;
	uint32_t log_val;

	p_cds_context = cds_get_global_context();
	if (!p_cds_context) {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
				"%s: cds context is Invald", __func__);
		return;
	}

	switch (log_level) {
	case LOG_LEVEL_NO_COLLECTION:
		log_val = WLAN_LOG_LEVEL_OFF;
		break;
	case LOG_LEVEL_NORMAL_COLLECT:
		log_val = WLAN_LOG_LEVEL_NORMAL;
		break;
	case LOG_LEVEL_ISSUE_REPRO:
		log_val = WLAN_LOG_LEVEL_REPRO;
		break;
	case LOG_LEVEL_ACTIVE:
	default:
		log_val = WLAN_LOG_LEVEL_ACTIVE;
		break;
	}

	if (ring_id == RING_ID_WAKELOCK) {
		p_cds_context->wakelock_log_level = log_val;
		return;
	} else if (ring_id == RING_ID_CONNECTIVITY) {
		p_cds_context->connectivity_log_level = log_val;
		return;
	} else if (ring_id == RING_ID_PER_PACKET_STATS) {
		p_cds_context->packet_stats_log_level = log_val;
		return;
	} else if (ring_id == RING_ID_DRIVER_DEBUG) {
		p_cds_context->driver_debug_log_level = log_val;
		return;
	} else if (ring_id == RING_ID_FIRMWARE_DEBUG) {
		p_cds_context->fw_debug_log_level = log_val;
		return;
	}
}

/**
 * cds_get_ring_log_level() - Get the a ring id's log level
 * @ring_id: Ring id
 *
 * Fetch and return the log level corresponding to a ring id
 *
 * Return: Log level corresponding to the ring ID
 */
enum wifi_driver_log_level cds_get_ring_log_level(uint32_t ring_id)
{
	p_cds_contextType p_cds_context;

	p_cds_context = cds_get_global_context();
	if (!p_cds_context) {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
				"%s: cds context is Invald", __func__);
		return WLAN_LOG_LEVEL_OFF;
	}

	if (ring_id == RING_ID_WAKELOCK)
		return p_cds_context->wakelock_log_level;
	else if (ring_id == RING_ID_CONNECTIVITY)
		return p_cds_context->connectivity_log_level;
	else if (ring_id == RING_ID_PER_PACKET_STATS)
		return p_cds_context->packet_stats_log_level;
	else if (ring_id == RING_ID_DRIVER_DEBUG)
		return p_cds_context->driver_debug_log_level;
	else if (ring_id == RING_ID_FIRMWARE_DEBUG)
		return p_cds_context->fw_debug_log_level;

	return WLAN_LOG_LEVEL_OFF;
}

/**
 * cds_set_multicast_logging() - Set mutlicast logging value
 * @value: Value of multicast logging
 *
 * Set the multicast logging value which will indicate
 * whether to multicast host and fw messages even
 * without any registration by userspace entity
 *
 * Return: None
 */
void cds_set_multicast_logging(uint8_t value)
{
	cds_multicast_logging = value;
}

/**
 * cds_is_multicast_logging() - Get multicast logging value
 *
 * Get the multicast logging value which will indicate
 * whether to multicast host and fw messages even
 * without any registration by userspace entity
 *
 * Return: 0 - Multicast logging disabled, 1 - Multicast logging enabled
 */
uint8_t cds_is_multicast_logging(void)
{
	return cds_multicast_logging;
}

/*
 * cds_init_log_completion() - Initialize log param structure
 *
 * This function is used to initialize the logging related
 * parameters
 *
 * Return: None
 */
void cds_init_log_completion(void)
{
	p_cds_contextType p_cds_context;

	p_cds_context = cds_get_global_context();
	if (!p_cds_context) {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
				"%s: cds context is Invalid", __func__);
		return;
	}

	p_cds_context->log_complete.is_fatal = WLAN_LOG_TYPE_NON_FATAL;
	p_cds_context->log_complete.indicator = WLAN_LOG_INDICATOR_UNUSED;
	p_cds_context->log_complete.reason_code = WLAN_LOG_REASON_CODE_UNUSED;
	p_cds_context->log_complete.is_report_in_progress = false;
	/* Attempting to initialize an already initialized lock
	 * results in a failure. This must be ok here.
	 */
	cdf_spinlock_init(&p_cds_context->bug_report_lock);
}

/**
 * cds_deinit_log_completion() - Deinitialize log param structure
 *
 * This function is used to deinitialize the logging related
 * parameters
 *
 * Return: None
 */
void cds_deinit_log_completion(void)
{
	p_cds_contextType p_cds_context;

	p_cds_context = cds_get_global_context();
	if (!p_cds_context) {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
				"%s: cds context is Invalid", __func__);
		return;
	}

	cdf_spinlock_destroy(&p_cds_context->bug_report_lock);
}

/**
 * cds_set_log_completion() - Store the logging params
 * @is_fatal: Indicates if the event triggering bug report is fatal or not
 * @indicator: Source which trigerred the bug report
 * @reason_code: Reason for triggering bug report
 *
 * This function is used to set the logging parameters based on the
 * caller
 *
 * Return: 0 if setting of params is successful
 */
CDF_STATUS cds_set_log_completion(uint32_t is_fatal,
		uint32_t indicator,
		uint32_t reason_code)
{
	p_cds_contextType p_cds_context;

	p_cds_context = cds_get_global_context();
	if (!p_cds_context) {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
				"%s: cds context is Invalid", __func__);
		return CDF_STATUS_E_FAILURE;
	}

	cdf_spinlock_acquire(&p_cds_context->bug_report_lock);
	p_cds_context->log_complete.is_fatal = is_fatal;
	p_cds_context->log_complete.indicator = indicator;
	p_cds_context->log_complete.reason_code = reason_code;
	p_cds_context->log_complete.is_report_in_progress = true;
	cdf_spinlock_release(&p_cds_context->bug_report_lock);
	return CDF_STATUS_SUCCESS;
}

/**
 * cds_get_log_completion() - Get the logging related params
 * @is_fatal: Indicates if the event triggering bug report is fatal or not
 * @indicator: Source which trigerred the bug report
 * @reason_code: Reason for triggering bug report
 *
 * This function is used to get the logging related parameters
 *
 * Return: None
 */
void cds_get_log_completion(uint32_t *is_fatal,
		uint32_t *indicator,
		uint32_t *reason_code)
{
	p_cds_contextType p_cds_context;

	p_cds_context = cds_get_global_context();
	if (!p_cds_context) {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
				"%s: cds context is Invalid", __func__);
		return;
	}

	cdf_spinlock_acquire(&p_cds_context->bug_report_lock);
	*is_fatal =  p_cds_context->log_complete.is_fatal;
	*indicator = p_cds_context->log_complete.indicator;
	*reason_code = p_cds_context->log_complete.reason_code;
	p_cds_context->log_complete.is_report_in_progress = false;
	cdf_spinlock_release(&p_cds_context->bug_report_lock);
}

/**
 * cds_is_log_report_in_progress() - Check if bug reporting is in progress
 *
 * This function is used to check if the bug reporting is already in progress
 *
 * Return: true if the bug reporting is in progress
 */
bool cds_is_log_report_in_progress(void)
{
	p_cds_contextType p_cds_context;

	p_cds_context = cds_get_global_context();
	if (!p_cds_context) {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
				"%s: cds context is Invalid", __func__);
		return true;
	}
	return p_cds_context->log_complete.is_report_in_progress;
}

/**
 * cds_flush_logs() - Report fatal event to userspace
 * @is_fatal: Indicates if the event triggering bug report is fatal or not
 * @indicator: Source which trigerred the bug report
 * @reason_code: Reason for triggering bug report
 *
 * This function sets the log related params and send the WMI command to the
 * FW to flush its logs. On receiving the flush completion event from the FW
 * the same will be conveyed to userspace
 *
 * Return: 0 on success
 */
CDF_STATUS cds_flush_logs(uint32_t is_fatal,
		uint32_t indicator,
		uint32_t reason_code)
{
	uint32_t ret;
	CDF_STATUS status;

	p_cds_contextType p_cds_context;

	p_cds_context = cds_get_global_context();
	if (!p_cds_context) {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
				"%s: cds context is Invalid", __func__);
		return CDF_STATUS_E_FAILURE;
	}

	if (cds_is_log_report_in_progress() == true) {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
				"%s: Bug report already in progress - dropping! type:%d, indicator=%d reason_code=%d",
				__func__, is_fatal, indicator, reason_code);
		return CDF_STATUS_E_FAILURE;
	}

	status = cds_set_log_completion(is_fatal, indicator, reason_code);
	if (CDF_STATUS_SUCCESS != status) {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
			"%s: Failed to set log trigger params", __func__);
		return CDF_STATUS_E_FAILURE;
	}

	CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_INFO,
			"%s: Triggering bug report: type:%d, indicator=%d reason_code=%d",
			__func__, is_fatal, indicator, reason_code);

	ret = sme_send_flush_logs_cmd_to_fw(p_cds_context->pMACContext);
	if (0 != ret) {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
				"%s: Failed to send flush FW log", __func__);
		cds_init_log_completion();
		return CDF_STATUS_E_FAILURE;
	}

	return CDF_STATUS_SUCCESS;
}

/**
 * cds_logging_set_fw_flush_complete() - Wrapper for FW log flush completion
 *
 * This function is used to send signal to the logger thread to indicate
 * that the flushing of FW logs is complete by the FW
 *
 * Return: None
 *
 */
void cds_logging_set_fw_flush_complete(void)
{
	wlan_logging_set_fw_flush_complete();
}
