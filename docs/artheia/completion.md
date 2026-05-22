# Shell completion

Both `artheia` and the workspace launcher `theia` use [click]'s
built-in shell completion. No extra dependencies, no
[argcomplete] / [bash-completion] needed — just source the
right snippet in your shell rc file.

[click]: https://click.palletsprojects.com/en/stable/shell-completion/
[argcomplete]: https://pypi.org/project/argcomplete/
[bash-completion]: https://github.com/scop/bash-completion

## Quick setup

Pick the line that matches your shell:

```bash
# bash — add to ~/.bashrc
eval "$(_ARTHEIA_COMPLETE=bash_source artheia)"
eval "$(_THEIA_COMPLETE=bash_source theia)"
```

```zsh
# zsh — add to ~/.zshrc
eval "$(_ARTHEIA_COMPLETE=zsh_source artheia)"
eval "$(_THEIA_COMPLETE=zsh_source theia)"
```

```fish
# fish — add to ~/.config/fish/config.fish
_ARTHEIA_COMPLETE=fish_source artheia | source
_THEIA_COMPLETE=fish_source theia | source
```

Open a new shell (or `source` the rc file) and tab-complete:

```
$ theia <Tab>
bazel                generate-manifest    gen-rig
executor             gen-app              gui
gen-app-composition  gen-app-dispatch     ...

$ theia gen-rig <Tab>
ART_FILE  (positional)

$ theia executor <Tab>
emit

$ theia executor emit <Tab>
--help  --out  --rig  ...
```

## How it works

The `eval "$(_<NAME>_COMPLETE=bash_source <cmd>)"` form is click's
[bash_source mode]. Click prints a small bash function that:

1. Hooks `complete -F` for the program.
2. On `<Tab>`, re-invokes the program with
   `COMP_WORDS`/`COMP_CWORD` env vars and
   `_<NAME>_COMPLETE=bash_complete`, which makes click emit
   completion suggestions on stdout.
3. The bash function reads them back and fills `COMPREPLY`.

[bash_source mode]: https://click.palletsprojects.com/en/stable/shell-completion/#enabling-completion

All click-defined subcommands, options, and `click.Path` /
`click.Choice` arguments tab-complete out of the box.

## Caching (faster startup)

If `eval $(...)` on every shell start feels slow (it parses click's
generator output each time), cache the completion script:

```bash
# Generate once, source on every shell start.
_ARTHEIA_COMPLETE=bash_source artheia > ~/.cache/artheia-completion.bash
_THEIA_COMPLETE=bash_source theia    > ~/.cache/theia-completion.bash

# Then in ~/.bashrc:
source ~/.cache/artheia-completion.bash
source ~/.cache/theia-completion.bash
```

Regenerate the cache when artheia or theia gains new commands
(rarely — once every few weeks at most).

## Workspace integration

If the repo ships an `envrc` or similar shell-setup script, drop
the two eval lines in there. Then `direnv` / sourced setup
auto-enables completion when you `cd` into the workspace.

The workspace's `.venv/bin/` must be on `$PATH` for the completion
hook to find the executables. Most users already have it on PATH
via `source .venv/bin/activate`.

## Troubleshooting

**`command not found: artheia`**
The venv isn't on `$PATH`. Either run `source .venv/bin/activate`
or prefix invocations with `.venv/bin/`.

**Tab does nothing**
- Confirm the eval line ran cleanly: `_ARTHEIA_COMPLETE=bash_source artheia | head -5` should print bash function code, not the program's `--help` output. If it prints `--help`, the env var name doesn't match (recheck: `_ARTHEIA_COMPLETE` not `_artheia_complete`).
- Confirm `complete -p artheia` shows the hooked function after sourcing.

**Tab is slow**
Use the cached form above. Click's `bash_source` mode does a full
program startup on every tab keystroke.

**Completion not showing the latest commands**
The completion source reflects whatever commands existed at
generation time. If you cached the script, regenerate it after
adding new commands.

## Why click and not argcomplete?

Both work. Click's was chosen because:
- It comes with click — no separate `pip install argcomplete`.
- Click subcommand groups (`artheia executor emit`) tab-complete
  natively without manual config.
- The same mechanism works for any click app on the workspace
  (artheia, theia, eventually others).

Mosaic's `moz` used [argcomplete] on `argparse` for the same
reason — different framework, same idea.
