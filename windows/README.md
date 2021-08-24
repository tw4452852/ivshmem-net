# IVSHMEM-NET Sample Driver

This is an implementation of a NetAdapter driver targetting the IVSHMEM-NET chipset. It implements OID handling, data path, and some basic offloads, including checksum offload.

## How to use this sample
This sample will guide you through implementing a complete network driver using the NetAdapter Class Extension to WDF.

## Landmarks

### [DriverEntry](./ivshmem-net/driver.cpp)
This is where your driver gets started. Check out DriverEntry to see how to initialize a WDF Driver with logging. Then check out EvtDriverDeviceAdd to see how to initialize a device with a NetAdapter object.

### [WdfDevice](./ivshmem-net/device.cpp)
One of the biggest benefits of the new NetAdapter class extension is that you have full access to the WDF framework. This contains the implementation of the IVSHMEM-NET resource assignment.

### [NetAdapter](./ivshmem-net/adapter.cpp)
Check out the unique features of the NetAdapter class extension. Here's where the adapter reports capabilities to the network stack and creates its datapath queues.

### [Power](./ivshmem-net/power.cpp)
NetAdapter drivers can take full advantage of the WDF power state machine. Check out how power tranisitions are implemented, and how NetAdapter concepts like WakeOnMagicPacket integrate directly with WDF power transitions.

### [NetRequestQueue](./ivshmem-net/oid.cpp)
The adapter's control path is how the network stack sets  and queries adapter parameters at runtime. NetAdapter allows you to provide specific query/set handlers that only apply to certain requests. Specific handlers reduce cognitive overhead while writing and understanding the query/set code.

### [NetTxQueue](./ivshmem-net/txqueue.cpp)
The sample driver implements its transmit data queue using the DMA IO Helper. This IO Helper simplifies scatter gather DMA operations and mapping NET_PACKET descriptors to IVSHMEM-NET's software transmit descriptors.

### [NetRxQueue](./ivshmem-net/rxqueue.cpp)
Implement your receive queue in 200 lines of code! The NetAdapter data path fits perfectly with IVSHMEM-NET's data path.

## Debugging and Development

### Build the Driver

Prerequisites:
- Visual Studio 2015
- Windows SDK for Windows 10, version 1703
- WDK for Windows 10, version 1703

Instructions to install all of these are available [here on MSDN](https://developer.microsoft.com/en-us/windows/hardware/windows-driver-kit).

The driver can be built using Visual Studio - open [the solution file](ivshmem-net.sln) in Visual Studio and use the Build menu to build the driver. The driver can also be built from the command line using msbuild.exe.

### Build Output

The driver package will be available, relative to this directory, in:
```
; x86
Debug\ivshmem-net

; amd64
x64\Debug\ivshmem-net
```

### Test Machine Setup
First, locate and install the IVSHMEM-NET NIC into your test machine.

Next, [enable kernel debugging](https://msdn.microsoft.com/en-us/library/windows/hardware/hh450944(v=vs.85).aspx).

You'll also need to [enable test signing](https://msdn.microsoft.com/en-us/windows/hardware/drivers/install/the-testsigning-boot-configuration-option) to allow you to install your unsigned driver.

### Installing the Driver

Open Device Manager: Win+X, M

Expand the "Network Adapters" tab.

Find the target device - for the IVSHMEM-NET, typically "Realtek PCIe GBE Family Controller".

Right click, click "Update Driver".

Click "Browse my computer for driver software"

Click "Let me pick from a list of available drivers on my computer"

Click "Have Disk..."

Browse to driver package

Click "OK"

Select "Realtek PCIe GBE Family Controller NetAdapter Sample Driver" and click Next

### Enable Logging

You can enable logging to the debug console in WinDbg by using !wmitrace.

```
!wmitrace.start RtTrace -kd
!wmitrace.enable RtTrace {5D364AAF-5B49-41A0-9E03-D3CB2AA2E03E}
```

### .kdfiles

Iterate quickly using the windbg .kdfiles command to quickly replace your driver binary.

Add a kdfiles entry in the kernel debugger using -m.

```
.kdfiles -m ivshmem-net.sys C:\directory\to\new\ivshmem-net.sys
```

You can also use a map file. For example:

```
; kdmap.txt

map
ivshmem-net.sys
C:\directory\to\new\ivshmem-net.sys
```

Then, in WinDbg:
```
.kdfiles .\path\to\kdmap.txt
```

Whenever the driver is loaded, your new driver will be used.

## Known Issues
- Windows 1703 bugchecks when the OS tries to send packet with 20 or more fragments
- VLAN is not yet implemented
- NDISTest, version 1703, has some false positives when running against a NetAdapter driver
- MAC address is not restored to the value in EEPROM until after a complete power cycle
