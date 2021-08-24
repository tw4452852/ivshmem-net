/*++

    THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
    ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
    THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
    PARTICULAR PURPOSE.

    Copyright (c) Microsoft Corporation. All rights reserved

--*/

#include "precomp.h"

#include "device.h"
#include "txqueue.h"
#include "trace.h"
#include "adapter.h"
#include "interrupt.h"

void
IvshmemNetUpdateSendStats(
    _In_ IVN_TXQUEUE *tx,
    _In_ NET_PACKET *packet
    )
{
    PCNET_DATAPATH_DESCRIPTOR descriptor = tx->DatapathDescriptor;
    if (packet->Layout.Layer2Type != NET_PACKET_LAYER2_TYPE_ETHERNET)
    {
        return;
    }

    auto fragment = NET_PACKET_GET_FRAGMENT(packet, descriptor, 0);
    // Ethernet header should be in first fragment
    if (fragment->ValidLength < sizeof(ETHERNET_HEADER))
    {
        return;
    }

    PUCHAR ethHeader = (PUCHAR)fragment->VirtualAddress + fragment->Offset;

    ULONG length = 0;
    for (UINT32 i = 0; i < packet->FragmentCount; i ++)
    {
        fragment = NET_PACKET_GET_FRAGMENT(packet, descriptor, i);
        length += (ULONG)fragment->ValidLength;
    }

    IVN_ADAPTER *adapter = tx->Adapter;

    if (ETH_IS_BROADCAST(ethHeader))
    {
        adapter->OutBroadcastPkts++;
        adapter->OutBroadcastOctets += length;
    }
    else if (ETH_IS_MULTICAST(ethHeader))
    {
        adapter->OutMulticastPkts++;
        adapter->OutMulticastOctets += length;
    }
    else
    {
        adapter->OutUCastPkts++;
        adapter->OutUCastOctets += length;
    }
}



static u32 ivshm_net_tx_advance(struct ivshm_net_queue *q, UINT32 *pos, UINT32 len)
{
	u32 p = *pos;

	len = IVSHM_NET_FRAME_SIZE(len);

	if (q->size - p < len)
		p = 0;
	*pos = p + len;

	return p;
}

static
void
IvshmemNetProgramDescriptors(
    _In_ IVN_TXQUEUE *tx,
    _In_ NET_PACKET *packet
    )
{
    PCNET_DATAPATH_DESCRIPTOR descriptor = tx->DatapathDescriptor;
	IVN_ADAPTER *adapter = tx->Adapter;
	struct ivshm_net_queue *q = &adapter->tx_queue;
	UINT32 head, i, len = 0;

    IvshmemNetUpdateSendStats(tx, packet);
    IVN_TCB *tcb = GetTcbFromPacketFromToken(tx->DatapathDescriptor, packet, tx->TcbToken);

	for (i = 0; i < packet->FragmentCount; i++)
	{
		bool const lastFragment = i + 1 == packet->FragmentCount;
		NET_PACKET_FRAGMENT *fragment = NET_PACKET_GET_FRAGMENT(packet, descriptor, i);

		// First fragment of packet
		if (i == 0)
		{
			tcb->NumFrag = 0;
		}

		len += (UINT32)fragment->ValidLength;

		if (lastFragment)
		{
			tcb->NumFrag = i + 1;
			break;
		}
	}

	USHORT desc_idx = (USHORT)q->free_head;
	vring_desc *txd = &q->vr.desc[desc_idx];

	q->free_head = txd->next;
	q->num_free--;

	tcb->TxDescId = desc_idx;

	// get a free buffer from shared region
	head = ivshm_net_tx_advance(q, &q->head, len);
	UINT8 *buf = q->data + head;
	
	txd->addr = buf - adapter->sharedRegion;
	txd->len = len;
	txd->flags = 0;

	UINT16 avail = q->last_avail_idx++ & (q->vr.num - 1);
	q->vr.avail->ring[avail] = desc_idx;
	q->num_added++;


	TraceLoggingWrite(
		IvshmemNetTraceProvider,
		"send a pkt",
		TraceLoggingUInt32(tcb->TxDescId, "id"),
		TraceLoggingUInt32(txd->len, "len"),
		TraceLoggingUInt32(tcb->NumFrag, "fragment")
	);

	for (i = 0; i < packet->FragmentCount; i++)
	{
        NET_PACKET_FRAGMENT *fragment = NET_PACKET_GET_FRAGMENT(packet, descriptor, i);
		// copy to shared region buffer and update the corresponding desc
		RtlCopyMemory(buf, (UINT8 *)fragment->VirtualAddress + fragment->Offset, fragment->ValidLength);
		buf += fragment->ValidLength;
    }
}

