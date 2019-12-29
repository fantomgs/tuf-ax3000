/*
 *	<<Broadcom-WL-IPTag/Proprietary:>>
 *
 *	wlc_taf.c
*
*	This file implements the WL driver infrastructure for the TAF module.
*
*      Copyright 2019 Broadcom
*
*      This program is the proprietary software of Broadcom and/or
*      its licensors, and may only be used, duplicated, modified or distributed
*      pursuant to the terms and conditions of a separate, written license
*      agreement executed between you and Broadcom (an "Authorized License").
*      Except as set forth in an Authorized License, Broadcom grants no license
*      (express or implied), right to use, or waiver of any kind with respect to
*      the Software, and Broadcom expressly reserves all rights in and to the
*      Software and all intellectual property rights therein.  IF YOU HAVE NO
*      AUTHORIZED LICENSE, THEN YOU HAVE NO RIGHT TO USE THIS SOFTWARE IN ANY
*      WAY, AND SHOULD IMMEDIATELY NOTIFY BROADCOM AND DISCONTINUE ALL USE OF
*      THE SOFTWARE.
*
*      Except as expressly set forth in the Authorized License,
*
*      1. This program, including its structure, sequence and organization,
*      constitutes the valuable trade secrets of Broadcom, and you shall use
*      all reasonable efforts to protect the confidentiality thereof, and to
*      use this information only in connection with your use of Broadcom
*      integrated circuit products.
*
*      2. TO THE MAXIMUM EXTENT PERMITTED BY LAW, THE SOFTWARE IS PROVIDED
*      "AS IS" AND WITH ALL FAULTS AND BROADCOM MAKES NO PROMISES,
*      REPRESENTATIONS OR WARRANTIES, EITHER EXPRESS, IMPLIED, STATUTORY, OR
*      OTHERWISE, WITH RESPECT TO THE SOFTWARE.  BROADCOM SPECIFICALLY
*      DISCLAIMS ANY AND ALL IMPLIED WARRANTIES OF TITLE, MERCHANTABILITY,
*      NONINFRINGEMENT, FITNESS FOR A PARTICULAR PURPOSE, LACK OF VIRUSES,
*      ACCURACY OR COMPLETENESS, QUIET ENJOYMENT, QUIET POSSESSION OR
*      CORRESPONDENCE TO DESCRIPTION. YOU ASSUME THE ENTIRE RISK ARISING
*      OUT OF USE OR PERFORMANCE OF THE SOFTWARE.
*
*      3. TO THE MAXIMUM EXTENT PERMITTED BY LAW, IN NO EVENT SHALL
*      BROADCOM OR ITS LICENSORS BE LIABLE FOR (i) CONSEQUENTIAL, INCIDENTAL,
*      SPECIAL, INDIRECT, OR EXEMPLARY DAMAGES WHATSOEVER ARISING OUT OF OR
*      IN ANY WAY RELATING TO YOUR USE OF OR INABILITY TO USE THE SOFTWARE EVEN
*      IF BROADCOM HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES; OR (ii)
*      ANY AMOUNT IN EXCESS OF THE AMOUNT ACTUALLY PAID FOR THE SOFTWARE ITSELF
*      OR U.S. $1, WHICHEVER IS GREATER. THESE LIMITATIONS SHALL APPLY
*      NOTWITHSTANDING ANY FAILURE OF ESSENTIAL PURPOSE OF ANY LIMITED REMEDY.
*
*	$Id$
*/
/*
 * Include files.
 */
#include <wlc.h>
#include <wlc_scb.h>
#include <wlc_dump.h>
#ifdef BCM_HOST_MEM_SCB
#include <wlc_scb_alloc.h>
#endif // endif

#include <wlc_ampdu.h>

#ifdef WLCFP
#include <wlc_key.h>
#include <wlc_cfp.h>
#include <wlc_cfp_priv.h>
#endif // endif
#include <wl_export.h>

#include <wlc_taf.h>
#include <wlc_taf_cmn.h>

#ifdef TAF_DEBUG_VERBOSE
#ifdef DONGLEBUILD
/*
 * used to temporarily avoid assert during attach otherwise DHD does not start and we don't see
 * the message
 */
bool taf_attach_complete = FALSE;
#endif /* DONGLEBUILD */
bool taf_trace = FALSE;
const char* taf_assert_fail = "TAF ASSERT fail";
#endif /* TAF_DEBUG_VERBOSE */

const char* wlc_taf_ordering_name[TAF_ORDER_NUM_OPTIONS] = {"TID order by SCB unified",
							    "SCB order with TID parallel"};
const uint8 wlc_taf_prec2prio[WLC_PREC_COUNT] = {
	2,	/* 0 */		2,
	1,	/* 2 */		1,
	0,	/* 4 */		0,
	3,	/* 6 */		3,
	4,	/* 8 */		4,
	5,	/* 10 */	5,
	6,	/* 12 */	6,
	7,	/* 14 */	7
};

static INLINE bool taf_link_state_method_update(taf_method_info_t* method, struct scb* scb, int tid,
	taf_source_type_t s_idx, taf_link_state_t state);

static INLINE int taf_scb_state_method_update(taf_method_info_t* method, struct scb* scb,
	void* update, taf_scb_state_t state);

static bool taf_is_bypass(wlc_taf_info_t *taf_info);

static taf_method_info_t* taf_get_method_by_name(wlc_taf_info_t* taf_info, const char* cmd);
static taf_list_t* taf_list_ea_find(taf_list_t** head, const struct ether_addr*  ea);

static void taf_list_demote_item(taf_method_info_t* method, uint32 prio);
static void taf_move_list_item(taf_scb_cubby_t* scb_taf, taf_method_info_t* dest_method);

typedef struct {
	void *  (*attach_fn) (wlc_taf_info_t *, taf_scheduler_kind);
	int     (*detach_fn) (void *);
} taf_scheduler_def_t;

static const taf_scheduler_def_t  taf_scheduler_definitions[NUM_TAF_SCHEDULERS] = {
#if TAF_ENABLE_SQS_PULL && defined(WLTAF_IAS)
	{
		/* TAF_VIRTUAL_MARKUP, */
		wlc_taf_ias_method_attach,
		wlc_taf_ias_method_detach
	},
#endif // endif
#ifdef WLTAF_IAS
	{
		/* TAF_EBOS, */
		wlc_taf_ias_method_attach,
		wlc_taf_ias_method_detach
	},

	{
		/* TAF_PSEUDO_RR, */
		wlc_taf_ias_method_attach,
		wlc_taf_ias_method_detach
	},

	{
		/* TAF_ATOS, */
		wlc_taf_ias_method_attach,
		wlc_taf_ias_method_detach
	},

	{
		/* TAF_ATOS2, */
		wlc_taf_ias_method_attach,
		wlc_taf_ias_method_detach
	},
#endif /* WLTAF_IAS */
};

const char* taf_undefined_string = "(undefined)";

const char* taf_tx_sources[TAF_NUM_SCHED_SOURCES + 1] = {
#if TAF_ENABLE_NAR
	TAF_NAR,
#endif // endif
#if TAF_ENABLE_AMPDU
	TAF_AMPDU,
#endif // endif
#if TAF_ENABLE_SQS_PULL
	TAF_SQSHOST,
#endif // endif
	"undefined"
};

#if defined(TAF_DBG) || defined(BCMDBG)
char* taf_trace_buf = NULL;
uint32 taf_trace_buf_len = 0;
int taf_trace_index = 0;
#endif // endif

#ifdef TAF_DEBUG_VERBOSE

#ifdef DONGLEBUILD
#define tputc(c)    putc(c)
#else
#define tputc(c)    printf("%c", (c))
#endif /* DONGLEBUILD */

void taf_memtrace_dump(void)
{
	if (taf_trace_buf && ((taf_trace_index > 0) || taf_trace_buf[0] != 0))
	{
		int index = taf_trace_index;

		WL_PRINT(("==========================TAF TRACE DUMP==========================\n"));
		OSL_DELAY(50);

		taf_trace_buf[index++] = 0;
		if (index == taf_trace_buf_len) {
			index = 0;
		}
		if (taf_trace_buf[index] == 0) {
			index = 0;
		}
		while (index < taf_trace_buf_len && taf_trace_buf[index]) {
			while (index < taf_trace_buf_len && taf_trace_buf[index] &&
					taf_trace_buf[index] != '\n') {
				tputc(taf_trace_buf[index++]);
			}
			if (index < taf_trace_buf_len &&
					taf_trace_buf[index] == '\n') {
				tputc(taf_trace_buf[index++]);
				OSL_DELAY(5);
			}
		}
		if (index == taf_trace_buf_len) {
			index = 0;
			while (index < taf_trace_index && taf_trace_buf[index]) {
				while (index < taf_trace_index &&
						taf_trace_buf[index] &&
						taf_trace_buf[index] != '\n') {
					tputc(taf_trace_buf[index++]);
				}
				if (index < taf_trace_index &&
						taf_trace_buf[index] == '\n') {
					tputc(taf_trace_buf[index++]);
					OSL_DELAY(50);
				}
			}
		}
		WL_PRINT(("==================================================================\n"));
	}
}
#endif /* TAF_DEBUG_VERBOSE */

#ifdef TAF_DBG
const char* taf_rel_complete_text[NUM_TAF_REL_COMPLETE_TYPES] = {
	"null",
	"nothing to send",
	"src emptied",
	"ps mode src emptied",
	"output full",
	"output blocked",
	"no buffer",
	"time limit",
	"release limit",
	"error",
	"ps mode blocked",
	"src limited",
	"fulfilled"
};

const char* taf_link_states_text[NUM_TAF_LINKSTATE_TYPES] = {
	"none",
	"init",
	"activated",
	"de-activated",
	"clean",
	"hard reset",
	"soft reset",
	"removed",
	"amsdu"
};

const char* taf_scb_states_text[NUM_TAF_SCBSTATE_TYPES] = {
	"none",
	"init",
	"exit",
	"reset",
	"source enable",
	"source disable",
	"wds",
	"dwds",
	"source update",
	"bsscfg update",
	"power save",
	"off channel",
	"data block other",
	"update mu-mimo",
	"check traffic active"
};

const char* taf_txpkt_status_text[TAF_NUM_TXPKT_STATUS_TYPES] = {
	"none",
	"regmpdu",
	"pktfree",
	"pktfree_reset",
	"pktfree_drop",
	"update retries",
	"update packets",
	"update rate",
	"suppressed",
	"suppressed free",
	"suppressed queued"
};

const char* taf_sched_state_text[TAF_NUM_SCHED_STATE_TYPES] = {
	"none",
	"data-block",
	"rewind",
	"reset"
};
#endif /* TAF_DBG */

/*
 * Module iovar handling.
 */
enum {
	IOV_TAF_DEFINE    /* universal configuration */
};

