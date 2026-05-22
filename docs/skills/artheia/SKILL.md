---
name: artheia
description: Run common artheia (Porsche CMP system DSL) workflows. Args: parse | import-fibex | import-dbc | gen-rig | regen-autosar | regen-netgraph | regen-host-netgraph | gen-app-dispatch | signal-filter | vendor-new | lsp | vscode-install | regen-all | completion
disable-model-invocation: true
---

Invoke artheia for one of the canonical workflows. The skill assumes
the workspace venv is active (`PATH="$PWD/.venv/bin:$PATH"` or
`source .venv/bin/activate`). For the architectural background, see
[docs/architecture/ARCHITECTURE.md](../../architecture/ARCHITECTURE.md).

## Usage

`/artheia <target>`

Some workflows take an extra argument; the `## Targets` section
documents each one's signature.

## Quick reference: what artheia produces from what

| Command | Reads | Writes |
| --- | --- | --- |
| `parse` | any `.art` | parse summary (validation) |
| `import-fibex` | FIBEX cluster `.xml` | per-bus `catalog.json` + `package.art` |
| `import-dbc` | `.dbc` file | per-bus `catalog.json` + `package.art` |
| `gen-netgraph-partition` | catalog.json | `netgraph.json` (PDU → bus address) |
| `gen-autosar-system` | one or more catalogs | `system.art` (mega-node per bus) |
| `gen-host-netgraph` | platform composition `.art` files | `host_netgraph.json` (port → TIPC) |
| `gen-app-dispatch` | PSP manifests + signal CSV | per-app dispatch_table.{c,h} |
| `gen-signal-filter` | vendor system tree | `signal_filter.csv` |
| `gen-rig` | top-level `.art` composition | vendor `rig.py` scaffold |
| `executor emit` | rig (Rig or SoftwareSpecification) | `executor.yaml` (supervisor tree) |
| `gui emit` | rig | `machines.yaml` (per-machine gRPC endpoints) |
| `generate-manifest` | rig | full deploy manifest YAML |

Full diagram + per-artefact schema is in the architecture doc.
For the complete current command list run `artheia --help` (or
`theia --help` for the workspace launcher). For the manifest DSL
(`SoftwareSpecification` + `Layer.squash`) see
[../../artheia/manifest-dsl.md](../../artheia/manifest-dsl.md).

## Targets

### `parse <file.art>` — syntax check

Round-trip a single `.art` through the textX parser; non-zero exit
on any error.

```sh
artheia parse platform/system/system.art
artheia parse vendor/odd_path_client/system/package.art
artheia parse autosar/mlbevo_gen2_cmp_psp/system/system.art
```

Common errors:
- **`Expecting ID, got '_'`** — your identifier starts with `_`. Rename it.
- **`No rule '...' found`** — typo on a keyword (`receiver` vs `received`).
- **Cross-file ref unresolved** — textX is single-file. Forward-declare
  the missing decl inline (see existing `// forward-decl stubs`
  comments throughout the workspace).

A FIBEX-scale `.art` (1000+ messages, 23000+ enums) takes 5–10 min to
parse. Add `--no-validate` to `import-fibex` to skip the post-import
round-trip.

### `import-fibex <bus>` — parse FIBEX into catalog + .art

```sh
PSP=autosar/mlbevo_gen2_cmp_psp
artheia import-fibex \
    --fibex $PSP/config/MLBevo_Gen2_Fx_Cluster_*.xml \
    --bus mlbevo_gen2 \
    --out $PSP/system/mlbevo_gen2 \
    --package autosar.mlbevo_gen2_cmp_psp.system \
    --no-validate
```

Output is **PDU-centric**: one `message <PduName> { ... }` per
APPLICATION PDU, plus companion `enum` decls for signals with value
tables. Wire-level addressing (slot, cycle, channel) lives in the
catalog under `messages[pdu].frame_triggers`.

### `import-dbc <bus>` — parse DBC into catalog + .art

```sh
PSP=autosar/mlbevo_gen2_cmp_psp
artheia import-dbc \
    --dbc $PSP/config/dbc/MLBevo_Gen2_*KCAN_*.dbc \
    --bus kcan \
    --out $PSP/system/kcan \
    --package autosar.mlbevo_gen2_cmp_psp.system
```

CAN side is already PDU-equivalent on the wire (one frame = one PDU);
output is per-frame.

### `regen-autosar` — full AUTOSAR layer rebuild

Run after a FIBEX/DBC update. Re-imports both buses, regenerates the
per-bus netgraph partitions, and re-emits the AUTOSAR mega-node
system.art.

