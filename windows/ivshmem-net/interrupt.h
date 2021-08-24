/*++

    THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
    ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
    THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
    PARTICULAR PURPOSE.

    Copyright (c) Microsoft Corporation. All rights reserved

--*/

#pragma once

typedef struct _IVN_INTERRUPT
{
    IVN_ADAPTER *Adapter;
    WDFINTERRUPT Handle;

    // Armed Notifications
    LONG RxNotifyArmed[IVN_NUMBER_OF_QUEUES];
    LONG TxNotifyArmed;


    union {
        volatile UINT16 * Address16;
        volatile UINT8 * Address8;
    } Isr[IVN_NUMBER_OF_QUEUES];

    union {
        volatile UINT16 * Address16;
        volatile UINT8 * Address8;
    } Imr[IVN_NUMBER_OF_QUEUES];

    // Fired Notificiations
    // Tracks un-served ISR interrupt fields. Masks in only
    // the IvshmemNetExpectedInterruptFlags
    UINT32 SavedIsr;

    // Statistical counters, for diagnostics only
    ULONG64 NumInterrupts;
    ULONG64 NumInterruptsNotOurs;
    ULONG64 NumInterruptsDisabled;
    ULONG64 NumRxInterrupts[IVN_NUMBER_OF_QUEUES];
    ULONG64 NumTxInterrupts;
} IVN_INTERRUPT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(IVN_INTERRUPT, IvshmemNetGetInterruptContext);

static const USHORT IvshmemNetTxInterruptFlags = ISRIMR_TOK | ISRIMR_TER;
static const USHORT IvshmemNetRxInterruptFlags = ISRIMR_ROK | ISRIMR_RER | ISRIMR_RDU;
static const USHORT IvshmemNetRxInterruptSecondaryFlags = ISR123_ROK | ISR123_RDU;
static const USHORT IvshmemNetDefaultInterruptFlags = ISRIMR_LINK_CHG;
static const USHORT IvshmemNetExpectedInterruptFlags = (IvshmemNetTxInterruptFlags | IvshmemNetRxInterruptFlags | IvshmemNetDefaultInterruptFlags | ISRIMR_RX_FOVW);
static const USHORT IvshmemNetInactiveInterrupt = 0xFFFF;

NTSTATUS
IvshmemNetInterruptCreate(
    _In_ WDFDEVICE wdfDevice,
    _In_ IVN_ADAPTER *adapter,
    _Out_ IVN_INTERRUPT **interrupt);

void IvshmemNetInterruptInitialize(_In_ IVN_INTERRUPT *interrupt);
void IvshmemNetUpdateImr(_In_ IVN_INTERRUPT *interrupt, ULONG QueueId);

EVT_WDF_INTERRUPT_ISR EvtInterruptIsr;
EVT_WDF_INTERRUPT_DPC EvtInterruptDpc;
EVT_WDF_INTERRUPT_ENABLE EvtInterruptEnable;
EVT_WDF_INTERRUPT_DISABLE EvtInterruptDisable;