static const bcm_iovar_t taf_iovars[] = {
	{"taf", IOV_TAF_DEFINE, 0, 0, IOVT_BUFFER, sizeof(wl_taf_define_t)},
	{NULL, 0, 0, 0, 0, 0}
};

taf_source_type_t wlc_taf_get_source_index(const void * data)
{
	taf_source_type_t s_idx;
	const char* source = (const char*)data;

	for (s_idx = 0; s_idx < TAF_NUM_SCHED_SOURCES && source != NULL; s_idx++) {
		if (!strcmp(TAF_SOURCE_NAME(s_idx), source)) {
			return s_idx;
		}
	}
	return TAF_SOURCE_UNDEFINED;
}

static int
taf_set_cubby_method(taf_scb_cubby_t *scb_taf, taf_method_info_t *dst_method, uint32 prio)
{
	taf_list_scoring_order_t ordering = dst_method->ordering;

	taf_move_list_item(scb_taf, dst_method);
#if TAF_ENABLE_SQS_PULL
	taf_scb_state_method_update(dst_method, scb_taf->scb, TAF_SQSHOST,
		TAF_SCBSTATE_SOURCE_ENABLE);
#endif // endif
	if (ordering != TAF_LIST_DO_NOT_SCORE) {
		if (ordering == TAF_LIST_SCORE_FIXED_INIT_MINIMUM) {
			/* demote existing entry at this priority (if it exists) */
			taf_list_demote_item(dst_method, prio);

		} else if (ordering == TAF_LIST_SCORE_FIXED_INIT_MAXIMUM) {
			TAF_ASSERT(0);
		}
		scb_taf->score = prio;
		wlc_taf_sort_list(&dst_method->list, ordering);
	}
	return BCME_OK;
}

static int taf_dump_list(taf_method_info_t* method, struct bcmstrbuf* b)
{
	taf_list_t* iter;
	uint32 list_index = 0;

	TAF_ASSERT(method);

	iter = method->list;

	if (iter) {
		bcm_bprintf(b, "Assigned %s entries:\n", TAF_SCHED_NAME(method));
	}
	else {
		bcm_bprintf(b, "No assigned entries\n");
	}

	while (iter) {
		bcm_bprintf(b, "%3d: "MACF, ++list_index, ETHER_TO_MACF(iter->scb_taf->ea));

		if ((method->ordering == TAF_LIST_SCORE_FIXED_INIT_MINIMUM) ||
				(method->ordering == TAF_LIST_SCORE_FIXED_INIT_MAXIMUM)) {
			bcm_bprintf(b, " : %u\n", iter->scb_taf->score);
		}
		else {
			bcm_bprintf(b, "\n");
		}
		iter = iter->next;
	}
	return BCME_OK;
}

int wlc_taf_param(const char** cmd, uint32* param, uint32 min, uint32 max, struct bcmstrbuf* b)
{
	*cmd += strlen(*cmd) + 1;

	if (**cmd) {
		/* an extra parameter was supplied, treat it as a 'set' operation */
		uint32 value;

		value = bcm_strtoul(*cmd, NULL, 0);
		if (value > max || value < min) {
			return BCME_RANGE;
		}
		*param = value;
		if (b) {
			bcm_bprintf(b, "%c", '\0');
		}
		return TAF_IOVAR_OK_SET;
	}
	else if (b) {
		/* no more parameter was given, treat as a 'get' operation */
		if (*param > 16) {
			bcm_bprintf(b, "%u (0x%x)\n", *param, *param);
		}
		else {
			bcm_bprintf(b, "%u\n", *param);
		}
		return TAF_IOVAR_OK_GET;
	}
	/* should never get here if buffer (b) is OK */
	return BCME_BUFTOOSHORT;
}

static int taf_method_iovar(wlc_taf_info_t* taf_info, taf_scheduler_kind type, const char* cmd,
	wl_taf_define_t* result, struct bcmstrbuf* b)
{
	taf_method_info_t* method = taf_get_method_info(taf_info, type);
	int err = BCME_UNSUPPORTED;

	if (method && method->funcs.iovar_fn) {
		err = method->funcs.iovar_fn(method, *cmd ? cmd : NULL, result, b);
	} else {
		WL_TAFM(method, "TO DO\n");
	}
	return err;
}