static void ivshm_net_notify_tx(IVN_ADAPTER *adapter, UINT32 num)
{
	u16 evt, old, cur;
	struct ivshm_net_queue *q = &adapter->tx_queue;

	MemoryBarrier();

	evt = vring_avail_event(&q->vr);
	old = q->last_avail_idx - (UINT16)num;
	cur = q->last_avail_idx;

	int should_notify = vring_need_event(evt, cur, old);
	TraceLoggingWrite(
		IvshmemNetTraceProvider,
		"ivshm_net_notify_tx",
		TraceLoggingUInt32(old, "old"),
		TraceLoggingUInt32(evt, "evt"),
		TraceLoggingUInt32(cur, "cur"),
		TraceLoggingUInt32(should_notify, "should_notify")
	);
	
	if (should_notify) {
		notifyPeer(adapter);
	}
}

static
void
IvshmemNetFlushTransation(
    _In_ IVN_TXQUEUE *tx
    )
{
	IVN_ADAPTER *adapter = tx->Adapter;
	struct ivshm_net_queue *q = &adapter->tx_queue;

    MemoryBarrier();
	q->vr.avail->idx = q->last_avail_idx;

	ivshm_net_notify_tx(adapter, q->num_added);
	q->num_added = 0;
}

#if 0
static bool
is_freed_desc(IVN_ADAPTER *adapter, USHORT desc_id)
{
	struct ivshm_net_queue *q = &adapter->tx_queue;
	struct vring_desc *desc;
	USHORT idx;

	if (q->num_free == 0) {
		return false;
	}
	
	idx = (USHORT)q->free_head;
	desc = &q->vr.desc[idx];
	for (size_t i = 0; i < q->num_free; i++) {
		if (idx == desc_id) {
			return true;
		}

		idx = desc->next;
		desc = &q->vr.desc[idx];
	}
	return false;
}

static
bool
IvshmemNetIsPacketTransferComplete(
    _In_ IVN_TXQUEUE *tx,
    _In_ NET_PACKET *packet
    )
{
    IVN_TCB *tcb = GetTcbFromPacketFromToken(tx->DatapathDescriptor, packet, tx->TcbToken);
	IVN_ADAPTER *adapter = tx->Adapter;

	for (size_t idx = 0; idx < tcb->NumTxDesc; idx++)
	{
		if (!is_freed_desc(adapter, tcb->TxDescIds[idx])) {
			return false;
		}
	}

    return true;
}
#endif

static size_t ivshm_net_tx_space(struct ivshm_net_queue *tx)
{
	u32 tail = tx->tail;
	u32 head = tx->head;
	u32 space;

	if (head < tail)
		space = tail - head;
	else
		space = max(tx->size - head, tail);

	return space;
}

static bool ivshm_net_tx_ok(struct ivshm_net_queue *q)
{
	return q->num_free >= 2 &&
		ivshm_net_tx_space(q) >= 2 * IVSHM_NET_FRAME_SIZE(1500);
}

