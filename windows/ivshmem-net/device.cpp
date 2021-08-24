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
#include "configuration.h"
#include "statistics.h"
#include "interrupt.h"
#include "link.h"

struct ivshmem_vendor_cap {
	PCI_CAPABILITIES_HEADER header;
	UCHAR len;
	UCHAR peer_id;
};

void
notifyPeer(_In_ IVN_ADAPTER *adapter)
{
	TraceEntry();
	adapter->regs->doorbell = adapter->peer_id << 16;
	TraceExit();
}

static NTSTATUS
IvshmemNetGetIds(_In_ IVN_ADAPTER *adapter)
{
	NTSTATUS status;
	BUS_INTERFACE_STANDARD BusInterface;

	TraceEntry();
	status = WdfFdoQueryForInterface(adapter->WdfDevice,
                                   &GUID_BUS_INTERFACE_STANDARD,
                                   (PINTERFACE) &BusInterface,
                                   sizeof(BUS_INTERFACE_STANDARD),
                                   1, // Version
                                   NULL); //InterfaceSpecificData
    if (!NT_SUCCESS (status)){
        goto Exit;
    }


	PCI_COMMON_HEADER Header;
	PCI_COMMON_CONFIG *pPciConfig = (PCI_COMMON_CONFIG *)&Header;
	struct ivshmem_vendor_cap cap;
	PPCI_CAPABILITIES_HEADER pcap = &cap.header;

	UCHAR CapabilityOffset;

	// Read the first part of the header
	// to get the status register and
	// the capabilities pointer.
	// The "capabilities pointer" is
	// actually an offset from the
	// beginning of the header to a
	// linked list of capabilities.
	BusInterface.GetBusData(BusInterface.Context,
    	PCI_WHICHSPACE_CONFIG,
    	pPciConfig, // output buffer
    	0, // offset of the capability to read
		sizeof(PCI_COMMON_HEADER)); // just 64 bytes

	if ((pPciConfig->HeaderType & (~PCI_MULTIFUNCTION)) == PCI_BRIDGE_TYPE) {
		CapabilityOffset = pPciConfig->u.type1.CapabilitiesPtr;
	} else if ((pPciConfig->HeaderType & (~PCI_MULTIFUNCTION)) == PCI_CARDBUS_BRIDGE_TYPE) {
		CapabilityOffset = pPciConfig->u.type2.CapabilitiesPtr;
	} else {
		CapabilityOffset = pPciConfig->u.type0.CapabilitiesPtr;
	}

	// find vendor capability offset
	bool found = false;
	while (CapabilityOffset != 0) {
		BusInterface.GetBusData(BusInterface.Context,
    		PCI_WHICHSPACE_CONFIG,
    		pcap,
    		CapabilityOffset,
		sizeof(PCI_CAPABILITIES_HEADER));

		if (pcap->CapabilityID == PCI_CAPABILITY_ID_VENDOR_SPECIFIC) {
			found = true;
			break;
		} else {
			CapabilityOffset = pcap->Next;
		}
	}
	if (!found) {
		TraceLoggingWrite(IvshmemNetTraceProvider,"vendor capability not exist", TraceLoggingLevel(TRACE_LEVEL_WARNING));
		status = STATUS_NOT_IMPLEMENTED;
		goto Exit;
	}

	BusInterface.GetBusData(BusInterface.Context,
    		PCI_WHICHSPACE_CONFIG,
    		&adapter->peer_id,
    		CapabilityOffset + FIELD_OFFSET(struct ivshmem_vendor_cap, peer_id),
		sizeof(UINT8));


	adapter->my_id = (UINT8)adapter->regs->ivProvision;

	TraceLoggingWrite(
			IvshmemNetTraceProvider,
			"tw",
			TraceLoggingHexUInt8(adapter->my_id, "my_id"),
			TraceLoggingHexUInt8(adapter->peer_id, "peer_id")
			);

Exit:
	TraceExitResult(status);
    return status;
}


