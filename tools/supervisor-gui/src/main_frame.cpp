#include "sup_gui/main_frame.h"
#include "sup_gui/panels.h"

#include "supervisor.pb.h"            // system_supervisor::TreeSnapshot
#include "supervisor_bridge.pb.h"     // services::com::AccelSample

#include <wx/wx.h>
#include <wx/notebook.h>
#include <wx/splitter.h>
#include <wx/statusbr.h>
#include <wx/timer.h>
#include <wx/menu.h>
#include <wx/aboutdlg.h>
#include <wx/filedlg.h>
#include <wx/file.h>
#include <wx/textdlg.h>

#include <sys/stat.h>      // mkdir for the connections.json config dir

#include <cctype>
#include <cstdlib>         // getenv
#include <fstream>         // connections.json load/save
#include <iterator>        // istreambuf_iterator
#include <map>
#include <set>
#include <utility>

namespace sup_gui {

wxDEFINE_EVENT(EVT_SUP_FRAME, wxThreadEvent);

namespace {
// Surface a fetched tombstone: offer a Save dialog (default name
// tombstone-<fc>.txt), write the bytes. A truncated body gets a header noting
// the full file's on-host path. Cancel just drops it (status line already
// reported the size).
void save_tombstone_to_file(wxWindow* parent, const std::string& fc,
                            const GrpcClient::TombstoneResult& t) {
    wxFileDialog dlg(parent, "Save tombstone",
                     "", wxString::Format("tombstone-%s.txt", fc.c_str()),
                     "Text files (*.txt)|*.txt|All files (*.*)|*.*",
                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) return;
    wxFile out(dlg.GetPath(), wxFile::write);
    if (!out.IsOpened()) {
        wxLogError("could not open %s for writing", dlg.GetPath());
        return;
    }
    if (t.truncated) {
        wxString hdr = wxString::Format(
            "# TRUNCATED: %u of %u bytes — full file at %s\n\n",
            static_cast<unsigned>(t.content.size()),
            static_cast<unsigned>(t.total_bytes), t.path.c_str());
        out.Write(hdr.mb_str(), hdr.length());
    }
    out.Write(t.content.data(), t.content.size());
}
}  // namespace

enum : int {
    ID_STATUS_TIMER = wxID_HIGHEST + 1,
    // File / Machines / View / Help menu IDs.
    ID_MENU_REFRESH_MACHINES = wxID_HIGHEST + 100,
    ID_MENU_CONNECT_ALL      = wxID_HIGHEST + 101,
    ID_MENU_DISCONNECT_ALL   = wxID_HIGHEST + 102,
    ID_MENU_CONNECT_HOST     = wxID_HIGHEST + 103,   // Connect… (host:port dialog)
    // The right-click context menu IDs in machines_panel.cpp.
    // Re-declared here for the dispatch in handle_focus_event().
    ID_CTX_CONNECT     = wxID_HIGHEST + 5001,
    ID_CTX_DISCONNECT  = wxID_HIGHEST + 5002,
    ID_CTX_SHOW_TRACE  = wxID_HIGHEST + 5003,
};

wxBEGIN_EVENT_TABLE(MainFrame, wxFrame)
    EVT_CLOSE(MainFrame::on_close)
    EVT_TIMER(ID_STATUS_TIMER, MainFrame::on_status_tick)
wxEND_EVENT_TABLE()

namespace {
struct FrameMsg {
    std::string machine_name;
    std::string payload;
};

// Stage 4 — the single central com AGGREGATES the cluster: its TreeSnapshot
// carries every machine's nodes, with the non-local machines' node + parent
// names prefixed "mN/" (com's merge tag). Split that one snapshot back into a
// per-machine snapshot (prefix stripped) so the existing panels — which key
// every row by machine_name — see clean per-machine trees, exactly as they did
// when the GUI held one gRPC client per machine. The local (unprefixed) nodes
// go under `local_name` (the machine the GUI is connected to, instance 0).
//
// A node name "m1/foo" → machine "m1", node "foo"; "m1/" with empty parent is
// that machine's root. A real node name never contains '/', so the split is
// unambiguous (com guarantees this).
std::map<std::string, system_supervisor::TreeSnapshot>
split_tree_by_machine(const system_supervisor::TreeSnapshot& agg,
                      const std::string& local_name) {
    std::map<std::string, system_supervisor::TreeSnapshot> out;
    auto strip = [](const std::string& s, std::string& machine,
                    std::string& bare) {
        // "m<digits>/rest" → machine="m<digits>", bare="rest". Else machine="".
        if (s.size() > 1 && s[0] == 'm') {
            auto slash = s.find('/');
            if (slash != std::string::npos && slash >= 1) {
                bool all_digits = true;
                for (size_t i = 1; i < slash; ++i)
                    if (!std::isdigit((unsigned char)s[i])) { all_digits = false; break; }
                if (all_digits) {
                    machine = s.substr(0, slash);
                    bare    = s.substr(slash + 1);
                    return;
                }
            }
        }
        machine.clear();
        bare = s;
    };
    for (const auto& ch : agg.children()) {
        std::string m, name_bare, parent_machine, parent_bare;
        strip(ch.name(), m, name_bare);
        strip(ch.parent_name(), parent_machine, parent_bare);
        const std::string machine = m.empty() ? local_name : m;
        auto& snap = out[machine];
        auto* dst = snap.add_children();
        *dst = ch;                         // copy all the resource fields verbatim
        dst->set_name(name_bare);          // prefix stripped
        dst->set_parent_name(parent_bare); // (parent shares the same prefix)
    }
    return out;
}
}  // namespace

MainFrame::MainFrame(std::vector<MachineEndpoint> machines)
    : wxFrame(nullptr, wxID_ANY, "supervisor-gui",
              wxDefaultPosition, wxSize(1400, 800)) {

    // --- menu bar ----------------------------------------------------
    {
        auto* mb = new wxMenuBar();

        auto* file_menu = new wxMenu();
        file_menu->Append(ID_MENU_REFRESH_MACHINES,
                          "&Refresh machines\tCtrl+R",
                          "Reload machine endpoints from machines.yaml");
        file_menu->AppendSeparator();
        file_menu->Append(wxID_EXIT,
                          "&Quit\tCtrl+Q",
                          "Close the supervisor GUI");
        mb->Append(file_menu, "&File");

        auto* machines_menu = new wxMenu();
        machines_menu->Append(ID_MENU_CONNECT_HOST,
                              "&Connect...\tCtrl+N",
                              "Connect to a com aggregator at host:port "
                              "(switches the hub)");
        machines_menu->AppendSeparator();
        machines_menu->Append(ID_MENU_CONNECT_ALL,
                              "Re&connect",
                              "Restart the gRPC client for the current hub");
        machines_menu->Append(ID_MENU_DISCONNECT_ALL,
                              "&Disconnect",
                              "Stop the gRPC client (state stays cached)");
        mb->Append(machines_menu, "&Machines");

        auto* help_menu = new wxMenu();
        help_menu->Append(wxID_ABOUT,
                          "&About supervisor-gui\tF1");
        mb->Append(help_menu, "&Help");

        SetMenuBar(mb);
    }

    // --- splitter: machines panel | notebook -------------------------
    auto* splitter = new wxSplitterWindow(this, wxID_ANY,
                                            wxDefaultPosition, wxDefaultSize,
                                            wxSP_LIVE_UPDATE | wxSP_3DBORDER);
    splitter->SetMinimumPaneSize(160);

    machines_panel_ = new MachinesPanel(splitter);
    notebook_       = new wxNotebook(splitter, wxID_ANY);
    system_panel_   = new SystemPanel       (notebook_);
    load_charts_    = new LoadChartsPanel   (notebook_);
    applications_   = new ApplicationsPanel (notebook_);
    processes_      = new ProcessesPanel    (notebook_);
    etcd_panel_     = new EtcdPanel         (notebook_);
    persistency_    = new PersistencyPanel  (notebook_);
    trace_          = new TracePanel        (notebook_);
    notebook_->AddPage(system_panel_, "System");
    notebook_->AddPage(load_charts_,  "Load Charts");
    notebook_->AddPage(applications_, "Applications");
    notebook_->AddPage(processes_,    "Processes");
    notebook_->AddPage(etcd_panel_,   "Table Viewer");
    notebook_->AddPage(persistency_,  "Persistency");
    notebook_->AddPage(trace_,        "Trace");

    splitter->SplitVertically(machines_panel_, notebook_, 220);

    // --- status bar (3 fields: connected count + focused machine + hb age)
    CreateStatusBar(3);
    SetStatusText("starting...", 0);
    SetStatusText("",          1);
    SetStatusText("",          2);

    Bind(EVT_SUP_FRAME, &MainFrame::on_sup_frame, this);

    // Catch all the menu-bar selections + the per-row context-menu
    // events that MachinesPanel posts up.
    Bind(wxEVT_MENU, &MainFrame::on_menu, this);
    Bind(EVT_MACHINE_FOCUS, &MainFrame::on_machine_focus, this);
    Bind(EVT_CONNECTION_SELECT, &MainFrame::on_connection_select, this);

    status_timer_ = new wxTimer(this, ID_STATUS_TIMER);
    status_timer_->Start(500);

    // Stage 4 — the GUI connects to ONE com: the cluster AGGREGATOR (central).
    // It already merges every machine's tree (mN/-prefixed) + the per-machine
    // SHWA telemetry, so a single Subscribe stream carries the whole cluster.
    // Pick the endpoint named "central" if present, else the first row.
    MachineEndpoint hub = machines.empty()
        ? MachineEndpoint{"central", "127.0.0.1", 7700}
        : machines.front();
    for (const auto& m : machines)
        if (m.name == "central") { hub = m; break; }
    local_name_ = hub.name;

    current_hub_ = hub.address + ":" + std::to_string(hub.port);

    // Connections (top section): load the persisted set, seed any machines.json
    // endpoints, and make sure the active hub is listed + marked Connecting. The
    // Machines section (bottom) starts EMPTY — note_machine() fills it from the
    // aggregated stream as peers are discovered, and auto-selects the first.
    load_connections();
    for (const auto& m : machines) {   // already excludes admin/host endpoints
        machines_panel_->add_connection(
            m.name, m.address + ":" + std::to_string(m.port),
            MachineConnState::Disconnected);
    }
    machines_panel_->add_connection(hub.name, current_hub_,
                                    MachineConnState::Connecting);
    machines_panel_->set_connection_state(current_hub_,
                                          MachineConnState::Connecting);
    save_connections();
    machines_panel_->set_machines({});   // machines fill in via note_machine

    // #365 — wire the ApplicationsPanel's right-click ConfigureTrace
    // dialog to the matching machine's GrpcClient.
    applications_->set_configure_trace_callback(
        [this](const std::string& machine, const std::string& node,
               const std::string& msg, bool enabled, uint32_t kind) {
            static const char* kKind[] = {"ALL", "CAST_OUT", "CAST_IN",
                                          "CALL_OUT", "CALL_IN", "STATEM"};
            const char* kn = (kind < 6) ? kKind[kind] : "?";
            for (auto& c : clients_) {
                if (c && c->machine_name() == machine) {
                    int rc = c->configure_trace(node, msg, enabled, kind);
                    wxString s = wxString::Format(
                        "trace %s %s on %s: rc=%d",
                        enabled ? "enabled" : "disabled", kn,
                        node.c_str(), rc);
                    SetStatusText(s, 0);
                    return;
                }
            }
        });

    // Applications panel context menu → Log level (FC/node), Restart subtree
    // (supervisor), Download tombstone (crashed FC). Each routes to the
    // matching machine's GrpcClient and returns a status line for the bar.
    applications_->set_log_level_callback(
        [this](const std::string& machine, const std::string& target,
               const std::string& level) -> std::string {
            for (auto& c : clients_) {
                if (c && c->machine_name() == machine) {
                    int rc = c->configure_log_level(target, level);
                    return "log level " + level + " on " + target +
                           ": rc=" + std::to_string(rc);
                }
            }
            return "no client for machine " + machine;
        });

    applications_->set_restart_callback(
        [this](const std::string& machine,
               const std::string& name) -> std::string {
            for (auto& c : clients_) {
                if (c && c->machine_name() == machine) {
                    std::string msg;
                    int rc = c->restart_child(name, &msg);
                    return "restart " + name + " on " + machine +
                           ": rc=" + std::to_string(rc) +
                           (msg.empty() ? "" : "  " + msg);
                }
            }
            return "no client for machine " + machine;
        });

    applications_->set_tombstone_callback(
        [this](const std::string& machine,
               const std::string& fc) -> std::string {
            for (auto& c : clients_) {
                if (c && c->machine_name() == machine) {
                    bool ok = false;
                    auto t = c->get_tombstone(fc, &ok);
                    if (!ok) return "tombstone fetch failed for " + fc;
                    if (!t.found)
                        return "no tombstone for " + fc + " (never crashed)";
                    save_tombstone_to_file(this, fc, t);
                    return "tombstone " + fc + ": " +
                           std::to_string(t.content.size()) + " bytes" +
                           (t.truncated ? " (truncated; full at " + t.path + ")"
                                        : "");
                }
            }
            return "no client for machine " + machine;
        });

    // Applications panel: Kill a PROCESS (RestartChild) + fetch a node's CURRENT
    // log level (for the Log-level submenu checkmark).
    applications_->set_kill_callback(
        [this](const std::string& machine,
               const std::string& proc) -> std::string {
            for (auto& c : clients_) {
                if (c && c->machine_name() == machine) {
                    std::string msg;
                    int rc = c->restart_child(proc, &msg);
                    return "kill " + proc + " on " + machine +
                           ": rc=" + std::to_string(rc) +
                           (msg.empty() ? "" : "  " + msg);
                }
            }
            return "no client for machine " + machine;
        });

    applications_->set_get_log_level_callback(
        [this](const std::string& machine,
               const std::string& node) -> std::string {
            for (auto& c : clients_) {
                if (c && c->machine_name() == machine)
                    return c->get_log_level(node);
            }
            return "";
        });

    // Processes panel right-click → Kill / Remove on the matching machine's
    // GrpcClient. kill = RestartChild (restarts); remove = TerminateChild
    // (no_restart=true, stop-and-hold).
    processes_->set_child_op_callback(
        [this](const std::string& machine, const std::string& name,
               const std::string& op) -> std::string {
            for (auto& c : clients_) {
                if (c && c->machine_name() == machine) {
                    std::string msg;
                    const int rc = (op == "remove")
                        ? c->terminate_child(name, &msg)
                        : c->restart_child(name, &msg);
                    return (op == "remove" ? "remove " : "kill ") + name +
                           " on " + machine + ": rc=" + std::to_string(rc) +
                           (msg.empty() ? "" : "  " + msg);
                }
            }
            return "no client for machine " + machine;
        });

    // Persistency panel — route ListSchemas / Snapshot to the FOCUSED machine's
    // GrpcClient (falling back to the first one). per is one-per-cluster, so a
    // single machine's com proxies it; the focused machine is the right target.
    persistency_->set_callbacks(
        [this](const std::string& config_type, bool* ok)
            -> std::vector<PersistencyPanel::Schema> {
            GrpcClient* c = client_for_focus();
            if (!c) { if (ok) *ok = false; return {}; }
            std::vector<PersistencyPanel::Schema> out;
            bool inner_ok = false;
            for (auto& s : c->list_schemas(config_type, &inner_ok))
                out.push_back({s.config_type, s.digest});
            if (ok) *ok = inner_ok;
            return out;
        },
        [this](const std::string& label, std::string* msg) -> int {
            GrpcClient* c = client_for_focus();
            if (!c) { if (msg) *msg = "no machine connected"; return -1; }
            return c->snapshot(label, msg);
        });

    // ONE GrpcClient — to the connected aggregator. The demux in on_sup_frame
    // splits its stream per machine; the Machines section lists those.
    {
        clients_.emplace_back(new GrpcClient(
            hub.name, current_hub_,
            [this](const std::string& mn, uint16_t tag, std::string payload) {
                post_frame_from_thread(mn, tag, std::move(payload));
            }));
        clients_.back()->start();
    }
}

// Switch the active connection to a new host:port. Stop the current client,
// reset the demux + the Machines section (the new connection may aggregate a
// different machine set), start a fresh client. local_name_ becomes the new
// host (its unprefixed/instance-0 nodes render under it); selected_machine_ is
// cleared so the first machine of the new cluster auto-selects.
void MainFrame::switch_hub(const std::string& host_port) {
    for (auto& c : clients_) if (c) c->stop();
    clients_.clear();

    current_hub_ = host_port;
    const auto colon = host_port.rfind(':');
    local_name_ = (colon == std::string::npos) ? host_port
                                               : host_port.substr(0, colon);
    seen_machines_.clear();
    selected_machine_.clear();
    if (machines_panel_) {
        machines_panel_->set_machines({});                 // empty until rediscovered
        machines_panel_->add_connection(local_name_, host_port,
                                        MachineConnState::Connecting);
        machines_panel_->set_connection_state(host_port,
                                              MachineConnState::Connecting);
    }
    save_connections();

    clients_.emplace_back(new GrpcClient(
        local_name_, host_port,
        [this](const std::string& mn, uint16_t tag, std::string payload) {
            post_frame_from_thread(mn, tag, std::move(payload));
        }));
    clients_.back()->start();
    SetStatusText(wxString::Format("connecting to %s...", host_port.c_str()), 0);
    wxLogStatus(this, "Hub -> %s", host_port.c_str());
}

GrpcClient* MainFrame::client_for_focus() {
    const std::string focus = machines_panel_ ? machines_panel_->focused()
                                              : std::string();
    if (!focus.empty()) {
        for (auto& c : clients_)
            if (c && c->machine_name() == focus) return c.get();
    }
    return clients_.empty() ? nullptr : clients_.front().get();
}

MainFrame::~MainFrame() {
    for (auto& c : clients_) {
        if (c) c->stop();
    }
}

void MainFrame::post_frame_from_thread(const std::string& machine_name,
                                       uint16_t tag,
                                       std::string payload) {
    auto* evt = new wxThreadEvent(EVT_SUP_FRAME);
    evt->SetInt(static_cast<int>(tag));
    FrameMsg msg;
    msg.machine_name = machine_name;
    msg.payload      = std::move(payload);
    evt->SetPayload(msg);
    wxQueueEvent(this, evt);
}

// Route one (machine_name, tag, payload) to the panels. The DATA panels
// (System / Load / Applications / Processes) scope to ONE machine — the one
// selected in the left dock — so they only receive frames for selected_machine_;
// N machines per connection can't all share the stacked UX. Trace is
// cluster-wide (cross-machine by design), so it always gets every frame.
void MainFrame::dispatch_to_panels(const std::string& machine_name,
                                   uint16_t tag,
                                   const std::string& payload) {
    if (machine_name == selected_machine_) {
        system_panel_ ->on_frame(machine_name, tag, payload);
        load_charts_  ->on_frame(machine_name, tag, payload);
        applications_ ->on_frame(machine_name, tag, payload);
        processes_    ->on_frame(machine_name, tag, payload);
    }
    trace_->on_frame(machine_name, tag, payload);   // cluster-wide
}

// Note a machine we've now seen in the aggregated stream and make sure the left
// panel has a row for it (so a peer that only appears via the merge still shows
// up without a dedicated gRPC connection).
void MainFrame::note_machine(const std::string& machine_name) {
    if (machine_name.empty()) return;
    if (!seen_machines_.insert(machine_name).second) return;   // already known
    if (machines_panel_) {
        machines_panel_->add_machine(machine_name, "(via " + local_name_ + ")",
                                     MachineConnState::Connected);
    }
    // Auto-select the FIRST machine discovered so the data panels aren't blank
    // right after connecting (the local/instance-0 machine tends to arrive
    // first). Later machines just appear in the list; the user clicks to switch.
    if (selected_machine_.empty()) {
        selected_machine_ = machine_name;
        if (machines_panel_) machines_panel_->select_machine(machine_name);
    }
}

void MainFrame::on_sup_frame(wxThreadEvent& evt) {
    const uint16_t tag = static_cast<uint16_t>(evt.GetInt());
    const FrameMsg msg = evt.GetPayload<FrameMsg>();

    if (tag == 0x0002) {
        last_heartbeat_.store(
            std::chrono::steady_clock::now().time_since_epoch().count());
    }

    // Stage 4 — demux the SINGLE central com's AGGREGATED stream into per-machine
    // dispatches so the panels (keyed by machine_name) render one machine each.
    if (tag == 0x0003) {
        // TreeSnapshot: split by the mN/ prefix → a clean per-machine snapshot.
        system_supervisor::TreeSnapshot agg;
        if (!agg.ParseFromString(msg.payload)) return;
        auto by_machine = split_tree_by_machine(agg, local_name_);
        for (auto& kv : by_machine) {
            note_machine(kv.first);
            dispatch_to_panels(kv.first, tag, kv.second.SerializeAsString());
        }
        return;
    }
    if (tag == 0x0006) {
        // AccelSample: the SHWA telemetry self-describes its machine. Route to
        // that machine's Load/System boxes (shwa is compute-only today, so this
        // is one machine, but the demux is general).
        services::com::AccelSample a;
        if (a.ParseFromString(msg.payload)) {
            const std::string m =
                a.machine_name().empty() ? local_name_ : a.machine_name();
            note_machine(m);
            dispatch_to_panels(m, tag, msg.payload);
        }
        return;
    }

    // Health (0x0002) + SystemInfo (0x0004) are RIG-COMMON (one supervisor
    // identity / beacon — central's): dispatch under the local machine name.
    dispatch_to_panels(local_name_, tag, msg.payload);
}

void MainFrame::on_close(wxCloseEvent& /*evt*/) {
    for (auto& c : clients_) {
        if (c) c->stop();
    }
    if (status_timer_) status_timer_->Stop();
    Destroy();
}

void MainFrame::on_status_tick(wxTimerEvent&) {
    size_t connected = 0;
    for (const auto& c : clients_) {
        if (!c) continue;
        const bool ok = c->is_connected();
        if (ok) ++connected;
        // Push per-row state into the side panel so its dots stay
        // current. We don't have a "down" signal from GrpcClient
        // yet — "not connected" is collapsed onto Connecting here.
        // The single client IS the active CONNECTION; reflect its transport
        // state on the hub's connection row (the Machines rows get their state
        // from the aggregated stream via note_machine, not from here).
        if (machines_panel_) {
            machines_panel_->set_connection_state(
                current_hub_,
                ok ? MachineConnState::Connected
                   : MachineConnState::Connecting);
        }
    }

    if (clients_.empty()) {
        SetStatusText("no machines configured", 0);
        SetStatusText("", 1);
        SetStatusText("", 2);
        return;
    }
    wxString status;
    if (connected == clients_.size()) {
        status.Printf("connected (%zu/%zu)", connected, clients_.size());
    } else if (connected == 0) {
        status.Printf("disconnected (0/%zu)", clients_.size());
    } else {
        status.Printf("partial (%zu/%zu)", connected, clients_.size());
    }
    SetStatusText(status, 0);

    if (machines_panel_) {
        std::string f = machines_panel_->focused();
        SetStatusText(f.empty() ? wxString("no machine focused")
                                 : wxString::Format("focus: %s", f.c_str()),
                       1);
    }

    if (last_heartbeat_.load() == 0) {
        SetStatusText("no heartbeat yet", 2);
    } else {
        auto now = std::chrono::steady_clock::now();
        auto hb  = std::chrono::steady_clock::time_point(
                       std::chrono::steady_clock::duration(last_heartbeat_.load()));
        auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - hb).count();
        SetStatusText(
            wxString::Format("hb age %lld ms", static_cast<long long>(age_ms)), 2);
    }
}

