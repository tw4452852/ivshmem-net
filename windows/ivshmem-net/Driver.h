/*++

Module Name:

    driver.h

Abstract:

    This file contains the driver definitions.

Environment:

    Kernel-mode Driver Framework

--*/
#pragma warning(disable:4214)

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include <netadaptercx.h>

#include "device.h"
#include "queue.h"
#include "trace.h"

EXTERN_C_START

//
// WDFDRIVER Events
//

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD ivshmemnetEvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP ivshmemnetEvtDriverContextCleanup;

EXTERN_C_END
