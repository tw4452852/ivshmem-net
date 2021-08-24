/*++

    THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
    ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
    THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
    PARTICULAR PURPOSE.

    Copyright (c) Microsoft Corporation. All rights reserved

--*/

#include "precomp.h"

#include "device.h"
#include "rxqueue.h"
#include "trace.h"
#include "adapter.h"
#include "interrupt.h"

static struct vring_desc *ivshm_net_rx_desc(struct ivshm_net_queue *rx)
{
	struct vring *vr = &rx->vr;
	unsigned int avail;
	u16 avail_idx;

	avail_idx = vr->avail->idx;

	if (avail_idx == rx->last_avail_idx)
		return NULL;

	avail = vr->avail->ring[rx->last_avail_idx++ & (vr->num - 1)];
	if (avail >= vr->num) {
		TraceLoggingWrite(
			IvshmemNetTraceProvider,
			"rx avail is invalid",
			TraceLoggingLevel(TRACE_LEVEL_ERROR),
			TraceLoggingUInt32(avail, "avail")
		);
		return NULL;
	}

	return &vr->desc[avail];
}

static void ivshm_net_rx_finish(struct ivshm_net_queue *rx, struct vring_desc *desc)
{
	struct vring *vr = &rx->vr;
	UINT32 desc_id = (UINT32)(desc - vr->desc);
	unsigned int used;

	used = rx->last_used_idx++ & (vr->num - 1);
	vr->used->ring[used].id = desc_id;
	vr->used->ring[used].len = 1;

	MemoryBarrier();
	vr->used->idx = rx->last_used_idx;
}

static void fill_in_pkt_layout(NET_PACKET *packet, void *data, int len)
{
	NET_PACKET_LAYOUT *layout = &packet->Layout;
	PETHERNET_HEADER eth_header;

	if (len < ETH_LENGTH_OF_HEADER) {
		return;
	}

	layout->Layer2Type = NET_PACKET_LAYER2_TYPE_ETHERNET;
	layout->Layer2HeaderLength = ETH_LENGTH_OF_HEADER;
	eth_header = (PETHERNET_HEADER)data;

	if (RtlUshortByteSwap(eth_header->Type) == NDIS_ETH_TYPE_IPV4) {
		PIPV4_HEADER ip_header = (PIPV4_HEADER)(eth_header + 1);
		
		if (ip_header->Version == 4) {
			packet->Layout.Layer3Type = NET_PACKET_LAYER3_TYPE_IPV4_UNSPECIFIED_OPTIONS;
		}
		else if (ip_header->Version == 6) {
			packet->Layout.Layer3Type = NET_PACKET_LAYER3_TYPE_IPV6_UNSPECIFIED_EXTENSIONS;
		}

		if (ip_header->Protocol == IPPROTO_TCP) {
			packet->Layout.Layer4Type = NET_PACKET_LAYER4_TYPE_TCP;
		}
		else if (ip_header->Protocol == IPPROTO_UDP) {
			packet->Layout.Layer4Type = NET_PACKET_LAYER4_TYPE_UDP;
		}
	}

	TraceLoggingWrite(
		IvshmemNetTraceProvider,
		"determine_layout",
		TraceLoggingUInt8(packet->Layout.Layer3Type, "packet->Layout.Layer3Type"),
		TraceLoggingUInt8(packet->Layout.Layer4Type, "packet->Layout.Layer4Type")
	);
}


static void ivshm_net_notify_rx(IVN_ADAPTER *adapter, UINT32 num)
{
	u16 evt, old, cur;
	struct ivshm_net_queue *q = &adapter->rx_queue;

	MemoryBarrier();

	evt = vring_used_event(&q->vr);
	old = q->last_used_idx - (UINT16)num;
	cur = q->last_used_idx;

	int should_notify = vring_need_event(evt, cur, old);
	TraceLoggingWrite(
		IvshmemNetTraceProvider,
		"ivshm_net_notify_rx",
		TraceLoggingUInt32(old, "old"),
		TraceLoggingUInt32(evt, "evt"),
		TraceLoggingUInt32(cur, "cur"),
		TraceLoggingUInt32(should_notify, "should_notify")
	);

	if (should_notify) {
		notifyPeer(adapter);
	}
}