```sh
PSP=autosar/mlbevo_gen2_cmp_psp

# 1. Catalogs (slow on FIBEX; --no-validate skips a textX round-trip)
artheia import-fibex \
    --fibex $PSP/config/MLBevo_Gen2_Fx_Cluster_*.xml \
    --bus mlbevo_gen2 \
    --out $PSP/system/mlbevo_gen2 \
    --package autosar.mlbevo_gen2_cmp_psp.system \
    --no-validate
artheia import-dbc \
    --dbc $PSP/config/dbc/MLBevo_Gen2_*KCAN_*.dbc \
    --bus kcan \
    --out $PSP/system/kcan \
    --package autosar.mlbevo_gen2_cmp_psp.system

# 2. Per-bus netgraph partitions
artheia gen-netgraph-partition \
    --catalog $PSP/system/mlbevo_gen2/catalog.json \
    --out     $PSP/system/mlbevo_gen2/netgraph.json
artheia gen-netgraph-partition \
    --catalog $PSP/system/kcan/catalog.json \
    --out     $PSP/system/kcan/netgraph.json

# 3. Mega-node system.art (one node per bus)
artheia gen-autosar-system \
    --catalog $PSP/system/kcan/catalog.json \
    --catalog $PSP/system/mlbevo_gen2/catalog.json \
    --out     $PSP/system/system.art \
    --package autosar.mlbevo_gen2_cmp_psp.system
```

### `regen-netgraph` — netgraph partitions only

If only the netgraph schema changed (not the catalog), regenerate the
partitions in-place:

```sh
PSP=autosar/mlbevo_gen2_cmp_psp
for bus in mlbevo_gen2 kcan; do
    artheia gen-netgraph-partition \
        --catalog $PSP/system/$bus/catalog.json \
        --out     $PSP/system/$bus/netgraph.json
done
```

### `regen-host-netgraph` — host TIPC LUT

Walks the platform composition `.art` files and emits the
`symbolic_port → TIPC address` LUT. Re-run any time the platform
composition or the vendor app's port list changes.

```sh
artheia gen-host-netgraph \
    --art platform/system/system.art \
    --art vendor/odd_path_client/system/components/odd_path_monitor.art \
    --out platform/config/host_netgraph.json
```

