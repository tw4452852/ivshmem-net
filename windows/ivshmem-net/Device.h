/*++

    THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
    ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
    THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
    PARTICULAR PURPOSE.

    Copyright (c) Microsoft Corporation. All rights reserved

--*/

#pragma once

typedef struct _IVN_DEVICE
{
    IVN_ADAPTER *Adapter;
} IVN_DEVICE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(IVN_DEVICE, IvshmemNetGetDeviceContext);

EVT_WDF_DEVICE_PREPARE_HARDWARE     EvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE     EvtDeviceReleaseHardware;

void
notifyPeer(_In_ IVN_ADAPTER *adapter);
