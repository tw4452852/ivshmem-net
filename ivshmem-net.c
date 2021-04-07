/*
 * Copyright 2021 Wei Tan <wei.tan@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#define DRV_NAME "ivshmem-net"
#define pr_fmt(fmt) DRV_NAME ": " fmt

#include <linux/bitops.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/pci.h>
#include <linux/rtnetlink.h>
#include <linux/virtio_ring.h>

#define PCI_DEVICE_ID_IVSHMEM 0x1110

#define IVSHMEM_INTX_ENABLE 0x1

#define IVSHM_NET_STATE_UNKNOWN (~0)
#define IVSHM_NET_STATE_RESET 0
#define IVSHM_NET_STATE_INIT 1
#define IVSHM_NET_STATE_READY 2
#define IVSHM_NET_STATE_RUN 3

#define IVSHM_NET_FLAG_RUN 0

#define IVSHM_NET_MTU_DEF 16384

#define IVSHM_NET_FRAME_SIZE(s) ALIGN(18 + (s), SMP_CACHE_BYTES)

#define IVSHM_NET_VQ_ALIGN 64

#define IVSHM_NET_INVALID_ID (~0)

static uint my_id = IVSHM_NET_INVALID_ID;
module_param(my_id, uint, 0660);
MODULE_PARM_DESC(my_id, "My ID, must be specified");

static uint peer_id = IVSHM_NET_INVALID_ID;
module_param(peer_id, uint, 0660);
MODULE_PARM_DESC(peer_id, "peer's ID, must be specified");

static uint capacity = 4;
module_param(capacity, uint, 0660);
MODULE_PARM_DESC(capacity, "capacity of the id, default to 4");

static bool debug = false;
module_param(debug, bool, 0600);
MODULE_PARM_DESC(debug, "enable debug, default to false");

#define MY_STATE (u32 *)(in->shm + my_id * (in->shmlen / capacity))
#define PEER_STATE (u32 *)(in->shm + peer_id * (in->shmlen / capacity))
#define MY_QUEUE (MY_STATE + 1)
#define PEER_QUEUE (PEER_STATE + 1)
#define QUEUE_SIZE (in->shmlen / capacity - 4)

#define _dbg(fmt, ...)                                                         \
	({                                                                     \
		if (debug) {                                                   \
			trace_printk(fmt, ##__VA_ARGS__);                      \
			pr_info(fmt, ##__VA_ARGS__);                           \
		}                                                              \
	})

struct ivshmem_regs {
	u32 intxctrl;
	u32 istat;
	u32 ivpos;
	u32 doorbell;
};

struct ivshm_net_queue {
	struct vring vr;
	u32 free_head;
	u32 num_free;
	u32 num_added;
	u16 last_avail_idx;
	u16 last_used_idx;

	void *data;
	void *end;
	u32 size;
	u32 head;
	u32 tail;
};

struct ivshm_net_stats {
	u32 interrupts;
	u32 tx_packets;
	u32 tx_notify;
	u32 tx_pause;
	u32 rx_packets;
	u32 rx_notify;
	u32 napi_poll;
	u32 napi_complete;
	u32 napi_poll_n[10];
};

struct ivshm_net {
	struct ivshm_net_queue rx;
	struct ivshm_net_queue tx;

	u32 vrsize;
	u32 qlen;
	u32 qsize;

	struct napi_struct napi;

	struct mutex state_lock;
	u32 state;
	u32 last_peer_state;

	unsigned long flags;

	struct workqueue_struct *state_wq;
	struct work_struct state_work;

	struct ivshm_net_stats stats;

	struct ivshmem_regs __iomem *ivshm_regs;
	void *shm;
	phys_addr_t shmaddr;
	resource_size_t shmlen;
	u32 peer_id;
	u32 my_id;

	struct pci_dev *pdev;
};

static void *ivshm_net_desc_data(struct ivshm_net *in,
				 struct ivshm_net_queue *q,
				 struct vring_desc *desc, u32 *len)
{
	u64 offs = READ_ONCE(desc->addr);
	u32 dlen = READ_ONCE(desc->len);
	u16 flags = READ_ONCE(desc->flags);
	void *data;

	if (flags)
		return NULL;

	if (offs >= in->shmlen)
		return NULL;

	data = in->shm + offs;

	if (data < q->data || data >= q->end)
		return NULL;

	if (dlen > q->end - data)
		return NULL;

	*len = dlen;

	return data;
}

static void ivshm_net_init_queue(struct ivshm_net *in,
				 struct ivshm_net_queue *q, void *mem,
				 unsigned int len)
{
	memset(q, 0, sizeof(*q));

	vring_init(&q->vr, len, mem, IVSHM_NET_VQ_ALIGN);
	q->data = mem + in->vrsize;
	q->end = q->data + in->qsize;
	q->size = in->qsize;
}

static void ivshm_net_init_queues(struct net_device *ndev)
{
	struct ivshm_net *in = netdev_priv(ndev);
	int i;

	memset(MY_QUEUE, 0, QUEUE_SIZE);

	ivshm_net_init_queue(in, &in->rx, PEER_QUEUE, in->qlen);
	ivshm_net_init_queue(in, &in->tx, MY_QUEUE, in->qlen);

	swap(in->rx.vr.used, in->tx.vr.used);

	in->tx.num_free = in->tx.vr.num;

	for (i = 0; i < in->tx.vr.num - 1; i++)
		in->tx.vr.desc[i].next = i + 1;
}

static int ivshm_net_calc_qsize(struct net_device *ndev)
{
	struct ivshm_net *in = netdev_priv(ndev);
	unsigned int vrsize;
	unsigned int qsize;
	unsigned int qlen;

	for (qlen = 4096; qlen > 32; qlen >>= 1) {
		vrsize = vring_size(qlen, IVSHM_NET_VQ_ALIGN);
		vrsize = ALIGN(vrsize, IVSHM_NET_VQ_ALIGN);
		if (vrsize < in->shmlen / 16)
			break;
	}

	if (vrsize > in->shmlen / capacity)
		return -EINVAL;

	qsize = in->shmlen / capacity - 4 - vrsize;

	if (qsize < 4 * ETH_MIN_MTU)
		return -EINVAL;

	in->vrsize = vrsize;
	in->qlen = qlen;
	in->qsize = qsize;

	return 0;
}

static void ivshm_net_notify_tx(struct ivshm_net *in, unsigned int num)
{
	u16 evt, old, new;

	virt_mb();

	evt = READ_ONCE(vring_avail_event(&in->tx.vr));
	old = in->tx.last_avail_idx - num;
	new = in->tx.last_avail_idx;

	_dbg("%s: evt[%d] old[%d] new[%d]\n", __func__, evt, old, new);
	if (vring_need_event(evt, new, old)) {
		writel(in->peer_id << 16, &in->ivshm_regs->doorbell);
		in->stats.tx_notify++;
	}
}

static void ivshm_net_enable_rx_irq(struct ivshm_net *in)
{
	vring_avail_event(&in->rx.vr) = in->rx.last_avail_idx;
	virt_wmb();
}

static void ivshm_net_notify_rx(struct ivshm_net *in, unsigned int num)
{
	u16 evt, old, new;

	virt_mb();

	evt = READ_ONCE(vring_used_event(&in->rx.vr));
	old = in->rx.last_used_idx - num;
	new = in->rx.last_used_idx;

	_dbg("%s: evt[%d] old[%d] new[%d]\n", __func__, evt, old, new);
	if (vring_need_event(evt, new, old)) {
		writel(in->peer_id << 16, &in->ivshm_regs->doorbell);
		in->stats.rx_notify++;
	}
}

static void ivshm_net_enable_tx_irq(struct ivshm_net *in)
{
	vring_used_event(&in->tx.vr) = in->tx.last_used_idx;
	virt_wmb();
}

static bool ivshm_net_rx_avail(struct ivshm_net *in)
{
	virt_mb();
	return READ_ONCE(in->rx.vr.avail->idx) != in->rx.last_avail_idx;
}

static size_t ivshm_net_tx_space(struct ivshm_net *in)
{
	struct ivshm_net_queue *tx = &in->tx;
	u32 tail = tx->tail;
	u32 head = tx->head;
	u32 space;

	if (head < tail)
		space = tail - head;
	else
		space = max(tx->size - head, tail);

	return space;
}

static bool ivshm_net_tx_ok(struct net_device *ndev)
{
	struct ivshm_net *in = netdev_priv(ndev);

	return in->tx.num_free >= 2 &&
	       ivshm_net_tx_space(in) >= 2 * IVSHM_NET_FRAME_SIZE(ndev->mtu);
}

static u32 ivshm_net_tx_advance(struct ivshm_net_queue *q, u32 *pos, u32 len)
{
	u32 p = *pos;

	len = IVSHM_NET_FRAME_SIZE(len);

	if (q->size - p < len)
		p = 0;
	*pos = p + len;

	return p;
}

static bool ivshm_net_tx_clean(struct net_device *ndev)
{
	struct ivshm_net *in = netdev_priv(ndev);
	struct ivshm_net_queue *tx = &in->tx;
	struct vring_used_elem *used;
	struct vring *vr = &tx->vr;
	struct vring_desc *desc;
	struct vring_desc *fdesc;
	u16 last = tx->last_used_idx;
	unsigned int num;
	bool tx_ok;
	u32 fhead;

	fdesc = NULL;
	fhead = 0;
	num = 0;

	while (last != virt_load_acquire(&vr->used->idx)) {
		void *data;
		u32 len;
		u32 tail;

		used = vr->used->ring + (last % vr->num);
		if (used->id >= vr->num || used->len != 1) {
			netdev_err(ndev, "invalid tx used->id %d ->len %d\n",
				   used->id, used->len);
			break;
		}

		desc = &vr->desc[used->id];

		data = ivshm_net_desc_data(in, &in->tx, desc, &len);
		if (!data) {
			netdev_err(ndev, "bad tx descriptor, data == NULL\n");
			break;
		}

		tail = ivshm_net_tx_advance(tx, &tx->tail, len);
		if (data != tx->data + tail) {
			netdev_err(ndev, "bad tx descriptor\n");
			break;
		}

		if (!num)
			fdesc = desc;
		else
			desc->next = fhead;

		fhead = used->id;
		tx->last_used_idx = ++last;
		num++;
		tx->num_free++;
		BUG_ON(tx->num_free > vr->num);

		tx_ok = ivshm_net_tx_ok(ndev);
		if (!tx_ok)
			ivshm_net_enable_tx_irq(in);
	}

	if (num) {
		fdesc->next = tx->free_head;
		tx->free_head = fhead;
	} else {
		tx_ok = ivshm_net_tx_ok(ndev);
	}

	return tx_ok;
}

static void ivshm_net_tx_poll(struct net_device *ndev)
{
	struct netdev_queue *txq = netdev_get_tx_queue(ndev, 0);

	if (!__netif_tx_trylock(txq))
		return;

	if (ivshm_net_tx_clean(ndev) && netif_queue_stopped(ndev))
		netif_wake_queue(ndev);

	__netif_tx_unlock(txq);
}

static struct vring_desc *ivshm_net_rx_desc(struct net_device *ndev)
{
	struct ivshm_net *in = netdev_priv(ndev);
	struct ivshm_net_queue *rx = &in->rx;
	struct vring *vr = &rx->vr;
	unsigned int avail;
	u16 avail_idx;

	avail_idx = virt_load_acquire(&vr->avail->idx);

	if (avail_idx == rx->last_avail_idx)
		return NULL;

	avail = vr->avail->ring[rx->last_avail_idx++ & (vr->num - 1)];
	if (avail >= vr->num) {
		netdev_err(ndev, "invalid rx avail %d\n", avail);
		return NULL;
	}

	return &vr->desc[avail];
}

static void ivshm_net_rx_finish(struct ivshm_net *in, struct vring_desc *desc)
{
	struct ivshm_net_queue *rx = &in->rx;
	struct vring *vr = &rx->vr;
	unsigned int desc_id = desc - vr->desc;
	unsigned int used;

	used = rx->last_used_idx++ & (vr->num - 1);
	vr->used->ring[used].id = desc_id;
	vr->used->ring[used].len = 1;

	virt_store_release(&vr->used->idx, rx->last_used_idx);
}

static int ivshm_net_poll(struct napi_struct *napi, int budget)
{
	struct net_device *ndev = napi->dev;
	struct ivshm_net *in = container_of(napi, struct ivshm_net, napi);
	int received = 0;

	in->stats.napi_poll++;

	ivshm_net_tx_poll(ndev);

	while (received < budget) {
		struct vring_desc *desc;
		struct sk_buff *skb;
		void *data;
		u32 len;

		desc = ivshm_net_rx_desc(ndev);
		if (!desc)
			break;

		data = ivshm_net_desc_data(in, &in->rx, desc, &len);
		if (!data) {
			netdev_err(ndev, "bad rx descriptor\n");
			break;
		}

		skb = napi_alloc_skb(napi, len);

		if (skb) {
			memcpy(skb_put(skb, len), data, len);
			skb->protocol = eth_type_trans(skb, ndev);
			napi_gro_receive(napi, skb);
		}

		ndev->stats.rx_packets++;
		ndev->stats.rx_bytes += len;

		ivshm_net_rx_finish(in, desc);
		received++;
	}

	if (received < budget) {
		in->stats.napi_complete++;
		napi_complete_done(napi, received);
		ivshm_net_enable_rx_irq(in);
		if (ivshm_net_rx_avail(in))
			napi_schedule(napi);
	}

	if (received)
		ivshm_net_notify_rx(in, received);

	in->stats.rx_packets += received;
	in->stats.napi_poll_n[received ? 1 + min(ilog2(received), 8) : 0]++;

	return received;
}

static netdev_tx_t ivshm_net_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct ivshm_net *in = netdev_priv(ndev);
	bool xmit_more = netdev_xmit_more();
	struct ivshm_net_queue *tx = &in->tx;
	struct vring *vr = &tx->vr;
	struct vring_desc *desc;
	unsigned int desc_idx;
	unsigned int avail;
	u32 head;
	void *buf;

	if (!ivshm_net_tx_clean(ndev)) {
		netif_stop_queue(ndev);

		netdev_err(ndev, "BUG: tx ring full when queue awake!\n");
		return NETDEV_TX_BUSY;
	}

	desc_idx = tx->free_head;
	desc = &vr->desc[desc_idx];
	tx->free_head = desc->next;
	tx->num_free--;
	ivshm_net_tx_clean(ndev);
	head = ivshm_net_tx_advance(tx, &tx->head, skb->len);

	if (!ivshm_net_tx_ok(ndev)) {
		ivshm_net_enable_tx_irq(in);
		netif_stop_queue(ndev);
		xmit_more = false;
		in->stats.tx_pause++;
	}

	buf = tx->data + head;
	skb_copy_and_csum_dev(skb, buf);

	desc->addr = buf - in->shm;
	desc->len = skb->len;
	desc->flags = 0;

	avail = tx->last_avail_idx++ & (vr->num - 1);
	vr->avail->ring[avail] = desc_idx;
	tx->num_added++;

	virt_store_release(&vr->avail->idx, tx->last_avail_idx);

	if (!xmit_more) {
		ivshm_net_notify_tx(in, tx->num_added);
		tx->num_added = 0;
	}

	in->stats.tx_packets++;
	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += skb->len;

	dev_consume_skb_any(skb);

	return NETDEV_TX_OK;
}

static void ivshm_net_set_state(struct ivshm_net *in, u32 state)
{
	virt_wmb();
	WRITE_ONCE(in->state, state);
	writel(state, MY_STATE);
}

static void ivshm_net_run(struct net_device *ndev)
{
	struct ivshm_net *in = netdev_priv(ndev);

	if (in->state < IVSHM_NET_STATE_READY)
		return;

	if (!netif_running(ndev))
		return;

	if (in->last_peer_state == IVSHM_NET_STATE_RUN)
		netif_carrier_on(ndev);

	if (test_and_set_bit(IVSHM_NET_FLAG_RUN, &in->flags))
		return;

	netif_start_queue(ndev);
	napi_enable(&in->napi);
	napi_schedule(&in->napi);
	ivshm_net_set_state(in, IVSHM_NET_STATE_RUN);
}

static void ivshm_net_do_stop(struct net_device *ndev)
{
	struct ivshm_net *in = netdev_priv(ndev);

	ivshm_net_set_state(in, IVSHM_NET_STATE_RESET);

	if (!test_and_clear_bit(IVSHM_NET_FLAG_RUN, &in->flags))
		return;

	netif_carrier_off(ndev);
	netif_stop_queue(ndev);
	napi_disable(&in->napi);
}

static void ivshm_net_state_change(struct work_struct *work)
{
	struct ivshm_net *in = container_of(work, struct ivshm_net, state_work);
	struct net_device *ndev = in->napi.dev;
	u32 rstate = readl(PEER_STATE);

	_dbg("%s: state[%d], last_peer_state[%d], current_peer_state[%d]\n",
	     __func__, in->state, in->last_peer_state, rstate);

	mutex_lock(&in->state_lock);
	if (in->last_peer_state == rstate) {
		mutex_unlock(&in->state_lock);
		return;
	}
	in->last_peer_state = rstate;

	switch (in->state) {
	case IVSHM_NET_STATE_RESET:
		/*
     * Wait for the remote to leave READY/RUN before transitioning
     * to INIT.
     */
		if (rstate < IVSHM_NET_STATE_READY)
			ivshm_net_set_state(in, IVSHM_NET_STATE_INIT);
		break;

	case IVSHM_NET_STATE_INIT:
		/*
     * Wait for the remote to leave RESET before performing the
     * initialization and moving to READY.
     */
		if (rstate > IVSHM_NET_STATE_RESET) {
			ivshm_net_init_queues(ndev);
			ivshm_net_set_state(in, IVSHM_NET_STATE_READY);

			rtnl_lock();
			call_netdevice_notifiers(NETDEV_CHANGEADDR, ndev);
			rtnl_unlock();
		}
		break;

	case IVSHM_NET_STATE_READY:
	case IVSHM_NET_STATE_RUN:
		if (rstate >= IVSHM_NET_STATE_READY) {
			/*
       * Link is up and we are running once the remote is in READY or
       * RUN.
       */
			ivshm_net_run(ndev);
		} else if (rstate == IVSHM_NET_STATE_RESET) {
			/*
       * If the remote goes to RESET, we need to follow immediately.
       */
			ivshm_net_do_stop(ndev);
		}
		break;
	}

	mutex_unlock(&in->state_lock);
	writel(in->peer_id << 16, &in->ivshm_regs->doorbell);
}