void MainFrame::on_menu(wxCommandEvent& evt) {
    switch (evt.GetId()) {
        case wxID_EXIT:
            Close(true);
            return;

        case wxID_ABOUT: {
            wxAboutDialogInfo info;
            info.SetName("supervisor-gui");
            info.SetVersion("0.1");
            info.SetDescription(
                "Theia supervisor / observer GUI.\n\n"
                "Modelled on Erlang/OTP observer — System / Load Charts /\n"
                "Applications / Processes / Table Viewer (etcd) / Trace.\n"
                "Connects to one or more target machines via services/com\n"
                "(gRPC) and reads etcd directly for the Table Viewer.");
            info.SetCopyright("(C) 2026 — Theia / robofortis.com");
            wxAboutBox(info, this);
            return;
        }

        case ID_MENU_REFRESH_MACHINES:
            // No-op today — machines.yaml is loaded once at startup.
            // Plumbing to re-read + diff + reconcile GrpcClients is
            // out of scope for this commit; the menu hook is in
            // place so the future change is one function away.
            wxLogStatus(this, "Refresh machines: not implemented");
            return;

        case ID_MENU_CONNECT_HOST: {
            // Prompt for host:port, default to the current hub. Switch on OK.
            wxString def = wxString::FromUTF8(
                (clients_.empty() ? std::string("127.0.0.1:7700")
                                  : current_hub_).c_str());
            wxTextEntryDialog dlg(this,
                "Connect to a com aggregator (host:port).\n"
                "The hub aggregates its whole cluster; the GUI demuxes per machine.",
                "Connect...", def);
            if (dlg.ShowModal() != wxID_OK) return;
            std::string hp = dlg.GetValue().Trim().Trim(false).ToStdString();
            if (hp.empty()) return;
            if (hp.find(':') == std::string::npos) hp += ":7700";   // default port
            switch_hub(hp);
            return;
        }

        case ID_MENU_CONNECT_ALL:
            for (auto& c : clients_) if (c) c->start();
            wxLogStatus(this, "Reconnect: %zu client(s)", clients_.size());
            return;

        case ID_MENU_DISCONNECT_ALL:
            for (auto& c : clients_) if (c) c->stop();
            wxLogStatus(this, "Disconnect: %zu client(s)", clients_.size());
            return;

        default:
            evt.Skip();
            return;
    }
}