NTSTATUS
IvshmemNetGetResources(
    _In_ IVN_ADAPTER *adapter,
    _In_ WDFCMRESLIST resourcesRaw,
    _In_ WDFCMRESLIST resourcesTranslated)
{
    TraceEntry();

    NTSTATUS status = STATUS_SUCCESS;
    ULONG errorCode = 0;
    ULONG errorValue = 0;

    bool hasMemoryResource = false;
    ULONG memRegCnt = 0;

    // According to https://msdn.microsoft.com/windows/hardware/drivers/wdf/raw-and-translated-resources
    // "Both versions represent the same set of hardware resources, in the same order."
    ULONG rawCount = WdfCmResourceListGetCount(resourcesRaw);
    NT_ASSERT(rawCount == WdfCmResourceListGetCount(resourcesTranslated));

    for (ULONG i = 0; i < rawCount; i++)
    {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR rawDescriptor = WdfCmResourceListGetDescriptor(resourcesRaw, i);
        PCM_PARTIAL_RESOURCE_DESCRIPTOR translatedDescriptor = WdfCmResourceListGetDescriptor(resourcesTranslated, i);

		TraceLoggingWrite(
			IvshmemNetTraceProvider,
			"tw",
			TraceLoggingUInt32(i, "i"),
			TraceLoggingHexUInt32(rawDescriptor->Type, "type")
			);

        if (rawDescriptor->Type == CmResourceTypeMemory)
        {
			hasMemoryResource = true;
			TraceLoggingWrite(
				IvshmemNetTraceProvider,
				"tw",
				TraceLoggingHexUInt64(translatedDescriptor->u.Memory.Start.QuadPart, "start"),
				TraceLoggingHexUInt32(rawDescriptor->u.Memory.Length, "length")
			);
            // ivshmem has 3 memory IO regions, first region is regs, second region is MSI-X and the third is shared-region
            if (memRegCnt == 0)
            {
                NT_ASSERT(rawDescriptor->u.Memory.Length >= sizeof(IVSHMEMDeviceRegisters));

				GOTO_WITH_INSUFFICIENT_RESOURCES_IF_NULL(Exit, status,
					adapter->regs = (PIVSHMEMDeviceRegisters)MmMapIoSpaceEx(
						translatedDescriptor->u.Memory.Start,
						sizeof(IVSHMEMDeviceRegisters),
						PAGE_READWRITE | PAGE_NOCACHE));
				TraceLoggingWrite(
					IvshmemNetTraceProvider,
					"tw",
					TraceLoggingHexUInt64((UINT64)(adapter->regs), "adapter->regs")
				);

				GOTO_IF_NOT_NT_SUCCESS(Exit, status, IvshmemNetGetIds(adapter));

            } else if (memRegCnt == 2) {
				GOTO_WITH_INSUFFICIENT_RESOURCES_IF_NULL(Exit, status,
					adapter->sharedRegion = (UINT8 *)MmMapIoSpace(
						translatedDescriptor->u.Memory.Start,
						translatedDescriptor->u.Memory.Length,
						MmCached));
				TraceLoggingWrite(
					IvshmemNetTraceProvider,
					"tw",
					TraceLoggingHexUInt64((UINT64)(adapter->sharedRegion), "adapter->sharedRegion")
				);
				adapter->sharedRegionSize = translatedDescriptor->u.Memory.Length;
				adapter->my_status = (UINT32 *)(adapter->sharedRegion + !!(adapter->my_id < adapter->peer_id) * (adapter->sharedRegionSize / 2));
				adapter->peer_status = (UINT32 *)(adapter->sharedRegion + !!(adapter->peer_id < adapter->my_id) * (adapter->sharedRegionSize / 2));
				adapter->my_queue = (UINT8 *)(adapter->my_status + 1);
				adapter->peer_queue = (UINT8 *)(adapter->peer_status + 1);
			}

            memRegCnt++;
        }
    }

    if (!hasMemoryResource)
    {
        status = STATUS_RESOURCE_TYPE_NOT_FOUND;
        errorCode = NDIS_ERROR_CODE_RESOURCE_CONFLICT;

        errorValue = ERRLOG_NO_MEMORY_RESOURCE;

        GOTO_IF_NOT_NT_SUCCESS(Exit, status, STATUS_NDIS_RESOURCE_CONFLICT);
    }

Exit:

    if (!NT_SUCCESS(status))
    {
        NdisWriteErrorLogEntry(
            adapter->NdisLegacyAdapterHandle,
            errorCode,
            1,
            errorValue);
    }

    TraceExitResult(status);
    return status;
}

NTSTATUS
IvshmemNetRegisterScatterGatherDma(
    _In_ IVN_ADAPTER *adapter)
{
    TraceEntryIvshmemNetAdapter(adapter);

    WDF_DMA_ENABLER_CONFIG dmaEnablerConfig;
    WDF_DMA_ENABLER_CONFIG_INIT(&dmaEnablerConfig, WdfDmaProfileScatterGather64, IVN_MAX_PACKET_SIZE);
    dmaEnablerConfig.Flags |= WDF_DMA_ENABLER_CONFIG_REQUIRE_SINGLE_TRANSFER;
    dmaEnablerConfig.WdmDmaVersionOverride = 3;

    NTSTATUS status = STATUS_SUCCESS;
    GOTO_IF_NOT_NT_SUCCESS(Exit, status,
        WdfDmaEnablerCreate(
            adapter->WdfDevice,
            &dmaEnablerConfig,
            WDF_NO_OBJECT_ATTRIBUTES,
            &adapter->DmaEnabler),
        TraceLoggingIvshmemNetAdapter(adapter));

Exit:
    if (!NT_SUCCESS(status))
    {
        NdisWriteErrorLogEntry(
            adapter->NdisLegacyAdapterHandle,
            NDIS_ERROR_CODE_OUT_OF_RESOURCES,
            1,
            ERRLOG_OUT_OF_SG_RESOURCES);
    }

    TraceExitResult(status);
    return status;
}