static int taf_define(wlc_taf_info_t* taf_info, const struct ether_addr* ea,
	wl_taf_define_t* result, wl_taf_define_t* input, struct bcmstrbuf* b, bool set)
{

	taf_scheduler_kind type;
	taf_method_info_t* method;
	const struct ether_addr undef_ea = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
	wlc_info_t*        wlc = TAF_WLCT(taf_info);
	char               local_input_cache[128];
	const char*        cmd = local_input_cache;
	int                copyi = 0;

	while ((input->text[copyi] || input->text[copyi + 1]) &&
			(copyi < (ARRAYSIZE(local_input_cache) - 2))) {
		local_input_cache[copyi] = input->text[copyi];
		copyi++;
	}
	local_input_cache[copyi++] = 0;
	local_input_cache[copyi++] = 0;

	/* if undef_ea is set, process as text arguments */
	if (eacmp(ea, &undef_ea) == 0) {
		result->ea = undef_ea;

		/* no argument given */
		if (cmd[0] == 0) {
			bcm_bprintf(b, "taf is %sabled (%u), use 'taf enable 0|1' to change\n",
				taf_info->enabled ? "en" : "dis", taf_info->enabled);
			return BCME_OK;
		}

#ifdef TAF_DEBUG_VERBOSE
		if (!strcmp(cmd, "trace_buf")) {
			uint32 len = taf_trace_buf_len / 1024;
			int err = wlc_taf_param(&cmd, &len, 0, 8, b);
			if ((err == TAF_IOVAR_OK_SET) && (taf_trace_buf != NULL) &&
					(taf_trace_buf_len > 0)) {
				MFREE(TAF_WLCT(taf_info)->pub->osh, taf_trace_buf,
					taf_trace_buf_len);
				taf_trace_buf = NULL;
				taf_trace_buf_len = 0;
			}
			if (err == TAF_IOVAR_OK_SET && len > 0) {
				taf_trace_buf_len = len * 1024;
				taf_trace_buf = MALLOCZ(wlc->pub->osh, taf_trace_buf_len);
				if (taf_trace_buf == 0) {
					taf_trace_buf_len = 0;
					return BCME_NOMEM;
				}
				taf_trace_index = 0;
			}
			return err;

		}
#endif /* TAF_DEBUG_VERBOSE */
		if (!strcmp(cmd, "enable")) {
			uint32 state = taf_info->enabled ? 1 : 0;
			int err = wlc_taf_param(&cmd, &state, 0, 1, b);

			/* try to interpret whether text param is 'set' or 'get' */
			if (!set && (err == TAF_IOVAR_OK_SET) && *cmd &&
					(state != (taf_info->enabled ? 1 : 0))) {
				set = TRUE;
			}
			/* was not in down state and there was attempt to change it */
			if (set && wlc->pub->up) {
				return BCME_NOTDOWN;
			}
			taf_info->enabled = state ? 1 : 0;
			result->misc = taf_info->enabled;
#ifdef TAF_DEBUG_VERBOSE
			if (taf_info->enabled) {
				uint32 len = 0; /* default disabled */
				if ((taf_trace_buf != NULL) && (taf_trace_buf_len > 0)) {
					MFREE(TAF_WLCT(taf_info)->pub->osh, taf_trace_buf,
					      taf_trace_buf_len);
					taf_trace_buf = NULL;
					taf_trace_buf_len = 0;
				}
				if (len > 0) {
					taf_trace_buf_len = len * 1024;
					taf_trace_buf = MALLOCZ(wlc->pub->osh, taf_trace_buf_len);
					if (taf_trace_buf == 0) {
						taf_trace_buf_len = 0;
						return BCME_NOMEM;
					}
				}
				taf_trace_index = 0;
			} else {
				if ((taf_trace_buf != NULL) && (taf_trace_buf_len > 0)) {
					MFREE(TAF_WLCT(taf_info)->pub->osh, taf_trace_buf,
					      taf_trace_buf_len);
					taf_trace_buf = NULL;
					taf_trace_buf_len = 0;
				}
			}
#endif /* TAF_DEBUG_VERBOSE */
#if TAF_ENABLE_SQS_PULL
			/* Configure BUS side scheduler based on TAF state */
			wl_bus_taf_scheduler_config(wlc->wl, taf_info->enabled &&
				!taf_info->bypass);
#endif /* TAF_ENABLE_SQS_PULL */
			return err;
		}
		if (!taf_info->enabled) {
			return BCME_NOTREADY;
		}
		if (!wlc->pub->up) {
			return BCME_NOTUP;
		}

		if (!strcmp(cmd, "order")) {
#if TAF_ENABLE_SQS_PULL
			const int err = BCME_OK;
#else
			int err = wlc_taf_param(&cmd, &taf_info->pending_ordering,
				TAF_ORDER_TID_SCB, TAF_ORDER_TID_PARALLEL, b);

			if (taf_info->ordering != taf_info->pending_ordering) {
				result->misc = taf_info->pending_ordering;
				bcm_bprintf(b, "taf order will change to be");
			}
			else
#endif /* TAF_ENABLE_SQS_PULL */
			{
				result->misc = taf_info->ordering;
				bcm_bprintf(b, "taf order is");
			}
			bcm_bprintf(b, " %d: %s\n", result->misc,
				wlc_taf_ordering_name[result->misc]);

			return err;
		}
		if (!strcmp(cmd, "bypass")) {
			uint32 state = taf_info->bypass;
			bool   prev_bypass = taf_info->bypass;
			int err = wlc_taf_param(&cmd, &state, FALSE, TRUE, b);

			taf_info->bypass = state;
			result->misc = taf_info->bypass;

			if (prev_bypass && !taf_info->bypass) {
				wlc_taf_reset_scheduling(taf_info, ALLPRIO, TRUE);
			}
#if TAF_ENABLE_SQS_PULL
			/* Configure BUS side scheduler based on TAF state */
			wl_bus_taf_scheduler_config(wlc->wl, taf_info->enabled &&
				!taf_info->bypass);
#endif /* TAF_ENABLE_SQS_PULL */
			return err;
		}
#ifdef WLTAF_IAS
		/* all of these are here for backwards compatibility with previous version where
		 * EBOS/ATOS/ATOS2 where inside TAF and had direct level iovar control
		 */
		if (!strcmp(cmd, "high") || !strcmp(cmd, "low")) {
			taf_scheduler_kind etype;
			int eresult;
			for (etype = FIRST_IAS_SCHEDULER; etype <= LAST_IAS_SCHEDULER; etype ++) {
				eresult = taf_method_iovar(taf_info, etype, cmd, result, b);
				if (eresult == TAF_IOVAR_OK_GET || eresult < BCME_OK) {
					return eresult;
				}
			}
			return eresult;
		}
		/* all of these are here for backwards compatibility with previous version TAF */
		if (!strcmp(cmd, "fallback") || !strcmp(cmd, "adapt") ||
				!strcmp(cmd, "high_max") || !strcmp(cmd, "low_max")) {
			return taf_method_iovar(taf_info, TAF_SCHEDULER_START, cmd, result, b);
		}
		/* all of these are here for backwards compatibility with previous version TAF */
		if (!strcmp(cmd, "atos_high") || !strcmp(cmd, "atos_low")) {
			return taf_method_iovar(taf_info, TAF_ATOS, cmd + sizeof("atos"),
				result, b);
		}
		/* all of these are here for backwards compatibility with previous version TAF */
		if (!strcmp(cmd, "atos2_high") || !strcmp(cmd, "atos2_low")) {
			return taf_method_iovar(taf_info, TAF_ATOS2, cmd + sizeof("atos2"),
				result, b);
		}
#endif /* WLTAF_IAS */
		if (!strcmp(cmd, "force")) {
			int err = wlc_taf_param(&cmd, &taf_info->force_time, 0,
				TAF_WINDOW_MAX, b);
			result->misc = taf_info->force_time;
			return err;
		}
		if (!strcmp(cmd, "list")) {
			for (type = TAF_SCHEDULER_START; type < TAF_SCHED_LAST_INDEX; type++) {
				method = taf_get_method_info(taf_info, type);
				bcm_bprintf(b, "%u: %s%s\n", type, TAF_SCHED_NAME(method),
					type == taf_info->default_scheduler ? " (default)":"");
			}
			return BCME_OK;
		}
		if (!strcmp(cmd, "default")) {
			cmd += strlen(cmd) + 1;

			if (*cmd) {
				if ((method = taf_get_method_by_name(taf_info, cmd))) {
					taf_info->default_scheduler = method->type;
					return BCME_OK;
				}
				return BCME_BADARG;
			}
			result->misc = taf_info->default_scheduler;
			method = taf_get_method_info(taf_info, taf_info->default_scheduler);
			bcm_bprintf(b, "%u: %s\n", result->misc, TAF_SCHED_NAME(method));
			return BCME_OK;
		}
		if (!strcmp(cmd, "wd_stall")) {
			uint32 wd_stall_limit = taf_info->watchdog_data_stall_limit;
			int err = wlc_taf_param(&cmd, &wd_stall_limit, 0, BCM_UINT16_MAX, b);
			if (err == TAF_IOVAR_OK_SET) {
				taf_info->watchdog_data_stall_limit = wd_stall_limit;
			}
			result->misc = taf_info->watchdog_data_stall_limit;
			return err;
		}
#ifdef TAF_MU_TX
		if (!strcmp(cmd, "mu_extend_coeff")) {
			int err = wlc_taf_param(&cmd, &taf_info->mu_extend_coeff,
				TAF_MU_EXTEND_COEFF_MIN, TAF_MU_EXTEND_COEFF_MAX, b);
			result->misc = taf_info->mu_extend_coeff;
			return err;
		}
#endif // endif
		if ((method = taf_get_method_by_name(taf_info, cmd))) {
			int err;
			cmd += strlen(cmd) + 1;

			if (*cmd) {
				if (!strcmp(cmd, "list")) {
					return taf_dump_list(method, b);
				}
				if (!strcmp(cmd, "default")) {
					taf_info->default_scheduler = method->type;
					taf_info->default_score = method->score_init;
					return BCME_OK;
				}
				if (!strcmp(cmd, "dump")) {
					if (method->funcs.dump_fn) {
						return method->funcs.dump_fn(method, b);
					}
				}
			}
			err = taf_method_iovar(taf_info, method->type, cmd, result, b);
			if (*cmd || (err != BCME_UNSUPPORTED)) {
				return err;
			}
			if (!*cmd) {
				return taf_dump_list(method, b);
			}
			return BCME_UNSUPPORTED;
		}
		return BCME_UNSUPPORTED;
	}

	/* at this point, a MAC address was supplied so it is not a text command. */

	/* find entry by MAC address operation */
	for (type = TAF_SCHEDULER_START; type < TAF_SCHED_LAST_INDEX; type++) {
		taf_list_t* found;
		taf_method_info_t* dst = NULL;
		taf_scb_cubby_t* scb_taf;
		bool fixed_scoring = FALSE;

		method = taf_get_method_info(taf_info, type);
		found = taf_list_ea_find(&method->list, ea);

		if (!found) {
			continue;
		}
		scb_taf = found->scb_taf;

		if (scb_taf && !strcmp(cmd, "reset")) {
			int tid;
			taf_source_type_t s_idx;

			WL_TAFM(scb_taf->method, MACF" iovar reset\n", CONST_ETHERP_TO_MACF(ea));

			for (s_idx = 0; s_idx < TAF_NUM_SCHED_SOURCES; s_idx++) {
				for (tid = 0; tid < TAF_NUMPRIO(s_idx); tid++) {
					taf_link_state_method_update(scb_taf->method, scb_taf->scb,
						tid, s_idx, TAF_LINKSTATE_HARD_RESET);
				}
			}
			taf_scb_state_method_update(scb_taf->method, scb_taf->scb, NULL,
				TAF_SCBSTATE_RESET);
			return BCME_OK;
		}

		if (!set) {
			/* this is 'get' */
			result->ea = *ea;
			result->sch = type;
			result->prio = found->scb_taf->score;
			result->misc = 0;
			bcm_bprintf(b, "%s", TAF_SCHED_NAME(method));
			return BCME_OK;
		}

		/* this is a 'set' */
		/* was a valid (numeric) scheduler given? */

		if (input->sch != (uint32)(~0)) {
			dst = taf_get_method_info(taf_info, input->sch);

		} else if (input->text[0]) {
			dst = taf_get_method_by_name(taf_info, (const char*)input->text);
		}

		if (!dst) {
			return BCME_NOTFOUND;
		}

		fixed_scoring = dst->ordering == TAF_LIST_SCORE_FIXED_INIT_MINIMUM ||
			dst->ordering == TAF_LIST_SCORE_FIXED_INIT_MAXIMUM;

		/* check priority correctly configured */
		if ((fixed_scoring && !input->prio) || (!fixed_scoring && input->prio)) {
			return BCME_BADARG;
		}

		if (dst->type != scb_taf->method->type ||
			(fixed_scoring && (input->prio != scb_taf->score))) {

			uint32 new_score = 0;
			int res;

			if (fixed_scoring) {
				new_score = input->prio;

			} else if (dst->ordering == TAF_LIST_SCORE_MINIMUM) {
				new_score = TAF_SCORE_MIN;

			} else if (dst->ordering == TAF_LIST_SCORE_MAXIMUM) {
				new_score = TAF_SCORE_MAX;

			} else {
				TAF_ASSERT(dst->ordering == TAF_LIST_DO_NOT_SCORE);
			}
			res = taf_set_cubby_method(scb_taf, dst, new_score);

			if (res != BCME_OK) {
				return res;
			}
		} else {
			WL_TAFT(taf_info, MACF" is unchanged (%s prio %u)\n",
				CONST_ETHERP_TO_MACF(ea), TAF_SCHED_NAME(dst),
				fixed_scoring ? scb_taf->score : 0);
		}
		return BCME_OK;
	}
	return BCME_NOTFOUND;
}

static int
taf_doiovar(void *handle, uint32 actionid, void *params, uint plen, void *arg, uint alen,
	uint vsize, struct wlc_if *wlcif)
{
	wlc_taf_info_t *taf_info = handle;
	int status = BCME_OK;

	if (D11REV_LT(TAF_WLCT(taf_info)->pub->corerev, 40)) {
		/* only support on latest chipsets */
		return BCME_UNSUPPORTED;
	}

	switch (actionid) {
		/* there is overlap in Set and Get for this iovar */
		case IOV_GVAL(IOV_TAF_DEFINE):
		case IOV_SVAL(IOV_TAF_DEFINE):
		{
			wl_taf_define_t* taf_def_return = (wl_taf_define_t*) arg;
			wl_taf_define_t* taf_def_input = (wl_taf_define_t*) params;
			const struct ether_addr ea = taf_def_input->ea;
			int result;

			struct bcmstrbuf b;
			int32  avail_len;

			/* only version 1 is currently supported */
			if (taf_def_input->version != 1) {
				WL_ERROR(("taf iovar version incorrect (%u/%u)\n",
					taf_def_input->version, 1));
				return BCME_VERSION;
			}

			avail_len = alen - OFFSETOF(wl_taf_define_t, text);
			avail_len = (avail_len > 0 ? avail_len : 0);
			b.origsize = b.size = avail_len;
			b.origbuf = b.buf = (char *) &taf_def_return->text[0];

			result = taf_define(taf_info, &ea, taf_def_return,
				taf_def_input, &b, actionid == IOV_SVAL(IOV_TAF_DEFINE));
			if (result == TAF_IOVAR_OK_SET || result == TAF_IOVAR_OK_GET) {
				result = BCME_OK;
			}
			return result;
		}
		break;

		default:
			status = BCME_UNSUPPORTED;
			break;

	}
	return status;
}

void BCMFASTPATH wlc_taf_list_add(taf_list_t** head, taf_list_t* item)
{
	if (head && *head == NULL) {
		*head = item;
		item->prev = NULL;
	}
	else {
		taf_list_t* iter = head ? *head : NULL;

		while (iter && iter->next) {
			iter = iter->next;
		}
		if (iter) {
			iter->next = item;
			item->prev = iter;
		}
	}
}

static taf_list_t* taf_list_ea_find(taf_list_t** head, const struct ether_addr*  ea)
{
	taf_list_t* iter = head ? *head : NULL;
	taf_scb_cubby_t* scb_taf;

	while (iter) {
		scb_taf = iter->scb_taf;
		if (eacmp(ea, (const char*)&(scb_taf->ea)) == 0) {
			return iter;
		}
		iter = iter->next;
	}
	return iter;
}

static INLINE taf_list_t* BCMFASTPATH taf_list_find(taf_list_t** head, taf_scb_cubby_t *scb_taf)
{
	return scb_taf ? &scb_taf->info.list : NULL;
}

void BCMFASTPATH wlc_taf_list_remove(taf_list_t** head, taf_list_t* item)
{
	taf_list_t*  above;
	taf_list_t*  below;

	if (!item) {
		return;
	}
	above = item->prev;
	below = item->next;

	if (head && item == *head) {
		*head = below;
	}

	if (above) {
		above->next = below;
	}
	if (below) {
		below->prev = above;
	}
	item->next = NULL;
	item->prev = NULL;
}

