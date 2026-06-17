// supervisor-gui main window. Visually models OTP's observer_wx.erl:
// top-level wxFrame with a wxNotebook of panels and a status bar.

#pragma once

#include "sup_gui/machines.h"
#include "sup_gui/grpc_client.h"

#include <wx/event.h>
#include <wx/frame.h>
#include <wx/notebook.h>
#include <wx/statusbr.h>
#include <wx/timer.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace sup_gui {

class SystemPanel;
class LoadChartsPanel;
class ApplicationsPanel;
class ProcessesPanel;
class EtcdPanel;
class PersistencyPanel;
class TracePanel;
class MachinesPanel;

// Custom wx event posted from any GrpcClient thread when a frame
// arrives. MainFrame handles it and routes to every panel.
wxDECLARE_EVENT(EVT_SUP_FRAME, wxThreadEvent);

class MainFrame : public wxFrame {
public:
    explicit MainFrame(std::vector<MachineEndpoint> machines);
    ~MainFrame() override;

private:
    void on_sup_frame(wxThreadEvent& evt);
    void on_close(wxCloseEvent& evt);
    void on_status_tick(wxTimerEvent& evt);
    void on_menu(wxCommandEvent& evt);
    void on_machine_focus(wxCommandEvent& evt);

    // The GrpcClient for the focused machine (falls back to the first one).
    // nullptr only when no machines are configured.
    GrpcClient* client_for_focus();

    void post_frame_from_thread(const std::string& machine_name,
                                 uint16_t tag,
                                 std::string payload);

    // Stage 4 demux helpers (see main_frame.cpp): fan one frame out to every
    // panel, and register a newly-seen machine in the left panel.
    void dispatch_to_panels(const std::string& machine_name,
                            uint16_t tag, const std::string& payload);
    void note_machine(const std::string& machine_name);

    // Connect… — switch the single aggregator hub to a new host:port: stop the
    // current GrpcClient, start one against the new endpoint, reset the demux's
    // seen-machine set so the left panel re-populates from the new cluster.
    void switch_hub(const std::string& host_port);

    wxNotebook*              notebook_{nullptr};
    MachinesPanel*           machines_panel_{nullptr};
    SystemPanel*             system_panel_{nullptr};
    LoadChartsPanel*         load_charts_{nullptr};
    ApplicationsPanel*       applications_{nullptr};
    ProcessesPanel*          processes_{nullptr};
    EtcdPanel*               etcd_panel_{nullptr};
    PersistencyPanel*        persistency_{nullptr};
    TracePanel*              trace_{nullptr};

    std::vector<std::unique_ptr<GrpcClient>> clients_;
    std::atomic<std::chrono::steady_clock::rep> last_heartbeat_{0};

    // Stage 4 — the GUI connects to ONE com (central, the cluster aggregator).
    // local_name_ is that machine's name (the instance-0 / unprefixed nodes in
    // the aggregated TreeSnapshot). seen_machines_ accumulates every machine the
    // aggregated stream reveals (local + each mN/ peer) so the left panel
    // auto-populates without a per-machine connection.
    std::string                  local_name_;
    std::string                  current_hub_;     // host:port of the active hub
    std::set<std::string>        seen_machines_;
    // The single machine the data panels (System/Load/Apps/Processes) currently
    // scope to. Empty until the first machine is auto-selected on connect.
    std::string                  selected_machine_;

    void on_connection_select(wxCommandEvent& evt);  // EVT_CONNECTION_SELECT
    void load_connections();                          // seed + persisted file
    void save_connections() const;
    std::string connections_path() const;             // ~/.config/theia-gui/…

    wxTimer*                 status_timer_{nullptr};

    wxDECLARE_EVENT_TABLE();
};

}  // namespace sup_gui
