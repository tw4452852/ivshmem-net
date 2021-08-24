/*++

    THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
    ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
    THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
    PARTICULAR PURPOSE.

    Copyright (c) Microsoft Corporation. All rights reserved

--*/

#include "precomp.h"

#include "trace.h"
#include "device.h"
#include "adapter.h"
#include "oid.h"
#include "txqueue.h"
#include "rxqueue.h"
#include "queue.h"

#define ETH_IS_ZERO(Address) ( \
    (((PUCHAR)(Address))[0] == ((UCHAR)0x00)) && \
    (((PUCHAR)(Address))[1] == ((UCHAR)0x00)) && \
    (((PUCHAR)(Address))[2] == ((UCHAR)0x00)) && \
    (((PUCHAR)(Address))[3] == ((UCHAR)0x00)) && \
    (((PUCHAR)(Address))[4] == ((UCHAR)0x00)) && \
    (((PUCHAR)(Address))[5] == ((UCHAR)0x00)))

NTSTATUS
IvshmemNetInitializeAdapterContext(
    _In_ IVN_ADAPTER *adapter,
    _In_ WDFDEVICE device,
    _In_ NETADAPTER netAdapter
    )
/*++
Routine Description:

    Allocate IVN_ADAPTER data block and do some initialization

Arguments:

    adapter     Pointer to receive pointer to our adapter

Return Value:

    NTSTATUS failure code, or STATUS_SUCCESS

--*/
{
    TraceEntry();

    NTSTATUS status = STATUS_SUCCESS;

    adapter->NetAdapter = netAdapter;
    adapter->WdfDevice = device;

    //
    // Get WDF miniport device context.
    //
    IvshmemNetGetDeviceContext(adapter->WdfDevice)->Adapter = adapter;

    RtlZeroMemory(adapter->RssIndirectionTable, sizeof(adapter->RssIndirectionTable));

    adapter->OffloadEncapsulation.Header.Revision = NDIS_OFFLOAD_ENCAPSULATION_REVISION_1;
    adapter->OffloadEncapsulation.Header.Size = NDIS_SIZEOF_OFFLOAD_ENCAPSULATION_REVISION_1;
    adapter->OffloadEncapsulation.Header.Type = NDIS_OBJECT_TYPE_OFFLOAD_ENCAPSULATION;

    adapter->EEPROMInUse = false;

    //spinlock
    WDF_OBJECT_ATTRIBUTES  attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = adapter->WdfDevice;

    GOTO_IF_NOT_NT_SUCCESS(Exit, status,
        WdfSpinLockCreate(&attributes, &adapter->Lock));

Exit:
    TraceExitResult(status);

    return status;

}