static void
free_done_descs(_In_ IVN_ADAPTER *adapter)
{
	struct vring_used_elem *used;
	struct vring_desc *fdesc = NULL;
	struct vring_desc *desc;
	struct ivshm_net_queue *q = &adapter->tx_queue;
	UINT16 last = q->last_used_idx;
	struct vring *vr = &q->vr;
	UINT32 fhead = 0;
	UINT32 num = 0;

	TraceEntry();

	while (last != *(volatile UINT16 *)(&vr->used->idx)) {
		TraceLoggingWrite(
			IvshmemNetTraceProvider,
			"free_done_descs",
			TraceLoggingUInt32(last, "last"),
			TraceLoggingUInt32(vr->used->idx, "vr->used->idx")
		);

		void *data;
		u32 len;
		u32 tail;

		used = vr->used->ring + (last % vr->num);
		if (used->id >= vr->num || used->len != 1) {
			TraceLoggingWrite(
				IvshmemNetTraceProvider,
				"InvalidTxUsed",
				TraceLoggingLevel(TRACE_LEVEL_ERROR),
				TraceLoggingUInt32(used->id, "used->id"),
				TraceLoggingUInt32(used->len, "used->len")
			);
			break;
		}

		desc = &vr->desc[used->id];

		data = ivshm_net_desc_data(adapter, q, desc, &len);
		if (!data) {
			TraceLoggingWrite(
				IvshmemNetTraceProvider,
				"tx data is NULL",
				TraceLoggingLevel(TRACE_LEVEL_ERROR),
				TraceLoggingUInt32(used->id, "id"),
				TraceLoggingHexUInt64(desc->addr, "desc->addr"),
				TraceLoggingUInt32(desc->len, "desc->len"),
				TraceLoggingHexUInt16(desc->flags, "desc->flags"),
				TraceLoggingUInt16(desc->next, "desc->next")
			);
			break;
		}

		tail = ivshm_net_tx_advance(q, &q->tail, len);
		if (data != q->data + tail) {
			TraceLoggingWrite(
				IvshmemNetTraceProvider,
				"tx data is invalid",
				TraceLoggingLevel(TRACE_LEVEL_ERROR),
				TraceLoggingUInt32(used->id, "id"),
				TraceLoggingHexUInt64((UINT64)data, "data"),
				TraceLoggingHexUInt64((UINT64)q->data + tail, "q->data + tail")
			);
			break;
		}

		if (!num)
			fdesc = desc;
		else
			desc->next = (USHORT)fhead;

		fhead = used->id;
		q->last_used_idx = ++last;
		num++;
		q->num_free++;
		TraceLoggingWrite(
			IvshmemNetTraceProvider,
			"free a done desc",
			TraceLoggingUInt32(fhead, "id")
		);
	}

	if (num) {
		fdesc->next = (USHORT)q->free_head;
		q->free_head = fhead;
	}

	TraceExit();
}

static
void
IvshmemNetTransmitPackets(
    _In_ IVN_TXQUEUE *tx
    )
{
    PCNET_DATAPATH_DESCRIPTOR descriptor = tx->DatapathDescriptor;
    NET_RING_BUFFER *ringBuffer = NET_DATAPATH_DESCRIPTOR_GET_PACKET_RING_BUFFER(descriptor);
    size_t programmedPackets = 0;

	TraceEntry(TraceLoggingPointer(tx, "TxQueue"));

	free_done_descs(tx->Adapter);

    while (ringBuffer->NextIndex != ringBuffer->EndIndex)
    {
        NET_PACKET *netPacket = NetRingBufferGetNextPacket(descriptor);

        if (!netPacket->IgnoreThisPacket)
        {
			if (!ivshm_net_tx_ok(&tx->Adapter->tx_queue)) {
				// let peer to notify us when receiving packets.
				vring_used_event(&tx->Adapter->tx_queue.vr) = tx->Adapter->tx_queue.last_used_idx;
				break;
			}
            IvshmemNetProgramDescriptors(tx, netPacket);

            programmedPackets++;
        }

        ringBuffer->NextIndex = NetRingBufferIncrementIndex(ringBuffer, ringBuffer->NextIndex);
    }

    if (programmedPackets > 0)
    {
        IvshmemNetFlushTransation(tx);
    }

	TraceExit();
}

static
void
IvshmemNetCompleteTransmitPackets(
    _In_ IVN_TXQUEUE *tx
    )
{
	TraceEntry(TraceLoggingPointer(tx, "TxQueue"));

    PCNET_DATAPATH_DESCRIPTOR descriptor = tx->DatapathDescriptor;
    NET_RING_BUFFER *ringBuffer = NET_DATAPATH_DESCRIPTOR_GET_PACKET_RING_BUFFER(descriptor);

	free_done_descs(tx->Adapter);

    while (ringBuffer->BeginIndex != ringBuffer->NextIndex)
    {
        ringBuffer->BeginIndex = NetRingBufferIncrementIndex(ringBuffer, ringBuffer->BeginIndex);
    }
	TraceExit();
}

_Use_decl_annotations_
void
EvtTxQueueAdvance(
    _In_ NETPACKETQUEUE txQueue
    )
{
    TraceEntry(TraceLoggingPointer(txQueue, "TxQueue"));

    IVN_TXQUEUE *tx = IvshmemNetGetTxQueueContext(txQueue);

    IvshmemNetTransmitPackets(tx);
    IvshmemNetCompleteTransmitPackets(tx);

    TraceExit();
}

