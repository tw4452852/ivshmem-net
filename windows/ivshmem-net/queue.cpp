#include "precomp.h"

#include "trace.h"
#include "adapter.h"
#include "queue.h"

NTSTATUS ivshm_net_calc_qsize(_In_ IVN_ADAPTER *adapter)
{
	unsigned int vrsize;
	unsigned int qsize;
	unsigned int qlen;

	for (qlen = 4096; qlen > 32; qlen >>= 1) {
		vrsize = vring_size(qlen, IVSHM_NET_VQ_ALIGN);
		vrsize = ALIGN(vrsize, IVSHM_NET_VQ_ALIGN);
		if (vrsize < adapter->sharedRegionSize / 16)
			break;
	}

	if (vrsize > adapter->sharedRegionSize / 2)
		return -1;

	qsize = adapter->sharedRegionSize / 2 - 4 - vrsize;

	if (qsize < 4 * 1500)
		return -1;

	adapter->vrsize = vrsize;
	adapter->qlen = qlen;
	adapter->qsize = qsize;

	return 0;
}

void ivshm_net_init_queue(_In_ IVN_ADAPTER *adapter,
	struct ivshm_net_queue *q, UINT8 *mem,
	unsigned int len)
{
	memset(q, 0, sizeof(*q));

	vring_init(&q->vr, len, mem, IVSHM_NET_VQ_ALIGN);
	q->data = mem + adapter->vrsize;
	q->end = q->data + adapter->qsize;
	q->size = adapter->qsize;
}

void *ivshm_net_desc_data(_In_ IVN_ADAPTER *adapter,
	struct ivshm_net_queue *q,
	struct vring_desc *desc, u32 *len)
{
	u64 offs = *(volatile u64 *)(&(desc->addr));
	u32 dlen = *(volatile u32 *)(&(desc->len));
	u16 flags = *(volatile u16 *)(&(desc->flags));
	void *data;

	if (flags)
		return NULL;

	if (offs >= adapter->sharedRegionSize)
		return NULL;

	data = adapter->sharedRegion + offs;

	if (data < q->data || data >= q->end)
		return NULL;

	if (dlen > q->end - (UINT8 *)data)
		return NULL;

	*len = dlen;

	return data;
}