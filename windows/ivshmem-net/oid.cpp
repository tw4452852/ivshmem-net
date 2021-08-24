/*++

    THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
    ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
    THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
    PARTICULAR PURPOSE.

    Copyright (c) Microsoft Corporation. All rights reserved

--*/

#include "precomp.h"

#include "trace.h"
#include "adapter.h"
#include "device.h"
#include "oid.h"
#include "statistics.h"
#include "rxqueue.h"
#include "link.h"


#define RTK_NIC_GBE_PCIE_ADAPTER_NAME "IvshmemNet PCIe GBE Family Controller"


// OID_GEN_VENDOR_DESCRIPTION
void
EvtNetRequestQueryVendorDescription(
    _In_ NETREQUESTQUEUE RequestQueue,
    _In_ NETREQUEST Request,
    _Out_writes_bytes_(OutputBufferLength)
    PVOID OutputBuffer,
    UINT OutputBufferLength)
{
    __analysis_assume(OutputBufferLength >= sizeof(RTK_NIC_GBE_PCIE_ADAPTER_NAME));

    UNREFERENCED_PARAMETER((RequestQueue, OutputBufferLength));

    TraceEntry();

    RtlCopyMemory(OutputBuffer, RTK_NIC_GBE_PCIE_ADAPTER_NAME, sizeof(RTK_NIC_GBE_PCIE_ADAPTER_NAME));

    NetRequestQueryDataComplete(Request, STATUS_SUCCESS, sizeof(RTK_NIC_GBE_PCIE_ADAPTER_NAME));

    TraceExit();
}

// OID_OFFLOAD_ENCAPSULATION
void
EvtNetRequestQueryOffloadEncapsulation(
    _In_ NETREQUESTQUEUE RequestQueue,
    _In_ NETREQUEST Request,
    _Out_writes_bytes_(OutputBufferLength)
    PVOID OutputBuffer,
    UINT OutputBufferLength)
{
    __analysis_assume(OutputBufferLength >= sizeof(NDIS_OFFLOAD_ENCAPSULATION));

    UNREFERENCED_PARAMETER((OutputBufferLength));

    NETADAPTER netAdapter = NetRequestQueueGetAdapter(RequestQueue);
    IVN_ADAPTER *adapter = IvshmemNetGetAdapterContext(netAdapter);

    TraceEntryIvshmemNetAdapter(adapter);

    RtlCopyMemory(OutputBuffer, &adapter->OffloadEncapsulation, sizeof(NDIS_OFFLOAD_ENCAPSULATION));

    NetRequestQueryDataComplete(Request, STATUS_SUCCESS, sizeof(NDIS_OFFLOAD_ENCAPSULATION));

    TraceExit();
}

void
EvtNetRequestQuerySuccess(
    _In_ NETREQUESTQUEUE RequestQueue,
    _In_ NETREQUEST Request,
    _Out_writes_bytes_(OutputBufferLength)
    PVOID OutputBuffer,
    UINT OutputBufferLength)
{
    UNREFERENCED_PARAMETER((RequestQueue, OutputBuffer, OutputBufferLength));

    TraceEntry();

    NetRequestCompleteWithoutInformation(Request, STATUS_SUCCESS);

    TraceExit();
}

