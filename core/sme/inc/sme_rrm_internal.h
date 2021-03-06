/*
 * Copyright (c) 2011-2012, 2014-2015 The Linux Foundation. All rights reserved.
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

#if !defined(__SMERRMINTERNAL_H)
#define __SMERRMINTERNAL_H

/**
 * \file  sme_rrm_internal.h
 *
 * \brief prototype for SME RRM APIs
 */

/*--------------------------------------------------------------------------
  Include Files
  ------------------------------------------------------------------------*/
#include "cdf_lock.h"
#include "cdf_trace.h"
#include "cdf_memory.h"
#include "cdf_types.h"
#include "rrm_global.h"

/*--------------------------------------------------------------------------
  Type declarations
  ------------------------------------------------------------------------*/
typedef struct sRrmNeighborReportDesc {
	tListElem List;
	tSirNeighborBssDescription *pNeighborBssDescription;
	uint32_t roamScore;
	uint8_t sessionId;
} tRrmNeighborReportDesc, *tpRrmNeighborReportDesc;

typedef void (*NeighborReportRspCallback)(void *context,
		CDF_STATUS cdf_status);

typedef struct sRrmNeighborRspCallbackInfo {
	uint32_t timeout;       /* in ms.. min value is 10 (10ms) */
	NeighborReportRspCallback neighborRspCallback;
	void *neighborRspCallbackContext;
} tRrmNeighborRspCallbackInfo, *tpRrmNeighborRspCallbackInfo;

typedef struct sRrmNeighborRequestControlInfo {
	/* To check whether a neighbor req is already sent & response pending */
	bool isNeighborRspPending;
	cdf_mc_timer_t neighborRspWaitTimer;
	tRrmNeighborRspCallbackInfo neighborRspCallbackInfo;
} tRrmNeighborRequestControlInfo, *tpRrmNeighborRequestControlInfo;

typedef struct sRrmSMEContext {
	uint16_t token;
	struct cdf_mac_addr sessionBssId;
	uint8_t regClass;
	/* list of all channels to be measured. */
	tCsrChannelInfo channelList;
	uint8_t currentIndex;
	/* SSID used in the measuring beacon report. */
	tAniSSID ssId;
	tSirMacAddr bssId;      /* bssid used for beacon report measurement. */
	/* Randomization interval to be used in subsequent measurements. */
	uint16_t randnIntvl;
	uint16_t duration[SIR_ESE_MAX_MEAS_IE_REQS];
	uint8_t measMode[SIR_ESE_MAX_MEAS_IE_REQS];
	struct rrm_config_param rrmConfig;
	cdf_mc_timer_t IterMeasTimer;
	tDblLinkList neighborReportCache;
	tRrmNeighborRequestControlInfo neighborReqControlInfo;

#if defined(FEATURE_WLAN_ESE) && defined(FEATURE_WLAN_ESE_UPLOAD)
	tCsrEseBeaconReq eseBcnReqInfo;
	bool eseBcnReqInProgress;
#endif /* FEATURE_WLAN_ESE && FEATURE_WLAN_ESE_UPLOAD */
	tRrmMsgReqSource msgSource;
} tRrmSMEContext, *tpRrmSMEContext;

typedef struct sRrmNeighborReq {
	uint8_t no_ssid;
	tSirMacSSid ssid;
} tRrmNeighborReq, *tpRrmNeighborReq;

#endif /* #if !defined( __SMERRMINTERNAL_H ) */
