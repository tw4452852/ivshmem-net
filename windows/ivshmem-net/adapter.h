/*++

    THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
    ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
    THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
    PARTICULAR PURPOSE.

    Copyright (c) Microsoft Corporation. All rights reserved

--*/

#pragma once

#include "queue.h"

typedef struct _IVN_INTERRUPT IVN_INTERRUPT;
typedef struct _IVN_TALLY IVN_TALLY;

#pragma region Setting Enumerations

typedef enum _IVN_DUPLEX_STATE : UCHAR {
    IvshmemNetDuplexNone = 0,
    IvshmemNetDuplexHalf = 1,
    IvshmemNetDuplexFull = 2,
} IVN_DUPLEX_STATE;

typedef enum _IVN_CHKSUM_OFFLOAD : UCHAR
{
    IvshmemNetChecksumOffloadDisabled = 0,
    IvshmemNetChecksumOffloadTxEnabled = 1,
    IvshmemNetChecksumOffloadRxEnabled = 2,
    IvshmemNetChecksumOffloadTxRxEnabled = 3,
} IVN_CHKSUM_OFFLOAD;

typedef enum _IVN_LSO_OFFLOAD : UCHAR
{
    IvshmemNetLsoOffloadDisabled = 0,
    IvshmemNetLsoOffloadEnabled = 1,
} IVN_LSO_OFFLOAD;

#define IVN_LSO_OFFLOAD_MAX_SIZE 64000
#define IVN_LSO_OFFLOAD_MIN_SEGMENT_COUNT 2

typedef enum _IVN_IM_MODE
{
    IvshmemNetInterruptModerationDisabled = 0,
    IvshmemNetInterruptModerationEnabled = 1,
} IVN_IM_MODE;

typedef enum _IVN_IM_LEVEL
{
    IvshmemNetInterruptModerationLow = 0,
    IvshmemNetInterruptModerationMedium = 1,
} IVN_IM_LEVEL;

typedef enum _IVN_FLOW_CONTROL
{
    IvshmemNetFlowControlDisabled = 0,
    IvshmemNetFlowControlTxEnabled = 1,
    IvshmemNetFlowControlRxEnabled = 2,
    IvshmemNetFlowControlTxRxEnabled = 3,
} IVN_FLOW_CONTROL;

typedef enum _IVN_CHIP_TYPE
{
    RTLUNKNOWN,
    RTL8168D,
    RTL8168D_REV_C_REV_D,
    RTL8168E
} IVN_CHIP_TYPE;

typedef enum _IVN_SPEED_DUPLEX_MODE {

    IvshmemNetSpeedDuplexModeAutoNegotiation = 0,
    IvshmemNetSpeedDuplexMode10MHalfDuplex = 1,
    IvshmemNetSpeedDuplexMode10MFullDuplex = 2,
    IvshmemNetSpeedDuplexMode100MHalfDuplex = 3,
    IvshmemNetSpeedDuplexMode100MFullDuplex = 4,
    // 1Gb Half Duplex is not supported
    IvshmemNetSpeedDuplexMode1GFullDuplex = 6,

} IVN_SPEED_DUPLEX_MODE;

#pragma endregion

#pragma align(push,4)
typedef struct IVSHMEMDeviceRegisters
{
	volatile ULONG irqMask;
	volatile ULONG irqStatus;
	volatile LONG  ivProvision;
	volatile ULONG doorbell;
	volatile UCHAR reserved[240];
}
IVSHMEMDeviceRegisters, *PIVSHMEMDeviceRegisters;
#pragma align(pop)