void reset_tx_queue(_In_ IVN_ADAPTER * adapter)
{
	TraceEntry();
	memset(adapter->my_queue, 0, adapter->sharedRegionSize / 2 - 4);
	ivshm_net_init_queue(adapter, &adapter->tx_queue, adapter->my_queue, adapter->qlen);

	adapter->tx_queue.num_free = adapter->tx_queue.vr.num;
	for (__virtio16 i = 0; i < adapter->tx_queue.vr.num - 1; i++)
		adapter->tx_queue.vr.desc[i].next = i + 1;
	TraceExit();
}

NTSTATUS
IvshmemNetTxQueueInitialize(
    _In_ NETPACKETQUEUE txQueue,
    _In_ IVN_ADAPTER * adapter
    )
{
	TraceEntry(TraceLoggingPointer(txQueue, "TxQueue"));
    NTSTATUS status = STATUS_SUCCESS;
    IVN_TXQUEUE *tx = IvshmemNetGetTxQueueContext(txQueue);

    tx->Adapter = adapter;

    tx->TcbToken = NetTxQueueGetPacketContextToken(txQueue, WDF_GET_CONTEXT_TYPE_INFO(IVN_TCB));

    //tx->TPPoll = &adapter->CSRAddress->TPPoll;
    tx->Interrupt = adapter->Interrupt;
    
    tx->DatapathDescriptor = NetTxQueueGetDatapathDescriptor(txQueue);

	reset_tx_queue(adapter);

	TraceExit();

    return status;
}

void
IvshmemNetTxQueueSetInterrupt(
    _In_ IVN_TXQUEUE *tx,
    _In_ BOOLEAN notificationEnabled
    )
{
	TraceEntry(TraceLoggingPointer(tx, "TxQueue"));
    InterlockedExchange(&tx->Interrupt->TxNotifyArmed, notificationEnabled);
    

	if (!notificationEnabled) {
		// block this thread until we're sure any outstanding DPCs are complete.
		// This is to guarantee we don't return from this function call until
		// any oustanding tx notification is complete.
		KeFlushQueuedDpcs();
	}
	else {
		vring_used_event(&tx->Adapter->tx_queue.vr) = tx->Adapter->tx_queue.last_used_idx;
	}
	TraceExit();
}

_Use_decl_annotations_
void
EvtTxQueueStart(
    _In_ NETPACKETQUEUE txQueue
    )
{
	TraceEntry(TraceLoggingPointer(txQueue, "TxQueue"));
    IVN_TXQUEUE *tx = IvshmemNetGetTxQueueContext(txQueue);
    IVN_ADAPTER *adapter = tx->Adapter;


	adapter->TxQueue = txQueue;
	TraceExit();
}

_Use_decl_annotations_
void
EvtTxQueueStop(
    NETPACKETQUEUE txQueue
    )
{
	TraceEntry(TraceLoggingPointer(txQueue, "TxQueue"));
    IVN_TXQUEUE *tx = IvshmemNetGetTxQueueContext(txQueue);
	//IVN_ADAPTER *adapter = tx->Adapter;

    WdfSpinLockAcquire(tx->Adapter->Lock);

    //tx->Adapter->CSRAddress->CmdReg &= ~CR_TE;

    IvshmemNetTxQueueSetInterrupt(tx, false);
    tx->Adapter->TxQueue = WDF_NO_HANDLE;

    WdfSpinLockRelease(tx->Adapter->Lock);
	TraceExit();
}

_Use_decl_annotations_
void
EvtTxQueueDestroy(
    _In_ WDFOBJECT txQueue
    )
{
	TraceEntry(TraceLoggingPointer(txQueue, "TxQueue"));


	TraceExit();
}

_Use_decl_annotations_
VOID
EvtTxQueueSetNotificationEnabled(
    _In_ NETPACKETQUEUE txQueue,
    _In_ BOOLEAN notificationEnabled
    )
{
    TraceEntry(TraceLoggingPointer(txQueue), TraceLoggingBoolean(notificationEnabled));

    IVN_TXQUEUE *tx = IvshmemNetGetTxQueueContext(txQueue);

    IvshmemNetTxQueueSetInterrupt(tx, notificationEnabled);

    TraceExit();
}

_Use_decl_annotations_
void
EvtTxQueueCancel(
    _In_ NETPACKETQUEUE txQueue
    )
{
    TraceEntry(TraceLoggingPointer(txQueue, "TxQueue"));

    //
    // If the chipset is able to cancel outstanding IOs, then it should do so
    // here. However, the RTL8168D does not seem to support such a feature, so
    // the queue will continue to be drained like normal.
    //

    TraceExit();
}
