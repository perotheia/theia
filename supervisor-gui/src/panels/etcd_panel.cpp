// Table Viewer (etcd) — observer_tv_wx.erl analogue for Theia's KV store.
//
// Layout (three regions, top-down + side-by-side):
//
//   +-----------------------------------------------------------------+
//   | endpoint [http://127.0.0.1:2379]  prefix [/theia]  [Watch] [Refresh]
//   +-----------------+-----------------------------------------------+
//   | key list        | value pane                                    |
//   | (wxListCtrl)    | +-------------------------------------------+ |
//   |  /theia/a       | | key:           /theia/a                   | |
//   |  /theia/b       | | mod-rev:       42                         | |
//   |  /theia/sub/c   | | create-rev:    7                          | |
//   |  …              | | lease:         (none)                     | |
//   |                 | | +---------------------------------------+ | |
//   |                 | | | <value contents, auto-detected:       | | |
//   |                 | | |   utf-8 text / json / hex>            | | |
//   |                 | | +---------------------------------------+ | |
//   |                 | | [Save]  [Delete]  [Copy key]              | |
//   |                 | +-------------------------------------------+ |
//   +-----------------+-----------------------------------------------+
//
// The endpoint default targets the host's theia-etcd
// (127.0.0.1:2379). Editing the field re-instantiates the client.
//
// Watch mode (toggle) sets up an etcd::Watcher on the current prefix.
// Each change event marshals back to the wx UI thread via wxQueueEvent,
// where we refresh the key list. List view, value pane, and watcher
// are decoupled — Watcher only triggers a list re-fetch; the value
// pane is only updated on explicit row selection.

#include "sup_gui/panels.h"

#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/splitter.h>
#include <wx/textctrl.h>
#include <wx/clipbrd.h>

// etcd-cpp-apiv3
#include <etcd/SyncClient.hpp>
#include <etcd/Watcher.hpp>
#include <etcd/Response.hpp>
#include <etcd/Value.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace sup_gui {

namespace {

// IDs only meaningful within this TU.
enum : int {
    ID_ENDPOINT      = wxID_HIGHEST + 4001,
    ID_PREFIX        = wxID_HIGHEST + 4002,
    ID_WATCH_TOGGLE  = wxID_HIGHEST + 4003,
    ID_REFRESH       = wxID_HIGHEST + 4004,
    ID_KEY_LIST      = wxID_HIGHEST + 4005,
    ID_VALUE_TEXT    = wxID_HIGHEST + 4006,
    ID_BTN_SAVE      = wxID_HIGHEST + 4007,
    ID_BTN_DELETE    = wxID_HIGHEST + 4008,
    ID_BTN_COPY_KEY  = wxID_HIGHEST + 4009,
};

// wx event posted from the Watcher thread → UI thread to trigger a
// re-fetch of the visible key list. We don't try to apply the
// individual events into the visible list (kvs come and go too fast
// and our refresh round-trips localhost etcd in <2ms anyway).
wxDEFINE_EVENT(EVT_ETCD_CHANGED, wxThreadEvent);

// "looks like JSON" heuristic — accept if first non-ws byte is {} or [].
bool looks_like_json(const std::string& s) {
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        return c == '{' || c == '[';
    }
    return false;
}

// "looks like UTF-8 text" — all bytes either ASCII printable, TAB,
// LF, CR, or part of a valid UTF-8 multi-byte sequence. Conservative;
// errs toward "binary" so we render hex.
bool looks_like_text(const std::string& s) {
    int cont = 0;
    for (unsigned char c : s) {
        if (cont > 0) {
            if ((c & 0xC0) != 0x80) return false;
            --cont; continue;
        }
        if (c == '\t' || c == '\n' || c == '\r') continue;
        if (c >= 0x20 && c < 0x7F) continue;
        // multi-byte UTF-8 lead bytes
        if      ((c & 0xE0) == 0xC0) cont = 1;
        else if ((c & 0xF0) == 0xE0) cont = 2;
        else if ((c & 0xF8) == 0xF0) cont = 3;
        else return false;
    }
    return cont == 0;
}

