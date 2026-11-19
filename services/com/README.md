# services/com — the gRPC bridge

com is the rig's gRPC edge: `ComGrpcProxy` (:7700, SupervisorView, PerView, NmView, Provisioning,
DiagView, UcmView, VucmView), `TraceForwarder` (:7710, TraceStream) and `LogForwarder` (:7711,
LogStream). The RPCs are defined in `proto/supervisor_bridge.proto`; the handlers live in `impl/`.

## Transport security

All three servers take their credentials from one helper, `impl/com_tls.hpp` (`make_server_creds`).
On a deployed rig the launcher (`theia-run.sh`) exports `THEIA_COM_TLS_REQUIRED=1`: com serves mutual
TLS against the fleet CA or does not start. A developer's `theia start` (no REQUIRED) keeps the opt-in
behaviour. See `review/hardening/SPEC-COM-MTLS.md` for the full contract.

## Per-RPC roles

With `THEIA_COM_TLS_REQUIRED=1` every call is authorized before any handler runs
(`impl/com_authz.hpp`: one table `kRpcTable`, one function `authorize()`, applied by a grpc
`AuthMetadataProcessor` that `make_server_creds` attaches to the TLS credentials of all three servers).

- **The caller's role** comes from the verified client certificate: the URI SAN `theia:<role>:<id>`
  (preferred), else the subject OU. The fleet signer (gs-api `/api/pki/sign`) writes both. Roles are
  ordered `rig` < `operator` < `gs`; a higher role may do everything a lower one may. A certificate
  with neither (or an unknown role) is denied every RPC.
- **debug** RPCs (raw injection) are denied for every role unless com runs with
  `THEIA_COM_DEBUG_RPCS=1`; then any role may call them.
- **Provision** needs role `gs` AND the certificate CN in `THEIA_COM_PROVISION_CN` (comma list,
  default `fleet-gs`). The handler checks the CN again.
- An RPC that is not in the table is **denied** (also RPCs of services com does not serve).
- A refused call ends `PERMISSION_DENIED` with the reason, e.g.
  `com: /services.com.SupervisorView/RestartChild needs role operator; caller 'rig-b' (role rig from uri)`,
  and com logs one `authz:` line.
- Without `THEIA_COM_TLS_REQUIRED` nothing is checked (developer run, as before).

| RPC | minimal role |
|---|---|
| SupervisorView/Subscribe | rig |
| SupervisorView/ListMachines | rig |
| SupervisorView/GetSystemInfo | rig |
| SupervisorView/GetTraceConfig | rig |
| SupervisorView/GetLogLevelConfig | rig |
| TraceStream/Subscribe | rig |
| LogStream/Subscribe | rig |
| PerView/ListSchemas | rig |
| NmView/GetStatus | rig |
| NmView/WifiScan | rig |
| SupervisorView/RestartChild | operator |
| SupervisorView/TerminateChild | operator |
| SupervisorView/DeleteChild | operator |
| SupervisorView/ConfigureLogLevel | operator |
| SupervisorView/ConfigureTrace | operator |
| SupervisorView/GetTombstone | operator (crash memory, not for peers) |
| PerView/Snapshot | operator (writes a backup file) |
| PerView/GetSnapshot | operator (the whole config store) |
| UcmView/GetProgress | operator |
| VucmView/GetCampaignStatus | operator |
| Provisioning/GetProvisionStatus | operator |
| UcmView/RequestUpdate | gs |
| UcmView/Confirm | gs |
| VucmView/CheckForCampaign | gs |
| VucmView/Decide | gs |
| NmView/AddWifi, RemoveWifi, SetVpn, SetAutoConnect, ConfirmConfig, AbortConfig | gs |
| Provisioning/Provision | gs + CN in `THEIA_COM_PROVISION_CN` |
| SupervisorView/StartChild | debug |
| SupervisorView/InjectSignal | debug |
| SupervisorView/CallService | debug |
| DiagView/SendUds | debug |

All names are `/services.com.<Service>/<Method>` on the wire. Who uses what: the ground station
(gs-api, role `gs`, CN `fleet-gs`) calls ListMachines, PerView/GetSnapshot, the UcmView and VucmView
RPCs, and is the only caller Provision admits; operators (supervisor-gui, rtdb, `tools/operator-cert.sh`, role `operator`) observe,
manage the tree and configure trace and log levels.

Adding an RPC to the proto means adding it to `kRpcTable`: `//services/com/test:com_roles_test` fails
on any RPC of the proto that the table does not name (and on table rows the proto does not have).

## Tests

- `//services/com/test:com_roles_test` — the role table against the proto, and an in-process server
  built with com's real credentials (`THEIA_COM_TLS_REQUIRED=1`) answering rig / operator / gs
  certificates generated in the test (debug RPCs, the Provision CN gate, unknown RPCs, OU fallback,
  URI-over-OU, a developer run staying open).
- `//services/com/test:test_systeminfo_roundtrip` — nanopb ↔ libprotobuf SystemInfo.
- `//services/com/test:test_cert_agreement` — com/crypto certificate agreement (skips without crypto).
