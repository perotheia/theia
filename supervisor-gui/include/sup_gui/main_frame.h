// supervisor-gui main window. Visually models OTP's observer_wx.erl:
// top-level wxFrame with a wxNotebook of panels and a status bar.
//
// Transport state: phase 0 of the TCP→TIPC→gRPC migration. The TcpClient
// path has been removed; the GUI runs without an active connection
// until services/com (phase 2) lands and we wire its gRPC stub here.

#pragma once

#include <wx/event.h>
#include <wx/frame.h>
#include <wx/notebook.h>
#include <wx/statusbr.h>
#include <wx/timer.h>

#include <atomic>
#include <chrono>
#include <string>

namespace sup_gui {

class SystemPanel;
class LoadChartsPanel;
class ApplicationsPanel;
class ProcessesPanel;
class TracePanel;

// Custom wx event posted from the (future) gRPC client thread when a
// frame arrives. MainFrame handles it and routes to every panel.
wxDECLARE_EVENT(EVT_SUP_FRAME, wxThreadEvent);

class MainFrame : public wxFrame {
public:
    MainFrame();
    ~MainFrame() override;

private:
    void on_sup_frame(wxThreadEvent& evt);
    void on_close(wxCloseEvent& evt);
    void on_status_tick(wxTimerEvent& evt);

    wxNotebook*              notebook_{nullptr};
    SystemPanel*             system_panel_{nullptr};
    LoadChartsPanel*         load_charts_{nullptr};
    ApplicationsPanel*       applications_{nullptr};
    ProcessesPanel*          processes_{nullptr};
    TracePanel*              trace_{nullptr};

    std::atomic<std::chrono::steady_clock::rep> last_heartbeat_{0};

    wxTimer*                 status_timer_{nullptr};

    wxDECLARE_EVENT_TABLE();
};

}  // namespace sup_gui
