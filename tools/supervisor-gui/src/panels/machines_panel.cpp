// Left-side dock — TWO sections: Connections (saved com endpoints, one
// connected) on top, Machines (discovered through the connected endpoint) below.
//
// Connections are the transport notion: a saved host:port (127.0.0.1, jetson,
// rpi4). Only ONE is connected at a time; right-click → Connect/Disconnect
// switches the hub. Machines are what the connected endpoint's com aggregates
// (the local one + each mN/ peer); selecting one scopes every data panel to that
// single machine (N machines per connection can't share the stacked UX).
//
// Connection selection/actions post EVT_CONNECTION_SELECT; machine selection
// posts EVT_MACHINE_FOCUS. MainFrame owns the connect/disconnect + scope logic.

#include "sup_gui/panels.h"

#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/menu.h>
#include <wx/statline.h>

#include <algorithm>

namespace sup_gui {

wxDEFINE_EVENT(EVT_MACHINE_FOCUS,     wxCommandEvent);
wxDEFINE_EVENT(EVT_CONNECTION_SELECT, wxCommandEvent);

namespace {

// Wireshark-style status colors. wxColour ctor is RGB.
const wxColour kColorConnected   (0x2E, 0x8B, 0x57);  // sea green
const wxColour kColorConnecting  (0xDA, 0xA5, 0x20);  // goldenrod
const wxColour kColorDown        (0xC9, 0x3C, 0x37);  // brick red
const wxColour kColorDisconnected(0x80, 0x80, 0x80);  // grey

const wxColour& state_color(MachineConnState s) {
    switch (s) {
        case MachineConnState::Connected:    return kColorConnected;
        case MachineConnState::Connecting:   return kColorConnecting;
        case MachineConnState::Down:         return kColorDown;
        case MachineConnState::Disconnected: return kColorDisconnected;
    }
    return kColorDisconnected;
}

const char* state_glyph(MachineConnState s) {
    switch (s) {
        case MachineConnState::Connected:    return "●";   // ● filled circle
        case MachineConnState::Connecting:   return "◐";   // ◐ half circle
        case MachineConnState::Down:         return "✖";   // ✖ heavy x
        case MachineConnState::Disconnected: return "○";   // ○ open circle
    }
    return "?";
}

enum : int {
    // Connection context menu (handled by MainFrame via EVT_CONNECTION_SELECT).
    ID_MENU_CONNECT     = wxID_HIGHEST + 5001,
    ID_MENU_DISCONNECT  = wxID_HIGHEST + 5002,
    // Machine context menu (handled by MainFrame via EVT_MACHINE_FOCUS).
    ID_MENU_SHOW_TRACE  = wxID_HIGHEST + 5003,
};

// Build a glyph/Name/Where report list. Shared shape for both sections.
wxListCtrl* make_list(wxWindow* parent, const char* col1) {
    auto* l = new wxListCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                             wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES);
    l->AppendColumn("",     wxLIST_FORMAT_LEFT,  28);
    l->AppendColumn("Name", wxLIST_FORMAT_LEFT, 110);
    l->AppendColumn(col1,   wxLIST_FORMAT_LEFT, 140);
    return l;
}

void fill_list(wxListCtrl* l, const std::vector<MachineRow>& rows,
               const std::string& keep_sel) {
    l->Freeze();
    l->DeleteAllItems();
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        long row = l->InsertItem((long)i, wxString::FromUTF8(state_glyph(r.state)));
        if (row < 0) continue;
        l->SetItemTextColour(row, state_color(r.state));
        l->SetItem(row, 1, wxString::FromUTF8(r.name.c_str(), r.name.size()));
        l->SetItem(row, 2, wxString::FromUTF8(r.address.c_str(), r.address.size()));
        if (!keep_sel.empty() && r.name == keep_sel) {
            l->SetItemState(row, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
            l->SetItemState(row, wxLIST_STATE_FOCUSED,  wxLIST_STATE_FOCUSED);
        }
    }
    l->Thaw();
}

std::string sel_name(wxListCtrl* l, const std::vector<MachineRow>& rows) {
    long s = l->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (s < 0 || (size_t)s >= rows.size()) return {};
    return rows[(size_t)s].name;
}

}  // namespace


