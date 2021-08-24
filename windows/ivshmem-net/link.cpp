/*++

    THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
    ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
    THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
    PARTICULAR PURPOSE.

    Copyright (c) Microsoft Corporation. All rights reserved

--*/

#include "precomp.h"

#include "trace.h"
#include "link.h"
#include "adapter.h"
#include "device.h"
#include "txqueue.h"
#include "rxqueue.h"

NET_IF_MEDIA_DUPLEX_STATE
IvshmemNetAdapterGetDuplexSetting(_In_ IVN_ADAPTER const *adapter)
{
    switch (adapter->SpeedDuplex)
    {
    case IvshmemNetSpeedDuplexMode10MHalfDuplex:
    case IvshmemNetSpeedDuplexMode100MHalfDuplex:
        return MediaDuplexStateHalf;

    case IvshmemNetSpeedDuplexMode10MFullDuplex:
    case IvshmemNetSpeedDuplexMode100MFullDuplex:
    case IvshmemNetSpeedDuplexMode1GFullDuplex:
        return MediaDuplexStateFull;

    default:
        return MediaDuplexStateUnknown;
    }
}

USHORT
IvshmemNetAdapterGetLinkSpeedSetting(_In_ IVN_ADAPTER const *adapter)
{
    switch (adapter->SpeedDuplex)
    {
    case IvshmemNetSpeedDuplexMode10MHalfDuplex:
    case IvshmemNetSpeedDuplexMode10MFullDuplex:
        return 10;

    case IvshmemNetSpeedDuplexMode100MHalfDuplex:
    case IvshmemNetSpeedDuplexMode100MFullDuplex:
        return 100;

    case IvshmemNetSpeedDuplexMode1GFullDuplex:
        return 1000;

    default:
        return 0;
    }
}

void
IvshmemNetAdapterUpdateLinkStatus(
    _In_ IVN_ADAPTER *adapter)
{
	TraceEntryIvshmemNetAdapter(adapter);
    // deafult to user setting
    adapter->LinkSpeed = IvshmemNetAdapterGetLinkSpeedSetting(adapter);
    adapter->DuplexMode = IvshmemNetAdapterGetDuplexSetting(adapter);

	TraceExit();
}


void
IvshmemNetAdapterCompleteAutoNegotiation(
    _In_ IVN_ADAPTER *adapter)
{
	adapter->LinkAutoNeg = true;
	IvshmemNetAdapterUpdateLinkStatus(adapter);
}

static void
update_my_status(IVN_ADAPTER *adapter)
{
	UINT32 previous_status = *adapter->my_status;
	UINT32 peer_status = *adapter->peer_status;
	UINT32 my_status = previous_status;

	TraceEntry();
	if (peer_status >= IVSHM_NET_STATE_INIT) {
		my_status += 1;
		if (my_status >= IVSHM_NET_STATE_RUN) {
			my_status = IVSHM_NET_STATE_RUN;
		}
	}
	else {
		my_status = IVSHM_NET_STATE_RESET;
	}

	TraceLoggingWrite(
		IvshmemNetTraceProvider,
		"tw",
		TraceLoggingUInt32(previous_status, "previous_my_status"),
		TraceLoggingUInt32(my_status, "my_status"),
		TraceLoggingUInt32(peer_status, "peer_status")
	);

	if (my_status == previous_status) {
		return;
	}

	if (my_status == IVSHM_NET_STATE_READY) {
		reset_tx_queue(adapter);
		reset_rx_queue(adapter);
	}

	*adapter->my_status = my_status;
	MemoryBarrier();
	notifyPeer(adapter);
	TraceExit();
}

void
IvshmemNetAdapterNotifyLinkChange(IVN_ADAPTER *adapter)
{
    NET_ADAPTER_LINK_STATE linkState;

    WdfSpinLockAcquire(adapter->Lock); {

		update_my_status(adapter);
        IvshmemNetAdapterQueryLinkState(adapter, &linkState);

    } WdfSpinLockRelease(adapter->Lock);

    NetAdapterSetCurrentLinkState(adapter->NetAdapter, &linkState);
}

