/*
 * Copyright (c) 2016-2019 The Linux Foundation. All rights reserved.
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

#include "hal_hw_headers.h"
#include "dp_types.h"
#include "dp_rx.h"
#include "dp_peer.h"
#include "hal_rx.h"
#include "hal_api.h"
#include "qdf_nbuf.h"
#ifdef MESH_MODE_SUPPORT
#include "if_meta_hdr.h"
#endif
#include "dp_internal.h"
#include "dp_rx_mon.h"
#include "dp_ipa.h"

#ifdef ATH_RX_PRI_SAVE
#define DP_RX_TID_SAVE(_nbuf, _tid) \
	(qdf_nbuf_set_priority(_nbuf, _tid))
#else
#define DP_RX_TID_SAVE(_nbuf, _tid)
#endif

#ifdef CONFIG_WIN
static inline bool dp_rx_check_ap_bridge(struct dp_vdev *vdev)
{
	return vdev->ap_bridge_enabled;
}
#else
static inline bool dp_rx_check_ap_bridge(struct dp_vdev *vdev)
{
	if (vdev->opmode != wlan_op_mode_sta)
		return true;
	else
		return false;
}
#endif

/*
 * dp_rx_dump_info_and_assert() - dump RX Ring info and Rx Desc info
 *
 * @soc: core txrx main context
 * @hal_ring: opaque pointer to the HAL Rx Ring, which will be serviced
 * @ring_desc: opaque pointer to the RX ring descriptor
 * @rx_desc: host rs descriptor
 *
 * Return: void
 */
void dp_rx_dump_info_and_assert(struct dp_soc *soc, void *hal_ring,
				void *ring_desc, struct dp_rx_desc *rx_desc)
{
	void *hal_soc = soc->hal_soc;

	dp_rx_desc_dump(rx_desc);
	hal_srng_dump_ring_desc(hal_soc, hal_ring, ring_desc);
	hal_srng_dump_ring(hal_soc, hal_ring);
	qdf_assert_always(0);
}

/*
 * dp_rx_buffers_replenish() - replenish rxdma ring with rx nbufs
 *			       called during dp rx initialization
 *			       and at the end of dp_rx_process.
 *
 * @soc: core txrx main context
 * @mac_id: mac_id which is one of 3 mac_ids
 * @dp_rxdma_srng: dp rxdma circular ring
 * @rx_desc_pool: Pointer to free Rx descriptor pool
 * @num_req_buffers: number of buffer to be replenished
 * @desc_list: list of descs if called from dp_rx_process
 *	       or NULL during dp rx initialization or out of buffer
 *	       interrupt.
 * @tail: tail of descs list
 * Return: return success or failure
 */
QDF_STATUS dp_rx_buffers_replenish(struct dp_soc *dp_soc, uint32_t mac_id,
				struct dp_srng *dp_rxdma_srng,
				struct rx_desc_pool *rx_desc_pool,
				uint32_t num_req_buffers,
				union dp_rx_desc_list_elem_t **desc_list,
				union dp_rx_desc_list_elem_t **tail)
{
	uint32_t num_alloc_desc;
	uint16_t num_desc_to_free = 0;
	struct dp_pdev *dp_pdev = dp_get_pdev_for_mac_id(dp_soc, mac_id);
	uint32_t num_entries_avail;
	uint32_t count;
	int sync_hw_ptr = 1;
	qdf_dma_addr_t paddr;
	qdf_nbuf_t rx_netbuf;
	void *rxdma_ring_entry;
	union dp_rx_desc_list_elem_t *next;
	QDF_STATUS ret;

	void *rxdma_srng;

	rxdma_srng = dp_rxdma_srng->hal_srng;

	if (!rxdma_srng) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_DEBUG,
				  "rxdma srng not initialized");
		DP_STATS_INC(dp_pdev, replenish.rxdma_err, num_req_buffers);
		return QDF_STATUS_E_FAILURE;
	}

	QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_DEBUG,
		"requested %d buffers for replenish", num_req_buffers);

	hal_srng_access_start(dp_soc->hal_soc, rxdma_srng);
	num_entries_avail = hal_srng_src_num_avail(dp_soc->hal_soc,
						   rxdma_srng,
						   sync_hw_ptr);

	QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_DEBUG,
		"no of available entries in rxdma ring: %d",
		num_entries_avail);

	if (!(*desc_list) && (num_entries_avail >
		((dp_rxdma_srng->num_entries * 3) / 4))) {
		num_req_buffers = num_entries_avail;
	} else if (num_entries_avail < num_req_buffers) {
		num_desc_to_free = num_req_buffers - num_entries_avail;
		num_req_buffers = num_entries_avail;
	}

	if (qdf_unlikely(!num_req_buffers)) {
		num_desc_to_free = num_req_buffers;
		hal_srng_access_end(dp_soc->hal_soc, rxdma_srng);
		goto free_descs;
	}

	/*
	 * if desc_list is NULL, allocate the descs from freelist
	 */
	if (!(*desc_list)) {
		num_alloc_desc = dp_rx_get_free_desc_list(dp_soc, mac_id,
							  rx_desc_pool,
							  num_req_buffers,
							  desc_list,
							  tail);

		if (!num_alloc_desc) {
			QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
				"no free rx_descs in freelist");
			DP_STATS_INC(dp_pdev, err.desc_alloc_fail,
					num_req_buffers);
			hal_srng_access_end(dp_soc->hal_soc, rxdma_srng);
			return QDF_STATUS_E_NOMEM;
		}

		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_DEBUG,
			"%d rx desc allocated", num_alloc_desc);
		num_req_buffers = num_alloc_desc;
	}


	count = 0;

	while (count < num_req_buffers) {
		rx_netbuf = qdf_nbuf_alloc(dp_soc->osdev,
					RX_BUFFER_SIZE,
					RX_BUFFER_RESERVATION,
					RX_BUFFER_ALIGNMENT,
					FALSE);

		if (qdf_unlikely(!rx_netbuf)) {
			DP_STATS_INC(dp_pdev, replenish.nbuf_alloc_fail, 1);
			break;
		}

		ret = qdf_nbuf_map_single(dp_soc->osdev, rx_netbuf,
					  QDF_DMA_FROM_DEVICE);
		if (qdf_unlikely(QDF_IS_STATUS_ERROR(ret))) {
			qdf_nbuf_free(rx_netbuf);
			DP_STATS_INC(dp_pdev, replenish.map_err, 1);
			continue;
		}

		paddr = qdf_nbuf_get_frag_paddr(rx_netbuf, 0);

		/*
		 * check if the physical address of nbuf->data is
		 * less then 0x50000000 then free the nbuf and try
		 * allocating new nbuf. We can try for 100 times.
		 * this is a temp WAR till we fix it properly.
		 */
		ret = check_x86_paddr(dp_soc, &rx_netbuf, &paddr, dp_pdev);
		if (ret == QDF_STATUS_E_FAILURE) {
			DP_STATS_INC(dp_pdev, replenish.x86_fail, 1);
			break;
		}

		count++;

		rxdma_ring_entry = hal_srng_src_get_next(dp_soc->hal_soc,
							 rxdma_srng);
		qdf_assert_always(rxdma_ring_entry);

		next = (*desc_list)->next;

		dp_rx_desc_prep(&((*desc_list)->rx_desc), rx_netbuf);

		/* rx_desc.in_use should be zero at this time*/
		qdf_assert_always((*desc_list)->rx_desc.in_use == 0);

		(*desc_list)->rx_desc.in_use = 1;

		dp_verbose_debug("rx_netbuf=%pK, buf=%pK, paddr=0x%llx, cookie=%d",
				 rx_netbuf, qdf_nbuf_data(rx_netbuf),
				 (unsigned long long)paddr,
				 (*desc_list)->rx_desc.cookie);

		hal_rxdma_buff_addr_info_set(rxdma_ring_entry, paddr,
						(*desc_list)->rx_desc.cookie,
						rx_desc_pool->owner);

		*desc_list = next;

		dp_ipa_handle_rx_buf_smmu_mapping(dp_soc, rx_netbuf, true);
	}

	hal_srng_access_end(dp_soc->hal_soc, rxdma_srng);

	dp_verbose_debug("replenished buffers %d, rx desc added back to free list %u",
			 count, num_desc_to_free);

	DP_STATS_INC_PKT(dp_pdev, replenish.pkts, count,
			 (RX_BUFFER_SIZE * count));

free_descs:
	DP_STATS_INC(dp_pdev, buf_freelist, num_desc_to_free);
	/*
	 * add any available free desc back to the free list
	 */
	if (*desc_list)
		dp_rx_add_desc_list_to_free_list(dp_soc, desc_list, tail,
			mac_id, rx_desc_pool);

	return QDF_STATUS_SUCCESS;
}

/*
 * dp_rx_deliver_raw() - process RAW mode pkts and hand over the
 *				pkts to RAW mode simulation to
 *				decapsulate the pkt.
 *
 * @vdev: vdev on which RAW mode is enabled
 * @nbuf_list: list of RAW pkts to process
 * @peer: peer object from which the pkt is rx
 *
 * Return: void
 */
void
dp_rx_deliver_raw(struct dp_vdev *vdev, qdf_nbuf_t nbuf_list,
					struct dp_peer *peer)
{
	qdf_nbuf_t deliver_list_head = NULL;
	qdf_nbuf_t deliver_list_tail = NULL;
	qdf_nbuf_t nbuf;

	nbuf = nbuf_list;
	while (nbuf) {
		qdf_nbuf_t next = qdf_nbuf_next(nbuf);

		DP_RX_LIST_APPEND(deliver_list_head, deliver_list_tail, nbuf);

		DP_STATS_INC(vdev->pdev, rx_raw_pkts, 1);
		DP_STATS_INC_PKT(peer, rx.raw, 1, qdf_nbuf_len(nbuf));
		/*
		 * reset the chfrag_start and chfrag_end bits in nbuf cb
		 * as this is a non-amsdu pkt and RAW mode simulation expects
		 * these bit s to be 0 for non-amsdu pkt.
		 */
		if (qdf_nbuf_is_rx_chfrag_start(nbuf) &&
			 qdf_nbuf_is_rx_chfrag_end(nbuf)) {
			qdf_nbuf_set_rx_chfrag_start(nbuf, 0);
			qdf_nbuf_set_rx_chfrag_end(nbuf, 0);
		}

		nbuf = next;
	}

	vdev->osif_rsim_rx_decap(vdev->osif_vdev, &deliver_list_head,
				 &deliver_list_tail, (struct cdp_peer*) peer);

	vdev->osif_rx(vdev->osif_vdev, deliver_list_head);
}


#ifdef DP_LFR
/*
 * In case of LFR, data of a new peer might be sent up
 * even before peer is added.
 */
static inline struct dp_vdev *
dp_get_vdev_from_peer(struct dp_soc *soc,
			uint16_t peer_id,
			struct dp_peer *peer,
			struct hal_rx_mpdu_desc_info mpdu_desc_info)
{
	struct dp_vdev *vdev;
	uint8_t vdev_id;

	if (unlikely(!peer)) {
		if (peer_id != HTT_INVALID_PEER) {
			vdev_id = DP_PEER_METADATA_ID_GET(
					mpdu_desc_info.peer_meta_data);
			QDF_TRACE(QDF_MODULE_ID_DP,
				QDF_TRACE_LEVEL_DEBUG,
				FL("PeerID %d not found use vdevID %d"),
				peer_id, vdev_id);
			vdev = dp_get_vdev_from_soc_vdev_id_wifi3(soc,
							vdev_id);
		} else {
			QDF_TRACE(QDF_MODULE_ID_DP,
				QDF_TRACE_LEVEL_DEBUG,
				FL("Invalid PeerID %d"),
				peer_id);
			return NULL;
		}
	} else {
		vdev = peer->vdev;
	}
	return vdev;
}
#else
static inline struct dp_vdev *
dp_get_vdev_from_peer(struct dp_soc *soc,
			uint16_t peer_id,
			struct dp_peer *peer,
			struct hal_rx_mpdu_desc_info mpdu_desc_info)
{
	if (unlikely(!peer)) {
		QDF_TRACE(QDF_MODULE_ID_DP,
			QDF_TRACE_LEVEL_DEBUG,
			FL("Peer not found for peerID %d"),
			peer_id);
		return NULL;
	} else {
		return peer->vdev;
	}
}
#endif

