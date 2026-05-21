1. ~~generate sample vendor project~~ — done (vendor/tornado/ via fusée + AUTOSAR import; commits 977a6ab artheia, 2f36b93 pero_theia)
2. move generator scripts from gateway/pero_cmp_lnx/tools/ into artheia (CLI subcommands + Jinja templates); pero_cmp_lnx becomes a library that ships as a -dev package, customized per-vendor by the embedded signal filter
3. add signal filter to gateway — use pero_cmp_gw_svc as test subject for the migrated generators; vendor/<v>/config/signal_filter.csv as input
   - add a Bloom filter on the Hercules side so the firmware drops signals the host doesn't want before they hit the wire
   - on the host side, protect gw from unknown-signal decoding: every CAN-id / FlexRay-slot the PSP receives must be in the catalog or be ignored (don't pb_decode against an unrelated descriptor)
4. add command completion