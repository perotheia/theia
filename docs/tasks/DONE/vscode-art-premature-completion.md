# vscode-art-premature-completion — DONE 2026-05-23

## Original report

> when cursor navigation completion inserts symbols
>
> i saw it also in emacs modes
>
> imo we need to propose completion only when at least on symbol entered on
> free tocken like name or when syntactic structure has continuation with
> syntax keywords

## Fix

`artheia/artheia/lsp/server.py`:

1. **Trigger characters narrowed.** Was `[".", " ", "\n"]`; now just
   `["."]`. Whitespace and newline no longer pop the completion menu.
2. **Three-branch completion contract:**
   - `CompletionTriggerKind.Invoked` (Ctrl+Space) → full keyword +
     workspace-symbol + catalog-message list.
   - Identifier prefix at cursor (`_identifier_prefix(line_prefix)`
     non-empty) → keywords/symbols filtered case-insensitively to
     those starting with the prefix.
   - Otherwise → empty list. **No items returned on bare cursor
     movement.** This is what prevents VS Code (and emacs lsp-mode)
     from auto-inserting an unwanted symbol when the arrow keys
     move past whitespace.

Memory recorded as `feedback-lsp-completion-only-on-prefix.md` so
this constraint doesn't regress.

## Smoke

`pytest artheia/tests/test_lsp_protocol.py::test_completion_returns_keywords_and_symbols`
covers all three branches end-to-end against a real `artheia-lsp`
subprocess.
