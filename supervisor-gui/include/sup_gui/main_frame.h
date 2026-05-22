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
#include <string>
#include <vector>

namespace sup_gui {

class SystemPanel;
class LoadChartsPanel;
class ApplicationsPanel;
class ProcessesPanel;
class TracePanel;

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

    void post_frame_from_thread(const std::string& machine_name,
                                 uint16_t tag,
                                 std::string payload);

    wxNotebook*              notebook_{nullptr};
    SystemPanel*             system_panel_{nullptr};
    LoadChartsPanel*         load_charts_{nullptr};
    ApplicationsPanel*       applications_{nullptr};
    ProcessesPanel*          processes_{nullptr};
    TracePanel*              trace_{nullptr};

    std::vector<std::unique_ptr<GrpcClient>> clients_;
    std::atomic<std::chrono::steady_clock::rep> last_heartbeat_{0};

    wxTimer*                 status_timer_{nullptr};

    wxDECLARE_EVENT_TABLE();
};

}  // namespace sup_gui