static void taf_list_demote_item(taf_method_info_t* method, uint32 prio)
{
	taf_list_t* found;

	/* only support minimum scoring currently */
	TAF_ASSERT(method->ordering == TAF_LIST_SCORE_MINIMUM ||
		method->ordering == TAF_LIST_SCORE_FIXED_INIT_MINIMUM);

	do {
		taf_list_t* iter = method->list;
		taf_scb_cubby_t* scb_taf;
		uint32 highest = 0;
		found = NULL;

		while (iter) {
			scb_taf = iter->scb_taf;

			if (scb_taf->score > highest) {
				highest = scb_taf->score;
			}
			if (scb_taf->score == prio) {
				found = iter;
			}
			iter = iter->next;
		}
		if (found) {
			found->scb_taf->score = highest + 1;
			WL_TAFM(method, "demoted item "MACF" with prio %u to prio %u\n",
				ETHER_TO_MACF(found->scb_taf->ea), prio, found->scb_taf->score);
		}
	} while (found);
}

static void taf_list_delete(wlc_taf_info_t* taf_info, taf_scb_cubby_t *scb_taf)
{
	taf_method_info_t* method = scb_taf->method;
	taf_list_t* item = taf_list_find(&method->list, scb_taf);

	if (item) {
		wlc_taf_list_remove(&method->list, item);
	}
}

INLINE static uint32 BCMFASTPATH taf_item_score(taf_list_t* item, bool *override)
{
	/*
	 * Put the 'force' links first, followed by the links with best scoring
	 * (most unrepresented).
	 * Push back links which have no traffic ready to send.
	 */
	*override = item->scb_taf->force ? TRUE : FALSE;

	return item->scb_taf->score;
}

static taf_list_t* BCMFASTPATH taf_list_maximum(taf_list_t *head)
{
	uint32 maximum = 0;
	taf_list_t *result = head;

	while (head) {
		bool max_score;
		uint32 score =  taf_item_score(head, &max_score);

		if (max_score) {
			result = head;
			break;
		} else if (score > maximum) {
			maximum = score;
			result = head;
		}
		head = head->next;
	}

	return result;
}

static taf_list_t* BCMFASTPATH taf_list_minimum(taf_list_t *head)
{
	uint32 minimum = TAF_SCORE_MAX;
	taf_list_t *result = head;

	while (head) {
		bool min_score;
		uint32 score = taf_item_score(head, &min_score);

		if (min_score) {
			result = head;
			break;
		} else if (score < minimum) {
			minimum = score;
			result = head;
		}
		head = head->next;
	}

	return result;
}

void BCMFASTPATH wlc_taf_sort_list(taf_list_t ** source_head, taf_list_scoring_order_t ordering)
{
	taf_list_t *local_list_head = NULL;
	taf_list_t*item;
	taf_list_t* (*find_fn) (taf_list_t *head);
	taf_list_t* local_list_end = NULL;

	if (ordering == TAF_LIST_SCORE_MINIMUM || ordering == TAF_LIST_SCORE_FIXED_INIT_MINIMUM) {

		find_fn = taf_list_minimum;

	} else if (ordering == TAF_LIST_SCORE_MAXIMUM ||
		ordering == TAF_LIST_SCORE_FIXED_INIT_MAXIMUM) {

		find_fn = taf_list_maximum;

	} else {
		return;
	}
	while ((item = find_fn(*source_head)) != NULL) {
		wlc_taf_list_remove(source_head, item);
		wlc_taf_list_add(local_list_end ? &local_list_end : &local_list_head, item);
		local_list_end = item;
	}
	*source_head = local_list_head;
}

static taf_list_t* taf_list_new(wlc_taf_info_t* taf_info, taf_scb_cubby_t *scb_taf)
{
	taf_list_t* list = NULL;

	if (scb_taf) {
		list = &scb_taf->info.list;
		bzero(list, sizeof(*list));
	}

	list->scb_taf = scb_taf;
	return list;
}

static void taf_move_list_item(taf_scb_cubby_t* scb_taf, taf_method_info_t* dest_method)
{
	taf_list_t** head_dst = &dest_method->list;
	taf_list_t*  item;
	taf_method_info_t* source_method = scb_taf->method;
	taf_scheduler_method_group source_group = source_method ?
		source_method->group : TAF_GROUP_UNDEFINED;
	taf_scheduler_method_group dest_group = dest_method->group;
	taf_link_state_t prev_state[TAF_NUM_SCHED_SOURCES][TAF_MAXPRIO];
	int tid;
	taf_source_type_t s_idx;

	if (source_method == dest_method) {
		return;
	}

	if (source_method) {
		item = taf_list_find(&source_method->list, scb_taf);
	} else {
		item = taf_list_new(dest_method->taf_info, scb_taf);
	}

	WL_TAFT(dest_method->taf_info, "removing "MACF" from %s and add to %s prio %d\n",
	        ETHER_TO_MACF(scb_taf->ea), TAF_SCHED_NAME(source_method),
	        TAF_SCHED_NAME(dest_method), scb_taf->score);

	if (source_method && (source_group != dest_group) &&
		(source_group != TAF_GROUP_UNDEFINED)) {

		for (s_idx = 0; s_idx < TAF_NUM_SCHED_SOURCES; s_idx++) {
			for (tid = 0; tid < TAF_NUMPRIO(s_idx); tid++) {

				prev_state[s_idx][tid] = scb_taf->info.linkstate[s_idx][tid];
				taf_link_state_method_update(source_method, scb_taf->scb, tid,
					s_idx, TAF_LINKSTATE_REMOVE);
				TAF_ASSERT(scb_taf->info.linkstate[s_idx][tid] ==
					TAF_LINKSTATE_NONE);
			}
		}

	}
	wlc_taf_list_remove(source_method ? &source_method->list : NULL, item);
	wlc_taf_list_add(head_dst, item);
	scb_taf->method = dest_method;
	for (s_idx = 0; s_idx < TAF_NUM_SCHED_SOURCES; s_idx++) {
		for (tid = 0; tid < TAF_NUMPRIO(s_idx); tid++) {
			taf_link_state_t  newstate;
			if ((source_group) != dest_group && (source_group != TAF_GROUP_UNDEFINED)) {
				newstate = prev_state[s_idx][tid];
			} else {
				newstate = TAF_LINKSTATE_SOFT_RESET;
			}
			taf_link_state_method_update(dest_method, scb_taf->scb, tid, s_idx,
				newstate);
		}
	}
	taf_scb_state_method_update(dest_method, scb_taf->scb, NULL, TAF_SCBSTATE_RESET);
}

static uint32 taf_total_traffic_pending(wlc_taf_info_t* taf_info)
{
	uint32 total_packets = 0;
	taf_scb_cubby_t *scb_taf = taf_info->head;

	while (scb_taf) {
		total_packets += wlc_taf_traffic_active(taf_info, scb_taf->scb);
		scb_taf = scb_taf->next;
	}
	return total_packets;
}

/*
 * Watchdog timer. Called approximatively once a second.
 */
static void
taf_watchdog(void *handle)
{
	wlc_taf_info_t *taf_info = handle;
	taf_scheduler_kind type;
	bool reset = FALSE;
	bool reset_hard = FALSE;
	uint32 total_traffic = 0;

	BCM_REFERENCE(reset_hard);
	BCM_REFERENCE(reset);

	if (taf_info == NULL || !taf_info->enabled || taf_info->bypass) {
		return;
	}

	if (taf_info->release_count == 0) {
		taf_info->watchdog_data_stall++;
		WL_TAFT(taf_info, "count (%u)\n", taf_info->watchdog_data_stall);
	} else {
		taf_info->watchdog_data_stall = 0;
	}

	if (taf_info->watchdog_data_stall_limit &&
			taf_info->watchdog_data_stall >= taf_info->watchdog_data_stall_limit) {
		reset = TRUE;
		reset_hard = (total_traffic = taf_total_traffic_pending(taf_info)) > 0;
		taf_info->watchdog_data_stall = 0;
	}

#if !TAF_ENABLE_SQS_PULL
	/* did we change TAF ordering ? */
	if (taf_info->ordering != taf_info->pending_ordering && taf_info->release_count == 0) {
		WL_TAFT(taf_info, "changing TAF ordering from %u to %u\n",
			taf_info->ordering, taf_info->pending_ordering);
		taf_info->ordering = taf_info->pending_ordering;
		reset = TRUE;
		reset_hard = TRUE;
	}
#endif /* !TAF_ENABLE_SQS_PULL */

	if (reset) {
		if (reset_hard) {
			WL_ERROR(("wl%u %s: require hard reset scheduling (total traffic %u)\n",
				WLCWLUNIT(TAF_WLCT(taf_info)), __FUNCTION__, total_traffic));
#ifdef TAF_DEBUG_VERBOSE
			taf_memtrace_dump();
#endif // endif
		}
#ifndef TAF_DBG
		wlc_taf_reset_scheduling(taf_info, ALLPRIO, reset_hard);
#endif // endif
	}
	/* process watchdog for scheduler methods */
	for (type = TAF_SCHED_FIRST_INDEX; type < TAF_SCHED_LAST_INDEX; type++) {
		taf_method_info_t* method = taf_get_method_info(taf_info, type);

		if (method->funcs.watchdog_fn) {
			method->funcs.watchdog_fn(method);
		}
	}
	taf_info->release_count = 0;
	return;
}

static taf_method_info_t* taf_get_method_by_name(wlc_taf_info_t* taf_info, const char* cmd)
{
	taf_scheduler_kind type;

	while (*cmd == ' ') {
		cmd++;
	}
	for (type = TAF_SCHED_FIRST_INDEX; type < TAF_SCHED_LAST_INDEX; type++) {
		taf_method_info_t* method = taf_get_method_info(taf_info, type);
		if (!strcmp(cmd, TAF_SCHED_NAME(method)) ||
			(cmd[0] >= '0' && cmd[0] <= '9' && bcm_strtoul(cmd, NULL, 0) == type)) {
			return method;
		}
	}
	return NULL;
}

bool BCMFASTPATH wlc_taf_enabled(wlc_taf_info_t* taf_info)
{
	return taf_enabled(taf_info);
}

bool BCMFASTPATH wlc_taf_in_use(wlc_taf_info_t* taf_info)
{
	return taf_in_use(taf_info);
}

bool BCMFASTPATH wlc_taf_nar_in_use(wlc_taf_info_t* taf_info, bool * enabled)
{
#if TAF_ENABLE_NAR
	bool taf_enabled = FALSE;
	bool taf_bypass = FALSE;

	if (taf_info) {
		taf_enabled = taf_info->enabled;
		taf_bypass = taf_info->bypass;
	}
	if (enabled) {
		*enabled = taf_enabled;
	}
	return taf_enabled && !taf_bypass;
#else
	if (enabled) {
		*enabled = FALSE;
	}
	return FALSE;
#endif /* TAF_ENABLE_NAR */
}