void
EvtNetRequestQueryUlong(
    _In_ NETREQUESTQUEUE RequestQueue,
    _In_ NETREQUEST Request,
    _Out_writes_bytes_(OutputBufferLength)
    PVOID OutputBuffer,
    UINT OutputBufferLength)
{
    __analysis_assume(OutputBufferLength >= sizeof(ULONG));

    UNREFERENCED_PARAMETER((OutputBufferLength));

    NDIS_OID oid = NetRequestGetId(Request);

    NETADAPTER netAdapter = NetRequestQueueGetAdapter(RequestQueue);
    IVN_ADAPTER *adapter = IvshmemNetGetAdapterContext(netAdapter);

    TraceEntryIvshmemNetAdapter(adapter, TraceLoggingUInt32(oid));

    ULONG result = 0;

    switch (oid)
    {
    case OID_GEN_HARDWARE_STATUS:
        result = NdisHardwareStatusReady;
        break;

    case OID_GEN_VENDOR_ID:
        result = *(PULONG)(adapter->PermanentAddress.Address);
        break;

    case OID_GEN_MEDIA_SUPPORTED:
    case OID_GEN_MEDIA_IN_USE:
        result = NdisMedium802_3;
        break;

    case OID_GEN_PHYSICAL_MEDIUM_EX:
        result = NdisPhysicalMedium802_3;
        break;

    case OID_GEN_CURRENT_LOOKAHEAD:
        // "Current Lookahead" is the number of bytes following the Ethernet header
        // that the NIC should indicate in the first NET_PACKET_FRAGMENT. Essentially
        // a Current Lookahead of 8 would mean that each indicated NET_PACKET's first
        // NET_PACKET_FRAGMENT would point to a buffer of at *least* size
        // ETH_LENGTH_OF_HEADER + 8.
        //
        // Since the RTL8168D *always* indicates all traffic in a single, contiguous buffer,
        // its driver just reports the maximum ethernet payload size as the current lookahead.
        __fallthrough;
    case OID_GEN_MAXIMUM_FRAME_SIZE:
        result = IVN_MAX_PACKET_SIZE - ETH_LENGTH_OF_HEADER;
        break;

    case OID_GEN_MAXIMUM_TOTAL_SIZE:
    case OID_GEN_TRANSMIT_BLOCK_SIZE:
    case OID_GEN_RECEIVE_BLOCK_SIZE:
        result = (ULONG)IVN_MAX_PACKET_SIZE;
        break;

    case OID_GEN_MAC_OPTIONS:
        result = NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA |
            NDIS_MAC_OPTION_TRANSFERS_NOT_PEND |
            NDIS_MAC_OPTION_NO_LOOPBACK;
        break;

    case OID_GEN_TRANSMIT_BUFFER_SPACE:
        result = IVN_MAX_PACKET_SIZE * adapter->NumTcb;
        break;

    case OID_GEN_RECEIVE_BUFFER_SPACE:
        if (adapter->RxQueues[0])
        {
            result = IVN_MAX_PACKET_SIZE * NET_DATAPATH_DESCRIPTOR_GET_PACKET_RING_BUFFER(NetRxQueueGetDatapathDescriptor(adapter->RxQueues[0]))->NumberOfElements;
        }
        else
        {
            result = 0;
        }
        break;

    case OID_GEN_VENDOR_DRIVER_VERSION:
        result = IVN_VENDOR_DRIVER_VERSION;
        break;

    case OID_802_3_MAXIMUM_LIST_SIZE:
        result = IVN_MAX_MCAST_LIST;
        break;

    default:
        NT_ASSERTMSG("Unexpected OID", false);
        break;
    }

    *(PULONG UNALIGNED)OutputBuffer = result;

    NetRequestQueryDataComplete(Request, STATUS_SUCCESS, sizeof(ULONG));

    TraceExit();
}


void
EvtNetRequestSetPacketFilter(
    _In_ NETREQUESTQUEUE RequestQueue,
    _In_ NETREQUEST      Request,
    _In_reads_bytes_(InputBufferLength)
    PVOID                InputBuffer,
    UINT                 InputBufferLength)
{
    UNREFERENCED_PARAMETER((InputBufferLength));

    NDIS_OID oid = NetRequestGetId(Request);

    NETADAPTER netAdapter = NetRequestQueueGetAdapter(RequestQueue);
    IVN_ADAPTER *adapter = IvshmemNetGetAdapterContext(netAdapter);

    TraceEntryIvshmemNetAdapter(adapter, TraceLoggingUInt32(oid));

    NET_PACKET_FILTER_TYPES_FLAGS packetFilter =
        *(NET_PACKET_FILTER_TYPES_FLAGS UNALIGNED*)InputBuffer;

    if (packetFilter & ~IVN_SUPPORTED_FILTERS)
    {
        NetRequestCompleteWithoutInformation(Request, STATUS_NOT_SUPPORTED);
        goto Exit;
    }

    WdfSpinLockAcquire(adapter->Lock); {

        adapter->PacketFilter = packetFilter;
        //IvshmemNetAdapterUpdateRcr(adapter);

        // Changing the packet filter might require clearing the active MCList
        //IvshmemNetAdapterPushMulticastList(adapter);

    } WdfSpinLockRelease(adapter->Lock);

    NetRequestSetDataComplete(Request, STATUS_SUCCESS, sizeof(ULONG));

Exit:

    TraceExit();
}