void
RxIndicateReceives(
    _In_ IVN_RXQUEUE *rx
    )
{
    PCNET_DATAPATH_DESCRIPTOR descriptor = rx->DatapathDescriptor;
    NET_RING_BUFFER *rb = NET_DATAPATH_DESCRIPTOR_GET_PACKET_RING_BUFFER(descriptor);
	IVN_ADAPTER *adapter = rx->Adapter;
	struct ivshm_net_queue *q = &adapter->rx_queue;
	struct vring_desc *desc;
	void *data;
	u32 len;
	TraceEntry();

    UINT32 i, num = 0;
    for (i = rb->BeginIndex; i != rb->NextIndex; i = NetRingBufferIncrementIndex(rb, i))
    {
		desc = ivshm_net_rx_desc(q);
		if (!desc)
			break;

		data = ivshm_net_desc_data(adapter, q, desc, &len);
		if (!data) {
			TraceLoggingWrite(
				IvshmemNetTraceProvider,
				"rx data is NULL",
				TraceLoggingLevel(TRACE_LEVEL_ERROR),
				TraceLoggingHexUInt64(desc->addr, "desc->addr"),
				TraceLoggingUInt32(desc->len, "desc->len"),
				TraceLoggingHexUInt16(desc->flags, "desc->flags"),
				TraceLoggingUInt16(desc->next, "desc->next")
			);
			break;
		}

		TraceLoggingWrite(
			IvshmemNetTraceProvider,
			"receive a pkt",
			TraceLoggingHexUInt64(desc->addr, "desc->addr"),
			TraceLoggingUInt32(desc->len, "desc->len"),
			TraceLoggingHexUInt16(desc->flags, "desc->flags"),
			TraceLoggingUInt16(desc->next, "desc->next")
		);

		NET_PACKET *packet = NetRingBufferGetPacketAtIndex(descriptor, i);
		auto fragment = NET_PACKET_GET_FRAGMENT(packet, descriptor, 0);
		RtlCopyMemory(fragment->VirtualAddress, data, len);
		fragment->ValidLength = len;
		fragment->Offset = 0;

		fill_in_pkt_layout(packet, data, len);
		
		ivshm_net_rx_finish(q, desc);
		rx->Adapter->InUcastOctets += len;
		num++;
    }

    rb->BeginIndex = i;

	if (num) {
		ivshm_net_notify_rx(adapter, num);
	}
	TraceExit();
}

void
RxPostBuffers(
    _In_ IVN_RXQUEUE *rx
    )
{
    PCNET_DATAPATH_DESCRIPTOR descriptor = rx->DatapathDescriptor;

	TraceEntry();

    while (true)
    {
        NET_PACKET *packet = NetRingBufferAdvanceNextPacket(descriptor);
        if (!packet)
            break;
    }

	TraceExit();
}

void reset_rx_queue(_In_ IVN_ADAPTER * adapter) {
	TraceEntry();
	ivshm_net_init_queue(adapter, &adapter->rx_queue, adapter->peer_queue, adapter->qlen);

	// tx queue should have been initialized, swap tx and rx used rings.
	struct vring_used *tmp = adapter->tx_queue.vr.used;
	adapter->tx_queue.vr.used = adapter->rx_queue.vr.used;
	adapter->rx_queue.vr.used = tmp;
	TraceExit();
}

NTSTATUS
IvshmemNetRxQueueInitialize(
    _In_ NETPACKETQUEUE rxQueue,
    _In_ IVN_ADAPTER *adapter
    )
{
	TraceEntry(TraceLoggingPointer(rxQueue, "RxQueue"));
    NTSTATUS status = STATUS_SUCCESS;

    IVN_RXQUEUE *rx = IvshmemNetGetRxQueueContext(rxQueue);

    rx->Adapter = adapter;
    rx->Interrupt = adapter->Interrupt;
    rx->DatapathDescriptor = NetRxQueueGetDatapathDescriptor(rxQueue);

	reset_rx_queue(adapter);

	TraceExitResult(status);
    return status;
}

