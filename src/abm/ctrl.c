// SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause)
/* Copyright (C) 2018 Netronome Systems, Inc. */

#include <linux/kernel.h>
#include <linux/log2.h>

#include "../nfpcore/nfp_cpp.h"
#include "../nfpcore/nfp_nffw.h"
#include "../nfp_app.h"
#include "../nfp_abi.h"
#include "../nfp_main.h"
#include "../nfp_net.h"
#include "main.h"

#define NFP_NUM_PRIOS_SYM_NAME	"_abi_pci_dscp_num_prio_%u"
#define NFP_NUM_BANDS_SYM_NAME	"_abi_pci_dscp_num_band_%u"

#define NFP_QLVL_SYM_NAME	"_abi_nfd_out_q_lvls_%u%s"
#define NFP_QLVL_STRIDE		16
#define NFP_QLVL_BLOG_BYTES	0
#define NFP_QLVL_BLOG_PKTS	4
#define NFP_QLVL_THRS		8

#define NFP_QMSTAT_SYM_NAME	"_abi_nfdqm%u_stats%s"
#define NFP_QMSTAT_STRIDE	32
#define NFP_QMSTAT_NON_STO	0
#define NFP_QMSTAT_STO		8
#define NFP_QMSTAT_DROP		16
#define NFP_QMSTAT_ECN		24

static int
nfp_abm_ctrl_stat(struct nfp_abm_link *alink, const struct nfp_rtsym *sym,
		  unsigned int stride, unsigned int offset, unsigned int band,
		  unsigned int queue, bool is_u64, u64 *res)
{
	struct nfp_cpp *cpp = alink->abm->app->cpp;
	u64 val, sym_offset;
	unsigned int qid;
	u32 val32;
	int err;

	qid = band * NFP_NET_MAX_RX_RINGS + alink->queue_base + queue;

	sym_offset = qid * stride + offset;
	if (is_u64)
		err = __nfp_rtsym_readq(cpp, sym, 3, 0, sym_offset, &val);
	else
		err = __nfp_rtsym_readl(cpp, sym, 3, 0, sym_offset, &val32);
	if (err) {
		nfp_err(cpp, "RED offload reading stat failed on vNIC %d band %d queue %d (+ %d)\n",
			alink->id, band, queue, alink->queue_base);
		return err;
	}

	*res = is_u64 ? val : val32;
	return 0;
}

int __nfp_abm_ctrl_set_q_lvl(struct nfp_abm *abm, unsigned int id, u32 val)
{
	struct nfp_cpp *cpp = abm->app->cpp;
	u64 sym_offset;
	int err;

	__clear_bit(id, abm->threshold_undef);
	if (abm->thresholds[id] == val)
		return 0;

	sym_offset = id * NFP_QLVL_STRIDE + NFP_QLVL_THRS;
	err = __nfp_rtsym_writel(cpp, abm->q_lvls, 4, 0, sym_offset, val);
	if (err) {
		nfp_err(cpp,
			"RED offload setting level failed on subqueue %d\n",
			id);
		return err;
	}

	abm->thresholds[id] = val;
	return 0;
}

int nfp_abm_ctrl_set_q_lvl(struct nfp_abm_link *alink, unsigned int band,
			   unsigned int queue, u32 val)
{
	unsigned int threshold;

	threshold = band * NFP_NET_MAX_RX_RINGS + alink->queue_base + queue;

	return __nfp_abm_ctrl_set_q_lvl(alink->abm, threshold, val);
}

u64 nfp_abm_ctrl_stat_non_sto(struct nfp_abm_link *alink, unsigned int i)
{
	u64 val;

	if (nfp_abm_ctrl_stat(alink, alink->abm->qm_stats, NFP_QMSTAT_STRIDE,
			      NFP_QMSTAT_NON_STO, 0, i, true, &val))
		return 0;
	return val;
}

u64 nfp_abm_ctrl_stat_sto(struct nfp_abm_link *alink, unsigned int i)
{
	u64 val;

	if (nfp_abm_ctrl_stat(alink, alink->abm->qm_stats, NFP_QMSTAT_STRIDE,
			      NFP_QMSTAT_STO, 0, i, true, &val))
		return 0;
	return val;
}

int nfp_abm_ctrl_read_q_stats(struct nfp_abm_link *alink, unsigned int band,
			      unsigned int queue, struct nfp_alink_stats *stats)
{
	int err;

	stats->tx_pkts += nn_readq(alink->vnic, NFP_NET_CFG_RXR_STATS(queue));
	stats->tx_bytes += nn_readq(alink->vnic,
				    NFP_NET_CFG_RXR_STATS(queue) + 8);

	err = nfp_abm_ctrl_stat(alink, alink->abm->q_lvls, NFP_QLVL_STRIDE,
				NFP_QLVL_BLOG_BYTES, band, queue, false,
				&stats->backlog_bytes);
	if (err)
		return err;

	err = nfp_abm_ctrl_stat(alink, alink->abm->q_lvls,
				NFP_QLVL_STRIDE, NFP_QLVL_BLOG_PKTS,
				band, queue, false, &stats->backlog_pkts);
	if (err)
		return err;

	err = nfp_abm_ctrl_stat(alink, alink->abm->qm_stats,
				NFP_QMSTAT_STRIDE, NFP_QMSTAT_DROP,
				band, queue, true, &stats->drops);
	if (err)
		return err;

	return nfp_abm_ctrl_stat(alink, alink->abm->qm_stats,
				 NFP_QMSTAT_STRIDE, NFP_QMSTAT_ECN,
				 band, queue, true, &stats->overlimits);
}

