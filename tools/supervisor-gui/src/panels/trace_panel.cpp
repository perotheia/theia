// Trace panel — Wireshark-style. Spec:
// docs/tasks/BACKLOG/GUI-trace-panel-wireshark-style.md
//
// DATA SOURCE: live message traces from com's TraceForwarder TraceStream
// (:7710, tag 0x0005) — the SAME records `tdb logcat` / `rtdb logcat` show
// (node→node casts/calls). (The supervisor-lifecycle SupervisionEvent feed is
// not emitted under the snapshot-only pull model; this panel now follows the
// message trace egress instead.)
//
// Layout (top-down):
//   filter [substring or key:value tokens] [Clear]
//   wxListCtrl (virtual)  — TIME | MACHINE | KIND | SRC | DST | MSG TYPE
//   wxTreeCtrl            — lazy-decoded selection: Header / Subject /
//                            Raw payload groups
//
// Top list is wxLC_VIRTUAL so we only format the rows wx asks
// about. Storage is a deque<EventRow> capped at 5000 (kRingCapacity).

#include "sup_gui/panels.h"

#include "supervisor_bridge.pb.h"   // services.com.TraceRecord

#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/treectrl.h>
#include <wx/splitter.h>
#include <wx/textctrl.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>

namespace sup_gui {

namespace {

constexpr uint16_t kTagTraceRecord = 0x0005;
constexpr size_t   kRingCapacity   = 5000;

// One trace record row. Maps services.com.TraceRecord
// (node_name/dst/msg_type/corr_id/ts_ns/kind/payload) onto the list model.
struct EventRow {
    int64_t      t_ms_local{};   // ts_ns/1e6 — local wall time for display
    std::string  machine;
    uint32_t     kind{};         // TraceKind
    std::string  src;            // node_name (emitter)
    std::string  dst;            // peer node, "" if none
    std::string  msg_type;       // proto message type name
    uint32_t     corr_id{};      // pairs Send/Recv/Dispatch
    std::string  raw_payload;    // proto-wire-v3 bytes
};

// TraceKind ordinal → name (platform_runtime.TraceKind, same as tdb).
const char* kind_name(uint32_t k) {
    switch (k) {
        case 0: return "OTHER";
        case 1: return "CAST_OUT";
        case 2: return "CAST_IN";
        case 3: return "CALL_OUT";
        case 4: return "CALL_IN";
        case 5: return "STATEM";
        default: return "?";
    }
}

wxColour kind_color(uint32_t k) {
    switch (k) {
        case 1: return wxColour(0xE0, 0xF5, 0xE0);   // CAST_OUT  green
        case 2: return wxColour(0xE0, 0xF0, 0xFF);   // CAST_IN   blue
        case 3: return wxColour(0xFF, 0xF6, 0xC0);   // CALL_OUT  yellow
        case 4: return wxColour(0xFF, 0xE8, 0xC8);   // CALL_IN   orange
        case 5: return wxColour(0xD8, 0xB6, 0xFF);   // STATEM    purple
        default: return *wxWHITE;
    }
}

wxString hex_dump(const std::string& bytes) {
    wxString out;
    char tmp[8];
    for (size_t off = 0; off < bytes.size(); off += 16) {
        std::snprintf(tmp, sizeof(tmp), "%04zx  ", off);
        out << tmp;
        for (size_t i = 0; i < 16; ++i) {
            if (off + i < bytes.size()) {
                std::snprintf(tmp, sizeof(tmp), "%02x ",
                              static_cast<unsigned char>(bytes[off + i]));
                out << tmp;
            } else out << "   ";
            if (i == 7) out << " ";
        }
        out << " ";
        for (size_t i = 0; i < 16 && off + i < bytes.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(bytes[off + i]);
            out << (c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '.');
        }
        out << "\n";
    }
    return out;
}


}  // namespace


// Virtual list bound to a deque<EventRow> via a separate visible[] index.
class TraceListCtrl : public wxListCtrl {
public:
    TraceListCtrl(wxWindow* parent,
                   const std::deque<EventRow>* rows,
                   const std::vector<size_t>* visible,
                   std::mutex* mtx)
        : wxListCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                      wxLC_REPORT | wxLC_VIRTUAL | wxLC_SINGLE_SEL),
          rows_(rows), visible_(visible), mtx_(mtx) {
        AppendColumn("Time",     wxLIST_FORMAT_LEFT, 140);
        AppendColumn("Machine",  wxLIST_FORMAT_LEFT,  90);
        AppendColumn("Kind",     wxLIST_FORMAT_LEFT, 100);
        AppendColumn("Src",      wxLIST_FORMAT_LEFT, 150);
        AppendColumn("Dst",      wxLIST_FORMAT_LEFT, 150);
        AppendColumn("Msg type", wxLIST_FORMAT_LEFT, 280);
    }

protected:
    wxString OnGetItemText(long item, long col) const override {
        std::lock_guard<std::mutex> lk(*mtx_);
        if (item < 0 || static_cast<size_t>(item) >= visible_->size())
            return wxString();
        const size_t idx = (*visible_)[item];
        if (idx >= rows_->size()) return wxString();
        const EventRow& r = (*rows_)[idx];

        switch (col) {
            case 0: {
                time_t t  = static_cast<time_t>(r.t_ms_local / 1000);
                int    fr = static_cast<int>(r.t_ms_local % 1000);
                struct tm lt;
                localtime_r(&t, &lt);
                return wxString::Format("%02d:%02d:%02d.%03d",
                                         lt.tm_hour, lt.tm_min, lt.tm_sec, fr);
            }
            case 1: return wxString::FromUTF8(r.machine.c_str(), r.machine.size());
            case 2: return wxString::FromAscii(kind_name(r.kind));
            case 3: return wxString::FromUTF8(r.src.c_str(), r.src.size());
            case 4: return wxString::FromUTF8(r.dst.c_str(), r.dst.size());
            case 5: return wxString::FromUTF8(r.msg_type.c_str(), r.msg_type.size());
        }
        return wxString();
    }

