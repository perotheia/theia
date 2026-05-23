# deploy/etcd/ — Theia state backbone

A single-node etcd daemon on the workspace host backs all live Theia
state. Plan in `docs/tasks/BACKLOG/etcd-state-backbone.md`; this
directory holds the runtime artifacts.

## Quick start

```bash
./deploy/etcd/setup.sh        # idempotent: install + start
```

Verify:

```bash
ETCDCTL_API=3 etcdctl --endpoints=127.0.0.1:2379 endpoint status -w table
ETCDCTL_API=3 etcdctl --endpoints=127.0.0.1:2379 put /theia/hello world
ETCDCTL_API=3 etcdctl --endpoints=127.0.0.1:2379 get --prefix /theia
```

## Topology

```
+----------------------+         +----------------------+
| host                 |         | docker compose       |
| (supdbg, GUI,        |         | (supervisor,         |
|  etcdctl, services/db|         |  services/com)       |
|  in the future)      |         |                      |
+--------+-------------+         +----------+-----------+
         |                                  |
         | http://127.0.0.1:2379            | http://172.30.0.1:2379
         |                                  |
         +-----------+----------------------+
                     |
              +------v-------+
              | theia-etcd   |  /var/lib/theia-etcd
              | (systemd)    |  ports 2379 + 2380
              +--------------+
```

The daemon binds two interfaces:

| Interface         | Who uses it                       |
|---|---|
| `127.0.0.1:2379`  | host-local processes (supdbg, etcdctl, GUI) |
| `172.30.0.1:2379` | docker compose containers via `theia_net` bridge gateway |

Both interfaces share state — etcd is one logical store with two
client listeners.

## Files

| File | Purpose |
|---|---|
| `theia-etcd.service` | systemd unit. Installed at `/etc/systemd/system/`. |
| `setup.sh`           | One-shot installer + smoke. Idempotent. |
| `README.md`          | This file. |

## Why not the apt-shipped `etcd.service`?

Two reasons:

1. **Listen address.** The default unit binds 127.0.0.1 only and
   pulls config from `/etc/default/etcd` (envvar-based). We need a
   second bridge-IP listener so compose containers can reach the
   same etcd — easier as a dedicated unit with explicit flags than
   layering env-var overrides.
2. **Data dir separation.** Our unit uses `/var/lib/theia-etcd/`
   so the apt-shipped one's `/var/lib/etcd/default/` can stay
   intact (disabled, but present) — clean rollback if needed.

`setup.sh` disables the apt service automatically.

## Key layout (planned — phase 2+)

Will be populated by the supervisor's etcd publisher:

```
/theia/
├─ machines/<name>                # machine metadata
├─ state/
│  └─ <machine>/
│     ├─ tree/<gen>               # TreeSnapshot, TTL'd via lease
│     ├─ health                   # HealthBeacon, TTL'd
│     └─ child/<name>             # ChildState per child
├─ events/<machine>/<ts>-<seq>    # SupervisionEvent ring
├─ tombstones/<machine>/<child>/  # post-mortem index
└─ app/                            # services/db-managed
   ├─ schema/<app>/v<n>           # schema version metadata
   └─ data/<app>/v<n>/<key>       # typed app state
```

## Operations

```bash
# Watch a prefix live
ETCDCTL_API=3 etcdctl --endpoints=127.0.0.1:2379 watch --prefix /theia

# Snapshot
ETCDCTL_API=3 etcdctl --endpoints=127.0.0.1:2379 snapshot save \
    /var/lib/theia-etcd/snap-$(date +%s).db

# Status of the daemon
sudo systemctl status theia-etcd
sudo journalctl -u theia-etcd -f
```

## Out of scope

- TLS / auth. Bound to localhost + the bridge gateway only; not
  accessible from outside the host. Fine for dev + first customer
  board.
- Multi-node Raft cluster. Single-node is enough until HA matters.
- Snapshot rotation / off-host backup. Add when we have a customer
  deploy story.