static void ivshm_net_check_state(struct net_device *ndev)
{
	struct ivshm_net *in = netdev_priv(ndev);

	queue_work(in->state_wq, &in->state_work);
}

static irqreturn_t ivshm_net_int(int irq, void *data)
{
	struct net_device *ndev = data;
	struct ivshm_net *in = netdev_priv(ndev);

	in->stats.interrupts++;

	ivshm_net_check_state(ndev);
	napi_schedule_irqoff(&in->napi);

	return IRQ_HANDLED;
}

static int ivshm_net_open(struct net_device *ndev)
{
	struct ivshm_net *in = netdev_priv(ndev);

	netdev_reset_queue(ndev);
	ndev->operstate = IF_OPER_UP;

	mutex_lock(&in->state_lock);
	ivshm_net_run(ndev);
	mutex_unlock(&in->state_lock);

	return 0;
}

static int ivshm_net_stop(struct net_device *ndev)
{
	struct ivshm_net *in = netdev_priv(ndev);

	ndev->operstate = IF_OPER_DOWN;

	mutex_lock(&in->state_lock);
	ivshm_net_do_stop(ndev);
	mutex_unlock(&in->state_lock);

	return 0;
}

static int ivshm_net_change_mtu(struct net_device *ndev, int mtu)
{
	if (netif_running(ndev)) {
		netdev_err(ndev, "must be stopped to change its MTU\n");
		return -EBUSY;
	};

	ndev->mtu = mtu;

	return 0;
}