static INLINE uint32 BCMFASTPATH
taf_set_force(wlc_taf_info_t* taf_info, struct scb* scb, int tid)
{
	taf_scb_cubby_t* scb_taf = *SCB_TAF_CUBBY_PTR(taf_info, scb);

	if (taf_info->force_time > 0) {
		scb_taf->force |= (1 << tid);
		WL_TAFM(scb_taf->method, "setting force option to "MACF" tid %u\n",
			TAF_ETHERC(scb_taf), tid);
	}

	return scb_taf->force;
}

uint16
wlc_taf_traffic_active(wlc_taf_info_t* taf_info, struct scb* scb)
{
	uint32 traffic_active = 0;

	wlc_taf_scb_state_update(taf_info, scb, TAF_PARAM(&traffic_active),
		TAF_SCBSTATE_GET_TRAFFIC_ACTIVE);

	return (uint16)traffic_active;
}

static INLINE bool BCMFASTPATH
taf_schedule(taf_method_info_t* method, taf_release_context_t * context)
{
	wlc_taf_info_t* taf_info = method->taf_info;
	taf_scheduler_kind type = method->type;
	bool finished = FALSE;
	bool  (*scheduler_fn) (wlc_taf_info_t *, taf_release_context_t *, void *);

	context->type = type;

	scheduler_fn = method->funcs.scheduler_fn;

	if (!scheduler_fn) {
		return FALSE;
	}

	finished = scheduler_fn(taf_info, context, method);
	return finished;
}

static INLINE bool
taf_push_schedule(wlc_taf_info_t *taf_info, bool force, taf_release_context_t* context)
{
	bool did_schedule = FALSE;
#ifdef WLTAF_PUSH_SCHED_AVAILABLE
	struct scb * scb = context->scb;
	int tid = context->tid;
	taf_scb_cubby_t* scb_taf = scb ? *SCB_TAF_CUBBY_PTR(taf_info, scb) : NULL;
	taf_method_info_t* method;

	if (!scb_taf) {
		goto taf_push_schedule_end;
	}
	TAF_ASSERT(tid >= 0);
	method = scb_taf->method;
	TAF_ASSERT(method);

	if (force) {
		*method->ready_to_schedule |= (1 << tid);
		WL_TAFM(method, "force r_t_s 0x%x\n", *method->ready_to_schedule);
	}
	if (method->scheme == TAF_SINGLE_PUSH_SCHEDULING) {
		/* this is packet push scheduling */

		if (*method->ready_to_schedule & (1 << tid)) {
			if (taf_schedule(method, context)) {
				did_schedule = TRUE;
			}
		}
	}
#endif /* WLTAF_PUSH_SCHED_AVAILABLE */
	return did_schedule;
}

bool BCMFASTPATH
wlc_taf_schedule(wlc_taf_info_t *taf_info, int tid, struct scb *scb, bool force)
{
	bool finished = FALSE;
	taf_release_context_t context = {0};
	taf_method_info_t* method;
	taf_scheduler_kind type;
	bool did_schedule = FALSE;
#ifdef TAF_DBG
	char* trig_msg = NULL;
#endif /* TAF_DBG */

	if (taf_is_bypass(taf_info)) {
		return FALSE;
	}

#ifdef TAF_ENABLE_SQS_PULL
	/* for normal trigger cases, handle exceptions */
	if ((tid >= 0) && (tid < TAF_MAXPRIO)) {

		if ((scb == NULL) && (taf_info->op_state == TAF_MARKUP_REAL_PACKETS)) {
			WL_TAFT(taf_info, "skipping NULL scb tid %u in markup phase\n", tid);
			return TRUE;
		}

		if (scb && SCB_DWDS(scb)) {
			WL_TAFT(taf_info, MACF" tid %u is DWDS, bypass TAF\n",
				tid, TAF_ETHERS(scb));
			return FALSE;
		}
	}
#endif // endif
	TAF_ASSERT(taf_info->scheduler_nest == 0);

	if (taf_info->scheduler_nest != 0) {
		return FALSE;
	}
	taf_info->scheduler_nest ++;

	if (force && scb && tid >= 0 && tid < TAF_MAXPRIO) {
		/* set the force flag
		*/
		taf_set_force(taf_info, scb, tid);
	}

#if TAF_ENABLE_SQS_PULL
	if (tid == TAF_SQS_TRIGGER_TID && taf_info->op_state != TAF_SCHEDULE_VIRTUAL_PACKETS) {
#ifdef TAF_DBG
		uint32 pull_complete_elapsed = taf_info->virtual_complete_time ?
			taf_timestamp(TAF_WLCT(taf_info)) - taf_info->virtual_complete_time : 0;
		TAF_ASSERT(pull_complete_elapsed < 100000);
#endif /* TAF_DBG */
		goto taf_schedule_end;
	}
#else
	TAF_ASSERT(taf_info->op_state == TAF_SCHEDULE_REAL_PACKETS);
#endif /* TAF_ENABLE_SQS_PULL */

	context.tid = tid;
	context.scb = scb;

	if ((tid >= 0) && (did_schedule = taf_push_schedule(taf_info, force, &context))) {
		goto taf_schedule_end;
	} /* else PULL schedule ..... */

#if TAF_ENABLE_SQS_PULL
	switch (tid) {
		case TAF_SQS_V2R_COMPLETE_TID:
			context.op_state = TAF_MARKUP_REAL_PACKETS;
#ifdef TAF_DBG
			trig_msg = "SQS v2r complete";
#endif /* TAF_DBG */
			tid = 0;
			break;
		case TAF_SQS_TRIGGER_TID:
			context.op_state = TAF_SCHEDULE_VIRTUAL_PACKETS;
#ifdef TAF_DBG
			trig_msg = "\n\nSQS";
#endif /* TAF_DBG */
			tid = 0;
			break;

		case ALLPRIO:
			tid = 0;
			/* fall through */
		default:
			context.op_state = taf_info->op_state;
#ifdef TAF_DBG
			trig_msg =  "normal";
#endif /* TAF_DBG */
			break;
	}
#else
#ifdef TAF_DBG
	trig_msg =  "normal";
#endif /* TAF_DBG */
	context.op_state = TAF_SCHEDULE_REAL_PACKETS;
#endif /* TAF_ENABLE_SQS_PULL */

	/* everything from here is ordered scheduling (packet pull scheduling) */
	for (type = TAF_SCHED_FIRST_INDEX; type < TAF_SCHED_LAST_INDEX; type++) {
		method = taf_get_method_info(taf_info, type);

		if (method->scheme == TAF_SINGLE_PUSH_SCHEDULING) {
			continue;
		}
		if (*method->ready_to_schedule & (1 << tid)) {
#ifdef TAF_DBG
			if (trig_msg) {
				WL_TAFT(taf_info, "%s (tid %d, in transit %u)\n", trig_msg, tid,
					TXPKTPENDTOT(TAF_WLCT(taf_info)));
				trig_msg = NULL;
			}
#endif /* TAF_DBG */
			finished = taf_schedule(method, &context);
			did_schedule = TRUE;
		}
		if (finished) {
			break;
		}
	}

taf_schedule_end:
#if TAF_ENABLE_SQS_PULL
	if (context.virtual_release) {
		taf_info->op_state = TAF_MARKUP_REAL_PACKETS;
#ifdef TAF_DBG
		taf_info->virtual_complete_time = taf_timestamp(TAF_WLCT(taf_info));
#endif // endif
		wlc_sqs_eops_rqst();
		taf_info->eops_rqst++;
	}
#else
	taf_info->scheduler_index ++;
#endif /* TAF_ENABLE_SQS_PULL */

	/* check elapsed time since last scheduler for stall debugging */
	if (did_schedule) {
		taf_info->last_scheduler_run = taf_timestamp(TAF_WLCT(taf_info));
	}
	else {
		uint32 now_time = taf_timestamp(TAF_WLCT(taf_info));
		uint32 scheduler_elapsed = taf_info->last_scheduler_run ?
			now_time - taf_info->last_scheduler_run : 0;

		/* STALL recovery */
		if (scheduler_elapsed > 250000) {
			if (taf_info->watchdog_data_stall < taf_info->watchdog_data_stall_limit) {
				WL_ERROR(("wl%u %s: reset scheduling (elapsed %u ms) STALL\n",
					WLCWLUNIT(TAF_WLCT(taf_info)), __FUNCTION__,
					scheduler_elapsed / 1000));
#ifdef TAF_DEBUG_VERBOSE
				taf_memtrace_dump();
#endif // endif
				/* advance WD data stall process */
				taf_info->watchdog_data_stall =
					taf_info->watchdog_data_stall_limit;

				taf_info->last_scheduler_run = now_time;
			}
		}
	}
	if (context.legacy_release) {
		TAF_ASSERT(tid >= 0 && tid < NUMPRIO);
		WL_TAFT(taf_info, MACF" tid %u revert to fallback default data path\n",
			TAF_ETHERS(scb), tid);
	}

	taf_info->scheduler_nest --;
	return (!context.legacy_release);
}
/*
 * SCB cubby functions, called when an SCB is created or destroyed.
 */
