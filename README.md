# ivshmem-net
NIC driver based on virtualized shared memory

## How to build

To make module for the currently running kernel:

```
make
```

Or to make for your specified kernel:

```
make KDIR=<path to your kernel source directory>
```

Once build successfully, `ivshmem-net.ko` will be generated under current directory.