static
NTSTATUS
IvshmemNetInitializeChipType(
    _In_ IVN_ADAPTER *adapter)
{
    if (IvshmemNetAdapterQueryChipType(adapter, &adapter->ChipType))
    {
        TraceLoggingWrite(
            IvshmemNetTraceProvider,
            "ChipType",
            TraceLoggingUInt32(adapter->ChipType));
        return STATUS_SUCCESS;
    }
    //
    // Unsupported card
    //
    NdisWriteErrorLogEntry(
        adapter->NdisLegacyAdapterHandle,
        NDIS_ERROR_CODE_ADAPTER_NOT_FOUND,
        0);
    return STATUS_SUCCESS;
}

static
void
IvshmemNetInitializeEeprom(
    _In_ IVN_ADAPTER *adapter)
{
	adapter->EEPROMSupported = false;
    if (!adapter->EEPROMSupported)
    {
        TraceLoggingWrite(
            IvshmemNetTraceProvider,
            "UnsupportedEEPROM",
            TraceLoggingLevel(TRACE_LEVEL_WARNING));
    }
}

static
void
IvshmemNetAdapterSetCurrentLinkState(
    _In_ IVN_ADAPTER *adapter)
{
    // Gathers and indicates current link state to NDIS
    //
    // Normally need to take the adapter lock before updating the NIC's
    // media state, but preparehardware already is serialized against all
    // other callbacks to the NetAdapter.

    NET_ADAPTER_LINK_STATE linkState;
    IvshmemNetAdapterQueryLinkState(adapter, &linkState);

    NetAdapterSetCurrentLinkState(adapter->NetAdapter, &linkState);
}

NTSTATUS
IvshmemNetInitializeHardware(
    _In_ IVN_ADAPTER *adapter,
    _In_ WDFCMRESLIST resourcesRaw,
    _In_ WDFCMRESLIST resourcesTranslated)
{
    TraceEntryIvshmemNetAdapter(adapter);
    //
    // Read the registry parameters
    //
    NTSTATUS status = STATUS_SUCCESS;

    adapter->NdisLegacyAdapterHandle =
        NetAdapterWdmGetNdisHandle(adapter->NetAdapter);

    GOTO_IF_NOT_NT_SUCCESS(Exit, status,
        IvshmemNetAdapterReadConfiguration(adapter));

    // Map in phy
    GOTO_IF_NOT_NT_SUCCESS(Exit, status,
        IvshmemNetGetResources(adapter, resourcesRaw, resourcesTranslated));
    //
    // Read additional info from NIC such as MAC address
    //
    GOTO_IF_NOT_NT_SUCCESS(Exit, status,
       IvshmemNetAdapterReadAddress(adapter));

    GOTO_IF_NOT_NT_SUCCESS(Exit, status,
        IvshmemNetRegisterScatterGatherDma(adapter),
        TraceLoggingIvshmemNetAdapter(adapter));

    IvshmemNetAdapterSetCurrentLinkState(adapter);

    GOTO_IF_NOT_NT_SUCCESS(Exit, status,
        IvshmemNetAdapterStart(adapter));

Exit:
    TraceExitResult(status);
    return status;
}

void
IvshmemNetReleaseHardware(
    _In_ IVN_ADAPTER *adapter)
{
    if (adapter->HwTallyMemAlloc)
    {
        WdfObjectDelete(adapter->HwTallyMemAlloc);
        adapter->HwTallyMemAlloc = WDF_NO_HANDLE;
    }

	if (adapter->sharedRegion)
	{
		MmUnmapIoSpace(
			adapter->sharedRegion,
			adapter->sharedRegionSize);
		adapter->sharedRegion = NULL;
		adapter->sharedRegionSize = 0;
		adapter->my_status = NULL;
		adapter->peer_status = NULL;
		adapter->my_queue = NULL;
		adapter->peer_queue = NULL;
	}
    if (adapter->regs)
    {
        MmUnmapIoSpace(
            adapter->regs,
            sizeof(IVSHMEMDeviceRegisters));
        adapter->regs = NULL;
    }

}

_Use_decl_annotations_
NTSTATUS
EvtDevicePrepareHardware(
    _In_ WDFDEVICE device,
    _In_ WDFCMRESLIST resourcesRaw,
    _In_ WDFCMRESLIST resourcesTranslated)
{
    IVN_ADAPTER *adapter = IvshmemNetGetDeviceContext(device)->Adapter;

    TraceEntryIvshmemNetAdapter(adapter);

    NTSTATUS status = STATUS_SUCCESS;
    GOTO_IF_NOT_NT_SUCCESS(Exit, status, IvshmemNetInitializeHardware(adapter, resourcesRaw, resourcesTranslated));

Exit:
    TraceExitResult(status);
    return status;
}

_Use_decl_annotations_
NTSTATUS
EvtDeviceReleaseHardware(
    _In_ WDFDEVICE device,
    _In_ WDFCMRESLIST resourcesTranslated)
{
    UNREFERENCED_PARAMETER(resourcesTranslated);
    IVN_ADAPTER *adapter = IvshmemNetGetDeviceContext(device)->Adapter;

    TraceEntryIvshmemNetAdapter(adapter);

    IvshmemNetReleaseHardware(adapter);

    NTSTATUS status = STATUS_SUCCESS;
    TraceExitResult(status);
    return status;
}