// Detects the NIC's current media state and updates the adapter context
// to reflect that updated media state of the hardware. The driver must serialize access
// to IVN_ADAPTER.
NDIS_MEDIA_CONNECT_STATE
IvshmemNetAdapterQueryMediaState(
    _In_ IVN_ADAPTER *adapter)
{
	TraceEntryIvshmemNetAdapter(adapter);
	UINT32 my_status = *adapter->my_status;
	UINT32 peer_status = *adapter->peer_status;
	NDIS_MEDIA_CONNECT_STATE ret = MediaConnectStateDisconnected;

	
    // Detect if auto-negotiation is complete and update adapter state with
    // link information.
    IvshmemNetAdapterCompleteAutoNegotiation(adapter);

	if (my_status == IVSHM_NET_STATE_RUN && peer_status == IVSHM_NET_STATE_RUN) {
		ret = MediaConnectStateConnected;
	}
	else {
		ret = MediaConnectStateDisconnected;
	}

	TraceExit();
	return ret;
}


_Use_decl_annotations_
void
IvshmemNetAdapterQueryLinkState(
    _In_    IVN_ADAPTER *adapter,
    _Inout_ NET_ADAPTER_LINK_STATE *linkState
)
/*++
Routine Description:
    This routine sends a NDIS_STATUS_LINK_STATE status up to NDIS

Arguments:

    adapter         Pointer to our adapter

Return Value:

    None
--*/

{
    TraceEntryIvshmemNetAdapter(adapter);

    NET_ADAPTER_AUTO_NEGOTIATION_FLAGS autoNegotiationFlags = NET_ADAPTER_AUTO_NEGOTIATION_NO_FLAGS;

    if (adapter->LinkAutoNeg)
    {
        autoNegotiationFlags |=
            NET_ADAPTER_LINK_STATE_XMIT_LINK_SPEED_AUTO_NEGOTIATED |
            NET_ADAPTER_LINK_STATE_RCV_LINK_SPEED_AUTO_NEGOTIATED |
            NET_ADAPTER_LINK_STATE_DUPLEX_AUTO_NEGOTIATED;
    }

    if (adapter->FlowControl != IvshmemNetFlowControlDisabled)
    {
        autoNegotiationFlags |=
            NET_ADAPTER_LINK_STATE_PAUSE_FUNCTIONS_AUTO_NEGOTIATED;
    }

    NET_ADAPTER_PAUSE_FUNCTIONS pauseFunctions = NetAdapterPauseFunctionsUnknown;

    switch (adapter->FlowControl)
    {
    case IvshmemNetFlowControlDisabled:
        pauseFunctions = NetAdapterPauseFunctionsUnsupported;
        break;
    case IvshmemNetFlowControlRxEnabled:
        pauseFunctions = NetAdapterPauseFunctionsReceiveOnly;
        break;
    case IvshmemNetFlowControlTxEnabled:
        pauseFunctions = NetAdapterPauseFunctionsSendOnly;
        break;
    case IvshmemNetFlowControlTxRxEnabled:
        pauseFunctions = NetAdapterPauseFunctionsSendAndReceive;
        break;
    }

	NDIS_MEDIA_CONNECT_STATE connected = IvshmemNetAdapterQueryMediaState(adapter);
    NET_ADAPTER_LINK_STATE_INIT(
        linkState,
        adapter->LinkSpeed * 1'000'000,
		connected,
        adapter->DuplexMode,
        pauseFunctions,
        autoNegotiationFlags);

	TraceLoggingWrite(
		IvshmemNetTraceProvider,
		"tw",
		TraceLoggingUInt32(adapter->LinkSpeed, "speed"),
		TraceLoggingUInt32(adapter->DuplexMode, "mode"),
		TraceLoggingUInt32(pauseFunctions, "pauseFunctions"),
		TraceLoggingUInt32(connected, "connected"),
		TraceLoggingUInt32(autoNegotiationFlags, "autoNegotiationFlags")
	);

    TraceExit();
}
