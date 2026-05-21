// Forward declarations + base for the five OTP-observer-flavoured panels.
//
// Each panel inherits the same wxPanel base and exposes a small handler
// interface that MainFrame dispatches to when a TIPC frame arrives.
// Bodies of the panels are stubs today — the .art schema and wire
// transport are wired end-to-end so we can iterate the UI without
// further plumbing changes.
//
// Heritage of each panel maps to OTP source:
//   TreePanel       — observer_app_wx.erl (the supervisor tree pane)
//   ProcessPanel    — observer_pro_wx.erl (flat process list)
//   TracePanel      — observer_trace_wx.erl + et_wx_viewer.erl (event log)
//   TombstonePanel  — cdv_*.erl crash dump viewer family
//   PerfPanel       — observer_perf_wx.erl + observer_sys_wx.erl

#pragma once

#include <wx/panel.h>

#include <cstdint>
#include <string>

// Forward-decl in the global namespace so `wxListCtrl*` below resolves
// correctly without #include <wx/listctrl.h> in this header.
class wxListCtrl;

namespace sup_gui {

class PanelBase : public wxPanel {
public:
    explicit PanelBase(wxWindow* parent) : wxPanel(parent) {}
    // Called by MainFrame on every incoming TIPC frame. Panels filter
    // by tag and decode the protobuf themselves so the dispatch is
    // cheap when the panel doesn't care about the kind.
    virtual void on_frame(uint16_t tag, const std::string& payload) = 0;
};

class TreePanel : public PanelBase {
public:
    explicit TreePanel(wxWindow* parent);
    void on_frame(uint16_t tag, const std::string& payload) override;
};

class ProcessPanel : public PanelBase {
public:
    explicit ProcessPanel(wxWindow* parent);
    void on_frame(uint16_t tag, const std::string& payload) override;
};

class TracePanel : public PanelBase {
public:
    explicit TracePanel(wxWindow* parent);
    void on_frame(uint16_t tag, const std::string& payload) override;

private:
    ::wxListCtrl* list_{nullptr};
};

class TombstonePanel : public PanelBase {
public:
    explicit TombstonePanel(wxWindow* parent);
    void on_frame(uint16_t tag, const std::string& payload) override;
};

class PerfPanel : public PanelBase {
public:
    explicit PerfPanel(wxWindow* parent);
    void on_frame(uint16_t tag, const std::string& payload) override;
};

}  // namespace sup_gui
