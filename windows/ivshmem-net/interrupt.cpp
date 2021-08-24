/*++

    THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
    ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
    THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
    PARTICULAR PURPOSE.

    Copyright (c) Microsoft Corporation. All rights reserved

--*/

#include "precomp.h"

#include "trace.h"
#include "interrupt.h"
#include "adapter.h"
#include "link.h"


NTSTATUS
IvshmemNetInterruptCreate(
    _In_ WDFDEVICE wdfDevice,
    _In_ IVN_ADAPTER *adapter,
    _Out_ IVN_INTERRUPT **interrupt)
{
    TraceEntryIvshmemNetAdapter(adapter);

    *interrupt = nullptr;

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, IVN_INTERRUPT);

    WDF_INTERRUPT_CONFIG config;
    WDF_INTERRUPT_CONFIG_INIT(&config, EvtInterruptIsr, EvtInterruptDpc);

    config.EvtInterruptEnable = EvtInterruptEnable;
    config.EvtInterruptDisable = EvtInterruptDisable;

    NTSTATUS status = STATUS_SUCCESS;

    WDFINTERRUPT wdfInterrupt;
    GOTO_IF_NOT_NT_SUCCESS(Exit, status,
        WdfInterruptCreate(wdfDevice, &config, &attributes, &wdfInterrupt));

    *interrupt = IvshmemNetGetInterruptContext(wdfInterrupt);

    (*interrupt)->Adapter = adapter;
    (*interrupt)->Handle = wdfInterrupt;

Exit:

    TraceExitResult(status);
    return status;
}

void
IvshmemNetInterruptInitialize(_In_ IVN_INTERRUPT *interrupt)
{
	UNREFERENCED_PARAMETER(interrupt);
}


_Use_decl_annotations_
NTSTATUS
EvtInterruptEnable(
    _In_ WDFINTERRUPT wdfInterrupt,
    _In_ WDFDEVICE wdfDevice)
{
    UNREFERENCED_PARAMETER((wdfDevice));

    TraceEntry(TraceLoggingPointer(wdfInterrupt));

    // Framework sychronizes EvtInterruptEnable with WdfInterruptAcquireLock
    // so do not grab the lock internally

    TraceExit();
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
EvtInterruptDisable(
    _In_ WDFINTERRUPT wdfInterrupt,
    _In_ WDFDEVICE wdfDevice)
{
    UNREFERENCED_PARAMETER((wdfDevice));

    TraceEntry(TraceLoggingPointer(wdfInterrupt));

    // Framework sychronizes EvtInterruptDisable with WdfInterruptAcquireLock
    // so do not grab the lock internally

    IvshmemNetInterruptInitialize(IvshmemNetGetInterruptContext(wdfInterrupt));

    TraceExit();
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
BOOLEAN
EvtInterruptIsr(
    _In_ WDFINTERRUPT wdfInterrupt,
    ULONG MessageID)
{
	TraceEntry(TraceLoggingPointer(wdfInterrupt));
    UNREFERENCED_PARAMETER((MessageID));

    IVN_INTERRUPT *interrupt = IvshmemNetGetInterruptContext(wdfInterrupt);

    interrupt->NumInterrupts++;

	WdfInterruptQueueDpcForIsr(wdfInterrupt);
	TraceExit();
    return true;
}

static
void
IvshmemNetRxNotify(
    _In_ IVN_INTERRUPT *interrupt,
    _In_ ULONG queueId
    )
{
    if (InterlockedExchange(&interrupt->RxNotifyArmed[queueId], false))
    {
        NetRxQueueNotifyMoreReceivedPacketsAvailable(
            interrupt->Adapter->RxQueues[queueId]);
    }
}

_Use_decl_annotations_
VOID
EvtInterruptDpc(
    _In_ WDFINTERRUPT Interrupt,
    _In_ WDFOBJECT AssociatedObject)
{
    UNREFERENCED_PARAMETER(AssociatedObject);

	TraceEntry(TraceLoggingPointer(Interrupt));

    IVN_INTERRUPT *interrupt = IvshmemNetGetInterruptContext(Interrupt);
    IVN_ADAPTER *adapter = interrupt->Adapter;

	IvshmemNetAdapterNotifyLinkChange(adapter);

	if (IvshmemNetAdapterQueryMediaState(adapter) == MediaConnectStateConnected) {
		IvshmemNetRxNotify(interrupt, 0);
		if (adapter->tx_queue.last_used_idx != adapter->tx_queue.vr.used->idx) {
			NetTxQueueNotifyMoreCompletedPacketsAvailable(adapter->TxQueue);
		}
	}

	TraceExit();
}
