

on file structure level

tree -L 1  services/
services/
├── manifest
├── pero_cmp_gw_svc -> chnage in 'repo/manifest' to platform/gateway
├── supervisor -> move platform/supervisor
└── system

also

tree -L 1  autosar
autosar -> vendor/autosar
├── demo - remove from 'repo/manifest'
└── mlbevo_gen2_cmp_psp - move from default manivest to .repo/local_manifests




on art level. relink all parts under platform/system to resolve dependenties in system definitions

platform/system

├── autosar -  autosar exports to reference signals
└── demo -> ../../demo  - current RIG we working

link to -> services - Platform components(FC).
   ├── com
   ├── core
   ├── crypto
   ├── diag
   ├── exec
   ├── fw
   ├── idsm
   ├── log
   ├── nm
   ├── osi
   ├── per
   ├── phm
   ├── rds
   ├── shwa
   ├── sm
   ├── tsync
   ├── ucm
   └── vucm

├── gateway    - not FC, part of platform
|- supervisor/ - not FC, part of platform