    wxListItemAttr* OnGetItemAttr(long item) const override {
        std::lock_guard<std::mutex> lk(*mtx_);
        if (item < 0 || static_cast<size_t>(item) >= visible_->size())
            return nullptr;
        const size_t idx = (*visible_)[item];
        if (idx >= rows_->size()) return nullptr;
        static thread_local wxListItemAttr attr;
        attr.SetBackgroundColour(kind_color((*rows_)[idx].kind));
        return &attr;
    }

private:
    const std::deque<EventRow>* rows_;
    const std::vector<size_t>*  visible_;
    std::mutex*                 mtx_;
};


// Panel-internal state. Kept out of panels.h to avoid a header
// churn ripple in this commit; the old `list_` member in
// TracePanel is repurposed to hold the new virtual list ptr.
struct TracePanelImpl {
    wxSplitterWindow*     split{nullptr};
    wxTextCtrl*           filter_input{nullptr};
    TraceListCtrl*        list{nullptr};
    wxTreeCtrl*           tree{nullptr};

    std::deque<EventRow>  ring;
    std::vector<size_t>   visible;
    std::string           current_filter;
    std::mutex            mtx;

    void apply_filter() {  // caller holds mtx
        visible.clear();
        if (current_filter.empty()) {
            for (size_t i = 0; i < ring.size(); ++i) visible.push_back(i);
            return;
        }
        // Tokenize on whitespace; each token is "[key:]substring" with
        // case-insensitive match.
        std::vector<std::pair<std::string,std::string>> terms;
        {
            std::string cur;
            for (char c : current_filter) {
                if (c == ' ' || c == '\t') {
                    if (!cur.empty()) {
                        terms.emplace_back(std::string(), cur);
                        cur.clear();
                    }
                } else cur.push_back(c);
            }
            if (!cur.empty()) terms.emplace_back(std::string(), cur);
        }
        for (auto& kv : terms) {
            size_t colon = kv.second.find(':');
            if (colon != std::string::npos) {
                kv.first  = kv.second.substr(0, colon);
                kv.second = kv.second.substr(colon + 1);
            }
            for (auto& c : kv.first)  c = static_cast<char>(std::tolower(c));
            for (auto& c : kv.second) c = static_cast<char>(std::tolower(c));
        }
        auto ci_find = [](const std::string& hay, const std::string& needle) {
            if (needle.empty()) return true;
            std::string h; h.reserve(hay.size());
            for (char c : hay) h.push_back(static_cast<char>(std::tolower(c)));
            return h.find(needle) != std::string::npos;
        };
        for (size_t i = 0; i < ring.size(); ++i) {
            const EventRow& r = ring[i];
            bool ok = true;
            for (const auto& t : terms) {
                bool m = false;
                // Bare token matches src/dst/msg_type (the common case).
                if (t.first.empty())
                    m = ci_find(r.src, t.second) || ci_find(r.dst, t.second) ||
                        ci_find(r.msg_type, t.second);
                else if (t.first == "kind") {
                    std::string k = kind_name(r.kind);
                    for (auto& c : k) c = static_cast<char>(std::tolower(c));
                    m = k.find(t.second) != std::string::npos;
                }
                else if (t.first == "src" || t.first == "node")
                                               m = ci_find(r.src,      t.second);
                else if (t.first == "dst")     m = ci_find(r.dst,      t.second);
                else if (t.first == "msg" || t.first == "type")
                                               m = ci_find(r.msg_type, t.second);
                else if (t.first == "machine") m = ci_find(r.machine, t.second);
                if (!m) { ok = false; break; }
            }
            if (ok) visible.push_back(i);
        }
    }

