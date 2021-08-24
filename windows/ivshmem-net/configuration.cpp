/*++

    THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
    ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
    THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
    PARTICULAR PURPOSE.

    Copyright (c) Microsoft Corporation. All rights reserved

--*/

#include "precomp.h"

#include "trace.h"
#include "configuration.h"
#include "adapter.h"

typedef struct _IVN_ADVANCED_PROPERTY
{
    NDIS_STRING RegName;  // variable name text
    UINT FieldOffset;     // offset to IVN_ADAPTER field
    UINT FieldSize;       // size (in bytes) of the field
    UINT Default;         // default value to use
    UINT Min;             // minimum value allowed
    UINT Max;             // maximum value allowed
} IVN_ADVANCED_PROPERTY;

#define IVN_OFFSET(field)   ((UINT)FIELD_OFFSET(IVN_ADAPTER,field))
#define IVN_SIZE(field)     sizeof(((IVN_ADAPTER *)0)->field)

IVN_ADVANCED_PROPERTY IvshmemNetSupportedProperties[] =
{
    // reg value name                                Offset in IVN_ADAPTER                 Field size                         Default Value                     Min                               Max

    // Standard Keywords
    { NDIS_STRING_CONST("*SpeedDuplex"),             IVN_OFFSET(SpeedDuplex),              IVN_SIZE(SpeedDuplex),              IvshmemNetSpeedDuplexMode1GFullDuplex, IvshmemNetSpeedDuplexModeAutoNegotiation, IvshmemNetSpeedDuplexMode1GFullDuplex },
    { NDIS_STRING_CONST("*ReceiveBuffers"),          IVN_OFFSET(ReceiveBuffers),           IVN_SIZE(ReceiveBuffers),           128,                              IVN_MIN_RX_DESC,                   IVN_MAX_RX_DESC },
    { NDIS_STRING_CONST("*TransmitBuffers"),         IVN_OFFSET(TransmitBuffers),          IVN_SIZE(TransmitBuffers),          128,                              IVN_MIN_TCB,                       IVN_MAX_TCB },
    { NDIS_STRING_CONST("*WakeOnMagicPacket"),       IVN_OFFSET(WakeOnMagicPacketEnabled), IVN_SIZE(WakeOnMagicPacketEnabled), true,                             false,                            true },
    { NDIS_STRING_CONST("*InterruptModeration"),     IVN_OFFSET(InterruptModerationMode),  IVN_SIZE(InterruptModerationMode),  IvshmemNetInterruptModerationEnabled,     IvshmemNetInterruptModerationDisabled,    IvshmemNetInterruptModerationEnabled },
    { NDIS_STRING_CONST("*FlowControl"),             IVN_OFFSET(FlowControl),              IVN_SIZE(FlowControl),              IvshmemNetFlowControlDisabled,         IvshmemNetFlowControlDisabled,            IvshmemNetFlowControlTxRxEnabled },
    { NDIS_STRING_CONST("*RSS"),                     IVN_OFFSET(RssEnabled),               IVN_SIZE(RssEnabled),               false,                            false,                            true },

    // Custom Keywords
    { NDIS_STRING_CONST("InterruptModerationLevel"), IVN_OFFSET(InterruptModerationLevel), IVN_SIZE(InterruptModerationLevel), IvshmemNetInterruptModerationLow,         IvshmemNetInterruptModerationLow,         IvshmemNetInterruptModerationMedium },
};

NTSTATUS
IvshmemNetAdapterReadConfiguration(
    _In_ IVN_ADAPTER *adapter)
/*++
Routine Description:

    Read the following from the registry
    1. All the parameters
    2. NetworkAddres

Arguments:

    adapter                         Pointer to our adapter

Return Value:

    STATUS_SUCCESS
    STATUS_INSUFFICIENT_RESOURCES

--*/
{
    TraceEntryIvshmemNetAdapter(adapter);

    NTSTATUS status = STATUS_SUCCESS;

    NETCONFIGURATION configuration;
    GOTO_IF_NOT_NT_SUCCESS(Exit, status,
        NetAdapterOpenConfiguration(adapter->NetAdapter, WDF_NO_OBJECT_ATTRIBUTES, &configuration));

    // read all the registry values
    for (UINT i = 0; i < ARRAYSIZE(IvshmemNetSupportedProperties); i++)
    {
        IVN_ADVANCED_PROPERTY *property = &IvshmemNetSupportedProperties[i];

        // Driver should NOT fail the initialization only because it can not
        // read the registry
        PUCHAR pointer = (PUCHAR)adapter + property->FieldOffset;

        // Get the configuration value for a specific parameter.  Under NT the
        // parameters are all read in as DWORDs.
        ULONG value = 0;
        status = NetConfigurationQueryUlong(
            configuration,
            NET_CONFIGURATION_QUERY_ULONG_NO_FLAGS,
            &property->RegName,
            &value);

        // If the parameter was present, then check its value for validity.
        if (NT_SUCCESS(status))
        {
            // Check that param value is not too small or too large

            if (value < property->Min ||
                value > property->Max)
            {
                value = property->Default;
            }
        }
        else
        {
            value = property->Default;
            status = STATUS_SUCCESS;
        }

        TraceLoggingWrite(
            IvshmemNetTraceProvider,
            "ReadConfiguration",
            TraceLoggingIvshmemNetAdapter(adapter),
            TraceLoggingUnicodeString(&property->RegName, "Key"),
            TraceLoggingUInt32(value, "Value"));

        // Store the value in the adapter structure.
        switch (property->FieldSize)
        {
        case 1:
            *((PUCHAR)pointer) = (UCHAR)value;
            break;

        case 2:
            *((PUSHORT)pointer) = (USHORT)value;
            break;

        case 4:
            *((PULONG)pointer) = (ULONG)value;
            break;

        default:
            TraceLoggingWrite(
                IvshmemNetTraceProvider,
                "InvalidFieldSize",
                TraceLoggingLevel(TRACE_LEVEL_ERROR),
                TraceLoggingIvshmemNetAdapter(adapter),
                TraceLoggingUnicodeString(&property->RegName, "Key"),
                TraceLoggingUInt32(value, "Value"),
                TraceLoggingUInt32(property->FieldSize, "FieldSize"));
            break;
        }
    }

    // Read NetworkAddress registry value
    // Use it as the current address if any
    status = NetConfigurationQueryLinkLayerAddress(
        configuration,
        &adapter->CurrentAddress);

    if ((status == STATUS_SUCCESS))
    {
        if (adapter->CurrentAddress.Length != ETH_LENGTH_OF_ADDRESS ||
            ETH_IS_MULTICAST(adapter->CurrentAddress.Address) ||
            ETH_IS_BROADCAST(adapter->CurrentAddress.Address))
        {
            TraceLoggingWrite(
                IvshmemNetTraceProvider,
                "InvalidNetworkAddress",
                TraceLoggingBinary(adapter->CurrentAddress.Address, adapter->CurrentAddress.Length));
        }
        else
        {
            adapter->OverrideAddress = TRUE;
        }
    }

    status = STATUS_SUCCESS;

    // initial number of TX and RX
    adapter->NumTcb = adapter->TransmitBuffers;


Exit:
    if (configuration)
    {
        NetConfigurationClose(configuration);
    }

    TraceExitResult(status);
    return STATUS_SUCCESS;
}