static int
taf_scbcubby_init(void *handle, struct scb *scb)
{
	wlc_taf_info_t *taf_info = handle;
	taf_scb_cubby_t *scb_taf = NULL;
	taf_method_info_t *method;

	/* Init function always called after SCB reset */
	TAF_ASSERT(*SCB_TAF_CUBBY_PTR(taf_info, scb) == NULL);

	if (taf_info->enabled && !SCB_INTERNAL(scb)) {
#ifdef TAF_KEEP_FREED_CUBBY
		taf_scb_cubby_t *prev;
		/* Remember the oldest cubby as we may reuse it */
		taf_scb_cubby_t *oldest = NULL, *prev_oldest = NULL;
		uint32 ts_oldest = -1U;

		/* Search pool for same MAC. This casting allows to modify the list seamlessly */
		prev = (taf_scb_cubby_t *) &taf_info->pool;

		for (scb_taf = taf_info->pool; scb_taf; prev = scb_taf, scb_taf = scb_taf->next) {
			if (memcmp(&scb_taf->ea, &scb->ea, ETHER_ADDR_LEN) == 0) {
				/* Found! remove it from the pool list */
				prev->next = scb_taf->next;
				WL_TAFT(taf_info, "TAF cby "MACF" found in pool\n",
					TAF_ETHERS(scb));
				break;
			}

			/* Oldest one? then save it */
			if (scb_taf->timestamp < ts_oldest) {
				ts_oldest = scb_taf->timestamp;
				prev_oldest = prev;
				oldest = scb_taf;
			}
		}
#endif /* TAF_KEEP_FREED_CUBBY */
		if (scb_taf == NULL) {
			if (taf_info->scb_cnt <= TAF_SCB_CNT_MAX) {
				wlc_info_t *wlc = TAF_WLCT(taf_info);

				/* Not found, initialize a new one */
#ifdef BCM_HOST_MEM_SCB
				if (SCB_ALLOC_ENAB(wlc->pub) && SCB_HOST(scb)) {
					scb_taf = (taf_scb_cubby_t *)wlc_scb_alloc_mem_get(wlc,
						SCB_CUBBY_ID_TAF, sizeof(taf_scb_cubby_t), 1);
				}

				if (!scb_taf)
#endif /* BCM_HOST_MEM_SCB */
				{
					scb_taf = MALLOC(wlc->osh, sizeof(*scb_taf));
				}
				if (scb_taf == NULL) {
					return BCME_NOMEM;
				}
				taf_info->scb_cnt++;
				WL_TAFT(taf_info, "TAF cby "MACF" allocated\n",
					TAF_ETHERS(scb));
			} else {
#ifdef TAF_KEEP_FREED_CUBBY
				TAF_ASSERT(oldest && prev_oldest);
				/* Not found and max count reached, reuse the oldest one */
				/* check if there is space available? */
				if (!oldest || !prev_oldest) {
					return BCME_NOMEM;
				}

				prev_oldest->next = oldest->next;
				scb_taf = oldest;
				WL_TAFT(taf_info, "TAF cby "MACF" reused\n",
					TAF_ETHERS(scb));
#endif /* TAF_KEEP_FREED_CUBBY */
			}
			memset(scb_taf, 0, sizeof(*scb_taf));
		}
		scb_taf->scb = scb;
		scb_taf->ea = scb->ea;
		scb_taf->method = NULL;

		method = taf_get_method_info(taf_info, taf_info->default_scheduler);

		TAF_ASSERT(method);

		/* Just put the cubby at the begining of the list as bsscfg is unknown */
		scb_taf->next = taf_info->head;
		taf_info->head = scb_taf;

		*SCB_TAF_CUBBY_PTR(taf_info, scb) = scb_taf;

		taf_set_cubby_method(scb_taf, method, taf_info->default_score);

		wlc_taf_scb_state_update(taf_info, scb, NULL, TAF_SCBSTATE_INIT);
	}

	return BCME_OK;
}

/*
 * The SCB cubby is saved to be reused later if the station re-associate, or for another STA.
 */
static void
taf_scbcubby_exit(void *handle, struct scb *scb)
{
	wlc_taf_info_t *taf_info = handle;
	taf_scb_cubby_t* scb_taf = *SCB_TAF_CUBBY_PTR(taf_info, scb);

	/* If we do have a cubby, clean up. */
	if (scb_taf) {
		taf_scb_cubby_t *prev, *curr;
		taf_source_type_t s_idx;

		WL_TAFT(taf_info, MACF" cubby exit\n", TAF_ETHERC(scb_taf));

		for (s_idx = 0; s_idx < TAF_NUM_SCHED_SOURCES; s_idx++) {
			int tid;
			for (tid = 0; tid < TAF_NUMPRIO(s_idx); tid++) {
				taf_link_state_method_update(scb_taf->method, scb, tid, s_idx,
					TAF_LINKSTATE_REMOVE);
			}
		}
		wlc_taf_scb_state_update(taf_info, scb, NULL, TAF_SCBSTATE_EXIT);

		taf_list_delete(taf_info, scb_taf);

		/* Trick to modify head seamlessly */
		prev = (taf_scb_cubby_t *) &taf_info->head;

		for (curr = taf_info->head; curr; prev = curr, curr = curr->next) {
			if (scb_taf == curr) {
				/* Found! remove it from the head list */
				prev->next = curr->next;
				break;
			}
		}
		TAF_ASSERT(curr);

#ifdef TAF_KEEP_FREED_CUBBY
		/* Save the cubby in the pool list */
		scb_taf->next = taf_info->pool;
		taf_info->pool = scb_taf;

		/* Clear the backlink which will become invalid */
		scb_taf->scb = NULL;

		/* Save time of last scb use for potential discard */
		scb_taf->timestamp = TAF_WLCT(taf_info)->pub->now;

		*SCB_TAF_CUBBY_PTR(taf_info, scb) = NULL;

		WL_TAFT(taf_info, "TAF cby "MACF" moved to pool list\n",
			TAF_ETHERS(scb));
#else /* TAF_KEEP_FREED_CUBBY */

		TAF_ASSERT(taf_info->scb_cnt);
		MFREE(TAF_WLCT(taf_info)->pub->osh, scb_taf, sizeof(*scb_taf));
		*SCB_TAF_CUBBY_PTR(taf_info, scb) = NULL;
		taf_info->scb_cnt--;
#endif /* TAF_KEEP_FREED_CUBBY */
	}
}

/* do not call multiple times for every tid - rather use tid=ALLPRIO to reset all tid */
bool BCMFASTPATH wlc_taf_reset_scheduling(wlc_taf_info_t *taf_info, int tid, bool hardreset)
{
	taf_scb_cubby_t *scb_taf = taf_info->head;

	if (!taf_info || !taf_info->enabled) {
		return FALSE;
	}

	WL_TAFT(taf_info, "tid %d %s\n", tid, hardreset ? "hard" : "soft");

	while (scb_taf) {
		taf_source_type_t s_idx;
		int ltid = (tid == ALLPRIO) ? 0 : tid;

		do {
			for (s_idx = 0; s_idx < TAF_NUM_SCHED_SOURCES; s_idx++) {
				taf_link_state_method_update(scb_taf->method, scb_taf->scb, ltid,
					s_idx, hardreset ?
					TAF_LINKSTATE_HARD_RESET : TAF_LINKSTATE_SOFT_RESET);
			}
		} while ((tid == ALLPRIO) && (++ltid < TAF_MAXPRIO));

		if (hardreset) {
			taf_scb_state_method_update(scb_taf->method, scb_taf->scb, NULL,
				TAF_SCBSTATE_RESET);
		}
		scb_taf = scb_taf->next;
	}
	if (hardreset) {
		int ltid = (tid == ALLPRIO) ? 0 : tid;

		WL_ERROR(("wl%u %s: hard reset\n", WLCWLUNIT(taf_info->wlc), __FUNCTION__));

		wlc_taf_sched_state(taf_info, NULL, tid, 0, NULL, TAF_SCHED_STATE_RESET);
		taf_info->watchdog_data_stall = 0;

		if (taf_info->scheduler_nest == 0) {
			do {
				wlc_taf_schedule(taf_info, TAF_TID(ltid), NULL, FALSE);
			} while (TAF_PARALLEL_ORDER(taf_info) && (tid == ALLPRIO) &&
				++ltid < TAF_MAXPRIO);
		}
	}
	return TRUE;
}

#ifdef TAF_DBG
static int
taf_dump(void *handle, struct bcmstrbuf *b)
{
	wlc_taf_info_t	*taf_info = handle;
	taf_scheduler_kind type = TAF_SCHEDULER_START;

	bcm_bprintf(b, "taf is %sabled%s\n", taf_info->enabled ? "en" : "dis",
		taf_info->bypass ? " BUT BYPASSED (not in use)" : "");

	if (!taf_info->enabled) {
		return BCME_OK;
	}

	while (type < TAF_SCHED_LAST_INDEX) {
		taf_method_info_t* method = taf_get_method_info(taf_info, type);
		bcm_bprintf(b, "\nMethod '%s' is available (scheduler index %u)",
			TAF_SCHED_NAME(method), type);

		if (taf_info->default_scheduler == type) {
			bcm_bprintf(b, " which is default for new stations");
		}
		bcm_bprintf(b, "; for detailed info, please do 'dump %s'\n", TAF_DUMP_NAME(method));
		taf_dump_list(method, b);
		type++;
	}
#ifdef TAF_DEBUG_VERBOSE
	if (taf_trace_buf)
	{
		int index = taf_trace_index;
		char end;

		bcm_bprintf(b, "Log (%u)\n", index);

		taf_trace_buf[index++] = 0;

		/* handle limited length ioctl buffer for dump */
		if (taf_trace_buf_len > 7200) {
			index += (taf_trace_buf_len - 7200);
		}
		if (index >= taf_trace_buf_len) {
			index -= taf_trace_buf_len;
		}
		if (taf_trace_buf[index] == 0) {
			index = 0;
		}
		end = taf_trace_buf[taf_trace_buf_len - 1];

		if (end) {
			taf_trace_buf[taf_trace_buf_len - 1] = 0;
			bcm_bprintf(b, "%s%c", taf_trace_buf + index, end);
			taf_trace_buf[taf_trace_buf_len - 1] = end;
		}
		bcm_bprintf(b, "%s\n", taf_trace_buf);
	}
#endif /* TAF_DEBUG_VERBOSE */
	return BCME_OK;
}
#endif /* TAF_DBG */

/*
 * taf_up() - interface coming up.
 */
static int
taf_up(void *handle)
{
	/* wlc_taf_info_t *taf_info = handle; */

	return BCME_OK;
}

/*
 * taf_down() - interface going down.
 */
static int
taf_down(void *handle)
{
	/* wlc_taf_info_t *taf_info = handle; */

	return BCME_OK;
}

/*
 * wlc_taf_attach() - attach function, called from wlc_attach_module().
 *
 * Allocate and initialise our context structure, register the module, register the txmod.
 *
 */