void
IvshmemNetAdapterQueryHardwareCapabilities(
    _Out_ NDIS_OFFLOAD *hardwareCaps
    )
{
    // TODO: when vlan is implemented, each of TCP_OFFLOAD's encapsulation
    // type has to be updated to include NDIS_ENCAPSULATION_IEEE_802_3_P_AND_Q_IN_OOB

    RtlZeroMemory(hardwareCaps, sizeof(*hardwareCaps));

    hardwareCaps->Header.Type = NDIS_OBJECT_TYPE_OFFLOAD;
    hardwareCaps->Header.Size = NDIS_SIZEOF_NDIS_OFFLOAD_REVISION_5;
    hardwareCaps->Header.Revision = NDIS_OFFLOAD_REVISION_5;

    // IPv4 checksum offloads supported
    hardwareCaps->Checksum.IPv4Transmit.Encapsulation = NDIS_ENCAPSULATION_IEEE_802_3;
    hardwareCaps->Checksum.IPv4Transmit.IpChecksum = NDIS_OFFLOAD_SUPPORTED;
    hardwareCaps->Checksum.IPv4Transmit.IpOptionsSupported = NDIS_OFFLOAD_SUPPORTED;
    hardwareCaps->Checksum.IPv4Transmit.TcpChecksum = NDIS_OFFLOAD_SUPPORTED;
    hardwareCaps->Checksum.IPv4Transmit.TcpOptionsSupported = NDIS_OFFLOAD_SUPPORTED;
    hardwareCaps->Checksum.IPv4Transmit.UdpChecksum = NDIS_OFFLOAD_SUPPORTED;

    hardwareCaps->Checksum.IPv4Receive.Encapsulation = NDIS_ENCAPSULATION_IEEE_802_3;
    hardwareCaps->Checksum.IPv4Receive.IpChecksum = NDIS_OFFLOAD_SUPPORTED;
    hardwareCaps->Checksum.IPv4Receive.IpOptionsSupported = NDIS_OFFLOAD_SUPPORTED;
    hardwareCaps->Checksum.IPv4Receive.TcpChecksum = NDIS_OFFLOAD_SUPPORTED;
    hardwareCaps->Checksum.IPv4Receive.TcpOptionsSupported = NDIS_OFFLOAD_SUPPORTED;
    hardwareCaps->Checksum.IPv4Receive.UdpChecksum = NDIS_OFFLOAD_SUPPORTED;

    // IPv6 checksum offloads supported
    hardwareCaps->Checksum.IPv6Transmit.Encapsulation = NDIS_ENCAPSULATION_IEEE_802_3;
    hardwareCaps->Checksum.IPv6Transmit.IpExtensionHeadersSupported = NDIS_OFFLOAD_SUPPORTED;
    hardwareCaps->Checksum.IPv6Transmit.TcpChecksum = NDIS_OFFLOAD_SUPPORTED;
    hardwareCaps->Checksum.IPv6Transmit.TcpOptionsSupported = NDIS_OFFLOAD_SUPPORTED;
    hardwareCaps->Checksum.IPv6Transmit.UdpChecksum = NDIS_OFFLOAD_SUPPORTED;

    hardwareCaps->Checksum.IPv6Receive.Encapsulation = NDIS_ENCAPSULATION_IEEE_802_3;
    hardwareCaps->Checksum.IPv6Receive.IpExtensionHeadersSupported = NDIS_OFFLOAD_SUPPORTED;
    hardwareCaps->Checksum.IPv6Receive.TcpChecksum = NDIS_OFFLOAD_SUPPORTED;
    hardwareCaps->Checksum.IPv6Receive.TcpOptionsSupported = NDIS_OFFLOAD_SUPPORTED;
    hardwareCaps->Checksum.IPv6Receive.UdpChecksum = NDIS_OFFLOAD_SUPPORTED;

    // LSOv1 IPv4 offload NOT supported
    hardwareCaps->LsoV1.IPv4.Encapsulation = NDIS_ENCAPSULATION_NOT_SUPPORTED;
    hardwareCaps->LsoV1.IPv4.MaxOffLoadSize = 0;
    hardwareCaps->LsoV1.IPv4.MinSegmentCount = 0;
    hardwareCaps->LsoV1.IPv4.TcpOptions = NDIS_OFFLOAD_NOT_SUPPORTED;
    hardwareCaps->LsoV1.IPv4.IpOptions = NDIS_OFFLOAD_NOT_SUPPORTED;

    // LSOv2 IPv4 offload supported
    hardwareCaps->LsoV2.IPv4.Encapsulation = NDIS_ENCAPSULATION_IEEE_802_3;
    hardwareCaps->LsoV2.IPv4.MaxOffLoadSize = IVN_LSO_OFFLOAD_MAX_SIZE;
    hardwareCaps->LsoV2.IPv4.MinSegmentCount = IVN_LSO_OFFLOAD_MIN_SEGMENT_COUNT;

    // LSOv2 IPv6 offload supported
    hardwareCaps->LsoV2.IPv6.Encapsulation = NDIS_ENCAPSULATION_IEEE_802_3;
    hardwareCaps->LsoV2.IPv6.MaxOffLoadSize = IVN_LSO_OFFLOAD_MAX_SIZE;
    hardwareCaps->LsoV2.IPv6.MinSegmentCount = IVN_LSO_OFFLOAD_MIN_SEGMENT_COUNT;
    hardwareCaps->LsoV2.IPv6.IpExtensionHeadersSupported = NDIS_OFFLOAD_SUPPORTED;
    hardwareCaps->LsoV2.IPv6.TcpOptionsSupported = NDIS_OFFLOAD_SUPPORTED;
}

