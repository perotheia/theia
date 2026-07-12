# tipc-tegra — out-of-tree TIPC for NVIDIA L4T kernels

L4T/tegra kernels ship `CONFIG_TIPC=n`, and `struct net_device::tipc_ptr` is
compiled out with it — an unmodified external tipc.ko cannot build. This kit
patches `bearer.c` to use `tipc_devmap.h` (an RCU side table keyed by the
`net_device` pointer) at the 4 `tipc_ptr` sites, and builds a minimal module
(eth bearer only: no udp_media, no crypto, no diag).

Run `./build-tipc-ko.sh` **on the board** (kernel headers must be installed —
L4T ships them). Proven on jason (AGX, 5.10.104-tegra) and exo (Orin Nano,
5.15.148-tegra). The module tainting (out-of-tree, unsigned) is harmless —
L4T does not enforce module signatures.

Persisted: `/lib/modules/<ver>/extra/tipc.ko` + depmod + modules-load.d, so
`modprobe tipc` works and the module auto-loads on boot. The eth BEARER is
NOT persisted — Theia's supervisor enables it at start (same as rpi4).
