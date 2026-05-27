// Machines side panel — observer_wx's "Nodes" menu, GUI-fied as
// a permanent left-side dock.
//
// One row per machine from the loaded machines.yaml. Each row shows
// a state glyph + the machine name + address. Selecting a row tells
// the rest of the GUI which machine to focus (panels can scope their
// view to that machine if they like; the Table Viewer ignores this
// and shows everything in etcd).
//
// State transitions come from MainFrame's status timer: it queries
// each GrpcClient's is_connected() and last-frame timestamp and
// translates to one of {Disconnected, Connecting, Connected, Down}.

#include "sup_gui/panels.h"

#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/menu.h>

#include <algorithm>

namespace sup_gui {

wxDEFINE_EVENT(EVT_MACHINE_FOCUS, wxCommandEvent);

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
        case MachineConnState::Connected:    return "●";   // U+25CF
        case MachineConnState::Connecting:   return "◐";   // U+25D0
        case MachineConnState::Down:         return "✖";   // U+2716
        case MachineConnState::Disconnected: return "○";   // U+25CB
    }
    return "?";
}

enum : int {
    ID_MENU_CONNECT     = wxID_HIGHEST + 5001,
    ID_MENU_DISCONNECT  = wxID_HIGHEST + 5002,
    ID_MENU_SHOW_TRACE  = wxID_HIGHEST + 5003,
};

}  // namespace


MachinesPanel::MachinesPanel(wxWindow* parent) : wxPanel(parent) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    auto* hdr = new wxStaticText(this, wxID_ANY, "Machines");
    wxFont hdrFont = hdr->GetFont();
    hdrFont.MakeBold();
    hdr->SetFont(hdrFont);
    sizer->Add(hdr, 0, wxLEFT | wxRIGHT | wxTOP, 6);

    list_ = new wxListCtrl(this, wxID_ANY,
                            wxDefaultPosition, wxDefaultSize,
                            wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES);
    list_->AppendColumn("",      wxLIST_FORMAT_LEFT,  28);
    list_->AppendColumn("Name",  wxLIST_FORMAT_LEFT, 110);
    list_->AppendColumn("Where", wxLIST_FORMAT_LEFT, 140);
    sizer->Add(list_, 1, wxEXPAND | wxALL, 4);

    SetSizer(sizer);

    list_->Bind(wxEVT_LIST_ITEM_SELECTED,
                 &MachinesPanel::on_select, this);
    list_->Bind(wxEVT_CONTEXT_MENU, [this](wxContextMenuEvent& evt) {
        wxMouseEvent fake(wxEVT_RIGHT_DOWN);
        fake.SetPosition(list_->ScreenToClient(evt.GetPosition()));
        on_right_click(fake);
    });
}

MachinesPanel::~MachinesPanel() = default;


void MachinesPanel::set_machines(std::vector<MachineRow> rows) {
    rows_ = std::move(rows);
    redraw();
}


void MachinesPanel::set_state(const std::string& machine_name,
                                MachineConnState s) {
    bool changed = false;
    for (auto& r : rows_) {
        if (r.name == machine_name && r.state != s) {
            r.state = s;
            changed = true;
            break;
        }
    }
    if (changed) redraw();
}


std::string MachinesPanel::focused() const {
    long sel = list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0 || static_cast<size_t>(sel) >= rows_.size()) return {};
    return rows_[sel].name;
}


void MachinesPanel::redraw() {
    // Preserve selection across the rebuild — picks the same machine
    // name if it's still in the new row set.
    std::string sel_name = focused();

    list_->Freeze();
    list_->DeleteAllItems();
    for (size_t i = 0; i < rows_.size(); ++i) {
        const auto& r = rows_[i];
        long row = list_->InsertItem(static_cast<long>(i),
                                      wxString::FromUTF8(state_glyph(r.state)));
        if (row < 0) continue;
        list_->SetItemTextColour(row, state_color(r.state));
        list_->SetItem(row, 1, wxString::FromUTF8(r.name.c_str(), r.name.size()));
        list_->SetItem(row, 2, wxString::FromUTF8(r.address.c_str(),
                                                    r.address.size()));
        if (!sel_name.empty() && r.name == sel_name) {
            list_->SetItemState(row, wxLIST_STATE_SELECTED,
                                 wxLIST_STATE_SELECTED);
            list_->SetItemState(row, wxLIST_STATE_FOCUSED,
                                 wxLIST_STATE_FOCUSED);
        }
    }
    list_->Thaw();
}


void MachinesPanel::on_select(wxListEvent& evt) {
    long idx = evt.GetIndex();
    std::string name;
    if (idx >= 0 && static_cast<size_t>(idx) < rows_.size()) {
        name = rows_[idx].name;
    }
    wxCommandEvent broadcast(EVT_MACHINE_FOCUS);
    broadcast.SetString(wxString::FromUTF8(name.c_str(), name.size()));
    broadcast.SetEventObject(this);
    // Bubble to the parent so MainFrame can react (e.g. status bar).
    // Other panels Bind() to EVT_MACHINE_FOCUS on the panel directly
    // if they want.
    wxPostEvent(GetParent(), broadcast);
}


void MachinesPanel::on_right_click(wxMouseEvent& /*evt*/) {
    long sel = list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0) return;

    wxMenu m;
    m.Append(ID_MENU_CONNECT,    "Connect");
    m.Append(ID_MENU_DISCONNECT, "Disconnect");
    m.AppendSeparator();
    m.Append(ID_MENU_SHOW_TRACE, "Show in Trace");

    // Right-click handlers route through events posted to the parent
    // (MainFrame) so connection management stays centralized. We
    // include the row name in the event's String for the handler.
    const std::string& name = rows_[static_cast<size_t>(sel)].name;
    Bind(wxEVT_MENU, [this, name](wxCommandEvent& e) {
        wxCommandEvent ev(EVT_MACHINE_FOCUS);
        ev.SetInt(e.GetId());   // re-purpose Int as the menu id; the
                                 // parent dispatches by id.
        ev.SetString(wxString::FromUTF8(name.c_str(), name.size()));
        ev.SetEventObject(this);
        wxPostEvent(GetParent(), ev);
    }, ID_MENU_CONNECT, ID_MENU_SHOW_TRACE);

    PopupMenu(&m);
}

}  // namespace sup_gui