_Use_decl_annotations_
NTSTATUS
EvtAdapterCreateTxQueue(
    _In_ NETADAPTER netAdapter,
    _Inout_ PNETTXQUEUE_INIT txQueueInit
    )
{
    NTSTATUS status = STATUS_SUCCESS;

    TraceEntryNetAdapter(netAdapter);

    IVN_ADAPTER *adapter = IvshmemNetGetAdapterContext(netAdapter);

#pragma region Create NETTXQUEUE

    WDF_OBJECT_ATTRIBUTES txAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&txAttributes, IVN_TXQUEUE);

    txAttributes.EvtDestroyCallback = EvtTxQueueDestroy;

    NET_PACKET_QUEUE_CONFIG txConfig;
    NET_PACKET_QUEUE_CONFIG_INIT(
        &txConfig,
        EvtTxQueueAdvance,
        EvtTxQueueSetNotificationEnabled,
        EvtTxQueueCancel);
    txConfig.EvtStart = EvtTxQueueStart;
    txConfig.EvtStop = EvtTxQueueStop;

    NET_PACKET_CONTEXT_ATTRIBUTES contextAttributes;
    NET_PACKET_CONTEXT_ATTRIBUTES_INIT_TYPE(&contextAttributes, IVN_TCB);

    GOTO_IF_NOT_NT_SUCCESS(Exit, status,
        NetTxQueueInitAddPacketContextAttributes(txQueueInit, &contextAttributes));

    NETPACKETQUEUE txQueue;
    GOTO_IF_NOT_NT_SUCCESS(Exit, status,
        NetTxQueueCreate(
            txQueueInit,
            &txAttributes,
            &txConfig,
            &txQueue));

#pragma endregion

#pragma region Initialize RTL8168D Transmit Queue

    GOTO_IF_NOT_NT_SUCCESS(Exit, status,
        IvshmemNetTxQueueInitialize(txQueue, adapter));

#pragma endregion

Exit:
    TraceExitResult(status);

    return status;
}

_Use_decl_annotations_
NTSTATUS
EvtAdapterCreateRxQueue(
    _In_ NETADAPTER netAdapter,
    _Inout_ PNETRXQUEUE_INIT rxQueueInit
    )
{
    NTSTATUS status = STATUS_SUCCESS;

    TraceEntryNetAdapter(netAdapter);

    IVN_ADAPTER *adapter = IvshmemNetGetAdapterContext(netAdapter);

#pragma region Create NETRXQUEUE

    WDF_OBJECT_ATTRIBUTES rxAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&rxAttributes, IVN_RXQUEUE);

    rxAttributes.EvtDestroyCallback = EvtRxQueueDestroy;

    NET_PACKET_QUEUE_CONFIG rxConfig;
    NET_PACKET_QUEUE_CONFIG_INIT(
        &rxConfig,
        EvtRxQueueAdvance,
        EvtRxQueueSetNotificationEnabled,
        EvtRxQueueCancel);
    rxConfig.EvtStart = EvtRxQueueStart;
    rxConfig.EvtStop = EvtRxQueueStop;

    const ULONG queueId = NetRxQueueInitGetQueueId(rxQueueInit);
    NETPACKETQUEUE rxQueue;
    GOTO_IF_NOT_NT_SUCCESS(Exit, status,
        NetRxQueueCreate(rxQueueInit, &rxAttributes, &rxConfig, &rxQueue));

#pragma endregion

#pragma region Initialize RTL8168D Receive Queue

    GOTO_IF_NOT_NT_SUCCESS(Exit, status,
        IvshmemNetRxQueueInitialize(rxQueue, adapter));

#pragma endregion

Exit:
    TraceExitResult(status);

    return status;
}



NTSTATUS
IvshmemNetAdapterReadAddress(
    _In_ IVN_ADAPTER *adapter
    )