#ifdef CONFIG_NET_POLL_CONTROLLER
static void ivshm_net_poll_controller(struct net_device *ndev)
{
	struct ivshm_net *in = netdev_priv(ndev);

	napi_schedule(&in->napi);
}
#endif

static const struct net_device_ops ivshm_net_ops = {
	.ndo_open = ivshm_net_open,
	.ndo_stop = ivshm_net_stop,
	.ndo_start_xmit = ivshm_net_xmit,
	.ndo_change_mtu = ivshm_net_change_mtu,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller = ivshm_net_poll_controller,
#endif
};

static const char ivshm_net_stats[][ETH_GSTRING_LEN] = {
	"tx_free_num",	"tx_free_size",	 "interrupts",	  "tx_packets",
	"tx_notify",	"tx_pause",	 "rx_packets",	  "rx_notify",
	"napi_poll",	"napi_complete", "napi_poll_0",	  "napi_poll_1",
	"napi_poll_2",	"napi_poll_4",	 "napi_poll_8",	  "napi_poll_16",
	"napi_poll_32", "napi_poll_64",	 "napi_poll_128", "napi_poll_256",
};

#define NUM_STATS ARRAY_SIZE(ivshm_net_stats)

static int ivshm_net_get_sset_count(struct net_device *ndev, int sset)
{
	if (sset == ETH_SS_STATS)
		return NUM_STATS;

	return -EOPNOTSUPP;
}