wlc_taf_info_t *
BCMATTACHFN(wlc_taf_attach)(wlc_info_t * wlc)
{
	wlc_taf_info_t *taf_info;
	int status;
	taf_scheduler_kind type;

	TAF_ASSERT(NUM_TAF_SCHEDULER_METHOD_GROUPS != 0);
	TAF_ASSERT(NUM_TAF_SCHEDULERS != 0);

	WL_TAFW(wlc, "%u\n", __LINE__);

	/* Allocate and initialise our main structure. */
	taf_info = MALLOCZ(wlc->pub->osh, sizeof(*taf_info));
	if (!taf_info) {
		return NULL;
	}

	/* Save backlink to wlc */
	TAF_WLCT(taf_info) = wlc;

	status = wlc_module_register(wlc->pub, taf_iovars, "taf", taf_info,
	taf_doiovar, taf_watchdog,  taf_up, taf_down);

	if (status != BCME_OK) {
		MFREE(TAF_WLCT(taf_info)->pub->osh, taf_info, sizeof(*taf_info));
		return NULL;
	}

	taf_info->enabled = FALSE;

	taf_info->ordering = TAF_ORDER_TID_SCB;
#if TAF_ENABLE_SQS_PULL
	TAF_ASSERT(TAF_UNITED_ORDER(taf_info));
	taf_info->op_state = TAF_SCHEDULE_VIRTUAL_PACKETS;
#else
	taf_info->op_state = TAF_SCHEDULE_REAL_PACKETS;
	taf_info->pending_ordering = taf_info->ordering;
#endif /* TAF_ENABLE_SQS_PULL */

	taf_info->watchdog_data_stall_limit = TAF_WD_STALL_RESET_LIMIT;

#ifdef TAF_DBG
	wlc_dump_register(wlc->pub, "taf", taf_dump, taf_info);
#endif /* TAF_DBG */

	if (D11REV_LT(wlc->pub->corerev, 40)) {
		/* only support on latest chipsets, proceed no more */
		return taf_info;
	}

	/*
	* set up an scb cubby - this returns an offset, or -1 on failure.
	*/
	if ((taf_info->scb_handle = wlc_scb_cubby_reserve(wlc, sizeof(taf_scb_cubby_t *),
		taf_scbcubby_init, taf_scbcubby_exit, NULL, taf_info)) < 0)
	{
		goto exitfail;
	}

	taf_info->default_scheduler = TAF_DEFAULT_SCHEDULER;
	taf_info->default_score = TAF_SCORE_MIN;

	/* the default amount of traffic (in microsecs) to send when forced,
	 * this helps to prevent a link from stalling completely
	 */
	taf_info->force_time = TAF_TIME_FORCE_DEFAULT;

#if TAF_ENABLE_SQS_PULL
	/* this is enabled later so default FALSE */
	wl_bus_taf_scheduler_config(wlc->wl, FALSE);
#endif /* TAF_ENABLE_SQS_PULL */

#if TAF_ENABLE_AMPDU
	taf_info->source_handle_p[TAF_SOURCE_AMPDU] = (void*)&wlc->ampdu_tx;
	taf_info->funcs[TAF_SOURCE_AMPDU].scb_h_fn = wlc_ampdu_get_taf_scb_info;
	taf_info->funcs[TAF_SOURCE_AMPDU].tid_h_fn = wlc_ampdu_get_taf_scb_tid_info;
	taf_info->funcs[TAF_SOURCE_AMPDU].pktqlen_fn = wlc_ampdu_get_taf_scb_tid_pktlen;
	taf_info->funcs[TAF_SOURCE_AMPDU].release_fn = wlc_ampdu_taf_release;
#endif /* TAF_ENABLE_AMPDU */
#if TAF_ENABLE_NAR
	taf_info->source_handle_p[TAF_SOURCE_NAR] = (void*)&wlc->nar_handle;
	taf_info->funcs[TAF_SOURCE_NAR].scb_h_fn = wlc_nar_get_taf_scb_info;
	taf_info->funcs[TAF_SOURCE_NAR].tid_h_fn = wlc_nar_get_taf_scb_prec_info;
	taf_info->funcs[TAF_SOURCE_NAR].pktqlen_fn = wlc_nar_get_taf_scb_prec_pktlen;
	taf_info->funcs[TAF_SOURCE_NAR].release_fn = wlc_nar_taf_release;
#endif /* TAF_ENABLE_NAR */
#if TAF_ENABLE_SQS_PULL
	taf_info->source_handle_p[TAF_SOURCE_HOST_SQS] = wlc_sqs_taf_get_handle(wlc);
	taf_info->funcs[TAF_SOURCE_HOST_SQS].scb_h_fn = wlc_sqs_taf_get_scb_info;
	taf_info->funcs[TAF_SOURCE_HOST_SQS].tid_h_fn = wlc_sqs_taf_get_scb_tid_info;
	taf_info->funcs[TAF_SOURCE_HOST_SQS].pktqlen_fn = wlc_sqs_taf_get_scb_tid_pkts;
	taf_info->funcs[TAF_SOURCE_HOST_SQS].release_fn = wlc_sqs_taf_release;
#endif /* TAF_ENABLE_SQS_PULL */

#if defined(TAF_MU_TX)
	taf_info->mu_extend_coeff = TAF_MU_EXTEND_COEFF_DEFAULT;
#endif // endif

	for (type = TAF_SCHED_FIRST_INDEX; type < TAF_SCHED_LAST_INDEX; type++) {
		taf_method_info_t* method = NULL;
		int index = (int)type - (int)TAF_SCHED_FIRST_INDEX;

		if (taf_scheduler_definitions[index].attach_fn) {
			method = taf_scheduler_definitions[index].attach_fn(taf_info, type);
		}

		if ((taf_info->scheduler_context[index] = method) == NULL) {
			WL_ERROR(("wl%u %s: failed to attach (%d)\n", WLCWLUNIT(wlc), __FUNCTION__,
				type));
			goto exitfail;
		}
	}

#if defined(TAF_DEBUG_VERBOSE) && defined(DONGLEBUILD)
	taf_attach_complete = TRUE;
#endif // endif
	/* All is fine, return handle */
	return taf_info;
exitfail:
	wlc_taf_detach(taf_info);
	return NULL;
}

/*
 * wlc_taf_detach() - wlc module detach function, called from wlc_detach_module().
 *
 */
int
BCMATTACHFN(wlc_taf_detach) (wlc_taf_info_t *taf_info)
{
#if defined(TAF_DEBUG_VERBOSE) && defined(DONGLEBUILD)
	taf_attach_complete = FALSE;
#endif // endif

	if (taf_info) {
		taf_scheduler_kind type;
#ifdef TAF_KEEP_FREED_CUBBY
		/* Free the pool list */
		taf_scb_cubby_t *next;
		taf_scb_cubby_t *scb_taf = taf_info->pool;

		while (scb_taf) {
			next = scb_taf->next;
			TAF_ASSERT(taf_info->scb_cnt);
			MFREE(TAF_WLCT(taf_info)->osh, scb_taf, sizeof(*scb_taf));
			taf_info->scb_cnt--;
			scb_taf = next;
		}
#endif /* TAF_KEEP_FREED_CUBBY */

		TAF_ASSERT(taf_info->scb_cnt == 0);

		for (type = TAF_SCHED_FIRST_INDEX; type < TAF_SCHED_LAST_INDEX; type++) {
			int index = (int)type - (int)TAF_SCHED_FIRST_INDEX;

			if (taf_scheduler_definitions[index].detach_fn) {
				void* sch_context = taf_get_method_info(taf_info, type);

				if (sch_context)
					taf_scheduler_definitions[index].detach_fn(sch_context);
			}
			taf_info->scheduler_context[index] = NULL;
		}
		wlc_module_unregister(TAF_WLCT(taf_info)->pub, "taf", taf_info);
		MFREE(TAF_WLCT(taf_info)->pub->osh, taf_info, sizeof(*taf_info));
	}
	return BCME_OK;
}

static INLINE bool
taf_link_state_method_update(taf_method_info_t* method, struct scb* scb, int tid,
	taf_source_type_t s_idx, taf_link_state_t state)
{
	bool finished = FALSE;

	TAF_ASSERT(method->funcs.linkstate_fn);
	finished = method->funcs.linkstate_fn(method, scb, tid, s_idx, state);

	return finished;
}

void wlc_taf_link_state(wlc_taf_info_t* taf_info, struct scb* scb, int tid, const void* data,
	taf_link_state_t state)
{
	taf_scb_cubby_t* scb_taf;

	if (!taf_info || !taf_info->enabled) {
		return;
	}
	scb_taf = *SCB_TAF_CUBBY_PTR(taf_info, scb);

	if (scb_taf) {
		TAF_ASSERT(tid >= 0 && tid < TAF_MAXPRIO);

		if (state != TAF_LINKSTATE_AMSDU_AGG) {
			taf_source_type_t s_idx = wlc_taf_get_source_index(data);

			if (s_idx != TAF_SOURCE_UNDEFINED) {
				TAF_ASSERT(tid < TAF_MAXPRIO);
				taf_link_state_method_update(scb_taf->method, scb, tid, s_idx,
					state);
			} else {
				WL_TAFM(scb_taf->method, MACF" tid %u - %s - TO DO\n",
					TAF_ETHERS(scb), tid, (const char*)data);
			}
		} else {
			WL_TAFM(scb_taf->method, MACF" TAF_LINKSTATE_AMSDU_AGG *TO DO* tid %u\n",
				TAF_ETHERS(scb), tid);
		}
	} else {
		WL_TAFT(taf_info, MACF" tid %u, has no TAF cubby (%s/%s)\n", TAF_ETHERS(scb), tid,
		       taf_link_states_text[state], (const char*) data);
	}
}

static INLINE int
taf_scb_state_method_update(taf_method_info_t* method, struct scb* scb, void* update,
	taf_scb_state_t state)
{
	int ret = BCME_ERROR;

	if (method->funcs.scbstate_fn) {
		ret = method->funcs.scbstate_fn(method, scb, update, state);
	} else {
		WL_TAFM(method, "TO DO\n");
	}
	return ret;
}

static INLINE int taf_scb_power_save(wlc_taf_info_t* taf_info, taf_scb_cubby_t* scb_taf, bool on)
{
	int result = BCME_OK;
	taf_method_info_t* owner = scb_taf->method;

	scb_taf->info.ps_mode = on;

	/* inform owner method SCB about power save */
	result = taf_scb_state_method_update(owner, scb_taf->scb, TAF_PARAM(on),
		TAF_SCBSTATE_POWER_SAVE);

	if (result != BCME_OK) {
		return result;
	}

	if (on) {
		/* nothing to do */
	}

#if TAF_ENABLE_SQS_PULL
	if (!on && taf_info->op_state == TAF_SCHEDULE_VIRTUAL_PACKETS)
#else
	if (!on)
#endif // endif
	{
		/* this is PS off - need to kick the scheduler */
		int tid = 0;

		do {
			WL_TAFT(taf_info, MACF" PS mode OFF - kick tid %d\n",
				TAF_ETHERC(scb_taf), TAF_TID(tid));

			wlc_taf_schedule(taf_info, TAF_TID(tid), scb_taf->scb, FALSE);
		} while (TAF_PARALLEL_ORDER(taf_info) && ++tid < NUMPRIO);
	}
	return result;

}
int wlc_taf_scb_state_update(wlc_taf_info_t* taf_info, struct scb* scb, void* update,
	taf_scb_state_t state)
{
	taf_scb_cubby_t* scb_taf;

	if (!taf_info || !taf_info->enabled) {
		return BCME_NOTREADY;
	}
	scb_taf = *SCB_TAF_CUBBY_PTR(taf_info, scb);

	if (scb_taf) {
		int result;
		/* handle common high level scb state change */
		switch (state) {
			case TAF_SCBSTATE_POWER_SAVE:
				result = taf_scb_power_save(taf_info, scb_taf,
					update ? TRUE : FALSE);
				break;
			default:
				/* then pass to the method.... */
				result = taf_scb_state_method_update(scb_taf->method, scb, update,
					state);
				break;
		}
		return result;
	}
	return BCME_NOTFOUND;
}

static INLINE void
taf_method_rate_override(wlc_taf_info_t* taf_info, taf_scheduler_kind type,
	ratespec_t rspec, wlcband_t *band)
{
	taf_method_info_t* method = taf_get_method_info(taf_info, type);

	if (method->funcs.rateoverride_fn) {
		method->funcs.rateoverride_fn(method, rspec, band);
	} else {
		WL_TAFM(method, "TO DO\n");
	}
}