void MainFrame::on_machine_focus(wxCommandEvent& evt) {
    // EVT_MACHINE_FOCUS:
    //  - GetInt() == 0          → machine row selected; scope the data panels
    //  - GetInt() == ID_CTX_*   → a machine context-menu action (Show in Trace)
    const std::string name = evt.GetString().ToStdString();
    switch (evt.GetInt()) {
        case 0:
            // Scope every DATA panel to this one machine. The panels key their
            // internal state by machine_name; once selected_machine_ changes,
            // dispatch_to_panels feeds only this machine's frames (the previous
            // machine's box/rows stop updating). Live data refills within ~1s.
            if (!name.empty() && name != selected_machine_) {
                selected_machine_ = name;
                // Drop the previously-shown machine from the accumulating data
                // panels so they render ONLY this machine.
                system_panel_->set_machine_filter(name);
                processes_->set_machine_filter(name);
                load_charts_->set_machine_filter(name);
                applications_->set_machine_filter(name);
                SetStatusText(wxString::Format("%s / %s",
                    local_name_.c_str(), name.c_str()), 1);
                wxLogStatus(this, "Machine -> %s", name.c_str());
            }
            return;

        case ID_CTX_SHOW_TRACE:
            // Trace is cluster-wide (cross-machine), so this just jumps to the
            // Trace tab — the records there already carry the node/machine.
            if (notebook_) {
                for (size_t i = 0; i < notebook_->GetPageCount(); ++i) {
                    if (notebook_->GetPageText(i) == "Trace") {
                        notebook_->SetSelection(i);
                        break;
                    }
                }
            }
            wxLogStatus(this, "Show in Trace: %s", name.c_str());
            return;

        default:
            return;
    }
}