static void ivshm_net_get_strings(struct net_device *ndev, u32 sset, u8 *buf)
{
	if (sset == ETH_SS_STATS)
		memcpy(buf, &ivshm_net_stats, sizeof(ivshm_net_stats));
}

static void ivshm_net_get_ethtool_stats(struct net_device *ndev,
					struct ethtool_stats *estats, u64 *st)
{
	struct ivshm_net *in = netdev_priv(ndev);
	unsigned int n = 0;
	unsigned int i;

	st[n++] = in->tx.num_free;
	st[n++] = ivshm_net_tx_space(in);
	st[n++] = in->stats.interrupts;
	st[n++] = in->stats.tx_packets;
	st[n++] = in->stats.tx_notify;
	st[n++] = in->stats.tx_pause;
	st[n++] = in->stats.rx_packets;
	st[n++] = in->stats.rx_notify;
	st[n++] = in->stats.napi_poll;
	st[n++] = in->stats.napi_complete;

	for (i = 0; i < ARRAY_SIZE(in->stats.napi_poll_n); i++)
		st[n++] = in->stats.napi_poll_n[i];

	memset(&in->stats, 0, sizeof(in->stats));
}

#define IVSHM_NET_REGS_LEN (4 * sizeof(u32) + 6 * sizeof(u16))