/*++
Routine Description:

    Read the mac addresss from the adapter

Arguments:

    adapter     Pointer to our adapter

Return Value:

    STATUS_SUCCESS
    STATUS_NDIS_INVALID_ADDRESS

--*/
{
    TraceEntryIvshmemNetAdapter(adapter);

    NTSTATUS status = STATUS_SUCCESS;

    if (adapter->EEPROMSupported)
    {
        //IvshmemNetAdapterReadEEPROMPermanentAddress(adapter);
    }
    else
    {
		*((PUSHORT)(&adapter->PermanentAddress.Address[0])) = 0x00;
		*((PUSHORT)(&adapter->PermanentAddress.Address[1])) = 0x16;
		*((PUSHORT)(&adapter->PermanentAddress.Address[2])) = 0x3E;
		*((PUSHORT)(&adapter->PermanentAddress.Address[3])) = 0x00;
		*((PUSHORT)(&adapter->PermanentAddress.Address[4])) = adapter->peer_id;
		*((PUSHORT)(&adapter->PermanentAddress.Address[5])) = adapter->my_id;

        adapter->PermanentAddress.Length = ETHERNET_ADDRESS_LENGTH;
    }

    TraceLoggingWrite(
        IvshmemNetTraceProvider,
        "PermanentAddressRead",
        TraceLoggingBinary(&adapter->PermanentAddress.Address, adapter->PermanentAddress.Length, "PermanentAddress"));

    if (ETH_IS_MULTICAST(adapter->PermanentAddress.Address) ||
            ETH_IS_BROADCAST(adapter->PermanentAddress.Address) ||
            ETH_IS_ZERO(adapter->PermanentAddress.Address))
    {
        NdisWriteErrorLogEntry(
            adapter->NdisLegacyAdapterHandle,
            NDIS_ERROR_CODE_NETWORK_ADDRESS,
            0);

        GOTO_IF_NOT_NT_SUCCESS(Exit, status, STATUS_NDIS_INVALID_ADDRESS);
    }

    if (!adapter->OverrideAddress)
    {
        RtlCopyMemory(
            &adapter->CurrentAddress,
            &adapter->PermanentAddress,
            sizeof(adapter->PermanentAddress));
    }

Exit:
    TraceExitResult(status);

    return status;
}



ULONG
ComputeCrc(
    _In_reads_(length) UCHAR const *buffer,
         UINT length
    )
{
    ULONG crc = 0xffffffff;

    for (UINT i = 0; i < length; i++)
    {
        UCHAR curByte = buffer[i];

        for (UINT j = 0; j < 8; j++)
        {
            ULONG carry = ((crc & 0x80000000) ? 1 : 0) ^ (curByte & 0x01);
            crc <<= 1;
            curByte >>= 1;

            if (carry)
            {
                crc = (crc ^ 0x04c11db6) | carry;
            }
        }
    }

    return crc;
}

void
GetMulticastBit(
    _In_reads_(ETH_LENGTH_OF_ADDRESS) UCHAR const *address,
    _Out_ _Post_satisfies_(*byte < MAX_NIC_MULTICAST_REG) UCHAR *byte,
    _Out_ UCHAR *value
    )
/*++

Routine Description:

    For a given multicast address, returns the byte and bit in
    the card multicast registers that it hashes to. Calls
    ComputeCrc() to determine the CRC value.

--*/
{
    ULONG crc = ComputeCrc(address, ETH_LENGTH_OF_ADDRESS);

    // The bit number is now in the 6 most significant bits of CRC.
    UINT bitNumber = (UINT)((crc >> 26) & 0x3f);
    *byte = (UCHAR)(bitNumber / 8);
    *value = (UCHAR)((UCHAR)1 << (bitNumber % 8));
}