/**
 * dp_rx_da_learn() - Add AST entry based on DA lookup
 *			This is a WAR for HK 1.0 and will
 *			be removed in HK 2.0
 *
 * @soc: core txrx main context
 * @rx_tlv_hdr	: start address of rx tlvs
 * @ta_peer	: Transmitter peer entry
 * @nbuf	: nbuf to retrieve destination mac for which AST will be added
 *
 */
#ifdef FEATURE_WDS
static void
dp_rx_da_learn(struct dp_soc *soc,
	       uint8_t *rx_tlv_hdr,
	       struct dp_peer *ta_peer,
	       qdf_nbuf_t nbuf)
{
	/* For HKv2 DA port learing is not needed */
	if (qdf_likely(soc->ast_override_support))
		return;

	if (qdf_unlikely(!ta_peer))
		return;

	if (qdf_unlikely(ta_peer->vdev->opmode != wlan_op_mode_ap))
		return;

	if (!soc->da_war_enabled)
		return;

	if (qdf_unlikely(!qdf_nbuf_is_da_valid(nbuf) &&
			 !qdf_nbuf_is_da_mcbc(nbuf))) {
		dp_peer_add_ast(soc,
				ta_peer,
				qdf_nbuf_data(nbuf),
				CDP_TXRX_AST_TYPE_DA,
				IEEE80211_NODE_F_WDS_HM);
	}
}
#else
static void
dp_rx_da_learn(struct dp_soc *soc,
	       uint8_t *rx_tlv_hdr,
	       struct dp_peer *ta_peer,
	       qdf_nbuf_t nbuf)
{
}
#endif

/**
 * dp_rx_intrabss_fwd() - Implements the Intra-BSS forwarding logic
 *
 * @soc: core txrx main context
 * @ta_peer	: source peer entry
 * @rx_tlv_hdr	: start address of rx tlvs
 * @nbuf	: nbuf that has to be intrabss forwarded
 *
 * Return: bool: true if it is forwarded else false
 */
static bool
dp_rx_intrabss_fwd(struct dp_soc *soc,
			struct dp_peer *ta_peer,
			uint8_t *rx_tlv_hdr,
			qdf_nbuf_t nbuf)
{
	uint16_t da_idx;
	uint16_t len;
	uint8_t is_frag;
	struct dp_peer *da_peer;
	struct dp_ast_entry *ast_entry;
	qdf_nbuf_t nbuf_copy;
	uint8_t tid = qdf_nbuf_get_tid_val(nbuf);
	struct cdp_tid_rx_stats *tid_stats =
		&ta_peer->vdev->pdev->stats.tid_stats.tid_rx_stats[tid];

	/* check if the destination peer is available in peer table
	 * and also check if the source peer and destination peer
	 * belong to the same vap and destination peer is not bss peer.
	 */

	if ((qdf_nbuf_is_da_valid(nbuf) && !qdf_nbuf_is_da_mcbc(nbuf))) {
		da_idx = hal_rx_msdu_end_da_idx_get(soc->hal_soc, rx_tlv_hdr);

		ast_entry = soc->ast_table[da_idx];
		if (!ast_entry)
			return false;

		if (ast_entry->type == CDP_TXRX_AST_TYPE_DA) {
			ast_entry->is_active = TRUE;
			return false;
		}

		da_peer = ast_entry->peer;

		if (!da_peer)
			return false;
		/* TA peer cannot be same as peer(DA) on which AST is present
		 * this indicates a change in topology and that AST entries
		 * are yet to be updated.
		 */
		if (da_peer == ta_peer)
			return false;

		if (da_peer->vdev == ta_peer->vdev && !da_peer->bss_peer) {
			len = QDF_NBUF_CB_RX_PKT_LEN(nbuf);
			is_frag = qdf_nbuf_is_frag(nbuf);
			memset(nbuf->cb, 0x0, sizeof(nbuf->cb));

			/* linearize the nbuf just before we send to
			 * dp_tx_send()
			 */
			if (qdf_unlikely(is_frag)) {
				if (qdf_nbuf_linearize(nbuf) == -ENOMEM)
					return false;

				nbuf = qdf_nbuf_unshare(nbuf);
				if (!nbuf) {
					DP_STATS_INC_PKT(ta_peer,
							 rx.intra_bss.fail,
							 1,
							 len);
					/* return true even though the pkt is
					 * not forwarded. Basically skb_unshare
					 * failed and we want to continue with
					 * next nbuf.
					 */
					tid_stats->fail_cnt[INTRABSS_DROP]++;
					return true;
				}
			}

			if (!dp_tx_send(ta_peer->vdev, nbuf)) {
				DP_STATS_INC_PKT(ta_peer, rx.intra_bss.pkts, 1,
						 len);
				return true;
			} else {
				DP_STATS_INC_PKT(ta_peer, rx.intra_bss.fail, 1,
						len);
				tid_stats->fail_cnt[INTRABSS_DROP]++;
				return false;
			}
		}
	}
	/* if it is a broadcast pkt (eg: ARP) and it is not its own
	 * source, then clone the pkt and send the cloned pkt for
	 * intra BSS forwarding and original pkt up the network stack
	 * Note: how do we handle multicast pkts. do we forward
	 * all multicast pkts as is or let a higher layer module
	 * like igmpsnoop decide whether to forward or not with
	 * Mcast enhancement.
	 */
	else if (qdf_unlikely((qdf_nbuf_is_da_mcbc(nbuf) &&
			       !ta_peer->bss_peer))) {
		nbuf_copy = qdf_nbuf_copy(nbuf);
		if (!nbuf_copy)
			return false;

		len = QDF_NBUF_CB_RX_PKT_LEN(nbuf);
		memset(nbuf_copy->cb, 0x0, sizeof(nbuf_copy->cb));

		if (dp_tx_send(ta_peer->vdev, nbuf_copy)) {
			DP_STATS_INC_PKT(ta_peer, rx.intra_bss.fail, 1, len);
			tid_stats->fail_cnt[INTRABSS_DROP]++;
			qdf_nbuf_free(nbuf_copy);
		} else {
			DP_STATS_INC_PKT(ta_peer, rx.intra_bss.pkts, 1, len);
			tid_stats->intrabss_cnt++;
		}
	}
	/* return false as we have to still send the original pkt
	 * up the stack
	 */
	return false;
}

#ifdef MESH_MODE_SUPPORT

/**
 * dp_rx_fill_mesh_stats() - Fills the mesh per packet receive stats
 *
 * @vdev: DP Virtual device handle
 * @nbuf: Buffer pointer
 * @rx_tlv_hdr: start of rx tlv header
 * @peer: pointer to peer
 *
 * This function allocated memory for mesh receive stats and fill the
 * required stats. Stores the memory address in skb cb.
 *
 * Return: void
 */

void dp_rx_fill_mesh_stats(struct dp_vdev *vdev, qdf_nbuf_t nbuf,
				uint8_t *rx_tlv_hdr, struct dp_peer *peer)
{
	struct mesh_recv_hdr_s *rx_info = NULL;
	uint32_t pkt_type;
	uint32_t nss;
	uint32_t rate_mcs;
	uint32_t bw;

	/* fill recv mesh stats */
	rx_info = qdf_mem_malloc(sizeof(struct mesh_recv_hdr_s));

	/* upper layers are resposible to free this memory */

	if (!rx_info) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			"Memory allocation failed for mesh rx stats");
		DP_STATS_INC(vdev->pdev, mesh_mem_alloc, 1);
		return;
	}

	rx_info->rs_flags = MESH_RXHDR_VER1;
	if (qdf_nbuf_is_rx_chfrag_start(nbuf))
		rx_info->rs_flags |= MESH_RX_FIRST_MSDU;

	if (qdf_nbuf_is_rx_chfrag_end(nbuf))
		rx_info->rs_flags |= MESH_RX_LAST_MSDU;

	if (hal_rx_attn_msdu_get_is_decrypted(rx_tlv_hdr)) {
		rx_info->rs_flags |= MESH_RX_DECRYPTED;
		rx_info->rs_keyix = hal_rx_msdu_get_keyid(rx_tlv_hdr);
		if (vdev->osif_get_key)
			vdev->osif_get_key(vdev->osif_vdev,
					&rx_info->rs_decryptkey[0],
					&peer->mac_addr.raw[0],
					rx_info->rs_keyix);
	}

	rx_info->rs_rssi = hal_rx_msdu_start_get_rssi(rx_tlv_hdr);
	rx_info->rs_channel = hal_rx_msdu_start_get_freq(rx_tlv_hdr);
	pkt_type = hal_rx_msdu_start_get_pkt_type(rx_tlv_hdr);
	rate_mcs = hal_rx_msdu_start_rate_mcs_get(rx_tlv_hdr);
	bw = hal_rx_msdu_start_bw_get(rx_tlv_hdr);
	nss = hal_rx_msdu_start_nss_get(vdev->pdev->soc->hal_soc, rx_tlv_hdr);
	rx_info->rs_ratephy1 = rate_mcs | (nss << 0x8) | (pkt_type << 16) |
				(bw << 24);

	qdf_nbuf_set_rx_fctx_type(nbuf, (void *)rx_info, CB_FTYPE_MESH_RX_INFO);

	QDF_TRACE(QDF_MODULE_ID_TXRX, QDF_TRACE_LEVEL_INFO_MED,
		FL("Mesh rx stats: flags %x, rssi %x, chn %x, rate %x, kix %x"),
						rx_info->rs_flags,
						rx_info->rs_rssi,
						rx_info->rs_channel,
						rx_info->rs_ratephy1,
						rx_info->rs_keyix);

}

/**
 * dp_rx_filter_mesh_packets() - Filters mesh unwanted packets
 *
 * @vdev: DP Virtual device handle
 * @nbuf: Buffer pointer
 * @rx_tlv_hdr: start of rx tlv header
 *
 * This checks if the received packet is matching any filter out
 * catogery and and drop the packet if it matches.
 *
 * Return: status(0 indicates drop, 1 indicate to no drop)
 */

QDF_STATUS dp_rx_filter_mesh_packets(struct dp_vdev *vdev, qdf_nbuf_t nbuf,
					uint8_t *rx_tlv_hdr)
{
	union dp_align_mac_addr mac_addr;

	if (qdf_unlikely(vdev->mesh_rx_filter)) {
		if (vdev->mesh_rx_filter & MESH_FILTER_OUT_FROMDS)
			if (hal_rx_mpdu_get_fr_ds(rx_tlv_hdr))
				return  QDF_STATUS_SUCCESS;

		if (vdev->mesh_rx_filter & MESH_FILTER_OUT_TODS)
			if (hal_rx_mpdu_get_to_ds(rx_tlv_hdr))
				return  QDF_STATUS_SUCCESS;

		if (vdev->mesh_rx_filter & MESH_FILTER_OUT_NODS)
			if (!hal_rx_mpdu_get_fr_ds(rx_tlv_hdr)
				&& !hal_rx_mpdu_get_to_ds(rx_tlv_hdr))
				return  QDF_STATUS_SUCCESS;

		if (vdev->mesh_rx_filter & MESH_FILTER_OUT_RA) {
			if (hal_rx_mpdu_get_addr1(rx_tlv_hdr,
					&mac_addr.raw[0]))
				return QDF_STATUS_E_FAILURE;

			if (!qdf_mem_cmp(&mac_addr.raw[0],
					&vdev->mac_addr.raw[0],
					QDF_MAC_ADDR_SIZE))
				return  QDF_STATUS_SUCCESS;
		}

		if (vdev->mesh_rx_filter & MESH_FILTER_OUT_TA) {
			if (hal_rx_mpdu_get_addr2(rx_tlv_hdr,
					&mac_addr.raw[0]))
				return QDF_STATUS_E_FAILURE;

			if (!qdf_mem_cmp(&mac_addr.raw[0],
					&vdev->mac_addr.raw[0],
					QDF_MAC_ADDR_SIZE))
				return  QDF_STATUS_SUCCESS;
		}
	}

	return QDF_STATUS_E_FAILURE;
}