static int ivshm_net_get_regs_len(struct net_device *ndev)
{
	return IVSHM_NET_REGS_LEN;
}

static void ivshm_net_get_regs(struct net_device *ndev,
			       struct ethtool_regs *regs, void *p)
{
	struct ivshm_net *in = netdev_priv(ndev);
	u32 *reg32 = p;
	u16 *reg16;

	*reg32++ = in->state;
	*reg32++ = in->last_peer_state;
	*reg32++ = in->qlen;
	*reg32++ = in->qsize;

	reg16 = (u16 *)reg32;

	*reg16++ = in->tx.vr.avail ? in->tx.vr.avail->idx : 0;
	*reg16++ = in->tx.vr.used ? in->tx.vr.used->idx : 0;
	*reg16++ = in->tx.vr.avail ? vring_avail_event(&in->tx.vr) : 0;

	*reg16++ = in->rx.vr.avail ? in->rx.vr.avail->idx : 0;
	*reg16++ = in->rx.vr.used ? in->rx.vr.used->idx : 0;
	*reg16++ = in->rx.vr.avail ? vring_avail_event(&in->rx.vr) : 0;
}

static const struct ethtool_ops ivshm_net_ethtool_ops = {
	.get_sset_count = ivshm_net_get_sset_count,
	.get_strings = ivshm_net_get_strings,
	.get_ethtool_stats = ivshm_net_get_ethtool_stats,
	.get_regs_len = ivshm_net_get_regs_len,
	.get_regs = ivshm_net_get_regs,
};