static
void
IvshmemNetAdapterSetLinkLayerCapabilities(
    _In_ IVN_ADAPTER *adapter
    )
{
    ULONG64 maxXmitLinkSpeed = IVN_MEDIA_MAX_SPEED;
    ULONG64 maxRcvLinkSpeed = IVN_MEDIA_MAX_SPEED;

    NET_ADAPTER_LINK_LAYER_CAPABILITIES linkLayerCapabilities;
    NET_ADAPTER_LINK_LAYER_CAPABILITIES_INIT(
        &linkLayerCapabilities,
        IVN_SUPPORTED_FILTERS,
        IVN_MAX_MCAST_LIST,
        NIC_SUPPORTED_STATISTICS,
        maxXmitLinkSpeed,
        maxRcvLinkSpeed);

    NetAdapterSetLinkLayerCapabilities(adapter->NetAdapter, &linkLayerCapabilities);
    NetAdapterSetLinkLayerMtuSize(adapter->NetAdapter, IVN_MAX_PACKET_SIZE - ETH_LENGTH_OF_HEADER);
    NetAdapterSetPermanentLinkLayerAddress(adapter->NetAdapter, &adapter->PermanentAddress);
    NetAdapterSetCurrentLinkLayerAddress(adapter->NetAdapter, &adapter->CurrentAddress);
}



static
void
IvshmemNetAdapterSetPowerCapabilities(
    _In_ IVN_ADAPTER const *adapter
    )
{
    NET_ADAPTER_POWER_CAPABILITIES powerCapabilities;
    NET_ADAPTER_POWER_CAPABILITIES_INIT(&powerCapabilities);

    powerCapabilities.SupportedWakePatterns = NET_ADAPTER_WAKE_MAGIC_PACKET;

    NetAdapterSetPowerCapabilities(adapter->NetAdapter, &powerCapabilities);
}

static
void
IvshmemNetAdapterSetDatapathCapabilities(
    _In_ IVN_ADAPTER const *adapter
    )
{
	TraceLoggingWrite(
		IvshmemNetTraceProvider,
		"tw",
		TraceLoggingUInt32(adapter->qlen, "qlen")
	);

	NET_ADAPTER_DMA_CAPABILITIES txDmaCapabilities;
	NET_ADAPTER_DMA_CAPABILITIES_INIT(&txDmaCapabilities, adapter->DmaEnabler);

    NET_ADAPTER_TX_CAPABILITIES txCapabilities;
    NET_ADAPTER_TX_CAPABILITIES_INIT_FOR_DMA(
        &txCapabilities,
		&txDmaCapabilities,
        IVN_MAX_FRAGMENT_SIZE,
        1);

    // LSO goes to 64K payload header + extra
    //sgConfig.MaximumPacketSize = IVN_MAX_FRAGMENT_SIZE * IVN_MAX_PHYS_BUF_COUNT;

    txCapabilities.FragmentRingNumberOfElementsHint = adapter->qlen;
    txCapabilities.MaximumNumberOfFragments = IVN_MAX_PHYS_BUF_COUNT;


	NET_ADAPTER_DMA_CAPABILITIES rxDmaCapabilities;
	NET_ADAPTER_DMA_CAPABILITIES_INIT(&rxDmaCapabilities, adapter->DmaEnabler);

    NET_ADAPTER_RX_CAPABILITIES rxCapabilities;
    NET_ADAPTER_RX_CAPABILITIES_INIT_SYSTEM_MANAGED_DMA(
        &rxCapabilities,
		&rxDmaCapabilities,
        IVN_MAX_PACKET_SIZE + FRAME_CRC_SIZE + RSVD_BUF_SIZE,
        1);

    //rxCapabilities.FragmentBufferAlignment = 64;
    rxCapabilities.FragmentRingNumberOfElementsHint = adapter->qlen;

    NetAdapterSetDataPathCapabilities(adapter->NetAdapter, &txCapabilities, &rxCapabilities);

}



_Use_decl_annotations_
NTSTATUS
IvshmemNetAdapterStart(
    IVN_ADAPTER *adapter
    )
{
    TraceEntryNetAdapter(adapter->NetAdapter);

    NTSTATUS status = STATUS_SUCCESS;

	GOTO_IF_NOT_NT_SUCCESS(
		Exit, status,
		ivshm_net_calc_qsize(adapter));

    IvshmemNetAdapterSetLinkLayerCapabilities(adapter);

    IvshmemNetAdapterSetPowerCapabilities(adapter);

    IvshmemNetAdapterSetDatapathCapabilities(adapter);


    GOTO_IF_NOT_NT_SUCCESS(
        Exit, status,
        NetAdapterStart(adapter->NetAdapter));

Exit:
    TraceExitResult(status);

    return status;
}

