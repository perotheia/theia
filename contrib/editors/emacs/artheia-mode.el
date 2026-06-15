;;; artheia-mode.el --- Major mode + LSP client for the artheia .art DSL  -*- lexical-binding: t; -*-

;; Copyright (C) 2026  Theia / RoboFortis
;; Author: Theia <theia@robofortis.com>
;; Version: 0.1.0
;; Package-Requires: ((emacs "27.1") (lsp-mode "8.0"))
;; Keywords: languages, tools
;; URL: https://cicd.skyway.porsche.com/PG50/pero_theia

;;; Commentary:

;; Emacs support for artheia `.art' files — the host-side DSL for
;; Adaptive-AUTOSAR-style Functional Clusters on the Theia actor runtime.
;;
;; Provides:
;;   - `artheia-mode': font-lock + comments + indentation for .art, the
;;     Emacs counterpart of the VS Code extension (contrib/editors/vscode).
;;   - an `lsp-mode' client that launches the `artheia-lsp' language server
;;     (the SAME server the VS Code extension uses), giving diagnostics,
;;     goto-definition (port -> interface, prototype -> node), and keyword /
;;     in-workspace-symbol completion.
;;
;; Install (with the repo on `load-path'):
;;
;;   (require 'artheia-mode)        ;; .art -> artheia-mode, lsp client registered
;;   (add-hook 'artheia-mode-hook #'lsp)   ;; auto-start the LSP on open
;;
;; The server must be on PATH (the `artheia-lsp' console script): install it
;; into your active venv with `pip install --find-links /opt/theia/wheels
;; artheia' (deb) or `pip install -e /path/to/artheia' (source). Override the
;; command with `M-x customize-variable RET artheia-lsp-server-command'.

;;; Code:

(require 'lsp-mode nil t)

(defgroup artheia nil
  "Support for the artheia .art DSL."
  :group 'languages
  :prefix "artheia-")

(defcustom artheia-lsp-server-command nil
  "Explicit command (program + args) that launches the artheia language server.
When nil (the default), the server is resolved per-buffer by
`artheia-lsp-resolve-command': a workspace `.venv/bin/artheia-lsp', then
~/.local/bin, then the `artheia-lsp' console script on PATH. Set this to pin a
specific server (mirrors the VS Code `artheia.serverCommand')."
  :type '(choice (const :tag "Auto-resolve" nil) (repeat string))
  :group 'artheia)

(defun artheia-lsp-resolve-command ()
  "Return the command list that launches `artheia-lsp' for the current buffer.
Honors `artheia-lsp-server-command' when set; otherwise prefers a workspace
venv (`.venv/bin/artheia-lsp' found by walking up from the file), then
~/.local/bin/artheia-lsp, then whatever `artheia-lsp' is on `exec-path'. This
makes a CONSUMING workspace use ITS OWN venv's server without configuration."
  (or artheia-lsp-server-command
      (let* ((dir (and buffer-file-name
                       (locate-dominating-file buffer-file-name ".venv")))
             (venv (and dir (expand-file-name ".venv/bin/artheia-lsp" dir)))
             (local (expand-file-name "~/.local/bin/artheia-lsp")))
        (list (cond ((and venv (file-executable-p venv)) venv)
                    ((file-executable-p local) local)
                    (t (or (executable-find "artheia-lsp") "artheia-lsp")))))))

(defcustom artheia-indent-offset 4
  "Number of spaces per indentation level in `artheia-mode'."
  :type 'integer
  :group 'artheia)

;; ---------------------------------------------------------------------------
;; Font-lock — keyword sets mirror artheia/lsp/server.py `_KEYWORDS' so
;; highlighting matches exactly what the LSP completes.
;; ---------------------------------------------------------------------------

(defconst artheia--structural-keywords
  '("package" "import" "message" "enum" "interface" "senderReceiver"
    "clientServer" "data" "operation" "returns" "node" "atomic" "runnable"
    "ports" "sender" "receiver" "client" "server" "provides" "requires"
    "composition" "prototype" "connect" "to" "cluster" "params" "config"
    "extern" "on" "process" "bus" "gateway_route" "signal" "statem" "states"
    "initial" "event" "timeout" "after" "halt" "tipc" "type" "instance"
    "requires_timers" "reporting")
  "Structural / declaration keywords of the .art grammar.")

(defconst artheia--modifier-keywords
  '("in" "out" "inout" "reliable" "best_effort")
  "Direction + delivery-semantics modifiers.")

(defconst artheia--type-keywords
  '("int32" "int64" "uint32" "uint64" "sint32" "sint64" "fixed32" "fixed64"
    "sfixed32" "sfixed64" "float" "double" "bool" "string" "bytes")
  "Scalar field types (proto-derived).")

(defconst artheia--constant-keywords
  '("true" "false")
  "Boolean literals.")

(defun artheia--kw-regexp (words)
  "Build a whole-word `font-lock' regexp matching any of WORDS."
  (concat "\\_<" (regexp-opt words t) "\\_>"))

(defconst artheia-font-lock-keywords
  (list
   ;; Declaration name: `node Foo', `message Bar', `composition Baz' …
   (list (concat "\\_<"
                 (regexp-opt '("package" "message" "enum" "interface" "node"
                               "composition" "cluster" "statem" "bus"
                               "prototype" "operation" "signal" "event"))
                 "\\_>[ \t]+\\([A-Za-z_][A-Za-z0-9_.]*\\)")
         '(1 font-lock-function-name-face))
   ;; TIPC address hints: type=0x..., instance=N
   '("\\_<\\(0x[0-9A-Fa-f]+\\)\\_>" 1 font-lock-constant-face)
   (cons (artheia--kw-regexp artheia--structural-keywords) 'font-lock-keyword-face)
   (cons (artheia--kw-regexp artheia--modifier-keywords)   'font-lock-builtin-face)
   (cons (artheia--kw-regexp artheia--type-keywords)       'font-lock-type-face)
   (cons (artheia--kw-regexp artheia--constant-keywords)   'font-lock-constant-face))
  "Font-lock highlighting for `artheia-mode'.")

;; ---------------------------------------------------------------------------
;; Syntax table — // line comments, /* */ block comments, _ in words.
;; ---------------------------------------------------------------------------

(defvar artheia-mode-syntax-table
  (let ((table (make-syntax-table)))
    (modify-syntax-entry ?_  "w"      table)   ; underscore is a word char
    (modify-syntax-entry ?.  "_"      table)   ; dotted FQNs are symbols
    (modify-syntax-entry ?/  ". 124b" table)   ; // and /* */
    (modify-syntax-entry ?*  ". 23"   table)
    (modify-syntax-entry ?\n "> b"    table)
    (modify-syntax-entry ?\" "\""     table)
    table)
  "Syntax table for `artheia-mode'.")

;; ---------------------------------------------------------------------------
;; Indentation — brace-driven, like a C-family block language.
;; ---------------------------------------------------------------------------

(defun artheia-indent-line ()
  "Indent the current line for `artheia-mode' by net open-brace depth."
  (interactive)
  (let ((indent 0) (closing nil))
    (save-excursion
      (beginning-of-line)
      (setq closing (looking-at "[ \t]*[})]"))
      (let ((depth 0))
        (save-excursion
          (goto-char (point-min))
          (let ((end (line-beginning-position)))
            (while (< (point) end)
              (cond ((looking-at "[{(]") (setq depth (1+ depth)))
                    ((looking-at "[})]") (setq depth (max 0 (1- depth)))))
              (forward-char 1))))
        (setq indent (* artheia-indent-offset (if closing (max 0 (1- depth)) depth)))))
    (if (<= (current-column) (current-indentation))
        (indent-line-to indent)
      (save-excursion (indent-line-to indent)))))

;; ---------------------------------------------------------------------------
;; The major mode.
;; ---------------------------------------------------------------------------

;;;###autoload
(define-derived-mode artheia-mode prog-mode "Artheia"
  "Major mode for editing artheia `.art' DSL files."
  :syntax-table artheia-mode-syntax-table
  (setq-local font-lock-defaults '(artheia-font-lock-keywords))
  (setq-local comment-start "// ")
  (setq-local comment-end "")
  (setq-local comment-start-skip "\\(//+\\|/\\*+\\)[ \t]*")
  (setq-local indent-line-function #'artheia-indent-line)
  (setq-local indent-tabs-mode nil)
  (setq-local tab-width artheia-indent-offset))

;;;###autoload
(add-to-list 'auto-mode-alist '("\\.art\\'" . artheia-mode))

;; ---------------------------------------------------------------------------
;; lsp-mode client — spawns `artheia-lsp' over stdio.
;; ---------------------------------------------------------------------------

(with-eval-after-load 'lsp-mode
  (add-to-list 'lsp-language-id-configuration '(artheia-mode . "artheia"))
  (lsp-register-client
   (make-lsp-client
    :new-connection (lsp-stdio-connection
                     #'artheia-lsp-resolve-command)
    :activation-fn (lsp-activate-on "artheia")
    :major-modes '(artheia-mode)
    ;; The server eagerly parses every .art for cross-file goto-definition;
    ;; watch the same files the VS Code client does.
    :server-id 'artheia-lsp
    :priority 0)))

(provide 'artheia-mode)
;;; artheia-mode.el ends here