void
EvtNetRequestSetCurrentLookahead(
    _In_ NETREQUESTQUEUE RequestQueue,
    _In_ NETREQUEST      Request,
    _In_reads_bytes_(InputBufferLength)
    PVOID                InputBuffer,
    UINT                 InputBufferLength)
{
    UNREFERENCED_PARAMETER((InputBufferLength));

    NDIS_OID oid = NetRequestGetId(Request);

    NETADAPTER netAdapter = NetRequestQueueGetAdapter(RequestQueue);
    IVN_ADAPTER *adapter = IvshmemNetGetAdapterContext(netAdapter);

    TraceEntryIvshmemNetAdapter(adapter, TraceLoggingUInt32(oid));

    if (*(UNALIGNED PULONG) InputBuffer > (IVN_MAX_PACKET_SIZE - ETH_LENGTH_OF_HEADER))
    {
        NetRequestCompleteWithoutInformation(Request, NDIS_STATUS_INVALID_DATA);
        goto Exit;
    }

    // the set "Current Lookahead" value is not used as the "Current Lookahead" for the
    // RTL8168D is *always* the maximum payload size.

    NetRequestSetDataComplete(Request, STATUS_SUCCESS, sizeof(ULONG));

Exit:

    TraceExit();
}

void
EvtNetRequestSetOffloadEncapsulation(
    _In_ NETREQUESTQUEUE RequestQueue,
    _In_ NETREQUEST      Request,
    _In_reads_bytes_(InputBufferLength)
    PVOID                InputBuffer,
    UINT                 InputBufferLength)
{
    UNREFERENCED_PARAMETER(InputBufferLength);

    NDIS_OID oid = NetRequestGetId(Request);

    NETADAPTER netAdapter = NetRequestQueueGetAdapter(RequestQueue);
    IVN_ADAPTER *adapter = IvshmemNetGetAdapterContext(netAdapter);

    TraceEntryIvshmemNetAdapter(adapter, TraceLoggingUInt32(oid));

    NDIS_OFFLOAD_ENCAPSULATION *setEncapsulation = (NDIS_OFFLOAD_ENCAPSULATION*)InputBuffer;

    adapter->OffloadEncapsulation.IPv4.Enabled           = setEncapsulation->IPv4.Enabled;
    adapter->OffloadEncapsulation.IPv4.EncapsulationType = setEncapsulation->IPv4.EncapsulationType;
    adapter->OffloadEncapsulation.IPv4.HeaderSize        = setEncapsulation->IPv4.HeaderSize;

    adapter->OffloadEncapsulation.IPv6.Enabled           = setEncapsulation->IPv6.Enabled;
    adapter->OffloadEncapsulation.IPv6.EncapsulationType = setEncapsulation->IPv6.EncapsulationType;
    adapter->OffloadEncapsulation.IPv6.HeaderSize        = setEncapsulation->IPv6.HeaderSize;

    NetRequestSetDataComplete(Request, STATUS_SUCCESS, sizeof(NDIS_OFFLOAD_ENCAPSULATION));

    TraceExit();
}

void
EvtNetRequestQueryInterruptModeration(
    _In_ NETREQUESTQUEUE RequestQueue,
    _In_ NETREQUEST Request,
    _Out_writes_bytes_(OutputBufferLength)
    PVOID OutputBuffer,
    _Pre_satisfies_(OutputBufferLength >= NDIS_SIZEOF_INTERRUPT_MODERATION_PARAMETERS_REVISION_1)
    UINT OutputBufferLength)
{
    NETADAPTER netAdapter = NetRequestQueueGetAdapter(RequestQueue);
    IVN_ADAPTER *adapter = IvshmemNetGetAdapterContext(netAdapter);
    NDIS_INTERRUPT_MODERATION_PARAMETERS *imParameters = (NDIS_INTERRUPT_MODERATION_PARAMETERS*)OutputBuffer;

    TraceEntryIvshmemNetAdapter(adapter);

    RtlZeroMemory(imParameters, OutputBufferLength);

    imParameters->Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    imParameters->Header.Revision = NDIS_INTERRUPT_MODERATION_PARAMETERS_REVISION_1;
    imParameters->Header.Size = NDIS_SIZEOF_INTERRUPT_MODERATION_PARAMETERS_REVISION_1;

    if (adapter->InterruptModerationMode == IvshmemNetInterruptModerationDisabled)
    {
        imParameters->InterruptModeration = NdisInterruptModerationNotSupported;
    }
    else if (adapter->InterruptModerationDisabled)
    {
        imParameters->InterruptModeration = NdisInterruptModerationDisabled;
    }
    else
    {
        imParameters->InterruptModeration = NdisInterruptModerationEnabled;
    }

    NetRequestQueryDataComplete(Request, STATUS_SUCCESS, NDIS_SIZEOF_INTERRUPT_MODERATION_PARAMETERS_REVISION_1);

    TraceExit();
}

