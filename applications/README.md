# applications/

Vendor applications live here.

Each vendor adds its app(s) under `applications/<vendor>/` (typically pulled
in via `.repo/local_manifests/<vendor>.xml` like the vendor system fragment).

`pero_cmp_gw_cln_demo` (the gateway client demo) used to live here; it
moved under `gateway/demo/` because it's a gateway-stack demo rather
than a vendor app. This directory is intentionally kept around for
upcoming vendor apps (Tornado etc.).