MachinesPanel::MachinesPanel(wxWindow* parent) : wxPanel(parent) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    auto bold = [&](const char* t) {
        auto* h = new wxStaticText(this, wxID_ANY, t);
        wxFont f = h->GetFont(); f.MakeBold(); h->SetFont(f);
        return h;
    };

    // ---- Connections (top) -----------------------------------------------
    sizer->Add(bold("Connections"), 0, wxLEFT | wxRIGHT | wxTOP, 6);
    conn_list_ = make_list(this, "Endpoint");
    sizer->Add(conn_list_, 1, wxEXPAND | wxALL, 4);

    sizer->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 4);

    // ---- Machines (bottom) -----------------------------------------------
    sizer->Add(bold("Machines"), 0, wxLEFT | wxRIGHT | wxTOP, 6);
    mach_list_ = make_list(this, "Where");
    sizer->Add(mach_list_, 1, wxEXPAND | wxALL, 4);

    SetSizer(sizer);

    conn_list_->Bind(wxEVT_LIST_ITEM_SELECTED, &MachinesPanel::on_conn_select, this);
    mach_list_->Bind(wxEVT_LIST_ITEM_SELECTED, &MachinesPanel::on_machine_select, this);
    conn_list_->Bind(wxEVT_CONTEXT_MENU, [this](wxContextMenuEvent& e) {
        wxMouseEvent fake(wxEVT_RIGHT_DOWN);
        fake.SetPosition(conn_list_->ScreenToClient(e.GetPosition()));
        on_conn_right_click(fake);
    });
    mach_list_->Bind(wxEVT_CONTEXT_MENU, [this](wxContextMenuEvent& e) {
        wxMouseEvent fake(wxEVT_RIGHT_DOWN);
        fake.SetPosition(mach_list_->ScreenToClient(e.GetPosition()));
        on_machine_right_click(fake);
    });
}

MachinesPanel::~MachinesPanel() = default;


// ---- Connections ---------------------------------------------------------

void MachinesPanel::set_connections(std::vector<MachineRow> rows) {
    conns_ = std::move(rows);
    redraw_conns();
}

void MachinesPanel::add_connection(const std::string& name,
                                   const std::string& host_port,
                                   MachineConnState s) {
    for (auto& r : conns_)
        if (r.name == name || r.address == host_port) return;  // idempotent
    conns_.push_back({name, host_port, s});
    redraw_conns();
}

void MachinesPanel::set_connection_state(const std::string& host_port,
                                         MachineConnState s) {
    bool changed = false;
    for (auto& r : conns_) {
        // The one matching host:port gets `s`; if it became Connected, every
        // OTHER connection drops to Disconnected (single active connection).
        if (r.address == host_port) {
            if (r.state != s) { r.state = s; changed = true; }
        } else if (s == MachineConnState::Connected &&
                   r.state != MachineConnState::Disconnected) {
            r.state = MachineConnState::Disconnected; changed = true;
        }
    }
    if (changed) redraw_conns();
}

std::string MachinesPanel::selected_connection() const {
    long s = conn_list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (s >= 0 && (size_t)s < conns_.size()) return conns_[(size_t)s].address;
    return conns_.empty() ? std::string() : conns_.front().address;
}

void MachinesPanel::redraw_conns() {
    fill_list(conn_list_, conns_, sel_name(conn_list_, conns_));
}

