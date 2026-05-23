```
reproducible - entering Table Viewer / prefix: /theia/events/central .. centralll <- crash
core.3103949


 Output/messages ──────────────────────────────────────────────────────────────────────────
[Thread 0x7fffeb7fe640 (LWP 3173583) exited]
[Thread 0x7fffea7fc640 (LWP 3173584) exited]
[Thread 0x7fffeaffd640 (LWP 3173582) exited]

Thread 1 "supervisor-gui" received signal SIGSEGV, Segmentation fault.
0x00007ffff77337fc in wxWindowBase::TryBefore(wxEvent&) () from /lib/x86_64-linux-gnu/libwx_gtk3u_core-3.2.so.0
─── Assembly ─────────────────────────────────────────────────────────────────────────────────
 0x00007ffff77337fc  ? cmp    0x10(%rsi),%rdi
 0x00007ffff7733800  ? je     0x7ffff7733810 <_ZN12wxWindowBase9TryBeforeER7wxEvent+32>
 0x00007ffff7733802  ? add    $0x10,%rsp
 0x00007ffff7733806  ? mov    %rbp,%rdi
 0x00007ffff7733809  ? pop    %rbp
 0x00007ffff773380a  ? jmp    0x7ffff74bc990 <_ZN12wxEvtHandler9TryBeforeER7wxEvent@plt>
 0x00007ffff773380f  ? nop
 0x00007ffff7733810  ? mov    (%rdi),%rax
 0x00007ffff7733813  ? lea    -0x21f25a(%rip),%rdx        # 0x7ffff75145c0
 0x00007ffff773381a  ? mov    0x2b0(%rax),%rax
─── Breakpoints ──────────────────────────────────────────────────────────────────────────────
─── Expressions ──────────────────────────────────────────────────────────────────────────────
─── History ──────────────────────────────────────────────────────────────────────────────────
─── Memory ───────────────────────────────────────────────────────────────────────────────────
─── Registers ────────────────────────────────────────────────────────────────────────────────
      rax 0x0000555555626fd8       rbx 0x00007fffffffa130         rcx 0x00007ffff541ac98
      rdx 0x0000555555dbd000       rsi 0x0000000555555e94         rdi 0x00005555557d3930
      rbp 0x00005555557d3930       rsp 0x00007fffffffa110          r8 0x0000555555e94530
       r9 0x0000555555e94530       r10 0x0000000000000007         r11 0x748134a39194b463
      r12 0x0000000000000004       r13 0x00007fffffffa2f0         r14 0x00007fffffffa608
      r15 0x00007fffffffa260       rip 0x00007ffff77337fc      eflags [ IF RF ]         
       cs 0x00000033                ss 0x0000002b                  ds 0x00000000        
       es 0x00000000                fs 0x00000000                  gs 0x00000000        
─── Source ───────────────────────────────────────────────────────────────────────────────────
─── Stack ────────────────────────────────────────────────────────────────────────────────────
[0] from 0x00007ffff77337fc in wxWindowBase::TryBefore(wxEvent&)
[1] from 0x00007ffff7778ad6 in wxLogGui::DoLogRecord(unsigned long, wxString const&, wxLogRecordInfo const&)
[2] from 0x00007ffff6f010d4 in wxLog::CallDoLogNow(unsigned long, wxString const&, wxLogRecordInfo const&)
[3] from 0x00005555555e01e1 in wxLogger::DoLogWithPtr(void*, wchar_t const*, ...)
[4] from 0x00005555555e80fa in sup_gui::EtcdPanelImpl::refresh_keys()
[5] from 0x00005555555e9293 in sup_gui::EtcdPanelImpl::on_prefix_enter(wxCommandEvent&)
[6] from 0x00007ffff6f9e5c6 in wxEvtHandler::ProcessEventIfMatchesId(wxEventTableEntryBase const&, wxEvtHandler*, wxEvent&)
[7] from 0x00007ffff6f9fdb6 in wxEvtHandler::SearchDynamicEventTable(wxEvent&)
[8] from 0x00007ffff6fa0144 in wxEvtHandler::TryHereOnly(wxEvent&)
[9] from 0x00007ffff6fa01ef in wxEvtHandler::ProcessEventLocally(wxEvent&)
[+]
─── Threads ──────────────────────────────────────────────────────────────────────────────────
[24] id 3173605 name grpc_global_tim from 0x00007ffff5291117 in __futex_abstimed_wait_common64+179 at ./nptl/futex-internal.c:57
[23] id 3173604 name supervisor-gui from 0x00007ffff5325eae in epoll_wait+94 at ../sysdeps/unix/sysv/linux/epoll_wait.c:30
[22] id 3173603 name grpc_global_tim from 0x00007ffff5291117 in __futex_abstimed_wait_common64+179 at ./nptl/futex-internal.c:57
[21] id 3173602 name resolver-execut from 0x00007ffff5291117 in __futex_abstimed_wait_common64+179 at ./nptl/futex-internal.c:57
[20] id 3173601 name default-executo from 0x00007ffff5291117 in __futex_abstimed_wait_common64+179 at ./nptl/futex-internal.c:57
[3] id 3173535 name gdbus from 0x00007ffff5318c4f in __GI___poll+79 at ../sysdeps/unix/sysv/linux/poll.c:29
[2] id 3173516 name gmain from 0x00007ffff5318c4f in __GI___poll+79 at ../sysdeps/unix/sysv/linux/poll.c:29
[1] id 3173487 name supervisor-gui from 0x00007ffff77337fc in wxWindowBase::TryBefore(wxEvent&)
─── Variables ────────────────────────────────────────────────────────────────────────────────
──────────────────────────────────────────────────────────────────────────────────────────────
>>> 


```