void
EvtNetRequestSetInterruptModeration(
    _In_ NETREQUESTQUEUE RequestQueue,
    _In_ NETREQUEST      Request,
    _In_reads_bytes_(InputBufferLength)
    PVOID                InputBuffer,
    UINT                 InputBufferLength)
{
    UNREFERENCED_PARAMETER((InputBufferLength));

    NETADAPTER netAdapter = NetRequestQueueGetAdapter(RequestQueue);
    IVN_ADAPTER *adapter = IvshmemNetGetAdapterContext(netAdapter);
    NDIS_INTERRUPT_MODERATION_PARAMETERS *imParameters = (NDIS_INTERRUPT_MODERATION_PARAMETERS*)InputBuffer;

    TraceEntryIvshmemNetAdapter(adapter);

    if (adapter->InterruptModerationMode == IvshmemNetInterruptModerationDisabled)
    {
        NetRequestSetDataComplete(Request, NDIS_STATUS_INVALID_DATA, NDIS_SIZEOF_INTERRUPT_MODERATION_PARAMETERS_REVISION_1);
    }
    else
    {
        switch (imParameters->InterruptModeration)
        {
        case NdisInterruptModerationEnabled:
            adapter->InterruptModerationDisabled = false;
            break;

        case NdisInterruptModerationDisabled:
            adapter->InterruptModerationDisabled = true;
            break;
        }

        //IvshmemNetAdapterUpdateInterruptModeration(adapter);

        NetRequestSetDataComplete(Request, STATUS_SUCCESS, NDIS_SIZEOF_INTERRUPT_MODERATION_PARAMETERS_REVISION_1);
    }

    TraceExit();
}


typedef struct _IVN_OID_QUERY {
    NDIS_OID Oid;
    PFN_NET_REQUEST_QUERY_DATA EvtQueryData;
    UINT MinimumSize;
} IVN_OID_QUERY, *PIVN_OID_QUERY;

const IVN_OID_QUERY ComplexQueries[] = {
    { OID_GEN_STATISTICS,           EvtNetRequestQueryAllStatistics,        sizeof(NDIS_STATISTICS_INFO) },
    { OID_GEN_VENDOR_DESCRIPTION,   EvtNetRequestQueryVendorDescription,    sizeof(RTK_NIC_GBE_PCIE_ADAPTER_NAME) },
    { OID_OFFLOAD_ENCAPSULATION,    EvtNetRequestQueryOffloadEncapsulation, sizeof(NDIS_OFFLOAD_ENCAPSULATION) },
    { OID_PNP_QUERY_POWER,          EvtNetRequestQuerySuccess,              0 },
    { OID_GEN_INTERRUPT_MODERATION, EvtNetRequestQueryInterruptModeration,  NDIS_SIZEOF_INTERRUPT_MODERATION_PARAMETERS_REVISION_1 },
};

typedef struct _IVN_OID_SET {
    NDIS_OID Oid;
    PFN_NET_REQUEST_SET_DATA EvtSetData;
    UINT MinimumSize;
} IVN_OID_SET, *PIVN_OID_SET;