// Context for NETADAPTER
typedef struct _IVN_ADAPTER
{
    // WDF handles associated with this context
    NETADAPTER NetAdapter;
    WDFDEVICE WdfDevice;

    // Handle to default Tx and Rx Queues
    NETPACKETQUEUE TxQueue;
    NETPACKETQUEUE RxQueues[IVN_NUMBER_OF_QUEUES];

    // Pointer to interrupt object
    IVN_INTERRUPT *Interrupt;

    // Handle given by NDIS when the NetAdapter registered itself.
    NDIS_HANDLE NdisLegacyAdapterHandle;

    // configuration
    NET_ADAPTER_LINK_LAYER_ADDRESS PermanentAddress;
    NET_ADAPTER_LINK_LAYER_ADDRESS CurrentAddress;
    BOOLEAN OverrideAddress;

    ULONG NumTcb;             // Total number of TCBs

                              // spin locks
    WDFSPINLOCK Lock;

    // Packet Filter and look ahead size.
    NET_PACKET_FILTER_TYPES_FLAGS PacketFilter;
    USHORT LinkSpeed;
    NET_IF_MEDIA_DUPLEX_STATE DuplexMode;

    // multicast list
    UINT MCAddressCount;
    UCHAR MCList[IVN_MAX_MCAST_LIST][ETH_LENGTH_OF_ADDRESS];

    // Packet counts
    ULONG64 InUcastOctets;
    ULONG64 InMulticastOctets;
    ULONG64 InBroadcastOctets;
    ULONG64 OutUCastPkts;
    ULONG64 OutMulticastPkts;
    ULONG64 OutBroadcastPkts;
    ULONG64 OutUCastOctets;
    ULONG64 OutMulticastOctets;
    ULONG64 OutBroadcastOctets;

    ULONG64 TotalTxErr;
    ULONG   TotalRxErr;

    ULONG64 HwTotalRxMatchPhy;
    ULONG64 HwTotalRxBroadcast;
    ULONG64 HwTotalRxMulticast;

    // Count of transmit errors
    ULONG TxAbortExcessCollisions;
    ULONG TxDmaUnderrun;
    ULONG TxOneRetry;
    ULONG TxMoreThanOneRetry;

    // Count of receive errors
    ULONG RcvResourceErrors;

	PIVSHMEMDeviceRegisters regs;
	UINT8 *sharedRegion;
	UINT32 sharedRegionSize;
	UINT32 *my_status;
	UINT32 *peer_status;
	UINT8 *my_queue;
	UINT8 *peer_queue;
	UINT8 my_id;
	UINT8 peer_id;
	struct ivshm_net_queue tx_queue;
	struct ivshm_net_queue rx_queue;

	UINT32 vrsize;
	UINT32 qsize;
	UINT32 qlen;

    // user "*SpeedDuplex"  setting
    IVN_SPEED_DUPLEX_MODE SpeedDuplex;

    WDFDMAENABLER DmaEnabler;

    WDFCOMMONBUFFER HwTallyMemAlloc;
    PHYSICAL_ADDRESS TallyPhy;
    IVN_TALLY *GTally;

    IVN_CHIP_TYPE ChipType;

    bool LinkAutoNeg;

    NDIS_OFFLOAD_ENCAPSULATION OffloadEncapsulation;

    USHORT ReceiveBuffers;
    USHORT TransmitBuffers;

    BOOLEAN IpHwChkSum;
    BOOLEAN TcpHwChkSum;
    BOOLEAN UdpHwChkSum;

    ULONG ChksumErrRxIpv4Cnt;
    ULONG ChksumErrRxTcpIpv6Cnt;
    ULONG ChksumErrRxTcpIpv4Cnt;
    ULONG ChksumErrRxUdpIpv6Cnt;
    ULONG ChksumErrRxUdpIpv4Cnt;

    // Tracks *WakeOnLan Keyword
    bool WakeOnMagicPacketEnabled;

    // Hardware capability, managed by INF keyword
    IVN_IM_MODE InterruptModerationMode;
    // Moderation Degree, managed by INF keyword
    IVN_IM_LEVEL InterruptModerationLevel;
    // Runtime disablement, controlled by OID
    bool InterruptModerationDisabled;

    // basic detection of concurrent EEPROM use
    bool EEPROMSupported;
    bool EEPROMInUse;

    // ReceiveScaling
    UINT32 RssIndirectionTable[IVN_INDIRECTION_TABLE_SIZE];

    IVN_FLOW_CONTROL FlowControl;

    IVN_LSO_OFFLOAD LSOv4;
    IVN_LSO_OFFLOAD LSOv6;
    bool RssEnabled;
} IVN_ADAPTER;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(IVN_ADAPTER, IvshmemNetGetAdapterContext);

EVT_NET_ADAPTER_CREATE_TXQUEUE   EvtAdapterCreateTxQueue;
EVT_NET_ADAPTER_CREATE_RXQUEUE   EvtAdapterCreateRxQueue;

EVT_WDF_DEVICE_CONTEXT_DESTROY   IvshmemNetDestroyAdapterContext;

inline
NTSTATUS
IvshmemNetConvertNdisStatusToNtStatus(
    _In_ NDIS_STATUS ndisStatus)
{
    if (ndisStatus == NDIS_STATUS_BUFFER_TOO_SHORT)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }
    else if (ndisStatus == NDIS_STATUS_REQUEST_ABORTED)
    {
        return STATUS_CANCELLED;
    }
    else
    {
        return (NTSTATUS)ndisStatus;
    }
}

NTSTATUS
IvshmemNetInitializeAdapterContext(
    _In_ IVN_ADAPTER *adapter,
    _In_ WDFDEVICE device,
    _In_ NETADAPTER netAdapter);

NTSTATUS
IvshmemNetAdapterStart(
    _In_ IVN_ADAPTER *adapter);

void IvshmemNetAdapterUpdateInterruptModeration(_In_ IVN_ADAPTER *adapter);

void
IvshmemNetAdapterUpdateHardwareChecksum(_In_ IVN_ADAPTER *adapter);

NTSTATUS
IvshmemNetAdapterReadAddress(_In_ IVN_ADAPTER *adapter);

void
IvshmemNetAdapterRefreshCurrentAddress(_In_ IVN_ADAPTER *adapter);

void
IvshmemNetAdapterSetupHardware(IVN_ADAPTER *adapter);

void
IvshmemNetAdapterIssueFullReset(_In_ IVN_ADAPTER *adapter);

void
IvshmemNetAdapterEnableCR9346Write(_In_ IVN_ADAPTER *adapter);

void
IvshmemNetAdapterDisableCR9346Write(_In_ IVN_ADAPTER *adapter);

void
IvshmemNetAdapterSetupCurrentAddress(_In_ IVN_ADAPTER *adapter);

void
IvshmemNetAdapterPushMulticastList(_In_ IVN_ADAPTER *adapter);

bool
IvshmemNetAdapterQueryChipType(_In_ IVN_ADAPTER *adapter, _Out_ IVN_CHIP_TYPE *chipType);