#else
void dp_rx_fill_mesh_stats(struct dp_vdev *vdev, qdf_nbuf_t nbuf,
				uint8_t *rx_tlv_hdr, struct dp_peer *peer)
{
}

QDF_STATUS dp_rx_filter_mesh_packets(struct dp_vdev *vdev, qdf_nbuf_t nbuf,
					uint8_t *rx_tlv_hdr)
{
	return QDF_STATUS_E_FAILURE;
}

#endif

#ifdef CONFIG_WIN
/**
 * dp_rx_nac_filter(): Function to perform filtering of non-associated
 * clients
 * @pdev: DP pdev handle
 * @rx_pkt_hdr: Rx packet Header
 *
 * return: dp_vdev*
 */
static
struct dp_vdev *dp_rx_nac_filter(struct dp_pdev *pdev,
		uint8_t *rx_pkt_hdr)
{
	struct ieee80211_frame *wh;
	struct dp_neighbour_peer *peer = NULL;

	wh = (struct ieee80211_frame *)rx_pkt_hdr;

	if ((wh->i_fc[1] & IEEE80211_FC1_DIR_MASK) != IEEE80211_FC1_DIR_TODS)
		return NULL;

	qdf_spin_lock_bh(&pdev->neighbour_peer_mutex);
	TAILQ_FOREACH(peer, &pdev->neighbour_peers_list,
				neighbour_peer_list_elem) {
		if (qdf_mem_cmp(&peer->neighbour_peers_macaddr.raw[0],
				wh->i_addr2, QDF_MAC_ADDR_SIZE) == 0) {
			QDF_TRACE(
				QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_DEBUG,
				FL("NAC configuration matched for mac-%2x:%2x:%2x:%2x:%2x:%2x"),
				peer->neighbour_peers_macaddr.raw[0],
				peer->neighbour_peers_macaddr.raw[1],
				peer->neighbour_peers_macaddr.raw[2],
				peer->neighbour_peers_macaddr.raw[3],
				peer->neighbour_peers_macaddr.raw[4],
				peer->neighbour_peers_macaddr.raw[5]);

				qdf_spin_unlock_bh(&pdev->neighbour_peer_mutex);

			return pdev->monitor_vdev;
		}
	}
	qdf_spin_unlock_bh(&pdev->neighbour_peer_mutex);

	return NULL;
}

/**
 * dp_rx_process_invalid_peer(): Function to pass invalid peer list to umac
 * @soc: DP SOC handle
 * @mpdu: mpdu for which peer is invalid
 * @mac_id: mac_id which is one of 3 mac_ids(Assuming mac_id and
 * pool_id has same mapping)
 *
 * return: integer type
 */
uint8_t dp_rx_process_invalid_peer(struct dp_soc *soc, qdf_nbuf_t mpdu,
				   uint8_t mac_id)
{
	struct dp_invalid_peer_msg msg;
	struct dp_vdev *vdev = NULL;
	struct dp_pdev *pdev = NULL;
	struct ieee80211_frame *wh;
	qdf_nbuf_t curr_nbuf, next_nbuf;
	uint8_t *rx_tlv_hdr = qdf_nbuf_data(mpdu);
	uint8_t *rx_pkt_hdr = hal_rx_pkt_hdr_get(rx_tlv_hdr);

	rx_pkt_hdr = hal_rx_pkt_hdr_get(rx_tlv_hdr);

	if (!HAL_IS_DECAP_FORMAT_RAW(rx_tlv_hdr)) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_DEBUG,
			  "Drop decapped frames");
		goto free;
	}

	wh = (struct ieee80211_frame *)rx_pkt_hdr;

	if (!DP_FRAME_IS_DATA(wh)) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_DEBUG,
			  "NAWDS valid only for data frames");
		goto free;
	}

	if (qdf_nbuf_len(mpdu) < sizeof(struct ieee80211_frame)) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			"Invalid nbuf length");
		goto free;
	}

	pdev = dp_get_pdev_for_mac_id(soc, mac_id);

	if (!pdev) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  "PDEV not found");
		goto free;
	}

	if (pdev->filter_neighbour_peers) {
		/* Next Hop scenario not yet handle */
		vdev = dp_rx_nac_filter(pdev, rx_pkt_hdr);
		if (vdev) {
			dp_rx_mon_deliver(soc, pdev->pdev_id,
					  pdev->invalid_peer_head_msdu,
					  pdev->invalid_peer_tail_msdu);

			pdev->invalid_peer_head_msdu = NULL;
			pdev->invalid_peer_tail_msdu = NULL;

			return 0;
		}
	}


	TAILQ_FOREACH(vdev, &pdev->vdev_list, vdev_list_elem) {

		if (qdf_mem_cmp(wh->i_addr1, vdev->mac_addr.raw,
				QDF_MAC_ADDR_SIZE) == 0) {
			goto out;
		}
	}

	if (!vdev) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			"VDEV not found");
		goto free;
	}

out:
	msg.wh = wh;
	qdf_nbuf_pull_head(mpdu, RX_PKT_TLVS_LEN);
	msg.nbuf = mpdu;
	msg.vdev_id = vdev->vdev_id;
	if (pdev->soc->cdp_soc.ol_ops->rx_invalid_peer)
		pdev->soc->cdp_soc.ol_ops->rx_invalid_peer(pdev->ctrl_pdev,
							&msg);

free:
	/* Drop and free packet */
	curr_nbuf = mpdu;
	while (curr_nbuf) {
		next_nbuf = qdf_nbuf_next(curr_nbuf);
		qdf_nbuf_free(curr_nbuf);
		curr_nbuf = next_nbuf;
	}

	return 0;
}

/**
 * dp_rx_process_invalid_peer_wrapper(): Function to wrap invalid peer handler
 * @soc: DP SOC handle
 * @mpdu: mpdu for which peer is invalid
 * @mpdu_done: if an mpdu is completed
 * @mac_id: mac_id which is one of 3 mac_ids(Assuming mac_id and
 * pool_id has same mapping)
 *
 * return: integer type
 */
void dp_rx_process_invalid_peer_wrapper(struct dp_soc *soc,
					qdf_nbuf_t mpdu, bool mpdu_done,
					uint8_t mac_id)
{
	/* Only trigger the process when mpdu is completed */
	if (mpdu_done)
		dp_rx_process_invalid_peer(soc, mpdu, mac_id);
}
#else
uint8_t dp_rx_process_invalid_peer(struct dp_soc *soc, qdf_nbuf_t mpdu,
				   uint8_t mac_id)
{
	qdf_nbuf_t curr_nbuf, next_nbuf;
	struct dp_pdev *pdev;
	struct dp_vdev *vdev = NULL;
	struct ieee80211_frame *wh;
	uint8_t *rx_tlv_hdr = qdf_nbuf_data(mpdu);
	uint8_t *rx_pkt_hdr = hal_rx_pkt_hdr_get(rx_tlv_hdr);

	wh = (struct ieee80211_frame *)rx_pkt_hdr;

	if (!DP_FRAME_IS_DATA(wh)) {
		QDF_TRACE_ERROR_RL(QDF_MODULE_ID_DP,
				   "only for data frames");
		goto free;
	}

	if (qdf_nbuf_len(mpdu) < sizeof(struct ieee80211_frame)) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  "Invalid nbuf length");
		goto free;
	}

	pdev = dp_get_pdev_for_mac_id(soc, mac_id);
	if (!pdev) {
		QDF_TRACE(QDF_MODULE_ID_DP,
			  QDF_TRACE_LEVEL_ERROR,
			  "PDEV not found");
		goto free;
	}

	qdf_spin_lock_bh(&pdev->vdev_list_lock);
	DP_PDEV_ITERATE_VDEV_LIST(pdev, vdev) {
		if (qdf_mem_cmp(wh->i_addr1, vdev->mac_addr.raw,
				QDF_MAC_ADDR_SIZE) == 0) {
			qdf_spin_unlock_bh(&pdev->vdev_list_lock);
			goto out;
		}
	}
	qdf_spin_unlock_bh(&pdev->vdev_list_lock);

	if (!vdev) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			  "VDEV not found");
		goto free;
	}

out:
	if (soc->cdp_soc.ol_ops->rx_invalid_peer)
		soc->cdp_soc.ol_ops->rx_invalid_peer(vdev->vdev_id, wh);
free:
	/* reset the head and tail pointers */
	pdev = dp_get_pdev_for_mac_id(soc, mac_id);
	if (pdev) {
		pdev->invalid_peer_head_msdu = NULL;
		pdev->invalid_peer_tail_msdu = NULL;
	}

	/* Drop and free packet */
	curr_nbuf = mpdu;
	while (curr_nbuf) {
		next_nbuf = qdf_nbuf_next(curr_nbuf);
		qdf_nbuf_free(curr_nbuf);
		curr_nbuf = next_nbuf;
	}

	return 0;
}

void dp_rx_process_invalid_peer_wrapper(struct dp_soc *soc,
					qdf_nbuf_t mpdu, bool mpdu_done,
					uint8_t mac_id)
{
	/* Process the nbuf */
	dp_rx_process_invalid_peer(soc, mpdu, mac_id);
}
#endif

#ifdef RECEIVE_OFFLOAD
/**
 * dp_rx_print_offload_info() - Print offload info from RX TLV
 * @rx_tlv: RX TLV for which offload information is to be printed
 *
 * Return: None
 */
static void dp_rx_print_offload_info(uint8_t *rx_tlv)
{
	dp_verbose_debug("----------------------RX DESC LRO/GRO----------------------");
	dp_verbose_debug("lro_eligible 0x%x", HAL_RX_TLV_GET_LRO_ELIGIBLE(rx_tlv));
	dp_verbose_debug("pure_ack 0x%x", HAL_RX_TLV_GET_TCP_PURE_ACK(rx_tlv));
	dp_verbose_debug("chksum 0x%x", HAL_RX_TLV_GET_TCP_CHKSUM(rx_tlv));
	dp_verbose_debug("TCP seq num 0x%x", HAL_RX_TLV_GET_TCP_SEQ(rx_tlv));
	dp_verbose_debug("TCP ack num 0x%x", HAL_RX_TLV_GET_TCP_ACK(rx_tlv));
	dp_verbose_debug("TCP window 0x%x", HAL_RX_TLV_GET_TCP_WIN(rx_tlv));
	dp_verbose_debug("TCP protocol 0x%x", HAL_RX_TLV_GET_TCP_PROTO(rx_tlv));
	dp_verbose_debug("TCP offset 0x%x", HAL_RX_TLV_GET_TCP_OFFSET(rx_tlv));
	dp_verbose_debug("toeplitz 0x%x", HAL_RX_TLV_GET_FLOW_ID_TOEPLITZ(rx_tlv));
	dp_verbose_debug("---------------------------------------------------------");
}