void MachinesPanel::on_conn_select(wxListEvent& evt) {
    long idx = evt.GetIndex();
    if (idx < 0 || (size_t)idx >= conns_.size()) return;
    wxCommandEvent ev(EVT_CONNECTION_SELECT);
    ev.SetInt(0);   // 0 = plain select
    ev.SetString(wxString::FromUTF8(conns_[(size_t)idx].address.c_str()));
    ev.SetEventObject(this);
    wxPostEvent(GetParent(), ev);
}

void MachinesPanel::on_conn_right_click(wxMouseEvent&) {
    long sel = conn_list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0 || (size_t)sel >= conns_.size()) return;
    const std::string hp = conns_[(size_t)sel].address;

    wxMenu m;
    m.Append(ID_MENU_CONNECT,    "Connect");
    m.Append(ID_MENU_DISCONNECT, "Disconnect");
    Bind(wxEVT_MENU, [this, hp](wxCommandEvent& e) {
        wxCommandEvent ev(EVT_CONNECTION_SELECT);
        ev.SetInt(e.GetId());
        ev.SetString(wxString::FromUTF8(hp.c_str()));
        ev.SetEventObject(this);
        wxPostEvent(GetParent(), ev);
    }, ID_MENU_CONNECT, ID_MENU_DISCONNECT);
    PopupMenu(&m);
}


// ---- Machines ------------------------------------------------------------

void MachinesPanel::set_machines(std::vector<MachineRow> rows) {
    rows_ = std::move(rows);
    redraw_machines();
}

void MachinesPanel::add_machine(const std::string& name,
                                const std::string& address,
                                MachineConnState s) {
    for (const auto& r : rows_)
        if (r.name == name) return;          // idempotent
    rows_.push_back({name, address, s});
    redraw_machines();
}

void MachinesPanel::set_state(const std::string& machine_name,
                              MachineConnState s) {
    for (auto& r : rows_)
        if (r.name == machine_name && r.state != s) {
            r.state = s; redraw_machines(); return;
        }
}

std::string MachinesPanel::focused() const {
    long s = mach_list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (s >= 0 && (size_t)s < rows_.size()) return rows_[(size_t)s].name;
    return rows_.empty() ? std::string() : rows_.front().name;
}

void MachinesPanel::select_machine(const std::string& name) {
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].name != name) continue;
        mach_list_->SetItemState((long)i, wxLIST_STATE_SELECTED,
                                 wxLIST_STATE_SELECTED);
        mach_list_->SetItemState((long)i, wxLIST_STATE_FOCUSED,
                                 wxLIST_STATE_FOCUSED);
        return;
    }
}

void MachinesPanel::redraw_machines() {
    fill_list(mach_list_, rows_, sel_name(mach_list_, rows_));
}

void MachinesPanel::on_machine_select(wxListEvent& evt) {
    long idx = evt.GetIndex();
    std::string name;
    if (idx >= 0 && (size_t)idx < rows_.size()) name = rows_[(size_t)idx].name;
    wxCommandEvent ev(EVT_MACHINE_FOCUS);
    ev.SetInt(0);
    ev.SetString(wxString::FromUTF8(name.c_str(), name.size()));
    ev.SetEventObject(this);
    wxPostEvent(GetParent(), ev);
}

void MachinesPanel::on_machine_right_click(wxMouseEvent&) {
    long sel = mach_list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0 || (size_t)sel >= rows_.size()) return;
    const std::string name = rows_[(size_t)sel].name;

    wxMenu m;
    m.Append(ID_MENU_SHOW_TRACE, "Show in Trace");
    Bind(wxEVT_MENU, [this, name](wxCommandEvent& e) {
        wxCommandEvent ev(EVT_MACHINE_FOCUS);
        ev.SetInt(e.GetId());
        ev.SetString(wxString::FromUTF8(name.c_str()));
        ev.SetEventObject(this);
        wxPostEvent(GetParent(), ev);
    }, ID_MENU_SHOW_TRACE);
    PopupMenu(&m);
}

}  // namespace sup_gui