Pass each contributing `.art` file via `--art` (one per file). Pass
**only files with real `tipc=` addresses** — the AUTOSAR mega-nodes
carry synthetic addresses and should not be in the host LUT (their
forward-decls in `gateway/system/package.art` won't surface here as
long as that file isn't passed in).

### `gen-app-dispatch` — per-app gateway dispatch glue

Drives the gateway service's codegen. Reads a signal-selection CSV
(per-vendor) and the PSP manifests; emits `dispatch_table.{c,h}` etc.

```sh
artheia gen-app-dispatch \
    --psp-root autosar/mlbevo_gen2_cmp_psp \
    --csv vendor/<vendor>/config/signal_filter.csv \
    --out platform/gateway/generated/
```

The gateway service's `platform/gateway/generate.sh`
wraps this and resolves the CSV by `$VENDOR` (e.g. `VENDOR=tornado`).

### `signal-filter` — interactive REPL

REPL for searching PSP signals and building a `signal_filter.csv`.
Loads FIBEX + DBC sources via `PlatformDb`, then exposes a search
prompt; on exit, prints the accumulated selection as CSV.

```sh
artheia signal-filter --config autosar/mlbevo_gen2_cmp_psp/config
```

REPL commands: `<signal_name>` (prefix match), `msg:<name>`,
`bus:<name>`, `sel`, `del`, `csv`, `clear`, `help`, `q`.

### `gen-rig <art_file>` — bootstrap a rig.py from a composition

Walks a top-level `.art` composition, groups prototypes by their
`on process X` annotation, and emits a starter `rig.py` exporting
a `SoftwareSpecification` composed against `FcSoftware`.

```sh
artheia gen-rig demo/system/demo/package.art \
    --composition Demo3Way \
    --vehicle-name demo \
    --machine-name demo_host \
    --bazel-package //demo \
    --out demo/manifest/rig.py
```

After generation the user edits TODO markers (machine CPU
arch / endpoint, per-process scheduling) in the emitted file.
Don't regenerate — that overwrites the edits.

Acceptance test: regenerating today's `demo/manifest/rig.py` from
`Demo3Way` and running `artheia executor emit` on the result
produces byte-identical `executor.yaml`.

Full tutorial: [../../artheia/gen-rig.md](../../artheia/gen-rig.md).
Manifest DSL reference: [../../artheia/manifest-dsl.md](../../artheia/manifest-dsl.md).

### `vendor-new <vendor>` — scaffold a vendor app

Step-by-step recipe (the skill does NOT automate the GitLab create
or the gitlab-default-README rebase; do those by hand).

1. **Create the GitLab repo** at `cicd.skyway.porsche.com:PG50/<vendor>.git`
   (PG50/ for sibling-of-pero_theia; PG50/ccstheia/ for under the
   ccstheia subgroup).

2. **Scaffold locally** (use existing vendors as templates):
   ```sh
   mkdir -p vendor/<vendor>/system/components
   # See vendor/odd_path_client or vendor/tornado for the .art layout.
   ```

3. **Init + push**:
   ```sh
   cd vendor/<vendor>
   git init -b main
   git remote add theia git@cicd.skyway.porsche.com:PG50/<vendor>.git
   git add -A && git commit -m "Initial commit"
   git fetch theia            # GitLab may have an auto-created README
   git rebase theia/main      # resolve README conflict in your favor
   git push theia main
   ```

4. **Wire into local manifest**:
   ```sh
   # 1. Add to .repo/local_manifests/vendor_<vendor>.xml (template:
   #    docs/local_manifests/vendor_tornado.xml).
   # 2. Distribute the template via docs/local_manifests/ for teammates.
   # 3. Drop the local stub and let repo manage the checkout:
   cd $WORKSPACE && rm -rf vendor/<vendor> && repo sync vendor/<vendor>
   cd vendor/<vendor> && git checkout main
   ```

5. **Update platform composition** in `platform/system/system.art`:
   forward-decl the vendor's node + add a `prototype` line in the
   `Platform` composition.

6. **Regenerate the host netgraph** to register the new node's ports:
   ```sh
   artheia gen-host-netgraph \
       --art platform/system/system.art \
       --art vendor/<vendor>/system/components/<node>.art \
       --out platform/config/host_netgraph.json
   ```

### `regen-all` — full chain (after FIBEX or composition change)

```sh
# AUTOSAR layer (catalogs + netgraph partitions + system.art)
/artheia regen-autosar

# Host TIPC LUT (re-walks platform composition)
/artheia regen-host-netgraph

# Each gateway-consuming vendor: re-run their gw codegen with the
# vendor CSV. For odd_path_client today the CSV is empty; for tornado
# it's the demo_signals fixture.
cd platform/gateway && VENDOR=tornado bash generate.sh
```

### `lsp` — start the language server

```sh
artheia-lsp --stdio
```

Editors connect over stdio. The LSP provides:
- Diagnostics from the textX parser
- Symbol lookup for messages, interfaces, nodes
- Cross-file message-ref completion when the workspace contains
  package files
- Go-to-definition for `requires <Iface>`, `provides <Iface>`,
  `data <Msg>` references

Install dependencies first: `pip install -e 'artheia/[lsp]'`.

### `vscode-install` — install the VS Code extension

```sh
cd artheia/vscode-extension
npm install
npm run package         # produces artheia-<version>.vsix
code --install-extension artheia-*.vsix
```

The extension provides:
- Syntax highlighting (TextMate grammar)
- Bracket/comment configuration
- LSP client wiring (talks to `artheia-lsp`)

Reload VS Code after installation. Open a `.art` file; status bar
should show "artheia" once the LSP attaches.

### `completion` — enable shell tab-completion

Click ships completion out of the box. Drop into your shell rc:

```bash
# bash (~/.bashrc)
eval "$(_ARTHEIA_COMPLETE=bash_source artheia)"
eval "$(_THEIA_COMPLETE=bash_source theia)"
```

Replace `bash_source` with `zsh_source` / `fish_source` for the
other shells. Open a new shell; `theia <Tab>` lists all
subcommands.

Full doc: [../../artheia/completion.md](../../artheia/completion.md).

## Notes

- All artheia commands accept `--help` for full flag listing.
- The workspace venv must be on `$PATH` (`PATH="$PWD/.venv/bin:$PATH"`)
  before any `artheia` invocation, including under Bazel actions.
- For Bazel-driven codegen (PSP, packaging), `.bazelrc` already has
  `build --action_env=PATH` so PATH propagates into actions.
- After a FIBEX-scale `import-fibex`, the round-trip parse takes
  5–10 minutes; add `--no-validate` to skip it. Skipping is safe
  because the catalog.json schema is well-tested and the emitter is
  deterministic.
- Generated `.art` files carry an `AUTO-GENERATED` header. Don't edit
  them by hand — re-run the generator instead.