/**
 * dp_rx_fill_gro_info() - Fill GRO info from RX TLV into skb->cb
 * @soc: DP SOC handle
 * @rx_tlv: RX TLV received for the msdu
 * @msdu: msdu for which GRO info needs to be filled
 *
 * Return: None
 */
static
void dp_rx_fill_gro_info(struct dp_soc *soc, uint8_t *rx_tlv,
			 qdf_nbuf_t msdu)
{
	if (!wlan_cfg_is_gro_enabled(soc->wlan_cfg_ctx))
		return;

	/* Filling up RX offload info only for TCP packets */
	if (!HAL_RX_TLV_GET_TCP_PROTO(rx_tlv))
		return;

	QDF_NBUF_CB_RX_LRO_ELIGIBLE(msdu) =
		 HAL_RX_TLV_GET_LRO_ELIGIBLE(rx_tlv);
	QDF_NBUF_CB_RX_TCP_PURE_ACK(msdu) =
			HAL_RX_TLV_GET_TCP_PURE_ACK(rx_tlv);
	QDF_NBUF_CB_RX_TCP_CHKSUM(msdu) =
			 HAL_RX_TLV_GET_TCP_CHKSUM(rx_tlv);
	QDF_NBUF_CB_RX_TCP_SEQ_NUM(msdu) =
			 HAL_RX_TLV_GET_TCP_SEQ(rx_tlv);
	QDF_NBUF_CB_RX_TCP_ACK_NUM(msdu) =
			 HAL_RX_TLV_GET_TCP_ACK(rx_tlv);
	QDF_NBUF_CB_RX_TCP_WIN(msdu) =
			 HAL_RX_TLV_GET_TCP_WIN(rx_tlv);
	QDF_NBUF_CB_RX_TCP_PROTO(msdu) =
			 HAL_RX_TLV_GET_TCP_PROTO(rx_tlv);
	QDF_NBUF_CB_RX_IPV6_PROTO(msdu) =
			 HAL_RX_TLV_GET_IPV6(rx_tlv);
	QDF_NBUF_CB_RX_TCP_OFFSET(msdu) =
			 HAL_RX_TLV_GET_TCP_OFFSET(rx_tlv);
	QDF_NBUF_CB_RX_FLOW_ID(msdu) =
			 HAL_RX_TLV_GET_FLOW_ID_TOEPLITZ(rx_tlv);

	dp_rx_print_offload_info(rx_tlv);
}
#else
static void dp_rx_fill_gro_info(struct dp_soc *soc, uint8_t *rx_tlv,
				qdf_nbuf_t msdu)
{
}
#endif /* RECEIVE_OFFLOAD */

/**
 * dp_rx_adjust_nbuf_len() - set appropriate msdu length in nbuf.
 *
 * @nbuf: pointer to msdu.
 * @mpdu_len: mpdu length
 *
 * Return: returns true if nbuf is last msdu of mpdu else retuns false.
 */
static inline bool dp_rx_adjust_nbuf_len(qdf_nbuf_t nbuf, uint16_t *mpdu_len)
{
	bool last_nbuf;

	if (*mpdu_len > (RX_BUFFER_SIZE - RX_PKT_TLVS_LEN)) {
		qdf_nbuf_set_pktlen(nbuf, RX_BUFFER_SIZE);
		last_nbuf = false;
	} else {
		qdf_nbuf_set_pktlen(nbuf, (*mpdu_len + RX_PKT_TLVS_LEN));
		last_nbuf = true;
	}

	*mpdu_len -= (RX_BUFFER_SIZE - RX_PKT_TLVS_LEN);

	return last_nbuf;
}

/**
 * dp_rx_sg_create() - create a frag_list for MSDUs which are spread across
 *		     multiple nbufs.
 * @nbuf: pointer to the first msdu of an amsdu.
 * @rx_tlv_hdr: pointer to the start of RX TLV headers.
 *
 *
 * This function implements the creation of RX frag_list for cases
 * where an MSDU is spread across multiple nbufs.
 *
 * Return: returns the head nbuf which contains complete frag_list.
 */
qdf_nbuf_t dp_rx_sg_create(qdf_nbuf_t nbuf, uint8_t *rx_tlv_hdr)
{
	qdf_nbuf_t parent, next, frag_list;
	uint16_t frag_list_len = 0;
	uint16_t mpdu_len;
	bool last_nbuf;

	mpdu_len = hal_rx_msdu_start_msdu_len_get(rx_tlv_hdr);
	/*
	 * this is a case where the complete msdu fits in one single nbuf.
	 * in this case HW sets both start and end bit and we only need to
	 * reset these bits for RAW mode simulator to decap the pkt
	 */
	if (qdf_nbuf_is_rx_chfrag_start(nbuf) &&
					qdf_nbuf_is_rx_chfrag_end(nbuf)) {
		qdf_nbuf_set_pktlen(nbuf, mpdu_len + RX_PKT_TLVS_LEN);
		qdf_nbuf_pull_head(nbuf, RX_PKT_TLVS_LEN);
		return nbuf;
	}

	/*
	 * This is a case where we have multiple msdus (A-MSDU) spread across
	 * multiple nbufs. here we create a fraglist out of these nbufs.
	 *
	 * the moment we encounter a nbuf with continuation bit set we
	 * know for sure we have an MSDU which is spread across multiple
	 * nbufs. We loop through and reap nbufs till we reach last nbuf.
	 */
	parent = nbuf;
	frag_list = nbuf->next;
	nbuf = nbuf->next;

	/*
	 * set the start bit in the first nbuf we encounter with continuation
	 * bit set. This has the proper mpdu length set as it is the first
	 * msdu of the mpdu. this becomes the parent nbuf and the subsequent
	 * nbufs will form the frag_list of the parent nbuf.
	 */
	qdf_nbuf_set_rx_chfrag_start(parent, 1);
	last_nbuf = dp_rx_adjust_nbuf_len(parent, &mpdu_len);

	/*
	 * this is where we set the length of the fragments which are
	 * associated to the parent nbuf. We iterate through the frag_list
	 * till we hit the last_nbuf of the list.
	 */
	do {
		last_nbuf = dp_rx_adjust_nbuf_len(nbuf, &mpdu_len);
		qdf_nbuf_pull_head(nbuf, RX_PKT_TLVS_LEN);
		frag_list_len += qdf_nbuf_len(nbuf);

		if (last_nbuf) {
			next = nbuf->next;
			nbuf->next = NULL;
			break;
		}

		nbuf = nbuf->next;
	} while (!last_nbuf);

	qdf_nbuf_set_rx_chfrag_start(nbuf, 0);
	qdf_nbuf_append_ext_list(parent, frag_list, frag_list_len);
	parent->next = next;

	qdf_nbuf_pull_head(parent, RX_PKT_TLVS_LEN);
	return parent;
}

/**
 * dp_rx_compute_delay() - Compute and fill in all timestamps
 *				to pass in correct fields
 *
 * @vdev: pdev handle
 * @tx_desc: tx descriptor
 * @tid: tid value
 * Return: none
 */
void dp_rx_compute_delay(struct dp_vdev *vdev, qdf_nbuf_t nbuf)
{
	int64_t current_ts = qdf_ktime_to_ms(qdf_ktime_get());
	uint32_t to_stack = qdf_nbuf_get_timedelta_ms(nbuf);
	uint8_t tid = qdf_nbuf_get_tid_val(nbuf);
	uint32_t interframe_delay =
		(uint32_t)(current_ts - vdev->prev_rx_deliver_tstamp);

	dp_update_delay_stats(vdev->pdev, to_stack, tid,
			      CDP_DELAY_STATS_REAP_STACK);
	/*
	 * Update interframe delay stats calculated at deliver_data_ol point.
	 * Value of vdev->prev_rx_deliver_tstamp will be 0 for 1st frame, so
	 * interframe delay will not be calculate correctly for 1st frame.
	 * On the other side, this will help in avoiding extra per packet check
	 * of vdev->prev_rx_deliver_tstamp.
	 */
	dp_update_delay_stats(vdev->pdev, interframe_delay, tid,
			      CDP_DELAY_STATS_RX_INTERFRAME);
	vdev->prev_rx_deliver_tstamp = current_ts;
}

/**
 * dp_rx_drop_nbuf_list() - drop an nbuf list
 * @pdev: dp pdev reference
 * @buf_list: buffer list to be dropepd
 *
 * Return: int (number of bufs dropped)
 */
static inline int dp_rx_drop_nbuf_list(struct dp_pdev *pdev,
				       qdf_nbuf_t buf_list)
{
	struct cdp_tid_rx_stats *stats = NULL;
	uint8_t tid = 0;
	int num_dropped = 0;
	qdf_nbuf_t buf, next_buf;

	buf = buf_list;
	while (buf) {
		next_buf = qdf_nbuf_queue_next(buf);
		tid = qdf_nbuf_get_tid_val(buf);
		stats = &pdev->stats.tid_stats.tid_rx_stats[tid];
		stats->fail_cnt[INVALID_PEER_VDEV]++;
		stats->delivered_to_stack--;
		qdf_nbuf_free(buf);
		buf = next_buf;
		num_dropped++;
	}

	return num_dropped;
}

#ifdef PEER_CACHE_RX_PKTS
/**
 * dp_rx_flush_rx_cached() - flush cached rx frames
 * @peer: peer
 * @drop: flag to drop frames or forward to net stack
 *
 * Return: None
 */
void dp_rx_flush_rx_cached(struct dp_peer *peer, bool drop)
{
	struct dp_peer_cached_bufq *bufqi;
	struct dp_rx_cached_buf *cache_buf = NULL;
	ol_txrx_rx_fp data_rx = NULL;
	int num_buff_elem;
	QDF_STATUS status;

	if (qdf_atomic_inc_return(&peer->flush_in_progress) > 1) {
		qdf_atomic_dec(&peer->flush_in_progress);
		return;
	}

	qdf_spin_lock_bh(&peer->peer_info_lock);
	if (peer->state >= OL_TXRX_PEER_STATE_CONN && peer->vdev->osif_rx)
		data_rx = peer->vdev->osif_rx;
	else
		drop = true;
	qdf_spin_unlock_bh(&peer->peer_info_lock);

	bufqi = &peer->bufq_info;

	qdf_spin_lock_bh(&bufqi->bufq_lock);
	if (qdf_list_empty(&bufqi->cached_bufq)) {
		qdf_spin_unlock_bh(&bufqi->bufq_lock);
		return;
	}
	qdf_list_remove_front(&bufqi->cached_bufq,
			      (qdf_list_node_t **)&cache_buf);
	while (cache_buf) {
		num_buff_elem = QDF_NBUF_CB_RX_NUM_ELEMENTS_IN_LIST(
								cache_buf->buf);
		bufqi->entries -= num_buff_elem;
		qdf_spin_unlock_bh(&bufqi->bufq_lock);
		if (drop) {
			bufqi->dropped = dp_rx_drop_nbuf_list(peer->vdev->pdev,
							      cache_buf->buf);
		} else {
			/* Flush the cached frames to OSIF DEV */
			status = data_rx(peer->vdev->osif_vdev, cache_buf->buf);
			if (status != QDF_STATUS_SUCCESS)
				bufqi->dropped = dp_rx_drop_nbuf_list(
							peer->vdev->pdev,
							cache_buf->buf);
		}
		qdf_mem_free(cache_buf);
		cache_buf = NULL;
		qdf_spin_lock_bh(&bufqi->bufq_lock);
		qdf_list_remove_front(&bufqi->cached_bufq,
				      (qdf_list_node_t **)&cache_buf);
	}
	qdf_spin_unlock_bh(&bufqi->bufq_lock);
	qdf_atomic_dec(&peer->flush_in_progress);
}

/**
 * dp_rx_enqueue_rx() - cache rx frames
 * @peer: peer
 * @rx_buf_list: cache buffer list
 *
 * Return: None
 */
