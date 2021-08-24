/*++

Module Name:

    public.h

Abstract:

    This module contains the common declarations shared by driver
    and user applications.

Environment:

    user and kernel

--*/

//
// Define an Interface Guid so that apps can find the device and talk to it.
//

DEFINE_GUID (GUID_DEVINTERFACE_ivshmemnet,
    0xb20875b6,0x6107,0x4ee4,0xb5,0x0f,0x8a,0x2d,0x38,0xb1,0xcf,0x61);
// {b20875b6-6107-4ee4-b50f-8a2d38b1cf61}
