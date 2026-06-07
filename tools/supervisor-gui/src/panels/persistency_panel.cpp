// Persistency panel — services/per's TYPED view, via com's PerView gRPC.
//
// Distinct from the raw etcd "Table Viewer": that reads the etcd KV mirror
// directly; this drives per's MANAGER ops (the sole etcd client) through com —
// the schema registry (ListSchemas) + a Snapshot trigger. The config hot path
// (Get/Put/Watch) stays node↔per (schema-aware decode the GUI can't do).
//
// Request/response, not frame-driven: a Refresh button calls ListSchemas, a
// Snapshot button calls Snapshot. main_frame wires both to the focused
// machine's GrpcClient.

#include "sup_gui/panels.h"

#include <wx/wx.h>
#include <wx/listctrl.h>

#include <string>
#include <vector>

namespace sup_gui {

struct PersistencyPanelImpl {
    wxListCtrl*  list{nullptr};
    wxStaticText* status{nullptr};
    wxTextCtrl*  label_input{nullptr};

    PersistencyPanel::ListSchemasCallback list_cb;
    PersistencyPanel::SnapshotCallback    snap_cb;

    void refresh() {
        list->DeleteAllItems();
        if (!list_cb) { status->SetLabel("(no machine connected)"); return; }
        bool ok = false;
        auto rows = list_cb("", &ok);
        if (!ok) {
            status->SetLabel("ListSchemas failed — is per reachable via com?");
            return;
        }
        long i = 0;
        for (const auto& r : rows) {
            list->InsertItem(i, wxString::FromUTF8(r.config_type.c_str()));
            list->SetItem(i, 1, wxString::FromUTF8(r.digest.c_str()));
            ++i;
        }
        status->SetLabel(wxString::Format("%ld schema(s) registered in per",
                                          static_cast<long>(rows.size())));
    }
};

PersistencyPanel::PersistencyPanel(wxWindow* parent)
    : wxPanel(parent), impl_(new PersistencyPanelImpl()) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    auto* hint = new wxStaticText(this, wxID_ANY,
        "Persistency (services/per via com's PerView) — the schema registry "
        "and config snapshots. The raw etcd KV is in the Table Viewer tab.");
    sizer->Add(hint, 0, wxALL, 8);

    // Controls row: Refresh + a snapshot label + Snapshot.
    auto* ctl = new wxBoxSizer(wxHORIZONTAL);
    auto* refresh_btn = new wxButton(this, wxID_ANY, "Refresh schemas");
    ctl->Add(refresh_btn, 0, wxRIGHT, 8);
    ctl->Add(new wxStaticText(this, wxID_ANY, "snapshot label:"),
             0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    impl_->label_input = new wxTextCtrl(this, wxID_ANY, "gui");
    ctl->Add(impl_->label_input, 0, wxRIGHT, 8);
    auto* snap_btn = new wxButton(this, wxID_ANY, "Snapshot");
    ctl->Add(snap_btn, 0);
    sizer->Add(ctl, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);

    impl_->status = new wxStaticText(this, wxID_ANY, "");
    sizer->Add(impl_->status, 0, wxLEFT | wxBOTTOM, 8);

    impl_->list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition,
                                 wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL);
    impl_->list->AppendColumn("Config type", wxLIST_FORMAT_LEFT, 360);
    impl_->list->AppendColumn("Digest",      wxLIST_FORMAT_LEFT, 320);
    sizer->Add(impl_->list, 1, wxEXPAND | wxALL, 8);

    SetSizer(sizer);

    refresh_btn->Bind(wxEVT_BUTTON,
        [this](wxCommandEvent&) { impl_->refresh(); });

    snap_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!impl_->snap_cb) {
            impl_->status->SetLabel("(no machine connected)");
            return;
        }
        const std::string label =
            impl_->label_input->GetValue().ToStdString();
        std::string msg;
        const int st = impl_->snap_cb(label, &msg);
        impl_->status->SetLabel(wxString::Format(
            "Snapshot '%s' -> status=%d  %s",
            label.c_str(), st, msg.c_str()));
    });
}

PersistencyPanel::~PersistencyPanel() = default;

void PersistencyPanel::set_callbacks(ListSchemasCallback ls,
                                     SnapshotCallback snap) {
    impl_->list_cb = std::move(ls);
    impl_->snap_cb = std::move(snap);
}

}  // namespace sup_gui