static QDF_STATUS
dp_rx_enqueue_rx(struct dp_peer *peer, qdf_nbuf_t rx_buf_list)
{
	struct dp_rx_cached_buf *cache_buf;
	struct dp_peer_cached_bufq *bufqi = &peer->bufq_info;
	int num_buff_elem;

	QDF_TRACE_DEBUG_RL(QDF_MODULE_ID_TXRX, "bufq->curr %d bufq->drops %d",
			   bufqi->entries, bufqi->dropped);

	if (!peer->valid) {
		bufqi->dropped = dp_rx_drop_nbuf_list(peer->vdev->pdev,
						      rx_buf_list);
		return QDF_STATUS_E_INVAL;
	}

	qdf_spin_lock_bh(&bufqi->bufq_lock);
	if (bufqi->entries >= bufqi->thresh) {
		bufqi->dropped = dp_rx_drop_nbuf_list(peer->vdev->pdev,
						      rx_buf_list);
		qdf_spin_unlock_bh(&bufqi->bufq_lock);
		return QDF_STATUS_E_RESOURCES;
	}
	qdf_spin_unlock_bh(&bufqi->bufq_lock);

	num_buff_elem = QDF_NBUF_CB_RX_NUM_ELEMENTS_IN_LIST(rx_buf_list);

	cache_buf = qdf_mem_malloc_atomic(sizeof(*cache_buf));
	if (!cache_buf) {
		QDF_TRACE(QDF_MODULE_ID_TXRX, QDF_TRACE_LEVEL_ERROR,
			  "Failed to allocate buf to cache rx frames");
		bufqi->dropped = dp_rx_drop_nbuf_list(peer->vdev->pdev,
						      rx_buf_list);
		return QDF_STATUS_E_NOMEM;
	}

	cache_buf->buf = rx_buf_list;

	qdf_spin_lock_bh(&bufqi->bufq_lock);
	qdf_list_insert_back(&bufqi->cached_bufq,
			     &cache_buf->node);
	bufqi->entries += num_buff_elem;
	qdf_spin_unlock_bh(&bufqi->bufq_lock);

	return QDF_STATUS_SUCCESS;
}

static inline
bool dp_rx_is_peer_cache_bufq_supported(void)
{
	return true;
}
#else
static inline
bool dp_rx_is_peer_cache_bufq_supported(void)
{
	return false;
}

static inline QDF_STATUS
dp_rx_enqueue_rx(struct dp_peer *peer, qdf_nbuf_t rx_buf_list)
{
	return QDF_STATUS_SUCCESS;
}
#endif

static inline void dp_rx_deliver_to_stack(struct dp_vdev *vdev,
						struct dp_peer *peer,
						qdf_nbuf_t nbuf_head,
						qdf_nbuf_t nbuf_tail)
{
	struct cdp_tid_rx_stats *stats = NULL;
	uint8_t tid = 0;
	/*
	 * highly unlikely to have a vdev without a registered rx
	 * callback function. if so let us free the nbuf_list.
	 */
	if (qdf_unlikely(!vdev->osif_rx)) {
		qdf_nbuf_t nbuf;
		do {
			nbuf = nbuf_head;
			nbuf_head = nbuf_head->next;
			tid = qdf_nbuf_get_priority(nbuf);
			stats = &vdev->pdev->stats.tid_stats.tid_rx_stats[tid];
			stats->fail_cnt[INVALID_PEER_VDEV]++;
			stats->delivered_to_stack--;
			qdf_nbuf_free(nbuf);
		} while (nbuf_head);

		return;
	}

	if (qdf_unlikely(vdev->rx_decap_type == htt_cmn_pkt_type_raw) ||
			(vdev->rx_decap_type == htt_cmn_pkt_type_native_wifi)) {
		vdev->osif_rsim_rx_decap(vdev->osif_vdev, &nbuf_head,
				&nbuf_tail, (struct cdp_peer *) peer);
	}

	vdev->osif_rx(vdev->osif_vdev, nbuf_head);

}

/**
 * dp_rx_cksum_offload() - set the nbuf checksum as defined by hardware.
 * @nbuf: pointer to the first msdu of an amsdu.
 * @rx_tlv_hdr: pointer to the start of RX TLV headers.
 *
 * The ipsumed field of the skb is set based on whether HW validated the
 * IP/TCP/UDP checksum.
 *
 * Return: void
 */
static inline void dp_rx_cksum_offload(struct dp_pdev *pdev,
				       qdf_nbuf_t nbuf,
				       uint8_t *rx_tlv_hdr)
{
	qdf_nbuf_rx_cksum_t cksum = {0};
	bool ip_csum_err = hal_rx_attn_ip_cksum_fail_get(rx_tlv_hdr);
	bool tcp_udp_csum_er = hal_rx_attn_tcp_udp_cksum_fail_get(rx_tlv_hdr);

	if (qdf_likely(!ip_csum_err && !tcp_udp_csum_er)) {
		cksum.l4_result = QDF_NBUF_RX_CKSUM_TCP_UDP_UNNECESSARY;
		qdf_nbuf_set_rx_cksum(nbuf, &cksum);
	} else {
		DP_STATS_INCC(pdev, err.ip_csum_err, 1, ip_csum_err);
		DP_STATS_INCC(pdev, err.tcp_udp_csum_err, 1, tcp_udp_csum_er);
	}
}

/**
 * dp_rx_msdu_stats_update() - update per msdu stats.
 * @soc: core txrx main context
 * @nbuf: pointer to the first msdu of an amsdu.
 * @rx_tlv_hdr: pointer to the start of RX TLV headers.
 * @peer: pointer to the peer object.
 * @ring_id: reo dest ring number on which pkt is reaped.
 *
 * update all the per msdu stats for that nbuf.
 * Return: void
 */
static void dp_rx_msdu_stats_update(struct dp_soc *soc,
				    qdf_nbuf_t nbuf,
				    uint8_t *rx_tlv_hdr,
				    struct dp_peer *peer,
				    uint8_t ring_id)
{
	bool is_ampdu, is_not_amsdu;
	uint32_t sgi, mcs, tid, nss, bw, reception_type, pkt_type;
	struct dp_vdev *vdev = peer->vdev;
	qdf_ether_header_t *eh;
	uint16_t msdu_len = qdf_nbuf_len(nbuf);

	is_not_amsdu = qdf_nbuf_is_rx_chfrag_start(nbuf) &
			qdf_nbuf_is_rx_chfrag_end(nbuf);

	DP_STATS_INC_PKT(peer, rx.rcvd_reo[ring_id], 1, msdu_len);
	DP_STATS_INCC(peer, rx.non_amsdu_cnt, 1, is_not_amsdu);
	DP_STATS_INCC(peer, rx.amsdu_cnt, 1, !is_not_amsdu);

	if (qdf_unlikely(qdf_nbuf_is_da_mcbc(nbuf) &&
			 (vdev->rx_decap_type == htt_cmn_pkt_type_ethernet))) {
		eh = (qdf_ether_header_t *)qdf_nbuf_data(nbuf);
		DP_STATS_INC_PKT(peer, rx.multicast, 1, msdu_len);
		if (QDF_IS_ADDR_BROADCAST(eh->ether_dhost)) {
			DP_STATS_INC_PKT(peer, rx.bcast, 1, msdu_len);

		}
	}

	/*
	 * currently we can return from here as we have similar stats
	 * updated at per ppdu level instead of msdu level
	 */
	if (!soc->process_rx_status)
		return;

	is_ampdu = hal_rx_mpdu_info_ampdu_flag_get(rx_tlv_hdr);
	DP_STATS_INCC(peer, rx.ampdu_cnt, 1, is_ampdu);
	DP_STATS_INCC(peer, rx.non_ampdu_cnt, 1, !(is_ampdu));

	sgi = hal_rx_msdu_start_sgi_get(rx_tlv_hdr);
	mcs = hal_rx_msdu_start_rate_mcs_get(rx_tlv_hdr);
	tid = qdf_nbuf_get_tid_val(nbuf);
	bw = hal_rx_msdu_start_bw_get(rx_tlv_hdr);
	reception_type = hal_rx_msdu_start_reception_type_get(soc->hal_soc,
							      rx_tlv_hdr);
	nss = hal_rx_msdu_start_nss_get(soc->hal_soc, rx_tlv_hdr);
	pkt_type = hal_rx_msdu_start_get_pkt_type(rx_tlv_hdr);

	DP_STATS_INC(peer, rx.bw[bw], 1);
	DP_STATS_INC(peer, rx.nss[nss], 1);
	DP_STATS_INC(peer, rx.sgi_count[sgi], 1);
	DP_STATS_INCC(peer, rx.err.mic_err, 1,
		      hal_rx_mpdu_end_mic_err_get(rx_tlv_hdr));
	DP_STATS_INCC(peer, rx.err.decrypt_err, 1,
		      hal_rx_mpdu_end_decrypt_err_get(rx_tlv_hdr));

	DP_STATS_INC(peer, rx.wme_ac_type[TID_TO_WME_AC(tid)], 1);
	DP_STATS_INC(peer, rx.reception_type[reception_type], 1);

	DP_STATS_INCC(peer, rx.pkt_type[pkt_type].mcs_count[MAX_MCS - 1], 1,
		      ((mcs >= MAX_MCS_11A) && (pkt_type == DOT11_A)));
	DP_STATS_INCC(peer, rx.pkt_type[pkt_type].mcs_count[mcs], 1,
		      ((mcs <= MAX_MCS_11A) && (pkt_type == DOT11_A)));
	DP_STATS_INCC(peer, rx.pkt_type[pkt_type].mcs_count[MAX_MCS - 1], 1,
		      ((mcs >= MAX_MCS_11B) && (pkt_type == DOT11_B)));
	DP_STATS_INCC(peer, rx.pkt_type[pkt_type].mcs_count[mcs], 1,
		      ((mcs <= MAX_MCS_11B) && (pkt_type == DOT11_B)));
	DP_STATS_INCC(peer, rx.pkt_type[pkt_type].mcs_count[MAX_MCS - 1], 1,
		      ((mcs >= MAX_MCS_11A) && (pkt_type == DOT11_N)));
	DP_STATS_INCC(peer, rx.pkt_type[pkt_type].mcs_count[mcs], 1,
		      ((mcs <= MAX_MCS_11A) && (pkt_type == DOT11_N)));
	DP_STATS_INCC(peer, rx.pkt_type[pkt_type].mcs_count[MAX_MCS - 1], 1,
		      ((mcs >= MAX_MCS_11AC) && (pkt_type == DOT11_AC)));
	DP_STATS_INCC(peer, rx.pkt_type[pkt_type].mcs_count[mcs], 1,
		      ((mcs <= MAX_MCS_11AC) && (pkt_type == DOT11_AC)));
	DP_STATS_INCC(peer, rx.pkt_type[pkt_type].mcs_count[MAX_MCS - 1], 1,
		      ((mcs >= MAX_MCS) && (pkt_type == DOT11_AX)));
	DP_STATS_INCC(peer, rx.pkt_type[pkt_type].mcs_count[mcs], 1,
		      ((mcs < MAX_MCS) && (pkt_type == DOT11_AX)));

	if ((soc->process_rx_status) &&
	    hal_rx_attn_first_mpdu_get(rx_tlv_hdr)) {
#if defined(FEATURE_PERPKT_INFO) && WDI_EVENT_ENABLE
		if (!vdev->pdev)
			return;

		dp_wdi_event_handler(WDI_EVENT_UPDATE_DP_STATS, vdev->pdev->soc,
				     &peer->stats, peer->peer_ids[0],
				     UPDATE_PEER_STATS,
				     vdev->pdev->pdev_id);
#endif

	}
}