static int ivshm_net_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct net_device *ndev;
	struct ivshm_net *in;
	struct ivshmem_regs __iomem *regs;
	resource_size_t shmaddr;
	resource_size_t shmlen;
	char *device_name;
	void *shm;
	int ret;

	ret = pcim_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "pci_enable_device: %d\n", ret);
		return ret;
	}

	ret = pcim_iomap_regions(pdev, BIT(0), DRV_NAME);
	if (ret) {
		dev_err(&pdev->dev, "pcim_iomap_regions: %d\n", ret);
		return ret;
	}

	regs = pcim_iomap_table(pdev)[0];

	shmlen = pci_resource_len(pdev, 2);
	if (!shmlen) {
		dev_err(&pdev->dev, "can't find size of shmem\n");
		return -EINVAL;
	}
	shmaddr = pci_resource_start(pdev, 2);

	if (!devm_request_mem_region(&pdev->dev, shmaddr, shmlen, DRV_NAME))
		return -EBUSY;

	shm = devm_memremap(&pdev->dev, shmaddr, shmlen, MEMREMAP_WB);
	if (!shm)
		return -ENOMEM;

	device_name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "%s[%s]", DRV_NAME,
				     dev_name(&pdev->dev));
	if (!device_name)
		return -ENOMEM;

	ndev = alloc_etherdev(sizeof(*in));
	if (!ndev)
		return -ENOMEM;

	pci_set_drvdata(pdev, ndev);
	SET_NETDEV_DEV(ndev, &pdev->dev);

	in = netdev_priv(ndev);
	in->ivshm_regs = regs;
	in->shm = shm;
	in->shmaddr = shmaddr;
	in->shmlen = shmlen;
	in->peer_id = peer_id;
	in->my_id = my_id;
	in->last_peer_state = IVSHM_NET_STATE_UNKNOWN;
	mutex_init(&in->state_lock);
	in->pdev = pdev;

	ret = ivshm_net_calc_qsize(ndev);
	if (ret)
		goto err_free;

	in->state_wq = alloc_ordered_workqueue(device_name, 0);
	if (!in->state_wq)
		goto err_free;

	INIT_WORK(&in->state_work, ivshm_net_state_change);

	eth_random_addr(ndev->dev_addr);
	ndev->netdev_ops = &ivshm_net_ops;
	ndev->ethtool_ops = &ivshm_net_ethtool_ops;
	ndev->mtu = min_t(u32, IVSHM_NET_MTU_DEF, in->qsize / 16);
	ndev->min_mtu = ETH_MIN_MTU;
	ndev->max_mtu = min_t(u32, ETH_MAX_MTU, in->qsize / 4);
	ndev->hw_features = NETIF_F_HW_CSUM | NETIF_F_SG;
	ndev->features = ndev->hw_features;

	netif_carrier_off(ndev);
	netif_napi_add(ndev, &in->napi, ivshm_net_poll, NAPI_POLL_WEIGHT);

	ret = register_netdev(ndev);
	if (ret)
		goto err_wq;

	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_LEGACY | PCI_IRQ_MSIX);
	if (ret < 0)
		goto err_alloc_irq;

	ret = request_irq(pci_irq_vector(pdev, 0), ivshm_net_int, 0,
			  device_name, ndev);
	if (ret)
		goto err_request_irq;

	pci_set_master(pdev);
	if (!pdev->msix_enabled)
		writel(IVSHMEM_INTX_ENABLE, &in->ivshm_regs->intxctrl);

	writel(IVSHM_NET_STATE_RESET, MY_STATE);
	ivshm_net_check_state(ndev);
	dev_info(&pdev->dev, "%s: done\n", __func__);

	return 0;