    void rebuild_tree(const EventRow& r) {
        tree->DeleteAllItems();
        wxTreeItemId root = tree->AddRoot(
            wxString::Format("trace: %s %s", kind_name(r.kind),
                             r.msg_type.c_str()));

        wxTreeItemId hdr = tree->AppendItem(root, "Header");
        {
            time_t t  = static_cast<time_t>(r.t_ms_local / 1000);
            int    fr = static_cast<int>(r.t_ms_local % 1000);
            struct tm lt;
            localtime_r(&t, &lt);
            tree->AppendItem(hdr,
                wxString::Format("timestamp:  %02d:%02d:%02d.%03d  (%lld ms)",
                    lt.tm_hour, lt.tm_min, lt.tm_sec, fr,
                    static_cast<long long>(r.t_ms_local)));
            tree->AppendItem(hdr,
                wxString::Format("machine:    %s", r.machine.c_str()));
            tree->AppendItem(hdr,
                wxString::Format("kind:       %s (%u)",
                                  kind_name(r.kind), r.kind));
            tree->AppendItem(hdr,
                wxString::Format("corr_id:    0x%X", r.corr_id));
        }

        wxTreeItemId subj = tree->AppendItem(root, "Subject");
        tree->AppendItem(subj, wxString::Format("src (node):  %s", r.src.c_str()));
        tree->AppendItem(subj, wxString::Format("dst (peer):  %s",
            r.dst.empty() ? "—" : r.dst.c_str()));
        tree->AppendItem(subj, wxString::Format("msg_type:    %s",
            r.msg_type.c_str()));

        if (!r.raw_payload.empty()) {
            wxTreeItemId raw = tree->AppendItem(root, "Raw payload");
            tree->AppendItem(raw,
                wxString::Format("size: %zu bytes", r.raw_payload.size()));
            wxString dump = hex_dump(r.raw_payload);
            wxString line;
            for (size_t i = 0; i < dump.size(); ++i) {
                if (dump[i] == '\n') {
                    tree->AppendItem(raw, line);
                    line.Clear();
                } else line += dump[i];
            }
        }

        tree->Expand(root);
        tree->Expand(hdr);
        tree->Expand(subj);
    }
};


