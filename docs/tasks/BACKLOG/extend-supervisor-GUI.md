# supervisor-gui: full observer port (BEAM → Linux/Theia)

**Goal:** ship a `supervisor-gui` that mirrors Erlang's `observer`
tool exactly in look and shape, modulo the semantic shift from a
single BEAM VM to a fleet of Linux supervisor processes. No stubs,
no "TODO: implement later" comments in committed code. After this
lands, the tester will iterate on real polish issues, not missing
features.

## Reference shape (observer)

| Observer tab | LOC (.erl) | What it shows |
| --- | --- | --- |
| System | 242 | host info, CPU/RAM totals, uptime, version |
| Load Charts | 879 | scheduler util, IO util, memory time-series |
| Memory Allocators | 328 | per-allocator carrier/block stats |
| Applications | 587 | supervisor trees, drawn graphically, per OTP app |
| Processes | 806 | sortable table of every process, drill-down |
| Ports | 639 | sortable table of every port, drill-down |
| Sockets | 739 | sortable table of every gen_tcp socket |
| Table Viewer | 539 | ETS tables — list, browse rows |
| Trace Overview | 1263 | live event log with left-side process/node filter |

Menus (observer): `File` (Examine Crashdump, Quit) · `Nodes`
(dynamic node list + Connect Node) · `Log` (Toggle log view) ·
`Help` (About, Help).

## Concept mapping (BEAM → Linux/Theia)

| Observer concept | Our equivalent | Source of truth |
| --- | --- | --- |
| node (BEAM VM) | machine (one supervisor per host) | `machines.yaml` |
| process (lightweight) | OS thread (per-thread sample) | `ChildState.threads_detail[]` |
| port (Erlang BIF) | open file descriptor / TIPC socket | `/proc/<pid>/fd/`, `lsof` |
| ETS table | etcd key/value store | etcd v3 gRPC (`Range` / `Watch`) |
| socket (gen_tcp) | TCP socket | `/proc/<pid>/net/tcp{,6}` |
| atom table | n/a — drop | — |
| allocator stats | per-process RSS/VSZ + system mem | `/proc/meminfo`, `ChildState.rss_kb` |
| application (OTP app) | supervisor branch | `TreeSnapshot.children` |
| crashdump | tombstone file | `/var/log/theia/tombstones/<child>/` |
| trace | `SupervisionEvent` stream (later +trace) | services/com `Subscribe` |

**Tabs after the mapping:** System, Load Charts, Memory, Applications,
Processes, Sockets, Table Viewer (etcd), Tombstones, Trace Overview.
Drop Ports (no analogue — TIPC sockets fold into Sockets). Table
Viewer keeps its name; backing store is etcd v3 (Theia's planned
distributed KV) instead of BEAM's ETS, but the row-browse UX is the
same.

## Phase plan

Each phase is one self-contained, mergeable commit. Tester iterates
on each before the next ships.

### Phase 1 — Menu + machines side panel (foundation)

**Why first:** every other phase needs to react to "connect/disconnect
machine" and "quit"; doing these once at the frame level is cheap and
unblocks the panels.

- `wxMenuBar` with `File` (Quit), `Machines` (dynamic list from
  `machines.yaml` + Connect/Disconnect), `View` (Toggle log dock),
  `Help` (About, Open online docs).
- Left-side `wxSplitterWindow`: machines list on the left, current
  notebook on the right.
- Machine list rows: name, status dot (green=connected, red=down,
  grey=disconnected), context-menu (Connect, Disconnect, Show in
  Trace, Restart all).
- `MainFrame::on_machine_state_changed` broadcast — panels subscribe.

### Phase 2 — System tab

- Per-machine `wxStaticBox` (already half-done). Add every
  HealthBeacon field: uptime, gen, total/active workers, restart
  count, tombstone count.
- Add a SystemInfo unary RPC to services/com (returns hostname,
  kernel, cpu count, total RAM, theia git sha). Render in the top
  of each machine box.
- Refresh button in the panel; auto-refresh checkbox.

### Phase 3 — Load Charts tab

- 4 rolling time-series strips per machine, ~60s window:
  1. Total CPU%
  2. Active/total workers
  3. Restarts/sec
  4. Aggregate RSS MB
- Draw via `wxGraphicsContext` (line graph + filled). Repaint on a
  1Hz timer; data points come from `HealthBeacon` + ChildState.
- Two-panel layout: machine selector at top, charts below.

### Phase 4 — Memory tab (replaces "Memory Allocation")

- Per-machine breakdown: total RSS, shared, data, by FC.
- Stacked bar chart of RSS by FC, sortable.
- Tooltip on hover: pid, threads, exact bytes.

### Phase 5 — Applications tab polish

- Existing `ApplicationsPanel` renders the supervisor tree; make it
  match `observer_app_wx.erl` exactly:
  - Vertical tree layout (root at top, children flowing down)
  - Boxes scaled per LOC observer does
  - Right-click a node → context menu (restart, terminate, view in
    Processes, show tombstones)
- Per-machine sub-tabs at the top of the panel.