static inline bool is_sa_da_idx_valid(struct dp_soc *soc,
				      void *rx_tlv_hdr,
				      qdf_nbuf_t nbuf)
{
	if ((qdf_nbuf_is_sa_valid(nbuf) &&
	     (hal_rx_msdu_end_sa_idx_get(rx_tlv_hdr) >
		wlan_cfg_get_max_ast_idx(soc->wlan_cfg_ctx))) ||
	    (qdf_nbuf_is_da_valid(nbuf) &&
	     (hal_rx_msdu_end_da_idx_get(soc->hal_soc,
					 rx_tlv_hdr) >
	      wlan_cfg_get_max_ast_idx(soc->wlan_cfg_ctx))))
		return false;

	return true;
}

#ifdef WDS_VENDOR_EXTENSION
int dp_wds_rx_policy_check(uint8_t *rx_tlv_hdr,
			   struct dp_vdev *vdev,
			   struct dp_peer *peer)
{
	struct dp_peer *bss_peer;
	int fr_ds, to_ds, rx_3addr, rx_4addr;
	int rx_policy_ucast, rx_policy_mcast;
	int rx_mcast = hal_rx_msdu_end_da_is_mcbc_get(rx_tlv_hdr);

	if (vdev->opmode == wlan_op_mode_ap) {
		TAILQ_FOREACH(bss_peer, &vdev->peer_list, peer_list_elem) {
			if (bss_peer->bss_peer) {
				/* if wds policy check is not enabled on this vdev, accept all frames */
				if (!bss_peer->wds_ecm.wds_rx_filter) {
					return 1;
				}
				break;
			}
		}
		rx_policy_ucast = bss_peer->wds_ecm.wds_rx_ucast_4addr;
		rx_policy_mcast = bss_peer->wds_ecm.wds_rx_mcast_4addr;
	} else {             /* sta mode */
		if (!peer->wds_ecm.wds_rx_filter) {
			return 1;
		}
		rx_policy_ucast = peer->wds_ecm.wds_rx_ucast_4addr;
		rx_policy_mcast = peer->wds_ecm.wds_rx_mcast_4addr;
	}

	/* ------------------------------------------------
	 *                       self
	 * peer-             rx  rx-
	 * wds  ucast mcast dir policy accept note
	 * ------------------------------------------------
	 * 1     1     0     11  x1     1      AP configured to accept ds-to-ds Rx ucast from wds peers, constraint met; so, accept
	 * 1     1     0     01  x1     0      AP configured to accept ds-to-ds Rx ucast from wds peers, constraint not met; so, drop
	 * 1     1     0     10  x1     0      AP configured to accept ds-to-ds Rx ucast from wds peers, constraint not met; so, drop
	 * 1     1     0     00  x1     0      bad frame, won't see it
	 * 1     0     1     11  1x     1      AP configured to accept ds-to-ds Rx mcast from wds peers, constraint met; so, accept
	 * 1     0     1     01  1x     0      AP configured to accept ds-to-ds Rx mcast from wds peers, constraint not met; so, drop
	 * 1     0     1     10  1x     0      AP configured to accept ds-to-ds Rx mcast from wds peers, constraint not met; so, drop
	 * 1     0     1     00  1x     0      bad frame, won't see it
	 * 1     1     0     11  x0     0      AP configured to accept from-ds Rx ucast from wds peers, constraint not met; so, drop
	 * 1     1     0     01  x0     0      AP configured to accept from-ds Rx ucast from wds peers, constraint not met; so, drop
	 * 1     1     0     10  x0     1      AP configured to accept from-ds Rx ucast from wds peers, constraint met; so, accept
	 * 1     1     0     00  x0     0      bad frame, won't see it
	 * 1     0     1     11  0x     0      AP configured to accept from-ds Rx mcast from wds peers, constraint not met; so, drop
	 * 1     0     1     01  0x     0      AP configured to accept from-ds Rx mcast from wds peers, constraint not met; so, drop
	 * 1     0     1     10  0x     1      AP configured to accept from-ds Rx mcast from wds peers, constraint met; so, accept
	 * 1     0     1     00  0x     0      bad frame, won't see it
	 *
	 * 0     x     x     11  xx     0      we only accept td-ds Rx frames from non-wds peers in mode.
	 * 0     x     x     01  xx     1
	 * 0     x     x     10  xx     0
	 * 0     x     x     00  xx     0      bad frame, won't see it
	 * ------------------------------------------------
	 */

	fr_ds = hal_rx_mpdu_get_fr_ds(rx_tlv_hdr);
	to_ds = hal_rx_mpdu_get_to_ds(rx_tlv_hdr);
	rx_3addr = fr_ds ^ to_ds;
	rx_4addr = fr_ds & to_ds;

	if (vdev->opmode == wlan_op_mode_ap) {
		if ((!peer->wds_enabled && rx_3addr && to_ds) ||
				(peer->wds_enabled && !rx_mcast && (rx_4addr == rx_policy_ucast)) ||
				(peer->wds_enabled && rx_mcast && (rx_4addr == rx_policy_mcast))) {
			return 1;
		}
	} else {           /* sta mode */
		if ((!rx_mcast && (rx_4addr == rx_policy_ucast)) ||
				(rx_mcast && (rx_4addr == rx_policy_mcast))) {
			return 1;
		}
	}
	return 0;
}
#else
int dp_wds_rx_policy_check(uint8_t *rx_tlv_hdr,
			   struct dp_vdev *vdev,
			   struct dp_peer *peer)
{
	return 1;
}
#endif

#ifdef RX_DESC_DEBUG_CHECK
/**
 * dp_rx_desc_nbuf_sanity_check - Add sanity check to catch REO rx_desc paddr
 *				  corruption
 *
 * @ring_desc: REO ring descriptor
 * @rx_desc: Rx descriptor
 *
 * Return: NONE
 */
static inline void dp_rx_desc_nbuf_sanity_check(void *ring_desc,
					   struct dp_rx_desc *rx_desc)
{
	struct hal_buf_info hbi;

	hal_rx_reo_buf_paddr_get(ring_desc, &hbi);
	/* Sanity check for possible buffer paddr corruption */
	qdf_assert_always((&hbi)->paddr ==
			  qdf_nbuf_get_frag_paddr(rx_desc->nbuf, 0));
}

/**
 * dp_rx_is_msdu_done_set - Add check to catch msdu_done DMA
 * failures
 *
 * @soc: core txrx main context
 * @rx_tlv_hdr: Rx TLV header start
 *
 * Return: NONE
 */
static inline bool dp_rx_is_msdu_done_set(struct dp_soc *soc,
					  uint8_t *rx_tlv_hdr)
{
	if (qdf_unlikely(!hal_rx_attn_msdu_done_get(rx_tlv_hdr))) {
		dp_err("MSDU DONE failure");
		DP_STATS_INC(soc, rx.err.msdu_done_fail, 1);
		hal_rx_dump_pkt_tlvs(hal_soc, rx_tlv_hdr, QDF_TRACE_LEVEL_INFO);
		return false;
	}
	return true;
}

#else
static inline void dp_rx_desc_nbuf_sanity_check(void *ring_desc,
					   struct dp_rx_desc *rx_desc)
{
}

static inline bool dp_rx_is_msdu_done_set(struct dp_soc *soc,
					  uint8_t *rx_tlv_hdr)
{
	return true;
}
#endif

/**
 * dp_rx_process() - Brain of the Rx processing functionality
 *		     Called from the bottom half (tasklet/NET_RX_SOFTIRQ)
 * @soc: core txrx main context
 * @hal_ring: opaque pointer to the HAL Rx Ring, which will be serviced
 * @reo_ring_num: ring number (0, 1, 2 or 3) of the reo ring.
 * @quota: No. of units (packets) that can be serviced in one shot.
 *
 * This function implements the core of Rx functionality. This is
 * expected to handle only non-error frames.
 *
 * Return: uint32_t: No. of elements processed
 */