void wlc_taf_rate_override(wlc_taf_info_t* taf_info, ratespec_t rspec, wlcband_t *band)
{
	taf_scheduler_kind type;

	if (!taf_info || !taf_info->enabled) {
		return;
	}
	for (type = TAF_SCHEDULER_START; type < TAF_SCHED_LAST_INDEX; type++) {
		taf_method_rate_override(taf_info, type, rspec, band);
	}
}

bool wlc_taf_txpkt_status(wlc_taf_info_t* taf_info, struct scb* scb, int tid, void* p,
	taf_txpkt_state_t status)
{
	taf_scb_cubby_t* scb_taf;
	bool ret = FALSE;
	taf_method_info_t* method;

	if (!taf_info || !taf_info->enabled || taf_info->bypass) {
		return FALSE;
	}
	TAF_ASSERT(tid < TAF_MAXPRIO);

	if ((scb = scb ? scb : WLPKTTAGSCBGET(p)) == NULL) {
		WL_TAFT(taf_info, "scb is null\n");
		return FALSE;
	}
	if ((scb_taf = *SCB_TAF_CUBBY_PTR(taf_info, scb)) == NULL) {
		WL_TAFT(taf_info, MACF" has no TAF cubby\n", TAF_ETHERS(scb));
		return FALSE;
	}
	method = scb_taf->method;

	ret = method->funcs.txstat_fn(method, scb_taf, tid, p, status);

	if (!ret && status == TAF_TXPKT_STATUS_PKTFREE_RESET) {
		ret = wlc_taf_reset_scheduling(taf_info, tid, FALSE);
	}
	if (!ret) {
		WL_TAFM(method, MACF" unhandled, %s\n", TAF_ETHERC(scb_taf),
			taf_txpkt_status_text[status]);
	}
	return ret;
}

static INLINE void
taf_method_bss_state_update(wlc_taf_info_t* taf_info, taf_scheduler_kind type, wlc_bsscfg_t *bsscfg,
	void* update, taf_bss_state_t state)
{
	taf_method_info_t* method = taf_get_method_info(taf_info, type);

	if (method->funcs.bssstate_fn) {
		method->funcs.bssstate_fn(method, bsscfg, update, state);
	} else {
		WL_TAFM(method, "TO DO\n");
	}
}

void wlc_taf_bss_state_update(wlc_taf_info_t* taf_info, wlc_bsscfg_t *bsscfg, void* update,
	taf_bss_state_t state)
{
	taf_scheduler_kind type;

	if (!taf_info || !taf_info->enabled) {
		return;
	}

	for (type = TAF_SCHEDULER_START; type < TAF_SCHED_LAST_INDEX; type++) {
		taf_method_bss_state_update(taf_info, type, bsscfg, update, state);
	}
}

#if TAF_ENABLE_SQS_PULL
static INLINE void taf_sqs_link_init(wlc_taf_info_t* taf_info, taf_scb_cubby_t* scb_taf, int tid)
{
	struct scb * scb = scb_taf->scb;

	if (!SCB_AMPDU(scb)) {
		return;
	}
	if (scb_taf->info.linkstate[TAF_SOURCE_HOST_SQS][tid] == TAF_LINKSTATE_NONE) {
		/* XXX this is a workaround in case AMPDU ini (which is required by SQS)
		* does not exist, it has to be created on demand
		*/
		void * handle = wlc_ampdu_taf_sqs_link_init(TAF_WLCT(taf_info), scb, tid);

		WL_TAFT(taf_info, MACF" tid %u %s\n", TAF_ETHERS(scb), tid,
			handle ? " AMPDU":"");

		if (handle) {
			taf_link_state_method_update(scb_taf->method, scb, tid,
				TAF_SOURCE_HOST_SQS, TAF_LINKSTATE_ACTIVE);
		} else {
			WL_TAFT(taf_info, "no SQS AMPDU context\n");
		}
	}
}
#endif /* TAF_ENABLE_SQS_PULL */

void
wlc_taf_pkts_enqueue(wlc_taf_info_t* taf_info, struct scb* scb, int tid, const void* data, int pkts)
{
#if TAF_ENABLE_SQS_PULL
	taf_scb_cubby_t* scb_taf;

	if (taf_info->enabled && (scb_taf = *SCB_TAF_CUBBY_PTR(taf_info, scb))) {
		TAF_ASSERT(tid >= 0 && tid < TAF_MAXPRIO);

		taf_sqs_link_init(taf_info, scb_taf, tid);

		scb_taf->info.traffic_map[TAF_SOURCE_HOST_SQS] |= (1 << tid);
	}
	return;
#endif /* TAF_ENABLE_SQS_PULL */
}

static INLINE void
taf_sched_state_method_update(taf_method_info_t* method, taf_scb_cubby_t* scb_taf, int tid,
	int count, taf_source_type_t s_idx, taf_link_state_t state)
{
	TAF_ASSERT(method->funcs.schedstate_fn);
	method->funcs.schedstate_fn(method, scb_taf, tid, count, s_idx, state);
}

void wlc_taf_sched_state(wlc_taf_info_t* taf_info, struct scb* scb, int tid, int count,
	const void* data, taf_sched_state_t state)
{
	taf_scb_cubby_t* scb_taf;
	taf_source_type_t s_idx;

	if (!taf_info || !taf_info->enabled) {
		return;
	}
	scb_taf = scb ? *SCB_TAF_CUBBY_PTR(taf_info, scb) : NULL;
	s_idx = wlc_taf_get_source_index(data);

	if (scb_taf) {
		taf_sched_state_method_update(scb_taf->method, scb_taf, tid, count, s_idx, state);
	} else {
		taf_scheduler_kind type;

		for (type = TAF_SCHED_FIRST_INDEX; type < TAF_SCHED_LAST_INDEX; type++) {
			taf_method_info_t* method = taf_get_method_info(taf_info, type);

			taf_sched_state_method_update(method, NULL, tid, count, s_idx, state);
		}
	}

	if (state == TAF_SCHED_STATE_RESET) {
		taf_info->release_count = 0;
		taf_info->last_scheduler_run = 0;
#if TAF_ENABLE_SQS_PULL
		taf_info->op_state = TAF_SCHEDULE_VIRTUAL_PACKETS;
		taf_info->total_pull_requests = 0;
		taf_info->eops_rqst = 0;
#ifdef TAF_DBG
		taf_info->virtual_complete_time = 0;
#endif /* TAF_DBG */
#endif /* TAF_ENABLE_SQS_PULL */
	}

	/* Trigger a TAF schedule if DATA fifo opens up */
	if ((state == TAF_SCHED_STATE_DATA_BLOCK_FIFO) && (count == 0) &&
#if TAF_ENABLE_SQS_PULL
		(taf_info->op_state == TAF_SCHEDULE_VIRTUAL_PACKETS) &&
#endif /* TAF_ENABLE_SQS_PULL */
		TRUE)
	{
		tid = 0;
		do {
			wlc_taf_schedule(taf_info, TAF_TID(tid), NULL, FALSE);
		} while (TAF_PARALLEL_ORDER(taf_info) && ++tid < NUMPRIO);
	}
}

#if TAF_ENABLE_SQS_PULL
bool wlc_taf_marked_up(wlc_taf_info_t *taf_info)
{
	if (taf_info->eops_rqst == 0) {
		taf_info->op_state = TAF_SCHEDULE_VIRTUAL_PACKETS;
		WL_TAFT(taf_info, "TAF_SCHEDULE_VIRTUAL_PACKETS (%u outstanding pull requests%s)\n",
			taf_info->total_pull_requests,
			taf_info->total_pull_requests > 0 ? " will be set to 0" : "");
		taf_info->total_pull_requests = 0;
		taf_info->scheduler_index ++;
		return TRUE;
	}
	return FALSE;
}
#endif /* TAF_ENABLE_SQS_PULL */

void wlc_taf_pkts_dequeue(wlc_taf_info_t* taf_info, struct scb* scb, int tid, int pkts)
{
#if TAF_ENABLE_SQS_PULL
	taf_scb_cubby_t* scb_taf;
#ifdef TAF_DBG
	uint16 prev_pull;
#endif // endif
	if (!scb) {
		WL_TAFT(taf_info, "scb null\n");
		return;
	}

	scb_taf = *SCB_TAF_CUBBY_PTR(taf_info, scb);

	if (scb_taf == NULL) {
		WL_TAFT(taf_info, MACF" has no TAF cubby\n", TAF_ETHERS(scb));
		return;
	}

	if (taf_info->op_state != TAF_MARKUP_REAL_PACKETS) {
		WL_TAFT(taf_info, "!!! taf_info->op_state != TAF_MARKUP_REAL_PACKETS !!! "
			MACF" tid %u, pkts %u, pull %u, ps %u\n", TAF_ETHERS(scb), tid, pkts,
			scb_taf->info.pkt_pull_request[tid], scb_taf->info.ps_mode);
		return;
	}

	scb_taf->info.pkt_pull_dequeue += pkts;

#ifdef TAF_DBG
	prev_pull = scb_taf->info.pkt_pull_request[tid];
#endif // endif
	if (scb_taf->info.pkt_pull_request[tid] >= pkts) {
		scb_taf->info.pkt_pull_request[tid] -= pkts;
	} else {
		scb_taf->info.pkt_pull_request[tid] = 0;

		WL_TAFT(taf_info, MACF" tid %u, pull MISCOUNT: prev = %u, "
			"dequeue = %u\n", TAF_ETHERS(scb), tid, prev_pull, pkts);

	}
	WL_TAFT(taf_info, MACF" tid %u has %u rpkts (%u req, %u wait)%s\n",
		TAF_ETHERS(scb), tid, pkts, prev_pull, scb_taf->info.pkt_pull_request[tid],
		scb_taf->info.ps_mode ? " in PS mode" : "");
#endif /* TAF_ENABLE_SQS_PULL */
}

void wlc_taf_v2r_complete(wlc_taf_info_t* taf_info)
{
#if TAF_ENABLE_SQS_PULL
	if (taf_info->op_state != TAF_MARKUP_REAL_PACKETS) {
		WL_TAFT(taf_info, "!!! taf_info->op_state != TAF_MARKUP_REAL_PACKETS !!!\n");
	}

	if (taf_info->eops_rqst > 0) {
		taf_info->eops_rqst--;
	} else {
		WL_ERROR(("wl%u %s: eops_rqst 0\n", WLCWLUNIT(taf_info->wlc), __FUNCTION__));
	}

	if (taf_info->eops_rqst == 0 && taf_info->op_state == TAF_MARKUP_REAL_PACKETS) {
		wlc_taf_schedule(taf_info, TAF_SQS_V2R_COMPLETE_TID, NULL, FALSE);
	}
#endif /* TAF_ENABLE_SQS_PULL */
}