int nfp_abm_ctrl_read_q_xstats(struct nfp_abm_link *alink,
			       unsigned int band, unsigned int queue,
			       struct nfp_alink_xstats *xstats)
{
	int err;

	err = nfp_abm_ctrl_stat(alink, alink->abm->qm_stats,
				NFP_QMSTAT_STRIDE, NFP_QMSTAT_DROP,
				band, queue, true, &xstats->pdrop);
	if (err)
		return err;

	return nfp_abm_ctrl_stat(alink, alink->abm->qm_stats,
				 NFP_QMSTAT_STRIDE, NFP_QMSTAT_ECN,
				 band, queue, true, &xstats->ecn_marked);
}

int nfp_abm_ctrl_qm_enable(struct nfp_abm *abm)
{
	return nfp_mbox_cmd(abm->app->pf, NFP_MBOX_PCIE_ABM_ENABLE,
			    NULL, 0, NULL, 0);
}

int nfp_abm_ctrl_qm_disable(struct nfp_abm *abm)
{
	return nfp_mbox_cmd(abm->app->pf, NFP_MBOX_PCIE_ABM_DISABLE,
			    NULL, 0, NULL, 0);
}

void nfp_abm_ctrl_read_params(struct nfp_abm_link *alink)
{
	alink->queue_base = nn_readl(alink->vnic, NFP_NET_CFG_START_RXQ);
	alink->queue_base /= alink->vnic->stride_rx;
}

static const struct nfp_rtsym *
nfp_abm_ctrl_find_rtsym(struct nfp_pf *pf, const char *name, unsigned int size)
{
	const struct nfp_rtsym *sym;

	sym = nfp_rtsym_lookup(pf->rtbl, name);
	if (!sym) {
		nfp_err(pf->cpp, "Symbol '%s' not found\n", name);
		return ERR_PTR(-ENOENT);
	}
	if (nfp_rtsym_size(sym) != size) {
		nfp_err(pf->cpp,
			"Symbol '%s' wrong size: expected %u got %llu\n",
			name, size, nfp_rtsym_size(sym));
		return ERR_PTR(-EINVAL);
	}

	return sym;
}

static const struct nfp_rtsym *
nfp_abm_ctrl_find_q_rtsym(struct nfp_abm *abm, const char *name_fmt,
			  size_t size)
{
	char pf_symbol[64];

	size = array3_size(size, abm->num_bands, NFP_NET_MAX_RX_RINGS);
	snprintf(pf_symbol, sizeof(pf_symbol), name_fmt,
		 abm->pf_id, nfp_abm_has_prio(abm) ? "_per_band" : "");

	return nfp_abm_ctrl_find_rtsym(abm->app->pf, pf_symbol, size);
}

int nfp_abm_ctrl_find_addrs(struct nfp_abm *abm)
{
	struct nfp_pf *pf = abm->app->pf;
	const struct nfp_rtsym *sym;
	int res;

	abm->pf_id = nfp_cppcore_pcie_unit(pf->cpp);

	/* Read count of prios and prio bands */
	res = nfp_pf_rtsym_read_optional(pf, NFP_NUM_BANDS_SYM_NAME, 1);
	if (res < 0)
		return res;
	abm->num_bands = res;

	res = nfp_pf_rtsym_read_optional(pf, NFP_NUM_PRIOS_SYM_NAME, 1);
	if (res < 0)
		return res;
	abm->num_prios = res;

	/* Check values are sane, U16_MAX is arbitrarily chosen as max */
	if (!is_power_of_2(abm->num_bands) || !is_power_of_2(abm->num_prios) ||
	    abm->num_bands > U16_MAX || abm->num_prios > U16_MAX ||
	    (abm->num_bands == 1) != (abm->num_prios == 1)) {
		nfp_err(pf->cpp,
			"invalid priomap description num bands: %u and num prios: %u\n",
			abm->num_bands, abm->num_prios);
		return -EINVAL;
	}

	/* Find level and stat symbols */
	sym = nfp_abm_ctrl_find_q_rtsym(abm, NFP_QLVL_SYM_NAME,
					NFP_QLVL_STRIDE);
	if (IS_ERR(sym))
		return PTR_ERR(sym);
	abm->q_lvls = sym;

	sym = nfp_abm_ctrl_find_q_rtsym(abm, NFP_QMSTAT_SYM_NAME,
					NFP_QMSTAT_STRIDE);
	if (IS_ERR(sym))
		return PTR_ERR(sym);
	abm->qm_stats = sym;

	return 0;
}
