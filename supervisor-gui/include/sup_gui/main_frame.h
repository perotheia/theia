// supervisor-gui main window. Modelled on OTP's observer_wx.erl:
// a top-level wxFrame with a wxNotebook of panels, plus a status bar
// that shows TIPC connection state and supervisor heartbeat.

#pragma once

#include "sup_gui/tipc_client.h"

#include <wx/event.h>
#include <wx/frame.h>
#include <wx/notebook.h>
#include <wx/statusbr.h>
#include <wx/timer.h>

#include <atomic>
#include <chrono>
#include <memory>

namespace sup_gui {

class TreePanel;
class ProcessPanel;
class TracePanel;
class TombstonePanel;
class PerfPanel;

// Custom wx event posted from the TIPC client thread when a frame
// arrives. The main frame handles it and routes to the right panel.
wxDECLARE_EVENT(EVT_SUP_FRAME, wxThreadEvent);

class MainFrame : public wxFrame {
public:
    MainFrame();
    ~MainFrame() override;

private:
    void on_sup_frame(wxThreadEvent& evt);
    void on_close(wxCloseEvent& evt);
    void on_status_tick(wxTimerEvent& evt);

    void post_frame_from_thread(uint16_t tag, std::string payload);

    wxNotebook*               notebook_{nullptr};
    TreePanel*                tree_{nullptr};
    ProcessPanel*             processes_{nullptr};
    TracePanel*               trace_{nullptr};
    TombstonePanel*           tombstones_{nullptr};
    PerfPanel*                perf_{nullptr};

    std::unique_ptr<TipcClient> tipc_;
    std::atomic<std::chrono::steady_clock::rep> last_heartbeat_{0};

    wxTimer*                  status_timer_{nullptr};

    wxDECLARE_EVENT_TABLE();
};

}  // namespace sup_gui
