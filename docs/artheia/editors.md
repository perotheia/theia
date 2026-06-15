# Artheia editor integrations

Editor support for Artheia `.art` files lives in the umbrella repo under
`contrib/editors/` (split out of the artheia subrepo so all editor tooling sits
together). Every client drives the **same** language server, `artheia-lsp`:

- TextMate / font-lock grammar for highlighting (works without the server).
- An LSP client for diagnostics, go-to-definition (port → interface,
  prototype → node), and keyword / in-workspace-symbol completion.

```
contrib/editors/
  vscode/   VS Code / VSCodium extension
  emacs/    Emacs major mode + lsp-mode client (artheia-mode.el)
```

## The language server

Install `artheia` into the Python you'll run the editor against — the
`artheia-lsp` console script comes with it:

```sh
# deb install — bundled wheels into YOUR venv:
python3 -m venv .venv && . .venv/bin/activate
pip install --find-links /opt/theia/wheels artheia

# source checkout (editable; live server edits):
pip install -e /path/to/artheia
```

Confirm `artheia-lsp` is on `PATH` (or override the command per client below).

## VS Code / VSCodium

```sh
cd contrib/editors/vscode
npm install
npm run compile
npx vsce package --no-dependencies
code --install-extension artheia-*.vsix
```

Settings:

- `artheia.serverCommand` — command to launch the language server (default
  `artheia-lsp`).
- `artheia.serverArgs` — extra args passed to the language server.

## Emacs

Requires Emacs 27.1+ and [`lsp-mode`](https://emacs-lsp.github.io/lsp-mode/).
Put `contrib/editors/emacs/` on the `load-path`:

```elisp
(require 'artheia-mode)                 ;; .art → artheia-mode, LSP client registered
(add-hook 'artheia-mode-hook #'lsp)     ;; auto-start the server on open
```

Override the server command with `artheia-lsp-server-command` (default
`'("artheia-lsp")`). `artheia-mode` works standalone (font-lock, comments,
indentation) even without `lsp-mode`.

## Workspace conventions

- The LSP looks for `*catalog*.json` files anywhere in the workspace to
  populate completion for gateway/AUTOSAR messages.
- Generate one with `artheia import-dbc --dbc ... --bus ... --out vendor/autosar/<bus>/`
  (or `import-fibex` for FlexRay). Each writes a `catalog.json` next to a `package.art`.