uint32_t dp_rx_process(struct dp_intr *int_ctx, void *hal_ring,
		       uint8_t reo_ring_num, uint32_t quota)
{
	void *hal_soc;
	void *ring_desc;
	struct dp_rx_desc *rx_desc = NULL;
	qdf_nbuf_t nbuf, next;
	union dp_rx_desc_list_elem_t *head[MAX_PDEV_CNT] = { NULL };
	union dp_rx_desc_list_elem_t *tail[MAX_PDEV_CNT] = { NULL };
	uint32_t rx_bufs_used = 0, rx_buf_cookie;
	uint32_t l2_hdr_offset = 0;
	uint16_t msdu_len = 0;
	uint16_t peer_id;
	struct dp_peer *peer = NULL;
	struct dp_vdev *vdev = NULL;
	uint32_t pkt_len = 0;
	struct hal_rx_mpdu_desc_info mpdu_desc_info = { 0 };
	struct hal_rx_msdu_desc_info msdu_desc_info = { 0 };
	enum hal_reo_error_status error;
	uint32_t peer_mdata;
	uint8_t *rx_tlv_hdr;
	uint32_t rx_bufs_reaped[MAX_PDEV_CNT] = { 0 };
	uint8_t mac_id = 0;
	struct dp_pdev *pdev;
	struct dp_pdev *rx_pdev;
	struct dp_srng *dp_rxdma_srng;
	struct rx_desc_pool *rx_desc_pool;
	struct dp_soc *soc = int_ctx->soc;
	uint8_t ring_id = 0;
	uint8_t core_id = 0;
	qdf_nbuf_t nbuf_head = NULL;
	qdf_nbuf_t nbuf_tail = NULL;
	qdf_nbuf_t deliver_list_head = NULL;
	qdf_nbuf_t deliver_list_tail = NULL;
	int32_t tid = 0;
	struct cdp_tid_rx_stats *tid_stats;
	bool is_prev_msdu_last = true;
	uint32_t num_entries_avail = 0;

	DP_HIST_INIT();
	/* Debug -- Remove later */
	qdf_assert(soc && hal_ring);

	hal_soc = soc->hal_soc;

	/* Debug -- Remove later */
	qdf_assert(hal_soc);

	hif_pm_runtime_mark_last_busy(soc->osdev->dev);

	if (qdf_unlikely(hal_srng_access_start(hal_soc, hal_ring))) {

		/*
		 * Need API to convert from hal_ring pointer to
		 * Ring Type / Ring Id combo
		 */
		DP_STATS_INC(soc, rx.err.hal_ring_access_fail, 1);
		QDF_TRACE(QDF_MODULE_ID_TXRX, QDF_TRACE_LEVEL_ERROR,
			FL("HAL RING Access Failed -- %pK"), hal_ring);
		hal_srng_access_end(hal_soc, hal_ring);
		goto done;
	}

	/*
	 * start reaping the buffers from reo ring and queue
	 * them in per vdev queue.
	 * Process the received pkts in a different per vdev loop.
	 */
	while (qdf_likely(quota &&
			  (ring_desc = hal_srng_dst_peek(hal_soc, hal_ring)))) {

		error = HAL_RX_ERROR_STATUS_GET(ring_desc);
		ring_id = hal_srng_ring_id_get(hal_ring);

		if (qdf_unlikely(error == HAL_REO_ERROR_DETECTED)) {
			QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			FL("HAL RING 0x%pK:error %d"), hal_ring, error);
			DP_STATS_INC(soc, rx.err.hal_reo_error[ring_id], 1);
			/* Don't know how to deal with this -- assert */
			qdf_assert(0);
		}

		rx_buf_cookie = HAL_RX_REO_BUF_COOKIE_GET(ring_desc);

		rx_desc = dp_rx_cookie_2_va_rxdma_buf(soc, rx_buf_cookie);
		qdf_assert(rx_desc);

		dp_rx_desc_nbuf_sanity_check(ring_desc, rx_desc);
		/*
		 * this is a unlikely scenario where the host is reaping
		 * a descriptor which it already reaped just a while ago
		 * but is yet to replenish it back to HW.
		 * In this case host will dump the last 128 descriptors
		 * including the software descriptor rx_desc and assert.
		 */
		if (qdf_unlikely(!rx_desc->in_use)) {
			DP_STATS_INC(soc, rx.err.hal_reo_dest_dup, 1);
			dp_err("Reaping rx_desc not in use!");
			dp_rx_dump_info_and_assert(soc, hal_ring,
						   ring_desc, rx_desc);
		}

		if (qdf_unlikely(!dp_rx_desc_check_magic(rx_desc))) {
			dp_err("Invalid rx_desc cookie=%d", rx_buf_cookie);
			DP_STATS_INC(soc, rx.err.rx_desc_invalid_magic, 1);
			dp_rx_dump_info_and_assert(soc, hal_ring,
						   ring_desc, rx_desc);
		}

		qdf_assert(rx_desc);

		/* TODO */
		/*
		 * Need a separate API for unmapping based on
		 * phyiscal address
		 */
		qdf_nbuf_unmap_single(soc->osdev, rx_desc->nbuf,
					QDF_DMA_FROM_DEVICE);
		rx_desc->unmapped = 1;

		core_id = smp_processor_id();
		DP_STATS_INC(soc, rx.ring_packets[core_id][ring_id], 1);

		/* Get MPDU DESC info */
		hal_rx_mpdu_desc_info_get(ring_desc, &mpdu_desc_info);

		/* Get MSDU DESC info */
		hal_rx_msdu_desc_info_get(ring_desc, &msdu_desc_info);

		if (qdf_unlikely(mpdu_desc_info.mpdu_flags &
				HAL_MPDU_F_RAW_AMPDU)) {
			/* previous msdu has end bit set, so current one is
			 * the new MPDU
			 */
			if (is_prev_msdu_last) {
				is_prev_msdu_last = false;
				/* Get number of entries available in HW ring */
				num_entries_avail =
				hal_srng_dst_num_valid(hal_soc, hal_ring, 1);

				/* For new MPDU check if we can read complete
				 * MPDU by comparing the number of buffers
				 * available and number of buffers needed to
				 * reap this MPDU
				 */
				if (((msdu_desc_info.msdu_len /
				     (RX_BUFFER_SIZE - RX_PKT_TLVS_LEN) + 1)) >
				     num_entries_avail)
					break;
			} else {
				if (msdu_desc_info.msdu_flags &
				    HAL_MSDU_F_LAST_MSDU_IN_MPDU)
					is_prev_msdu_last = true;
			}
			qdf_nbuf_set_raw_frame(rx_desc->nbuf, 1);
		}

		/* Pop out the descriptor*/
		hal_srng_dst_get_next(hal_soc, hal_ring);

		rx_bufs_reaped[rx_desc->pool_id]++;
		peer_mdata = mpdu_desc_info.peer_meta_data;
		QDF_NBUF_CB_RX_PEER_ID(rx_desc->nbuf) =
			DP_PEER_METADATA_PEER_ID_GET(peer_mdata);

		/*
		 * save msdu flags first, last and continuation msdu in
		 * nbuf->cb, also save mcbc, is_da_valid, is_sa_valid and
		 * length to nbuf->cb. This ensures the info required for
		 * per pkt processing is always in the same cache line.
		 * This helps in improving throughput for smaller pkt
		 * sizes.
		 */
		if (msdu_desc_info.msdu_flags & HAL_MSDU_F_FIRST_MSDU_IN_MPDU)
			qdf_nbuf_set_rx_chfrag_start(rx_desc->nbuf, 1);

		if (msdu_desc_info.msdu_flags & HAL_MSDU_F_MSDU_CONTINUATION)
			qdf_nbuf_set_rx_chfrag_cont(rx_desc->nbuf, 1);

		if (msdu_desc_info.msdu_flags & HAL_MSDU_F_LAST_MSDU_IN_MPDU)
			qdf_nbuf_set_rx_chfrag_end(rx_desc->nbuf, 1);

		if (msdu_desc_info.msdu_flags & HAL_MSDU_F_DA_IS_MCBC)
			qdf_nbuf_set_da_mcbc(rx_desc->nbuf, 1);

		if (msdu_desc_info.msdu_flags & HAL_MSDU_F_DA_IS_VALID)
			qdf_nbuf_set_da_valid(rx_desc->nbuf, 1);

		if (msdu_desc_info.msdu_flags & HAL_MSDU_F_SA_IS_VALID)
			qdf_nbuf_set_sa_valid(rx_desc->nbuf, 1);

		QDF_NBUF_CB_RX_PKT_LEN(rx_desc->nbuf) =	msdu_desc_info.msdu_len;

		qdf_nbuf_set_tid_val(rx_desc->nbuf,
				     HAL_RX_REO_QUEUE_NUMBER_GET(ring_desc));

		QDF_NBUF_CB_RX_CTX_ID(rx_desc->nbuf) = reo_ring_num;

		DP_RX_LIST_APPEND(nbuf_head, nbuf_tail, rx_desc->nbuf);

		/*
		 * if continuation bit is set then we have MSDU spread
		 * across multiple buffers, let us not decrement quota
		 * till we reap all buffers of that MSDU.
		 */
		if (qdf_likely(!qdf_nbuf_is_rx_chfrag_cont(rx_desc->nbuf)))
			quota -= 1;

		dp_rx_add_to_free_desc_list(&head[rx_desc->pool_id],
						&tail[rx_desc->pool_id],
						rx_desc);
	}
done:
	hal_srng_access_end(hal_soc, hal_ring);

	if (nbuf_tail)
		QDF_NBUF_CB_RX_FLUSH_IND(nbuf_tail) = 1;

	for (mac_id = 0; mac_id < MAX_PDEV_CNT; mac_id++) {
		/*
		 * continue with next mac_id if no pkts were reaped
		 * from that pool
		 */
		if (!rx_bufs_reaped[mac_id])
			continue;

		pdev = soc->pdev_list[mac_id];
		dp_rxdma_srng = &pdev->rx_refill_buf_ring;
		rx_desc_pool = &soc->rx_desc_buf[mac_id];

		dp_rx_buffers_replenish(soc, mac_id, dp_rxdma_srng,
					rx_desc_pool, rx_bufs_reaped[mac_id],
					&head[mac_id], &tail[mac_id]);
	}

	/* Peer can be NULL is case of LFR */
	if (qdf_likely(peer))
		vdev = NULL;

	/*
	 * BIG loop where each nbuf is dequeued from global queue,
	 * processed and queued back on a per vdev basis. These nbufs
	 * are sent to stack as and when we run out of nbufs
	 * or a new nbuf dequeued from global queue has a different
	 * vdev when compared to previous nbuf.
	 */
	nbuf = nbuf_head;
	while (nbuf) {
		next = nbuf->next;
		rx_tlv_hdr = qdf_nbuf_data(nbuf);
		/* Get TID from struct cb->tid_val, save to tid */
		if (qdf_nbuf_is_rx_chfrag_start(nbuf))
			tid = qdf_nbuf_get_tid_val(nbuf);

		/*
		 * Check if DMA completed -- msdu_done is the last bit
		 * to be written
		 */
		rx_pdev = soc->pdev_list[rx_desc->pool_id];
		DP_RX_TID_SAVE(nbuf, tid);
		if (qdf_unlikely(rx_pdev->delay_stats_flag))
			qdf_nbuf_set_timestamp(nbuf);

		tid_stats = &rx_pdev->stats.tid_stats.tid_rx_stats[tid];
		if (qdf_unlikely(!dp_rx_is_msdu_done_set(soc, rx_tlv_hdr))) {
			tid_stats->fail_cnt[MSDU_DONE_FAILURE]++;
			qdf_nbuf_free(nbuf);
			qdf_assert(0);
			nbuf = next;
			continue;
		}

		tid_stats->msdu_cnt++;
		if (qdf_unlikely(qdf_nbuf_is_da_mcbc(nbuf))) {
			tid_stats->mcast_msdu_cnt++;
			if (qdf_nbuf_is_bcast_pkt(nbuf))
				tid_stats->bcast_msdu_cnt++;
		}

		peer_mdata =  QDF_NBUF_CB_RX_PEER_ID(nbuf);
		peer_id = DP_PEER_METADATA_PEER_ID_GET(peer_mdata);
		peer = dp_peer_find_by_id(soc, peer_id);

		if (peer) {
			QDF_NBUF_CB_DP_TRACE_PRINT(nbuf) = false;
			qdf_dp_trace_set_track(nbuf, QDF_RX);
			QDF_NBUF_CB_RX_DP_TRACE(nbuf) = 1;
			QDF_NBUF_CB_RX_PACKET_TRACK(nbuf) =
				QDF_NBUF_RX_PKT_DATA_TRACK;
		}

		rx_bufs_used++;

		if (deliver_list_head && peer && (vdev != peer->vdev)) {
			dp_rx_deliver_to_stack(vdev, peer, deliver_list_head,
					deliver_list_tail);
			deliver_list_head = NULL;
			deliver_list_tail = NULL;
		}

		if (qdf_likely(peer)) {
			vdev = peer->vdev;
		} else {
			DP_STATS_INC_PKT(soc, rx.err.rx_invalid_peer, 1,
					 QDF_NBUF_CB_RX_PKT_LEN(nbuf));
			tid_stats->fail_cnt[INVALID_PEER_VDEV]++;
			qdf_nbuf_free(nbuf);
			nbuf = next;
			continue;
		}

		if (qdf_unlikely(!vdev)) {
			tid_stats->fail_cnt[INVALID_PEER_VDEV]++;
			qdf_nbuf_free(nbuf);
			nbuf = next;
			DP_STATS_INC(soc, rx.err.invalid_vdev, 1);
			dp_peer_unref_del_find_by_id(peer);
			continue;
		}

		DP_HIST_PACKET_COUNT_INC(vdev->pdev->pdev_id);
		/*
		 * First IF condition:
		 * 802.11 Fragmented pkts are reinjected to REO
		 * HW block as SG pkts and for these pkts we only
		 * need to pull the RX TLVS header length.
		 * Second IF condition:
		 * The below condition happens when an MSDU is spread
		 * across multiple buffers. This can happen in two cases
		 * 1. The nbuf size is smaller then the received msdu.
		 *    ex: we have set the nbuf size to 2048 during
		 *        nbuf_alloc. but we received an msdu which is
		 *        2304 bytes in size then this msdu is spread
		 *        across 2 nbufs.
		 *
		 * 2. AMSDUs when RAW mode is enabled.
		 *    ex: 1st MSDU is in 1st nbuf and 2nd MSDU is spread
		 *        across 1st nbuf and 2nd nbuf and last MSDU is
		 *        spread across 2nd nbuf and 3rd nbuf.
		 *
		 * for these scenarios let us create a skb frag_list and
		 * append these buffers till the last MSDU of the AMSDU
		 * Third condition:
		 * This is the most likely case, we receive 802.3 pkts
		 * decapsulated by HW, here we need to set the pkt length.
		 */
		if (qdf_unlikely(qdf_nbuf_is_frag(nbuf))) {
			bool is_mcbc, is_sa_vld, is_da_vld;

			is_mcbc = hal_rx_msdu_end_da_is_mcbc_get(rx_tlv_hdr);
			is_sa_vld = hal_rx_msdu_end_sa_is_valid_get(rx_tlv_hdr);
			is_da_vld = hal_rx_msdu_end_da_is_valid_get(rx_tlv_hdr);

			qdf_nbuf_set_da_mcbc(nbuf, is_mcbc);
			qdf_nbuf_set_da_valid(nbuf, is_da_vld);
			qdf_nbuf_set_sa_valid(nbuf, is_sa_vld);

			qdf_nbuf_pull_head(nbuf, RX_PKT_TLVS_LEN);
		} else if (qdf_nbuf_is_raw_frame(nbuf)) {
			msdu_len = QDF_NBUF_CB_RX_PKT_LEN(nbuf);
			nbuf = dp_rx_sg_create(nbuf, rx_tlv_hdr);

			DP_STATS_INC(vdev->pdev, rx_raw_pkts, 1);
			DP_STATS_INC_PKT(peer, rx.raw, 1, msdu_len);

			next = nbuf->next;
		} else {
			l2_hdr_offset =
				hal_rx_msdu_end_l3_hdr_padding_get(rx_tlv_hdr);

			msdu_len = hal_rx_msdu_start_msdu_len_get(rx_tlv_hdr);
			pkt_len = msdu_len + l2_hdr_offset + RX_PKT_TLVS_LEN;

			qdf_nbuf_set_pktlen(nbuf, pkt_len);
			qdf_nbuf_pull_head(nbuf,
					   RX_PKT_TLVS_LEN +
					   l2_hdr_offset);
		}

		if (!dp_wds_rx_policy_check(rx_tlv_hdr, vdev, peer)) {
			QDF_TRACE(QDF_MODULE_ID_DP,
					QDF_TRACE_LEVEL_ERROR,
					FL("Policy Check Drop pkt"));
			tid_stats->fail_cnt[POLICY_CHECK_DROP]++;
			/* Drop & free packet */
			qdf_nbuf_free(nbuf);
			/* Statistics */
			nbuf = next;
			dp_peer_unref_del_find_by_id(peer);
			continue;
		}

		if (qdf_unlikely(peer && peer->bss_peer)) {
			QDF_TRACE(QDF_MODULE_ID_DP,
				QDF_TRACE_LEVEL_ERROR,
				FL("received pkt with same src MAC"));
			tid_stats->fail_cnt[MEC_DROP]++;
			DP_STATS_INC_PKT(peer, rx.mec_drop, 1, msdu_len);

			/* Drop & free packet */
			qdf_nbuf_free(nbuf);
			/* Statistics */
			nbuf = next;
			dp_peer_unref_del_find_by_id(peer);
			continue;
		}

		if (qdf_unlikely(peer && (peer->nawds_enabled) &&
				 (qdf_nbuf_is_da_mcbc(nbuf)) &&
				 (hal_rx_get_mpdu_mac_ad4_valid(rx_tlv_hdr) ==
				  false))) {
			tid_stats->fail_cnt[NAWDS_MCAST_DROP]++;
			DP_STATS_INC(peer, rx.nawds_mcast_drop, 1);
			qdf_nbuf_free(nbuf);
			nbuf = next;
			dp_peer_unref_del_find_by_id(peer);
			continue;
		}

		if (soc->process_rx_status)
			dp_rx_cksum_offload(vdev->pdev, nbuf, rx_tlv_hdr);

		/* Update the protocol tag in SKB based on CCE metadata */
		dp_rx_update_protocol_tag(soc, vdev, nbuf, rx_tlv_hdr,
					  reo_ring_num, false, true);

		dp_rx_msdu_stats_update(soc, nbuf, rx_tlv_hdr, peer, ring_id);

		if (qdf_unlikely(vdev->mesh_vdev)) {
			if (dp_rx_filter_mesh_packets(vdev, nbuf, rx_tlv_hdr)
					== QDF_STATUS_SUCCESS) {
				QDF_TRACE(QDF_MODULE_ID_DP,
						QDF_TRACE_LEVEL_INFO_MED,
						FL("mesh pkt filtered"));
				tid_stats->fail_cnt[MESH_FILTER_DROP]++;
				DP_STATS_INC(vdev->pdev, dropped.mesh_filter,
					     1);

				qdf_nbuf_free(nbuf);
				nbuf = next;
				dp_peer_unref_del_find_by_id(peer);
				continue;
			}
			dp_rx_fill_mesh_stats(vdev, nbuf, rx_tlv_hdr, peer);
		}

#ifdef QCA_WIFI_NAPIER_EMULATION_DBG /* Debug code, remove later */
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_ERROR,
			"p_id %d msdu_len %d hdr_off %d",
			peer_id, msdu_len, l2_hdr_offset);

		print_hex_dump(KERN_ERR,
			       "\t Pkt Data:", DUMP_PREFIX_NONE, 32, 4,
				qdf_nbuf_data(nbuf), 128, false);