ULONG
IvshmemNetConvertPacketFilterToRcr(
    _In_ NET_PACKET_FILTER_TYPES_FLAGS packetFilter
    )
{
    if (packetFilter & NET_PACKET_FILTER_TYPE_PROMISCUOUS)
    {
        return (RCR_AAP | RCR_APM | RCR_AM | RCR_AB | RCR_AR | RCR_AER);
    }

    return
        ((packetFilter & NET_PACKET_FILTER_TYPE_ALL_MULTICAST) ? RCR_AM  : 0) |
        ((packetFilter & NET_PACKET_FILTER_TYPE_MULTICAST)     ? RCR_AM  : 0) |
        ((packetFilter & NET_PACKET_FILTER_TYPE_BROADCAST)     ? RCR_AB  : 0) |
        ((packetFilter & NET_PACKET_FILTER_TYPE_DIRECTED)      ? RCR_APM : 0);
}

void
IvshmemNetRxQueueSetInterrupt(
    _In_ IVN_RXQUEUE *rx,
    _In_ BOOLEAN notificationEnabled
    )
{
    InterlockedExchange(&rx->Interrupt->RxNotifyArmed[rx->QueueId], notificationEnabled);

	if (!notificationEnabled) {
		// block this thread until we're sure any outstanding DPCs are complete.
		// This is to guarantee we don't return from this function call until
		// any oustanding rx notification is complete.
		KeFlushQueuedDpcs();
	}
	else {
		// let peer to notify us when sending packets.
		vring_avail_event(&rx->Adapter->rx_queue.vr) = rx->Adapter->rx_queue.last_avail_idx;
	}
}



_Use_decl_annotations_
void
EvtRxQueueStart(
    NETPACKETQUEUE rxQueue
    )
{
	TraceEntry(TraceLoggingPointer(rxQueue, "RxQueue"));
    IVN_RXQUEUE *rx = IvshmemNetGetRxQueueContext(rxQueue);
    IVN_ADAPTER *adapter = rx->Adapter;

    WdfSpinLockAcquire(adapter->Lock);

	adapter->RxQueues[rx->QueueId] = rxQueue;
    WdfSpinLockRelease(adapter->Lock);
	TraceExit();
}

_Use_decl_annotations_
void
EvtRxQueueStop(
    NETPACKETQUEUE rxQueue
    )
{
	TraceEntry(TraceLoggingPointer(rxQueue, "RxQueue"));
    IVN_RXQUEUE *rx = IvshmemNetGetRxQueueContext(rxQueue);
	//IVN_ADAPTER *adapter = rx->Adapter;

    WdfSpinLockAcquire(rx->Adapter->Lock);

    rx->Adapter->RxQueues[rx->QueueId] = WDF_NO_HANDLE;

    WdfSpinLockRelease(rx->Adapter->Lock);
	TraceExit();
}

_Use_decl_annotations_
void
EvtRxQueueDestroy(
    _In_ WDFOBJECT rxQueue
    )
{
    TraceEntry(TraceLoggingPointer(rxQueue, "RxQueue"));

    TraceExit();
}

_Use_decl_annotations_
VOID
EvtRxQueueSetNotificationEnabled(
    _In_ NETPACKETQUEUE rxQueue,
    _In_ BOOLEAN notificationEnabled
    )
{
    TraceEntry(TraceLoggingPointer(rxQueue), TraceLoggingBoolean(notificationEnabled));

    IVN_RXQUEUE *rx = IvshmemNetGetRxQueueContext(rxQueue);

	UNREFERENCED_PARAMETER(rx);
    IvshmemNetRxQueueSetInterrupt(rx, notificationEnabled);

    TraceExit();
}

_Use_decl_annotations_
void
EvtRxQueueAdvance(
    _In_ NETPACKETQUEUE rxQueue
    )
{
    TraceEntry(TraceLoggingPointer(rxQueue, "RxQueue"));

    IVN_RXQUEUE *rx = IvshmemNetGetRxQueueContext(rxQueue);

    RxIndicateReceives(rx);
    RxPostBuffers(rx);

    TraceExit();
}

_Use_decl_annotations_
void
EvtRxQueueCancel(
    _In_ NETPACKETQUEUE rxQueue
    )
{
    TraceEntry(TraceLoggingPointer(rxQueue, "RxQueue"));

    IVN_RXQUEUE *rx = IvshmemNetGetRxQueueContext(rxQueue);

    // try (but not very hard) to grab anything that may have been
    // indicated during rx disable. advance will continue to be called
    // after cancel until all packets are returned to the framework.
    RxIndicateReceives(rx);

    NetRingBufferReturnAllPackets(NetRxQueueGetDatapathDescriptor(rxQueue));

    TraceExit();
}