const IVN_OID_SET ComplexSets[] = {
    //{ OID_802_3_MULTICAST_LIST,      EvtNetRequestSetMulticastList,        0 },
    { OID_GEN_CURRENT_PACKET_FILTER, EvtNetRequestSetPacketFilter,         sizeof(ULONG) },
    { OID_GEN_CURRENT_LOOKAHEAD,     EvtNetRequestSetCurrentLookahead,     sizeof(ULONG) },
    { OID_OFFLOAD_ENCAPSULATION,     EvtNetRequestSetOffloadEncapsulation, sizeof(NDIS_OFFLOAD_ENCAPSULATION) },
    { OID_GEN_INTERRUPT_MODERATION,  EvtNetRequestSetInterruptModeration,  NDIS_SIZEOF_INTERRUPT_MODERATION_PARAMETERS_REVISION_1 },
};

const NDIS_OID UlongQueries[] = {
    OID_GEN_HARDWARE_STATUS,
    OID_GEN_VENDOR_ID,
    OID_GEN_MEDIA_SUPPORTED,
    OID_GEN_MEDIA_IN_USE,
    OID_GEN_PHYSICAL_MEDIUM_EX,
    OID_GEN_CURRENT_LOOKAHEAD,
    OID_GEN_MAXIMUM_FRAME_SIZE,
    OID_GEN_MAXIMUM_TOTAL_SIZE,
    OID_GEN_TRANSMIT_BLOCK_SIZE,
    OID_GEN_RECEIVE_BLOCK_SIZE,
    OID_GEN_MAC_OPTIONS,
    OID_GEN_TRANSMIT_BUFFER_SPACE,
    OID_GEN_RECEIVE_BUFFER_SPACE,
    OID_GEN_VENDOR_DRIVER_VERSION,
    OID_802_3_MAXIMUM_LIST_SIZE,
};

const NDIS_OID StatisticQueries[] = {
    OID_GEN_XMIT_OK,
    OID_GEN_RCV_OK,
    OID_802_3_XMIT_ONE_COLLISION,
    OID_802_3_XMIT_MORE_COLLISIONS,
    OID_802_3_XMIT_MAX_COLLISIONS,
    OID_802_3_XMIT_UNDERRUN,
};

NTSTATUS
IvshmemNetInitializeAdapterRequestQueue(
    _In_ IVN_ADAPTER *adapter)
{
    NTSTATUS status;
    NET_REQUEST_QUEUE_CONFIG queueConfig;

    TraceEntryIvshmemNetAdapter(adapter);

    NET_REQUEST_QUEUE_CONFIG_INIT_DEFAULT_SEQUENTIAL(
        &queueConfig,
        adapter->NetAdapter);

    // registers those OIDs that can be completed as ULONGs
    for (ULONG i = 0; i < ARRAYSIZE(UlongQueries); i++)
    {
        NET_REQUEST_QUEUE_CONFIG_ADD_QUERY_DATA_HANDLER(
            &queueConfig,
            UlongQueries[i],
            EvtNetRequestQueryUlong,
            sizeof(ULONG));
    }

    // registers individual statistic OIDs
    for (ULONG i = 0; i < ARRAYSIZE(StatisticQueries); i++)
    {
        NET_REQUEST_QUEUE_CONFIG_ADD_QUERY_DATA_HANDLER(
            &queueConfig,
            StatisticQueries[i],
            EvtNetRequestQueryIndividualStatistics,
            sizeof(ULONG));
    }

    // registers query OIDs with complex behaviors
    for (ULONG i = 0; i < ARRAYSIZE(ComplexQueries); i++)
    {
        NET_REQUEST_QUEUE_CONFIG_ADD_QUERY_DATA_HANDLER(
            &queueConfig,
            ComplexQueries[i].Oid,
            ComplexQueries[i].EvtQueryData,
            ComplexQueries[i].MinimumSize);
    }

    // registers set OIDs with complex behaviors
    for (ULONG i = 0; i < ARRAYSIZE(ComplexSets); i++)
    {
        NET_REQUEST_QUEUE_CONFIG_ADD_SET_DATA_HANDLER(
            &queueConfig,
            ComplexSets[i].Oid,
            ComplexSets[i].EvtSetData,
            ComplexSets[i].MinimumSize);
    }


    //
    // Create the default NETREQUESTQUEUE.
    //
    GOTO_IF_NOT_NT_SUCCESS(Exit, status,
        NetRequestQueueCreate(&queueConfig, WDF_NO_OBJECT_ATTRIBUTES, NULL));

Exit:
    TraceExitResult(status);
    return status;
}