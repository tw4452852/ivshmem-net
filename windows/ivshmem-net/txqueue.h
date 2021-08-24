/*++

    THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
    ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
    THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
    PARTICULAR PURPOSE.

    Copyright (c) Microsoft Corporation. All rights reserved

--*/

#pragma once

typedef struct _IVN_TXQUEUE
{
    IVN_ADAPTER *Adapter;
    IVN_INTERRUPT *Interrupt;

    PCNET_DATAPATH_DESCRIPTOR DatapathDescriptor;
    NET_PACKET_CONTEXT_TOKEN *TcbToken;

    // descriptor information
    //WDFCOMMONBUFFER TxdArray;
    //IVN_TX_DESC *TxdBase;
    //size_t TxSize;

    //USHORT NumTxDesc;
    //USHORT TxFreeDescIndex;

    //UCHAR volatile *TPPoll;

    //size_t ChecksumExtensionOffSet;
    //size_t LsoExtensionOffset;
} IVN_TXQUEUE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(IVN_TXQUEUE, IvshmemNetGetTxQueueContext);


//--------------------------------------
// TCB (Transmit Control Block)
//--------------------------------------

typedef struct _IVN_TCB
{
    USHORT TxDescId;
    ULONG NumFrag;
} IVN_TCB;

NET_PACKET_DECLARE_CONTEXT_TYPE_WITH_NAME(IVN_TCB, GetTcbFromPacket);

NTSTATUS IvshmemNetTxQueueInitialize(_In_ NETPACKETQUEUE txQueue, _In_ IVN_ADAPTER *adapter);

_Requires_lock_held_(tx->Adapter->Lock)
void IvshmemNetTxQueueStart(_In_ IVN_TXQUEUE *tx);

EVT_WDF_OBJECT_CONTEXT_DESTROY EvtTxQueueDestroy;

EVT_PACKET_QUEUE_SET_NOTIFICATION_ENABLED EvtTxQueueSetNotificationEnabled;
EVT_PACKET_QUEUE_ADVANCE EvtTxQueueAdvance;
EVT_PACKET_QUEUE_CANCEL EvtTxQueueCancel;
EVT_PACKET_QUEUE_START EvtTxQueueStart;
EVT_PACKET_QUEUE_STOP EvtTxQueueStop;

void reset_tx_queue(_In_ IVN_ADAPTER * adapter);