// EVT_CONNECTION_SELECT — the transport notion. A plain select (Int==0) or
// Connect switches the hub to that endpoint; Disconnect stops the client.
void MainFrame::on_connection_select(wxCommandEvent& evt) {
    const std::string hp = evt.GetString().ToStdString();
    if (hp.empty()) return;
    switch (evt.GetInt()) {
        case 0:                              // plain select → make it the hub
        case ID_MENU_CONNECT_HOST:           // (unused here; kept for symmetry)
            if (hp != current_hub_ || clients_.empty()) switch_hub(hp);
            return;
        default:
            break;
    }
    // The connection context-menu Connect/Disconnect (ids from machines_panel).
    const int id = evt.GetInt();
    if (id == (wxID_HIGHEST + 5001)) {       // ID_MENU_CONNECT
        if (hp != current_hub_ || clients_.empty()) switch_hub(hp);
    } else if (id == (wxID_HIGHEST + 5002)) {  // ID_MENU_DISCONNECT
        for (auto& c : clients_) if (c) c->stop();
        if (machines_panel_)
            machines_panel_->set_connection_state(hp,
                MachineConnState::Disconnected);
        wxLogStatus(this, "Disconnect %s", hp.c_str());
    }
}

// ---- connection persistence ---------------------------------------------

