/*++

    THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
    ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
    THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
    PARTICULAR PURPOSE.

    Copyright (c) Microsoft Corporation. All rights reserved

--*/

#include "precomp.h"

#include "trace.h"
#include "power.h"
#include "device.h"
#include "adapter.h"
#include "link.h"
#include "interrupt.h"


void
IvshmemNetAdapterRaiseToD0(_In_ IVN_ADAPTER *adapter)
{
	UNREFERENCED_PARAMETER(adapter);
}

_Use_decl_annotations_
NTSTATUS
EvtDeviceD0Entry(
    _In_ WDFDEVICE wdfDevice,
         WDF_POWER_DEVICE_STATE previousState)
{
    IVN_ADAPTER *adapter = IvshmemNetGetDeviceContext(wdfDevice)->Adapter;

    TraceEntryIvshmemNetAdapter(
        adapter,
        TraceLoggingUInt32(previousState, "PreviousState"));

    // Interrupts will be fully enabled in EvtInterruptEnable
    IvshmemNetInterruptInitialize(adapter->Interrupt);
    //IvshmemNetAdapterUpdateHardwareChecksum(adapter);
    //IvshmemNetAdapterUpdateInterruptModeration(adapter);

    if (previousState != WdfPowerDeviceD3Final)
    {
        // We're coming back from low power, undo what
        // we did in EvtDeviceD0Exit
        IvshmemNetAdapterRaiseToD0(adapter);

        // Set up the multicast list address
        // return to D0, WOL no more require to RX all multicast packets
      //  IvshmemNetAdapterPushMulticastList(adapter);

        // Update link state
        // Lock not required because of serialized power transition.
        NET_ADAPTER_LINK_STATE linkState;
        IvshmemNetAdapterQueryLinkState(adapter, &linkState);

        NetAdapterSetCurrentLinkState(adapter->NetAdapter, &linkState);

		
    }
	*adapter->my_status = IVSHM_NET_STATE_INIT;
	MemoryBarrier();
	notifyPeer(adapter);

    TraceExit();
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
EvtDeviceD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
)
{
    IVN_ADAPTER *adapter = IvshmemNetGetDeviceContext(Device)->Adapter;

    TraceEntry();

    if (TargetState != WdfPowerDeviceD3Final)
    {
        NET_ADAPTER_LINK_STATE linkState;
        NET_ADAPTER_LINK_STATE_INIT(
            &linkState,
            NDIS_LINK_SPEED_UNKNOWN,
            MediaConnectStateUnknown,
            MediaDuplexStateUnknown,
            NetAdapterPauseFunctionsUnknown,
            NET_ADAPTER_AUTO_NEGOTIATION_NO_FLAGS);

        NetAdapterSetCurrentLinkState(adapter->NetAdapter, &linkState);
    }

	*adapter->my_status = IVSHM_NET_STATE_RESET;
	MemoryBarrier();
	notifyPeer(adapter);

    TraceExit();
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
EvtDeviceArmWakeFromSx(
    _In_ WDFDEVICE wdfDevice)
{
    IVN_ADAPTER *adapter = IvshmemNetGetDeviceContext(wdfDevice)->Adapter;

    TraceEntryIvshmemNetAdapter(adapter);

    // Use NETPOWERSETTINGS to check if we should enable wake from magic packet
    NETPOWERSETTINGS powerSettings = NetAdapterGetPowerSettings(adapter->NetAdapter);
    ULONG enabledWakePatterns = NetPowerSettingsGetEnabledWakePatternFlags(powerSettings);

    if (enabledWakePatterns & NET_ADAPTER_WAKE_MAGIC_PACKET)
    {
        //IvshmemNetAdapterEnableMagicPacket(adapter);
    }

    TraceExit();
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
void
EvtDeviceDisarmWakeFromSx(
    _In_ WDFDEVICE wdfDevice)
{
    IVN_ADAPTER *adapter = IvshmemNetGetDeviceContext(wdfDevice)->Adapter;

    TraceEntryIvshmemNetAdapter(adapter);

    //IvshmemNetAdapterDisableMagicPacket(adapter);

    TraceExit();
}
