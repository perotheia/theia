# Editor integrations for artheia (`.art`)

Editor clients for the **artheia** DSL — syntax highlighting plus a Language
Server (LSP) client giving diagnostics, goto-definition (port → interface,
prototype → node), and keyword / in-workspace-symbol completion. Every client
here drives the **same** language server, `artheia-lsp`, so the experience is
identical across editors.

```
contrib/editors/
  vscode/   VS Code / VSCodium extension (TypeScript, vscode-languageclient)
  emacs/    Emacs major mode + lsp-mode client (artheia-mode.el)
```

## The language server

All clients spawn the `artheia-lsp` console script (stdio transport). It ships
with the `artheia` package, so it resolves once artheia is installed in the
active Python:

```sh
# deb install — from the bundled wheels into YOUR venv:
python3 -m venv .venv && . .venv/bin/activate
pip install --find-links /opt/theia/wheels artheia

# source checkout:
pip install -e /path/to/artheia        # editable; live server edits
```

`which artheia-lsp` should then succeed. Each client lets you override the
command if it lives elsewhere (see below).

## VS Code / VSCodium

```sh
cd contrib/editors/vscode
npm install && npm run compile        # builds out/extension.js
```

Then load it as a dev extension (`F5` in VS Code on this folder) or package it
with `vsce package` and install the `.vsix`. Settings:

- `artheia.serverCommand` (default `artheia-lsp`)
- `artheia.serverArgs` (default `[]`)

## Emacs

Requires Emacs 27.1+ and [`lsp-mode`](https://emacs-lsp.github.io/lsp-mode/)
(from MELPA). Put `emacs/` on the `load-path` and:

```elisp
(require 'artheia-mode)                 ;; .art → artheia-mode, LSP client registered
(add-hook 'artheia-mode-hook #'lsp)     ;; auto-start the server on open
```

Override the server command with
`M-x customize-variable RET artheia-lsp-server-command` (default
`'("artheia-lsp")`). `artheia-mode` works standalone (font-lock, comments,
indentation) even without `lsp-mode`; the LSP client only registers when
`lsp-mode` is present.

---

These clients were split out of the artheia subrepo so all editor tooling lives
together under the umbrella repo's `contrib/`. The server they depend on
(`artheia.lsp.server`) stays in the `artheia` package.