std::string MainFrame::connections_path() const {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base = (xdg && *xdg) ? std::string(xdg)
        : (std::string(std::getenv("HOME") ? std::getenv("HOME") : ".")
           + "/.config");
    return base + "/theia-gui/connections.json";
}

// Seed the Connections list: persisted file first, then any machines.json
// endpoints not already saved. Minimal hand-rolled JSON (one {"name","host_port"}
// per entry) so the GUI needs no JSON dep here.
void MainFrame::load_connections() {
    std::vector<MachineRow> conns;
    auto add = [&](const std::string& name, const std::string& hp) {
        for (auto& c : conns) if (c.address == hp) return;
        conns.push_back({name, hp, MachineConnState::Disconnected});
    };
    std::ifstream f(connections_path());
    if (f) {
        std::string s((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
        // crude scan for "host_port":"..." (and the preceding "name":"...").
        size_t pos = 0;
        while ((pos = s.find("\"host_port\"", pos)) != std::string::npos) {
            auto q1 = s.find('"', s.find(':', pos) + 1);
            auto q2 = (q1 == std::string::npos) ? q1 : s.find('"', q1 + 1);
            if (q1 == std::string::npos || q2 == std::string::npos) break;
            std::string hp = s.substr(q1 + 1, q2 - q1 - 1);
            // name = the host part for display.
            auto colon = hp.rfind(':');
            std::string nm = colon == std::string::npos ? hp : hp.substr(0, colon);
            add(nm, hp);
            pos = q2 + 1;
        }
    }
    if (machines_panel_) machines_panel_->set_connections(std::move(conns));
}

void MainFrame::save_connections() const {
    if (!machines_panel_) return;
    const std::string path = connections_path();
    // mkdir -p the parent (theia-gui/).
    auto slash = path.rfind('/');
    if (slash != std::string::npos) {
        const std::string dir = path.substr(0, slash);
        std::string acc;
        for (char ch : dir) {
            acc += ch;
            if (ch == '/' && acc.size() > 1) (void)::mkdir(acc.c_str(), 0755);
        }
        (void)::mkdir(dir.c_str(), 0755);
    }
    std::ofstream f(path);
    if (!f) return;
    f << "{\n  \"connections\": [\n";
    const auto conns = machines_panel_->connections();
    for (size_t i = 0; i < conns.size(); ++i) {
        f << "    {\"name\": \"" << conns[i].name
          << "\", \"host_port\": \"" << conns[i].address << "\"}"
          << (i + 1 < conns.size() ? "," : "") << "\n";
    }
    f << "  ]\n}\n";
}

}  // namespace sup_gui
