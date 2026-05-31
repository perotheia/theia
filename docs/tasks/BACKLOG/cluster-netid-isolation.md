# Cluster isolation by TIPC netid

A `cluster` (the distribution bundle — e.g. `gateway`, `Services`) should map to
a TIPC **netid** so clusters on different machines are addressable + isolated:

```
Host  -> netid 4711
RPI   -> netid 4712
```

This lets `tdb attach <cluster>` target a specific cluster/machine
(`tdb attach gateway` → the gateway cluster on its netid) and keeps service
`(type,instance)` namespaces from colliding across machines.

We do NOT have this yet — today TIPC runs one flat netid and clusters share the
service-address space. Until it lands, `tdb attach` works only against the local
cluster; cross-cluster/remote attach is out of scope.

Surfaced from `docs/tasks/TODO/composition-isolation-test.md` (the `tdb`
design). Scope when picked up:
- map `cluster <Name>` (artheia) → a netid; carry it in the manifest/rig.
- bring TIPC up per-machine with the cluster's netid.
- `tdb attach <cluster>` resolves the netid and scopes the observer subscription
  + node listing to it.