#endif /* NAPIER_EMULATION */

		if (qdf_likely(vdev->rx_decap_type ==
			       htt_cmn_pkt_type_ethernet) &&
		    qdf_likely(!vdev->mesh_vdev)) {
			/* WDS Destination Address Learning */
			dp_rx_da_learn(soc, rx_tlv_hdr, peer, nbuf);

			/* Due to HW issue, sometimes we see that the sa_idx
			 * and da_idx are invalid with sa_valid and da_valid
			 * bits set
			 *
			 * in this case we also see that value of
			 * sa_sw_peer_id is set as 0
			 *
			 * Drop the packet if sa_idx and da_idx OOB or
			 * sa_sw_peerid is 0
			 */
			if (!is_sa_da_idx_valid(soc, rx_tlv_hdr, nbuf)) {
				qdf_nbuf_free(nbuf);
				nbuf = next;
				DP_STATS_INC(soc, rx.err.invalid_sa_da_idx, 1);
				continue;
			}
			/* WDS Source Port Learning */
			if (qdf_likely(vdev->wds_enabled))
				dp_rx_wds_srcport_learn(soc, rx_tlv_hdr,
							peer, nbuf);

			/* Intrabss-fwd */
			if (dp_rx_check_ap_bridge(vdev))
				if (dp_rx_intrabss_fwd(soc,
							peer,
							rx_tlv_hdr,
							nbuf)) {
					nbuf = next;
					dp_peer_unref_del_find_by_id(peer);
					tid_stats->intrabss_cnt++;
					continue; /* Get next desc */
				}
		}

		dp_rx_fill_gro_info(soc, rx_tlv_hdr, nbuf);
		qdf_nbuf_cb_update_peer_local_id(nbuf, peer->local_id);

		DP_RX_LIST_APPEND(deliver_list_head,
				  deliver_list_tail,
				  nbuf);
		DP_STATS_INC_PKT(peer, rx.to_stack, 1,
				 QDF_NBUF_CB_RX_PKT_LEN(nbuf));

		tid_stats->delivered_to_stack++;
		nbuf = next;
		dp_peer_unref_del_find_by_id(peer);
	}

	/* Update histogram statistics by looping through pdev's */
	DP_RX_HIST_STATS_PER_PDEV();

	if (deliver_list_head)
		dp_rx_deliver_to_stack(vdev, peer, deliver_list_head,
				       deliver_list_tail);

	return rx_bufs_used; /* Assume no scale factor for now */
}

/**
 * dp_rx_detach() - detach dp rx
 * @pdev: core txrx pdev context
 *
 * This function will detach DP RX into main device context
 * will free DP Rx resources.
 *
 * Return: void
 */
void
dp_rx_pdev_detach(struct dp_pdev *pdev)
{
	uint8_t pdev_id = pdev->pdev_id;
	struct dp_soc *soc = pdev->soc;
	struct rx_desc_pool *rx_desc_pool;

	rx_desc_pool = &soc->rx_desc_buf[pdev_id];

	if (rx_desc_pool->pool_size != 0) {
		if (!dp_is_soc_reinit(soc))
			dp_rx_desc_pool_free(soc, pdev_id, rx_desc_pool);
		else
			dp_rx_desc_nbuf_pool_free(soc, rx_desc_pool);
	}

	return;
}

/**
 * dp_rx_attach() - attach DP RX
 * @pdev: core txrx pdev context
 *
 * This function will attach a DP RX instance into the main
 * device (SOC) context. Will allocate dp rx resource and
 * initialize resources.
 *
 * Return: QDF_STATUS_SUCCESS: success
 *         QDF_STATUS_E_RESOURCES: Error return
 */
QDF_STATUS
dp_rx_pdev_attach(struct dp_pdev *pdev)
{
	uint8_t pdev_id = pdev->pdev_id;
	struct dp_soc *soc = pdev->soc;
	uint32_t rxdma_entries;
	union dp_rx_desc_list_elem_t *desc_list = NULL;
	union dp_rx_desc_list_elem_t *tail = NULL;
	struct dp_srng *dp_rxdma_srng;
	struct rx_desc_pool *rx_desc_pool;

	if (wlan_cfg_get_dp_pdev_nss_enabled(pdev->wlan_cfg_ctx)) {
		QDF_TRACE(QDF_MODULE_ID_DP, QDF_TRACE_LEVEL_INFO,
			  "nss-wifi<4> skip Rx refil %d", pdev_id);
		return QDF_STATUS_SUCCESS;
	}

	pdev = soc->pdev_list[pdev_id];
	dp_rxdma_srng = &pdev->rx_refill_buf_ring;
	rxdma_entries = dp_rxdma_srng->num_entries;

	soc->process_rx_status = CONFIG_PROCESS_RX_STATUS;

	rx_desc_pool = &soc->rx_desc_buf[pdev_id];
	dp_rx_desc_pool_alloc(soc, pdev_id,
			      DP_RX_DESC_ALLOC_MULTIPLIER * rxdma_entries,
			      rx_desc_pool);

	rx_desc_pool->owner = DP_WBM2SW_RBM;
	/* For Rx buffers, WBM release ring is SW RING 3,for all pdev's */

	dp_rx_buffers_replenish(soc, pdev_id, dp_rxdma_srng, rx_desc_pool,
				0, &desc_list, &tail);

	return QDF_STATUS_SUCCESS;
}

/*
 * dp_rx_nbuf_prepare() - prepare RX nbuf
 * @soc: core txrx main context
 * @pdev: core txrx pdev context
 *
 * This function alloc & map nbuf for RX dma usage, retry it if failed
 * until retry times reaches max threshold or succeeded.
 *
 * Return: qdf_nbuf_t pointer if succeeded, NULL if failed.
 */
qdf_nbuf_t
dp_rx_nbuf_prepare(struct dp_soc *soc, struct dp_pdev *pdev)
{
	uint8_t *buf;
	int32_t nbuf_retry_count;
	QDF_STATUS ret;
	qdf_nbuf_t nbuf = NULL;

	for (nbuf_retry_count = 0; nbuf_retry_count <
		QDF_NBUF_ALLOC_MAP_RETRY_THRESHOLD;
			nbuf_retry_count++) {
		/* Allocate a new skb */
		nbuf = qdf_nbuf_alloc(soc->osdev,
					RX_BUFFER_SIZE,
					RX_BUFFER_RESERVATION,
					RX_BUFFER_ALIGNMENT,
					FALSE);

		if (!nbuf) {
			DP_STATS_INC(pdev,
				replenish.nbuf_alloc_fail, 1);
			continue;
		}

		buf = qdf_nbuf_data(nbuf);

		memset(buf, 0, RX_BUFFER_SIZE);

		ret = qdf_nbuf_map_single(soc->osdev, nbuf,
				    QDF_DMA_FROM_DEVICE);

		/* nbuf map failed */
		if (qdf_unlikely(QDF_IS_STATUS_ERROR(ret))) {
			qdf_nbuf_free(nbuf);
			DP_STATS_INC(pdev, replenish.map_err, 1);
			continue;
		}
		/* qdf_nbuf alloc and map succeeded */
		break;
	}

	/* qdf_nbuf still alloc or map failed */
	if (qdf_unlikely(nbuf_retry_count >=
			QDF_NBUF_ALLOC_MAP_RETRY_THRESHOLD))
		return NULL;

	return nbuf;
}