// hex dump 16 bytes per line; mirrors `xxd`.
wxString render_hex(const std::string& s) {
    wxString out;
    char hex[4];
    for (size_t off = 0; off < s.size(); off += 16) {
        out.Printf("%s%08zx  ", out.IsEmpty() ? "" : "\n", off);
        for (size_t i = 0; i < 16; ++i) {
            if (off + i < s.size()) {
                std::snprintf(hex, sizeof(hex), "%02x ",
                              static_cast<unsigned char>(s[off + i]));
                out << hex;
            } else {
                out << "   ";
            }
            if (i == 7) out << " ";
        }
        out << " ";
        for (size_t i = 0; i < 16 && off + i < s.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(s[off + i]);
            out << (c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '.');
        }
    }
    return out;
}

}  // namespace


// ---------------------------------------------------------------------
// EtcdPanelImpl — actual implementation; EtcdPanel is just a pimpl
// holder so panels.h can stay free of etcd headers.
// ---------------------------------------------------------------------

class EtcdPanelImpl : public wxEvtHandler {
public:
    explicit EtcdPanelImpl(EtcdPanel* outer) : outer_(outer) {
        build_ui();
        Bind(EVT_ETCD_CHANGED, &EtcdPanelImpl::on_etcd_changed, this);
        reconnect();
        refresh_keys();
    }

    ~EtcdPanelImpl() {
        stop_watcher();
        outer_->Unbind(wxEVT_LIST_ITEM_SELECTED,
                       &EtcdPanelImpl::on_key_selected, this);
    }

private:
    EtcdPanel*                       outer_;
    std::unique_ptr<etcd::SyncClient> client_;
    std::unique_ptr<etcd::Watcher>    watcher_;
    std::string                       last_endpoint_;
    std::vector<std::string>          last_keys_;

    wxTextCtrl*  endpoint_{nullptr};
    wxTextCtrl*  prefix_{nullptr};
    wxCheckBox*  watch_{nullptr};
    wxButton*    refresh_{nullptr};
    wxListCtrl*  list_{nullptr};
    wxTextCtrl*  detail_meta_{nullptr};   // multi-line "key: …" header
    wxTextCtrl*  value_text_{nullptr};    // value body
    wxButton*    btn_save_{nullptr};
    wxButton*    btn_delete_{nullptr};
    wxButton*    btn_copy_{nullptr};