### Phase 6 — Processes tab polish

- Existing `wxDataViewCtrl` is htop-style; remap columns to match
  observer_pro_wx.erl exactly:
  Pid, Name, Parent (sup), State, Reds-equivalent (CPU%), Memory,
  Threads, Uptime, Start cmd.
- Right-click row: same context menu as Applications.
- Drill-down dialog (double-click) showing every field from
  ChildState including the threads_detail list with per-thread
  CPU/state.

### Phase 7 — Sockets tab (new)

- Parse `/proc/<pid>/net/tcp` + `tcp6` server-side (extend
  ChildState .art with a SocketInfo list), one row per open socket.
- Columns: machine, pid, local addr:port, remote, state (LISTEN /
  ESTABLISHED / TIME_WAIT…), inode.
- Right-click: lsof-style detail, kill connection (server sends
  RST via supervisor).

### Phase 8 — Table Viewer (etcd) — observer's ETS analogue

- Connect to the cluster's etcd v3 endpoint (separate from
  services/com — pulled from machines.yaml's `etcd_endpoints` once
  we add it).
- List keys (paged, with prefix filter), columns: key, modRev,
  createRev, value size, lease.
- Double-click row → value pane (text/JSON/hex auto-detect, like
  observer's row-browse dialog).
- Watch mode: subscribe to a prefix, surface changes as they happen.
- Right-click: copy key/value, delete (with confirm), put new value.
- Pre-work: add `etcd_endpoints` to machine manifest, ship the
  Theia etcd client library (consider `etcd-cpp-apiv3` or shell out
  to `etcdctl` for v1).

### Phase 9 — DROPPED (tombstones fold into Trace)

Tombstones do NOT get their own tab. The supervisor already emits
a `SupervisionEvent` with `tombstone_path` set whenever it writes
one; that event flows through `services/log` (BACKLOG #196) into
`/theia/events/` and lands in the Trace tab like any other
supervision event.

Trace's lazy-decode tree (see Phase 10 / `GUI-trace-panel-wireshark-style.md`)
gets a **Tombstone** detail group: when the selected event has a
non-empty `tombstone_path`, the group renders a file-tail of the
tombstone log + stderr. Lazy — only loaded when the user clicks
the row.

This avoids a redundant tab and keeps "things that happened" in
one place. The operator scrolls Trace, sees a RED tombstone row,
clicks it, sees the post-mortem. Same UX as Wireshark "Follow
TCP stream" minus the wandering.

### Phase 10 — Trace Overview rework (Wireshark-style)

Detailed spec in
[`GUI-trace-panel-wireshark-style.md`](GUI-trace-panel-wireshark-style.md).
Short version: top pane is a virtual list of events
(time-offset / machine / kind / child / parent / brief detail);
bottom pane is a wxTreeCtrl that lazy-decodes the *selected* row
into Header / Subject / Cause / Tombstone / Raw groups. Filter
input at the top. Data source: etcd watch on `/theia/events/`.
Lazy protobuf decode — top list shows only header fields, bottom
pane runs full decode on selection.

### Phase 11 — End-to-end polish

- Status bar: connected machines count, current backend port, build
  timestamp, log dock toggle.
- Persistent column widths + sort orders (wxConfig).
- Dark/light theme toggle in View menu.
- About dialog with version + git sha.
- Run against the docker compose stack with supdbg-driven
  perturbations (kill children, watch the UI react in real-time);
  fix any flicker / race / leak.

## Pre-work needed in the supervisor backend

These are blocking dependencies that aren't pure GUI work; flag each
as a separate task to schedule before the phase that needs it.

- Phase 2: `SystemInfo` RPC + .art message — supervisor side ~80 LOC
- Phase 7: extend ChildState with `SocketInfo[]`, supervisor samples
  `/proc/<pid>/net/tcp` — ~150 LOC
- Phase 10 (Trace tombstones): `services/log` binary (BACKLOG #196)
  consumes the supervisor's SupervisionEvent stream and persists each
  event into `/theia/events/<machine>/<ts>-<seq>`. The supervisor
  ALREADY writes tombstones to disk + emits the event with the path;
  services/log just bridges. ~150 LOC.
- Phase 8: superseded by `etcd-state-backbone.md` — the Table Viewer
  tab consumes the etcd data published by that plan's Phase 2.
- Phase 9: tombstone list RPC (read-only walk of /var/log/theia/
  tombstones) — ~60 LOC

## What "no stubs" means in practice

- Every tab renders real data when a machine is connected.
- Every tab has a graceful empty-state when no machine connected
  ("Connect a machine via Machines → Connect…").
- Every right-click menu item does something or isn't shown.
- Every TODO comment in the panel files is either resolved or
  filed as a separate followup task (linked from this file).
- Tester can run `supervisor-gui machines.yaml` and click through
  every panel without seeing placeholder text.

## Tester handoff target

After Phase 11:

- One README in `supervisor-gui/` covering build, run, troubleshoot.
- Screenshot inventory in `docs/supervisor-gui-screenshots/`.
- Known-issues list in this file; tester appends, we burn down.