err_request_irq:
	pci_free_irq_vectors(pdev);
err_alloc_irq:
	unregister_netdev(ndev);
err_wq:
	destroy_workqueue(in->state_wq);
err_free:
	free_netdev(ndev);

	return ret;
}

static void ivshm_net_remove(struct pci_dev *pdev)
{
	struct net_device *ndev = pci_get_drvdata(pdev);
	struct ivshm_net *in = netdev_priv(ndev);

	writel(IVSHM_NET_STATE_RESET, MY_STATE);

	if (!pdev->msix_enabled)
		writel(0, &in->ivshm_regs->intxctrl);
	free_irq(pci_irq_vector(pdev, 0), ndev);
	pci_free_irq_vectors(pdev);

	unregister_netdev(ndev);
	cancel_work_sync(&in->state_work);
	destroy_workqueue(in->state_wq);
	free_netdev(ndev);
	dev_info(&pdev->dev, "%s: done\n", __func__);
}

static const struct pci_device_id ivshm_net_id_table[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_REDHAT_QUMRANET, PCI_DEVICE_ID_IVSHMEM) },
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, ivshm_net_id_table);

static struct pci_driver ivshm_net_driver = {
	.name = DRV_NAME,
	.id_table = ivshm_net_id_table,
	.probe = ivshm_net_probe,
	.remove = ivshm_net_remove,
};

static int __init ivshm_net_init(void)
{
	if (peer_id == my_id || capacity < 2 || peer_id >= capacity ||
	    my_id >= capacity) {
		pr_err("invalid peer's id[%u], my id[%u] or capacity[%u]\n",
		       peer_id, my_id, capacity);
		return -EINVAL;
	}

	return pci_register_driver(&ivshm_net_driver);
}

static void __exit ivshm_net_exit(void)
{
	pci_unregister_driver(&ivshm_net_driver);
}

module_init(ivshm_net_init);
module_exit(ivshm_net_exit);

MODULE_AUTHOR("Wei Tan <wei.tan@intel.com>");
MODULE_LICENSE("GPL");
