# Trace panel — Wireshark-style layout

> **STATUS (implemented + live):** the panel ships in
> `tools/supervisor-gui/src/panels/trace_panel.cpp` — virtual `wxListCtrl` top
> + lazy `wxTreeCtrl` detail + filter, exactly this spec. **Data source CHANGED**
> from the spec below: it now follows com's **TraceForwarder TraceStream**
> (:7710, tag 0x0005) — the live message traces `tdb logcat` / `rtdb logcat`
> show (node→node casts/calls) — NOT the etcd `/theia/events/` SupervisionEvent
> feed (the supervisor lifecycle firehose has no remote egress under the
> snapshot-only pull model). Columns are TIME｜MACHINE｜KIND｜SRC｜DST｜MSG TYPE;
> detail groups are Header / Subject / Raw payload. The KIND coloring,
> key:value filter (`src:`/`dst:`/`msg:`/`kind:`/`machine:`), and lazy hex
> payload all work. Deferred from this spec: tombstone tail group (lifecycle-
> only), Save-to-`.tracelog`, filter presets, per-FC custom dissectors.

Refines Phase 10 of `extend-supervisor-GUI.md` (Trace Overview rework)
with a concrete look modeled on Wireshark's three-pane window: list
on top, decoded tree below, optional bytes pane at the bottom.

## Layout

```
+------------------------------------------------------------------+
| filter: [kind:RESTART child:demo_p1____________] [Clear] [Save]  |
+------------------------------------------------------------------+
| TIME-OFFSET | MACHINE | KIND       | CHILD    | PARENT  | DETAIL |
|-------------|---------|------------|----------|---------|--------|
| +0.000s     | central | STARTED    | exec     | core_sup|        |
| +0.012s     | central | STARTED    | core     | core_sup|        |
| +1.234s     | central | EXITED     | demo_p1  | app_sup | exit=127
| +1.234s     | central | RESTARTED  | demo_p1  | app_sup |        |  ← selected
| +5.678s     | compute | TOMBSTONE  | demo_p3  | app_sup | sig=11
| ...                                                              |
+------------------------------------------------------------------+
| Selected event detail                                            |
|                                                                  |
| ▼ Header                                                         |
|     timestamp_ms       1735901234567                             |
|     machine            central                                   |
|     kind               RESTARTED  (3)                            |
|     correlation_id     0x42                                      |
| ▼ Subject                                                        |
|     child_name         demo_p1                                   |
|     supervisor_name    app_sup                                   |
|     pid                1245   →   click to drill into Processes  |
| ▼ Cause                                                          |
|     exit_code          127                                       |
|     strategy           one_for_one                               |
|     detail             execvp failed: …                          |
| ▶ Tombstone            (collapsed; click to expand path + tail)  |
| ▶ Raw payload          (collapsed; bytes view on demand)         |
+------------------------------------------------------------------+
```

Top pane = chronological list (wxListCtrl, virtual mode for scale).
Bottom pane = wxTreeCtrl with collapsible groups; only the selected
row is decoded — **lazy**.

## Why this shape

- Same mental model every network engineer already has (Wireshark,
  tshark, dbeaver), zero ramp-up.
- Cheap to render: only the visible rows in the top list are
  formatted; only the selected row in the bottom pane is decoded.
- Scales to bursts: tens of thousands of events stay snappy as
  long as the list is virtual and the detail pane is lazy.

## Concrete behaviors

### Top list
- **Time offset** column shows `+S.mmm`s from the first visible
  event (Wireshark-style). Absolute timestamp is in the detail
  pane.
- Columns sortable; default sort by ts ascending; auto-scroll to
  bottom while "Follow new events" is checked (default on).
- Row colors by kind: STARTED green, EXITED yellow, TOMBSTONE
  red, RESTARTED orange, escalation cascade in purple. (Match
  Wireshark's tcp.analysis.flags coloring habit.)
- Right-click row:
  - "Mark" — pin the row with a left-margin glyph so it stays
    visible during scrolling.
  - "Drill into Processes" — switch tabs, select the row's child
    in the Processes panel.
  - "Show tombstone" — only if kind == TOMBSTONE; opens the file
    in the OS's default editor.
  - "Copy row as JSON" — for ticket pasting.

### Bottom detail pane
- Tree node groups: **Header** (ts/machine/kind/corr-id) ·
  **Subject** (child/parent/pid) · **Cause** (exit_code/strategy/
  detail) · **Tombstone** (path + tail, lazy) · **Raw payload**
  (hex+ascii, lazy).
- Each leaf is `name : value` — keep it grep-able even when
  read aloud over a call.
- Click `pid` → Processes panel + select that row.
- Click `child_name` → Applications panel + highlight that node
  in the drawn supervisor tree.

### Filter
- Filter input at the top accepts the same key:value syntax we use
  for supdbg-style queries: `kind:EXITED child:demo_p1 machine:central`.
- Empty → show everything. Hit Enter to apply; live filtering as
  you type would be nice-to-have, defer if it lags.
- Save/Load filter presets to `~/.config/theia/trace_filters.yaml`.

### Data source
- Subscribes to etcd watch on `/theia/events/` (per
  etcd-state-backbone.md). Events are protobuf-encoded; the panel
  keeps the raw bytes plus a decoded view-model alongside.
- "Save" button dumps the current visible (post-filter) rows to
  a `.tracelog` file — protobuf-length-prefixed so we can replay
  later via a `--load` flag.

### Lazy protobuf decode
- Rows in the top list show only the small "header" fields
  (timestamp, kind, child name, parent, brief detail). No protobuf
  parsing beyond that — we keep the raw bytes around.
- The bottom detail pane runs the *full* decode only when its
  row becomes selected. Avoids paying the parsing cost for events
  that flash by during a restart cascade.

## What's not in this spec (deferred)

- Custom dissectors per FC payload — once application traffic is
  exposed via the trace stream, each FC's payload needs its own
  decoder. Defer until apps actually emit such events.
- Cross-event correlation visualization (Wireshark's "Follow TCP
  stream" analogue) — would let an operator pick one
  correlation_id and see the chain of RPCs that produced it.
  Useful but not v1.
- Time-warp playback / step-through — replay a saved tracelog at
  N× speed, watch the GUI react. Operator-toy nice-to-have.

## Implementation notes

- wxListCtrl in `wxLC_VIRTUAL` mode is the right call — calls
  back into `OnGetItemText` only for visible rows.
- wxTreeCtrl for the detail pane — collapse all groups by default,
  remember per-kind expansion preferences in wxConfig.
- The top/bottom split is a `wxSplitterWindow`; remember the sash
  position in wxConfig.
- The lazy-decode hook needs a `pb_msgdesc_t*` lookup keyed by
  message type, since the trace stream carries multiple proto
  types (SupervisionEvent today; later trace records too). A
  small TypeRegistry::decode(bytes, type_id) → wxTreeNodeData is
  the right abstraction.

## Relation to neighbors

- Supersedes the "Phase 10 — Trace Overview rework" entry in
  `extend-supervisor-GUI.md` with this concrete spec.
- Consumes the `/theia/events/<machine>/<ts>-<seq>` keys
  published by the supervisor's etcd publisher (already shipped
  in `etcd-state-backbone.md` Phase 2).
- Once `services/log` lands (BACKLOG task #196), this same
  panel may grow a parallel feed for live runtime traces — same
  layout, different data source.