TracePanel::TracePanel(wxWindow* parent) : PanelBase(parent) {
    impl_ = new TracePanelImpl();

    auto* sizer = new wxBoxSizer(wxVERTICAL);

    auto* top = new wxBoxSizer(wxHORIZONTAL);
    top->Add(new wxStaticText(this, wxID_ANY, "filter:"),
             0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    impl_->filter_input = new wxTextCtrl(this, wxID_ANY, "",
                                          wxDefaultPosition, wxDefaultSize,
                                          wxTE_PROCESS_ENTER);
    top->Add(impl_->filter_input, 1, wxRIGHT, 4);
    auto* clear_btn = new wxButton(this, wxID_ANY, "Clear");
    top->Add(clear_btn, 0);
    sizer->Add(top, 0, wxEXPAND | wxALL, 4);

    impl_->split = new wxSplitterWindow(this, wxID_ANY,
                                          wxDefaultPosition, wxDefaultSize,
                                          wxSP_LIVE_UPDATE | wxSP_3DBORDER);
    impl_->split->SetMinimumPaneSize(80);
    impl_->list = new TraceListCtrl(impl_->split, &impl_->ring,
                                      &impl_->visible, &impl_->mtx);
    impl_->tree = new wxTreeCtrl(impl_->split, wxID_ANY,
                                   wxDefaultPosition, wxDefaultSize,
                                   wxTR_DEFAULT_STYLE | wxTR_HIDE_ROOT);
    impl_->split->SplitHorizontally(impl_->list, impl_->tree, -260);
    sizer->Add(impl_->split, 1, wxEXPAND);
    SetSizer(sizer);

    list_ = impl_->list;   // panels.h's TracePanel::list_ retained
                            // for any code that still grabs it.

    impl_->filter_input->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->current_filter = impl_->filter_input->GetValue().ToStdString();
        impl_->apply_filter();
        const long n = static_cast<long>(impl_->visible.size());
        impl_->list->SetItemCount(n);
        impl_->list->Refresh();
    });

    clear_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        impl_->filter_input->SetValue("");
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->current_filter.clear();
        impl_->apply_filter();
        const long n = static_cast<long>(impl_->visible.size());
        impl_->list->SetItemCount(n);
        impl_->list->Refresh();
    });

    impl_->list->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent& evt) {
        const long idx = evt.GetIndex();
        EventRow row_copy;
        {
            std::lock_guard<std::mutex> lk(impl_->mtx);
            if (idx < 0 ||
                static_cast<size_t>(idx) >= impl_->visible.size()) return;
            const size_t ring_idx = impl_->visible[idx];
            if (ring_idx >= impl_->ring.size()) return;
            row_copy = impl_->ring[ring_idx];
        }
        impl_->rebuild_tree(row_copy);
    });
}


void TracePanel::on_frame(const std::string& machine_name,
                            uint16_t tag,
                            const std::string& payload) {
    if (tag != kTagTraceRecord) return;
    if (!impl_) return;

    services::com::TraceRecord tr;
    if (!tr.ParseFromString(payload)) return;

    EventRow r;
    // ts_ns is the emitter's steady_clock ns; render as local wall ms.
    r.t_ms_local  = static_cast<int64_t>(tr.ts_ns() / 1000000ULL);
    r.machine     = machine_name;
    r.kind        = tr.kind();
    r.src         = tr.node_name();
    r.dst         = tr.dst();
    r.msg_type    = tr.msg_type();
    r.corr_id     = tr.corr_id();
    r.raw_payload = tr.payload();

    long new_count = 0;
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->ring.push_back(std::move(r));
        while (impl_->ring.size() > kRingCapacity) impl_->ring.pop_front();
        impl_->apply_filter();
        new_count = static_cast<long>(impl_->visible.size());
    }
    impl_->list->SetItemCount(new_count);
    impl_->list->Refresh();
    if (new_count > 0) impl_->list->EnsureVisible(new_count - 1);
}

}  // namespace sup_gui
