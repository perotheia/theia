# Table Viewer panel — SIGSEGV on prefix edit (FIXED)

## Reproducer (original)

> reproducible — entering Table Viewer, prefix `/theia/events/central` with a stray suffix (`centralll`), Enter → SIGSEGV.

Stack:

```
[0] wxWindowBase::TryBefore(wxEvent&)           ← SEGV
[1] wxLogGui::DoLogRecord(...)
[2] wxLog::CallDoLogNow(...)
[3] wxLogger::DoLogWithPtr(...)
[4] EtcdPanelImpl::refresh_keys()
[5] EtcdPanelImpl::on_prefix_enter(wxCommandEvent&)
```

## Root cause

Two-stage:

1. **Original (pre-`9bac995`):** `refresh_keys()` called
   `wxLogStatus(outer_, ...)`. `wxLogStatus` walks the parent chain
   of `outer_` looking for a wxFrame to call `SetStatusText` on.
   The default active log target was `wxLogGui`, which during that
   walk dereferenced `outer_` while the panel was mid-event-dispatch
   from `on_prefix_enter` — `TryBefore` SIGSEGV.
2. **Latent in `9bac995`'s fix:** that commit switched to
   `wxLogMessage(...)` and added `wxLog::SetActiveTarget(new
   wxLogStderr())` in main.cpp. That bypasses the wxLogGui parent
   walk for most calls, but `wxLogMessage` STILL routes through
   `wxLogger / CallDoLogNow / DoLogRecord` which can synthesize
   wx events and recurse into the event loop. Less crash-prone
   but not crash-free.

Separate latent bug in the same code path: implicit
`std::string → wxString` conversions are locale-dependent and
can produce strings with embedded NULs on non-UTF-8 bytes — would
manifest as garbage-rendered list rows or downstream crashes on
binary etcd values.

## Fix (committed in this round)

- **Drop the wx log dispatcher entirely from this panel.** New
  file-scope `log_line(fmt, ...)` writes timestamped lines
  straight to `stderr`. No wxLogger / wxLog::CallDoLogNow /
  wxLogGui / wxLogStderr involvement at all. The wxLogStderr
  install in main.cpp stays as a defence in depth.
- **All `wxString` conversions are explicit:**
  - keys/values → `wxString::FromUTF8(data, size)` with
    empty-fallback to hex render
  - integer fields → `wxString::Format("%lld", ...)`
  - constants → `wxString::FromAscii("...")`
- **`SetValue` → `ChangeValue`** on the right-pane text controls
  so updating the metadata/value pane doesn't fire `wxEVT_TEXT`
  that re-enters the event handlers.
- **Freeze/Thaw** around list repopulation — prevents intermediate
  paint events firing during partial-state list mutation.
- **Detail-pane cleared** when refreshing the list, so a stale
  selection from a previous prefix can't reference a row that no
  longer exists.

After the fix, typing a non-matching prefix prints:

```
10:14:00.675 etcd_panel: ls: 0 keys under '/theia/events/centralll'
```

…and the list goes empty without any wx event flying around.

## Verification

```bash
LD_LIBRARY_PATH=$(pwd)/third_party/etcd-cpp-apiv3/install/lib \
  bazel-bin/supervisor-gui/supervisor-gui/bin/supervisor-gui
# Open Table Viewer tab.
# Type random prefixes, Enter each time.
# Try a binary value:
#   ETCDCTL_API=3 etcdctl --endpoints=127.0.0.1:2379 \
#     put /theia/binary "$(printf '\xff\xfe\x00\x01\x02')"
# Click the row. Panel renders the value as hex; no crash.
```

## Related GTK warnings (separate)

```
Gtk-WARNING **: gtk_widget_size_allocate(): attempt to allocate widget with width 13 and height -7
```

Layout sizing issue — the splitter's right pane shrinks to a
negative height in certain window-resize sequences. Not
crash-causing; track separately if it becomes a usability issue.
