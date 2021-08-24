/*++

    THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
    ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
    THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
    PARTICULAR PURPOSE.

    Copyright (c) Microsoft Corporation. All rights reserved

--*/

#pragma once

struct IVN_RXQUEUE
{
    IVN_ADAPTER *Adapter;
    IVN_INTERRUPT *Interrupt;

    PCNET_DATAPATH_DESCRIPTOR DatapathDescriptor;

    //WDFCOMMONBUFFER RxdArray;
    //IVN_RX_DESC *RxdBase;
    //size_t RxdSize;

    size_t ChecksumExtensionOffSet;

    ULONG QueueId;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(IVN_RXQUEUE, IvshmemNetGetRxQueueContext);

NTSTATUS IvshmemNetRxQueueInitialize(_In_ NETPACKETQUEUE rxQueue, _In_ IVN_ADAPTER * adapter);

_Requires_lock_held_(adapter->Lock)
void IvshmemNetAdapterUpdateRcr(_In_ IVN_ADAPTER *adapter);

EVT_WDF_OBJECT_CONTEXT_DESTROY EvtRxQueueDestroy;

EVT_PACKET_QUEUE_SET_NOTIFICATION_ENABLED EvtRxQueueSetNotificationEnabled;
EVT_PACKET_QUEUE_ADVANCE EvtRxQueueAdvance;
EVT_PACKET_QUEUE_CANCEL EvtRxQueueCancel;
EVT_PACKET_QUEUE_START EvtRxQueueStart;
EVT_PACKET_QUEUE_STOP EvtRxQueueStop;

void reset_rx_queue(_In_ IVN_ADAPTER * adapter);