    void build_ui() {
        // --- top toolbar ------------------------------------------------
        auto* top = new wxBoxSizer(wxHORIZONTAL);
        top->Add(new wxStaticText(outer_, wxID_ANY, "endpoint:"),
                 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        endpoint_ = new wxTextCtrl(outer_, ID_ENDPOINT,
                                   "http://127.0.0.1:2379",
                                   wxDefaultPosition, wxSize(220, -1),
                                   wxTE_PROCESS_ENTER);
        top->Add(endpoint_, 0, wxRIGHT, 8);

        top->Add(new wxStaticText(outer_, wxID_ANY, "prefix:"),
                 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        prefix_ = new wxTextCtrl(outer_, ID_PREFIX, "/theia",
                                 wxDefaultPosition, wxSize(220, -1),
                                 wxTE_PROCESS_ENTER);
        top->Add(prefix_, 1, wxRIGHT, 8);

        watch_   = new wxCheckBox(outer_, ID_WATCH_TOGGLE, "Watch");
        top->Add(watch_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

        refresh_ = new wxButton(outer_, ID_REFRESH, "Refresh");
        top->Add(refresh_, 0);

        // --- bottom split -----------------------------------------------
        auto* split = new wxSplitterWindow(outer_, wxID_ANY,
                          wxDefaultPosition, wxDefaultSize,
                          wxSP_LIVE_UPDATE | wxSP_BORDER);
        split->SetMinimumPaneSize(120);

        list_ = new wxListCtrl(split, ID_KEY_LIST,
                               wxDefaultPosition, wxDefaultSize,
                               wxLC_REPORT | wxLC_SINGLE_SEL);
        list_->AppendColumn("Key",         wxLIST_FORMAT_LEFT, 360);
        list_->AppendColumn("Mod-rev",     wxLIST_FORMAT_RIGHT,  80);
        list_->AppendColumn("Size",        wxLIST_FORMAT_RIGHT,  70);

        auto* rightPanel = new wxPanel(split);
        auto* rightSz    = new wxBoxSizer(wxVERTICAL);
        detail_meta_ = new wxTextCtrl(rightPanel, wxID_ANY, "",
                            wxDefaultPosition, wxSize(-1, 90),
                            wxTE_MULTILINE | wxTE_READONLY |
                            wxTE_DONTWRAP | wxBORDER_NONE);
        detail_meta_->SetFont(wxFont(wxFontInfo().Family(wxFONTFAMILY_TELETYPE)));
        rightSz->Add(detail_meta_, 0, wxEXPAND | wxALL, 4);

        value_text_ = new wxTextCtrl(rightPanel, ID_VALUE_TEXT, "",
                            wxDefaultPosition, wxDefaultSize,
                            wxTE_MULTILINE | wxTE_DONTWRAP);
        value_text_->SetFont(wxFont(wxFontInfo().Family(wxFONTFAMILY_TELETYPE)));
        rightSz->Add(value_text_, 1, wxEXPAND | wxALL, 4);

        auto* btnRow = new wxBoxSizer(wxHORIZONTAL);
        btn_save_   = new wxButton(rightPanel, ID_BTN_SAVE,     "Save");
        btn_delete_ = new wxButton(rightPanel, ID_BTN_DELETE,   "Delete");
        btn_copy_   = new wxButton(rightPanel, ID_BTN_COPY_KEY, "Copy key");
        btnRow->Add(btn_save_,   0, wxRIGHT, 4);
        btnRow->Add(btn_delete_, 0, wxRIGHT, 4);
        btnRow->Add(btn_copy_,   0);
        rightSz->Add(btnRow, 0, wxALL, 4);

        rightPanel->SetSizer(rightSz);

        split->SplitVertically(list_, rightPanel, 480);

        // --- assemble ---------------------------------------------------
        auto* main = new wxBoxSizer(wxVERTICAL);
        main->Add(top,   0, wxEXPAND | wxALL, 6);
        main->Add(split, 1, wxEXPAND);
        outer_->SetSizer(main);

        // --- handlers ---------------------------------------------------
        endpoint_->Bind(wxEVT_TEXT_ENTER,  &EtcdPanelImpl::on_endpoint_enter, this);
        prefix_  ->Bind(wxEVT_TEXT_ENTER,  &EtcdPanelImpl::on_prefix_enter,   this);
        watch_   ->Bind(wxEVT_CHECKBOX,    &EtcdPanelImpl::on_watch_toggle,   this);
        refresh_ ->Bind(wxEVT_BUTTON,      &EtcdPanelImpl::on_refresh,        this);
        outer_   ->Bind(wxEVT_LIST_ITEM_SELECTED,
                        &EtcdPanelImpl::on_key_selected, this, ID_KEY_LIST);
        btn_save_  ->Bind(wxEVT_BUTTON, &EtcdPanelImpl::on_save,    this);
        btn_delete_->Bind(wxEVT_BUTTON, &EtcdPanelImpl::on_delete,  this);
        btn_copy_  ->Bind(wxEVT_BUTTON, &EtcdPanelImpl::on_copy_key,this);

        set_buttons_enabled(false);
    }

    // ----------- etcd plumbing ---------------------------------------

    void reconnect() {
        stop_watcher();
        last_endpoint_ = endpoint_->GetValue().ToStdString();
        try {
            client_ = std::make_unique<etcd::SyncClient>(last_endpoint_);
        } catch (const std::exception& e) {
            client_.reset();
            wxLogStatus(outer_, "etcd connect failed: %s", e.what());
            return;
        }
        // Force one Range — proves the channel works AND populates initial
        // state. If the endpoint is wrong we surface it here, not in some
        // later async callback.
        try {
            auto r = client_->ls(prefix_->GetValue().ToStdString());
            if (!r.is_ok() && r.error_code() != 0) {
                wxLogStatus(outer_, "etcd ls error: %d %s",
                            r.error_code(), r.error_message().c_str());
            }
        } catch (const std::exception& e) {
            wxLogStatus(outer_, "etcd ls exception: %s", e.what());
        }
    }

    void start_watcher() {
        if (!client_) return;
        std::string prefix = prefix_->GetValue().ToStdString();
        stop_watcher();
        try {
            // recursive=true means "watch any key starting with <prefix>".
            // The callback runs on the Watcher's own thread; we marshal
            // back to the UI thread to refresh.
            EtcdPanelImpl* self = this;
            watcher_ = std::make_unique<etcd::Watcher>(
                *client_,
                prefix,
                [self](etcd::Response /*resp*/) {
                    auto* ev = new wxThreadEvent(EVT_ETCD_CHANGED);
                    wxQueueEvent(self, ev);
                },
                /*recursive=*/true);
        } catch (const std::exception& e) {
            wxLogStatus(outer_, "etcd watch failed: %s", e.what());
            watch_->SetValue(false);
        }
    }

    void stop_watcher() {
        if (watcher_) {
            watcher_->Cancel();
            watcher_.reset();
        }
    }

    // ----------- view refresh ---------------------------------------

    void refresh_keys() {
        if (!client_) {
            list_->DeleteAllItems();
            return;
        }
        std::string prefix = prefix_->GetValue().ToStdString();
        etcd::Response resp;
        try {
            resp = client_->ls(prefix);
        } catch (const std::exception& e) {
            wxLogStatus(outer_, "etcd ls exception: %s", e.what());
            return;
        }
        if (!resp.is_ok() && resp.error_code() != 0) {
            wxLogStatus(outer_, "etcd ls error: %d %s",
                        resp.error_code(), resp.error_message().c_str());
            return;
        }

        list_->DeleteAllItems();
        last_keys_.clear();
        for (size_t i = 0; i < resp.keys().size(); ++i) {
            const auto& k = resp.key(i);
            const auto& v = resp.value(i);
            long row = list_->InsertItem(list_->GetItemCount(), k);
            list_->SetItem(row, 1,
                wxString::Format("%lld", static_cast<long long>(v.modified_index())));
            list_->SetItem(row, 2,
                wxString::Format("%zu", v.as_string().size()));
            last_keys_.push_back(k);
        }
        wxLogStatus(outer_, "etcd: %zu keys under '%s'",
                    last_keys_.size(), prefix.c_str());
    }

    void show_value_for(const std::string& key) {
        if (!client_) return;
        etcd::Response resp;
        try {
            resp = client_->get(key);
        } catch (const std::exception& e) {
            wxLogStatus(outer_, "etcd get exception: %s", e.what());
            return;
        }
        if (!resp.is_ok()) {
            detail_meta_->SetValue(wxString::Format("get error: %d %s",
                resp.error_code(), resp.error_message().c_str()));
            value_text_->Clear();
            set_buttons_enabled(false);
            return;
        }
        const auto& v = resp.value();

        wxString meta;
        meta << "key:         " << v.key() << "\n";
        meta << "mod-rev:     " << static_cast<long long>(v.modified_index()) << "\n";
        meta << "create-rev:  " << static_cast<long long>(v.created_index()) << "\n";
        meta << "version:     " << static_cast<long long>(v.version()) << "\n";
        meta << "lease:       "
             << (v.lease() == 0 ? wxString("(none)")
                                : wxString::Format("%lld", static_cast<long long>(v.lease())))
             << "\n";
        meta << "size:        " << v.as_string().size() << " bytes";
        detail_meta_->SetValue(meta);

        const std::string& body = v.as_string();
        wxString rendered;
        if (looks_like_json(body) || looks_like_text(body)) {
            rendered = wxString::FromUTF8(body.c_str(), body.size());
        } else {
            rendered = render_hex(body);
        }
        value_text_->SetValue(rendered);
        // Only enable Save when content looks like editable text.
        bool editable = (looks_like_json(body) || looks_like_text(body));
        value_text_->SetEditable(editable);
        set_buttons_enabled(true);
    }

    void set_buttons_enabled(bool e) {
        btn_save_  ->Enable(e);
        btn_delete_->Enable(e);
        btn_copy_  ->Enable(e);
    }

    // ----------- handlers --------------------------------------------

    void on_endpoint_enter(wxCommandEvent&) {
        reconnect();
        refresh_keys();
        if (watch_->IsChecked()) start_watcher();
    }

    void on_prefix_enter(wxCommandEvent&) {
        refresh_keys();
        if (watch_->IsChecked()) start_watcher();
    }

    void on_watch_toggle(wxCommandEvent& evt) {
        if (evt.IsChecked()) start_watcher();
        else                 stop_watcher();
    }

    void on_refresh(wxCommandEvent&) {
        refresh_keys();
    }

    void on_etcd_changed(wxThreadEvent&) {
        // A change arrived under the watched prefix; re-fetch keys.
        // Cheap on localhost (~ms). If we ever target a remote etcd
        // the right move is incremental application of the Watcher's
        // Response.events() — leave that for a later commit.
        refresh_keys();
        // If the currently-selected key still exists, refresh its
        // detail pane too.
        long sel = list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (sel >= 0) {
            wxString k = list_->GetItemText(sel, 0);
            show_value_for(k.ToStdString());
        }
    }

    void on_key_selected(wxListEvent& evt) {
        wxString k = list_->GetItemText(evt.GetIndex(), 0);
        show_value_for(k.ToStdString());
    }

    void on_save(wxCommandEvent&) {
        long sel = list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (sel < 0 || !client_) return;
        std::string key = list_->GetItemText(sel, 0).ToStdString();
        std::string value = value_text_->GetValue().ToStdString();
        etcd::Response resp;
        try {
            resp = client_->put(key, value);
        } catch (const std::exception& e) {
            wxLogStatus(outer_, "etcd put exception: %s", e.what());
            return;
        }
        if (!resp.is_ok()) {
            wxLogStatus(outer_, "etcd put error: %d %s",
                        resp.error_code(), resp.error_message().c_str());
            return;
        }
        wxLogStatus(outer_, "saved %s (rev=%lld)",
                    key.c_str(), static_cast<long long>(resp.index()));
        // Refresh the row's metadata.
        show_value_for(key);
    }

    void on_delete(wxCommandEvent&) {
        long sel = list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (sel < 0 || !client_) return;
        std::string key = list_->GetItemText(sel, 0).ToStdString();
        if (wxMessageBox(wxString::Format("Delete key %s?", key.c_str()),
                         "Confirm delete",
                         wxYES_NO | wxICON_QUESTION, outer_) != wxYES) {
            return;
        }
        try {
            auto r = client_->rm(key);
            if (!r.is_ok()) {
                wxLogStatus(outer_, "etcd rm error: %d %s",
                            r.error_code(), r.error_message().c_str());
                return;
            }
        } catch (const std::exception& e) {
            wxLogStatus(outer_, "etcd rm exception: %s", e.what());
            return;
        }
        wxLogStatus(outer_, "deleted %s", key.c_str());
        refresh_keys();
        value_text_->Clear();
        detail_meta_->Clear();
        set_buttons_enabled(false);
    }

    void on_copy_key(wxCommandEvent&) {
        long sel = list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (sel < 0) return;
        wxString k = list_->GetItemText(sel, 0);
        if (wxTheClipboard->Open()) {
            wxTheClipboard->SetData(new wxTextDataObject(k));
            wxTheClipboard->Close();
            wxLogStatus(outer_, "copied: %s", k.c_str().AsChar());
        }
    }
};


// ---------------------------------------------------------------------
// EtcdPanel — thin shell. All real work lives in EtcdPanelImpl.
// ---------------------------------------------------------------------

EtcdPanel::EtcdPanel(wxWindow* parent)
    : wxPanel(parent),
      impl_(std::make_unique<EtcdPanelImpl>(this)) {}

EtcdPanel::~EtcdPanel() = default;

}  // namespace sup_gui
