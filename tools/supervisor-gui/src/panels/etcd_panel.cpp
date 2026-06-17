// Table Viewer — per's config store, read over com→per (PerView.GetSnapshot).
//
// NOT a direct etcd client: etcd lives in the DMZ (unreachable except on
// localhost) and services/per is the SOLE etcd client by design. This panel
// pulls the store rows through com's PerView gRPC proxy (GrpcClient::
// get_store_snapshot, injected by main_frame) and decodes each row's RAW config
// proto bytes CLIENT-SIDE to JSON (TraceDecoderLib). Read-only: config mutation
// goes through per's typed path, never a GUI poke.
//
// Layout:
//
//   +-----------------------------------------------------------------+
//   | [Refresh]   View: ( Current | Pending (N+1) )                   |
//   +-----------------+-----------------------------------------------+
//   | row list        | value pane                                    |
//   | (wxListCtrl)    | +-------------------------------------------+ |
//   |  CounterConfig  | | config-type:  system_demo_CounterConfig   | |
//   |  ObserverConfig | | digest:       <full sha>                  | |
//   |  …              | | +---------------------------------------+ | |
//   |                 | | | <decoded JSON, or hex/text fallback>  | | |
//   |                 | | +---------------------------------------+ | |
//   +-----------------+-----------------------------------------------+
//
// Refresh (and first show) calls the injected snapshot callback → vector<
// StoreRow>; one list row per StoreRow (config_type + short digest). On select,
// decode config bytes via TraceDecoderLib.decode(config_type, config, json).
// Decode failure falls back to the text/hex auto-detect (the old etcd panel's
// path, reused verbatim). The "Pending (N+1)" view is a labelled stub — the N+1
// migration transform isn't exposed yet — defaulting to Current.

#include "sup_gui/panels.h"
#include "sup_gui/trace_decoder_lib.h"   // config proto→JSON (client-side decode)

#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/splitter.h>
#include <wx/textctrl.h>
#include <wx/clipbrd.h>

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace sup_gui {

namespace {

// IDs only meaningful within this TU.
enum : int {
    ID_REFRESH      = wxID_HIGHEST + 4001,
    ID_VIEW_CHOICE  = wxID_HIGHEST + 4002,
    ID_KEY_LIST     = wxID_HIGHEST + 4003,
    ID_VALUE_TEXT   = wxID_HIGHEST + 4004,
    ID_BTN_COPY     = wxID_HIGHEST + 4005,
};

// View modes for the toggle.
enum ViewMode { kViewCurrent = 0, kViewPending = 1 };

// "looks like JSON" heuristic — accept if first non-ws byte is {} or [].
bool looks_like_json(const std::string& s) {
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        return c == '{' || c == '[';
    }
    return false;
}

// "looks like UTF-8 text" — all bytes either ASCII printable, TAB, LF, CR, or
// part of a valid UTF-8 multi-byte sequence. Conservative; errs toward "binary"
// so we render hex.
bool looks_like_text(const std::string& s) {
    int cont = 0;
    for (unsigned char c : s) {
        if (cont > 0) {
            if ((c & 0xC0) != 0x80) return false;
            --cont; continue;
        }
        if (c == '\t' || c == '\n' || c == '\r') continue;
        if (c >= 0x20 && c < 0x7F) continue;
        if      ((c & 0xE0) == 0xC0) cont = 1;
        else if ((c & 0xF0) == 0xE0) cont = 2;
        else if ((c & 0xF8) == 0xF0) cont = 3;
        else return false;
    }
    return cont == 0;
}

// Lightweight stderr logger (same shape as the old panel's — no walk through
// wxLog dispatch from a panel handler).
void log_line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log_line(const char* fmt, ...) {
    char tsbuf[32] = {};
    struct timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm{};
    localtime_r(&ts.tv_sec, &tm);
    std::snprintf(tsbuf, sizeof(tsbuf), "%02d:%02d:%02d.%03ld ",
        tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1'000'000);
    std::fputs(tsbuf,         stderr);
    std::fputs("table_viewer: ", stderr);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
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

// Short digest for the list column (first 12 chars + ellipsis).
wxString short_digest(const std::string& d) {
    if (d.size() <= 14) return wxString::FromUTF8(d.c_str());
    return wxString::FromUTF8(d.substr(0, 12).c_str()) + "...";
}

}  // namespace


// ---------------------------------------------------------------------
// EtcdPanelImpl — actual implementation; EtcdPanel is just a pimpl holder
// so panels.h stays free of wx + decoder headers.
// ---------------------------------------------------------------------

class EtcdPanelImpl : public wxEvtHandler {
public:
    explicit EtcdPanelImpl(EtcdPanel* outer) : outer_(outer) {
        build_ui();
    }

    ~EtcdPanelImpl() {
        outer_->Unbind(wxEVT_LIST_ITEM_SELECTED,
                       &EtcdPanelImpl::on_row_selected, this);
    }

    void set_snapshot_callback(EtcdPanel::SnapshotCallback cb) {
        snapshot_cb_ = std::move(cb);
        refresh_rows();   // first show: pull as soon as we're wired
    }

private:
    EtcdPanel*                      outer_;
    EtcdPanel::SnapshotCallback     snapshot_cb_;
    std::vector<EtcdPanel::StoreRow> rows_;
    TraceDecoderLib                 decoder_;   // config proto→JSON

    wxButton*    refresh_{nullptr};
    wxChoice*    view_{nullptr};
    wxListCtrl*  list_{nullptr};
    wxTextCtrl*  detail_meta_{nullptr};   // multi-line "config-type: …" header
    wxTextCtrl*  value_text_{nullptr};    // decoded value body
    wxButton*    btn_copy_{nullptr};

    void build_ui() {
        // --- top toolbar ------------------------------------------------
        auto* top = new wxBoxSizer(wxHORIZONTAL);
        refresh_ = new wxButton(outer_, ID_REFRESH, "Refresh");
        top->Add(refresh_, 0, wxRIGHT, 12);

        top->Add(new wxStaticText(outer_, wxID_ANY, "View:"),
                 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        wxArrayString views;
        views.Add("Current");
        views.Add("Pending (N+1)");
        view_ = new wxChoice(outer_, ID_VIEW_CHOICE,
                             wxDefaultPosition, wxDefaultSize, views);
        view_->SetSelection(kViewCurrent);   // default to Current
        top->Add(view_, 0, wxALIGN_CENTER_VERTICAL);

        // --- bottom split -----------------------------------------------
        auto* split = new wxSplitterWindow(outer_, wxID_ANY,
                          wxDefaultPosition, wxDefaultSize,
                          wxSP_LIVE_UPDATE | wxSP_BORDER);
        split->SetMinimumPaneSize(120);

        list_ = new wxListCtrl(split, ID_KEY_LIST,
                               wxDefaultPosition, wxDefaultSize,
                               wxLC_REPORT | wxLC_SINGLE_SEL);
        list_->AppendColumn("Config type", wxLIST_FORMAT_LEFT,  300);
        list_->AppendColumn("Digest",      wxLIST_FORMAT_LEFT,  140);

        auto* rightPanel = new wxPanel(split);
        auto* rightSz    = new wxBoxSizer(wxVERTICAL);
        detail_meta_ = new wxTextCtrl(rightPanel, wxID_ANY, "",
                            wxDefaultPosition, wxSize(-1, 70),
                            wxTE_MULTILINE | wxTE_READONLY |
                            wxTE_DONTWRAP | wxBORDER_NONE);
        detail_meta_->SetFont(wxFont(wxFontInfo().Family(wxFONTFAMILY_TELETYPE)));
        rightSz->Add(detail_meta_, 0, wxEXPAND | wxALL, 4);

        value_text_ = new wxTextCtrl(rightPanel, ID_VALUE_TEXT, "",
                            wxDefaultPosition, wxDefaultSize,
                            wxTE_MULTILINE | wxTE_DONTWRAP | wxTE_READONLY);
        value_text_->SetFont(wxFont(wxFontInfo().Family(wxFONTFAMILY_TELETYPE)));
        rightSz->Add(value_text_, 1, wxEXPAND | wxALL, 4);

        auto* btnRow = new wxBoxSizer(wxHORIZONTAL);
        btn_copy_ = new wxButton(rightPanel, ID_BTN_COPY, "Copy type");
        btnRow->Add(btn_copy_, 0);
        rightSz->Add(btnRow, 0, wxALL, 4);

        rightPanel->SetSizer(rightSz);
        split->SplitVertically(list_, rightPanel, 460);

        // --- assemble ---------------------------------------------------
        auto* main = new wxBoxSizer(wxVERTICAL);
        main->Add(top,   0, wxEXPAND | wxALL, 6);
        main->Add(split, 1, wxEXPAND);
        outer_->SetSizer(main);

        // --- handlers ---------------------------------------------------
        refresh_->Bind(wxEVT_BUTTON,   &EtcdPanelImpl::on_refresh,     this);
        view_   ->Bind(wxEVT_CHOICE,   &EtcdPanelImpl::on_view_change, this);
        outer_  ->Bind(wxEVT_LIST_ITEM_SELECTED,
                       &EtcdPanelImpl::on_row_selected, this, ID_KEY_LIST);
        btn_copy_->Bind(wxEVT_BUTTON,  &EtcdPanelImpl::on_copy_type,   this);

        btn_copy_->Enable(false);
    }

    // ----------- data pull -------------------------------------------

    void refresh_rows() {
        if (!list_) return;

        list_->Freeze();
        list_->DeleteAllItems();
        rows_.clear();
        detail_meta_->ChangeValue(wxString());
        value_text_->ChangeValue(wxString());
        btn_copy_->Enable(false);

        if (view_->GetSelection() == kViewPending) {
            // N+1 view: the migration transform isn't exposed yet. Show a clear
            // placeholder rather than blocking on the full migration diff.
            list_->Thaw();
            detail_meta_->ChangeValue(
                "N+1 view: pending migration tooling.\n"
                "The N+1 transform is not yet exposed over com→per; "
                "this view is a stub.");
            value_text_->ChangeValue(
                "Switch to \"Current\" to browse per's live config store.");
            return;
        }

        if (!snapshot_cb_) {
            list_->Thaw();
            detail_meta_->ChangeValue("(no machine connected)");
            return;
        }

        bool ok = false;
        rows_ = snapshot_cb_("", &ok);   // "" → all config types
        if (!ok) {
            list_->Thaw();
            detail_meta_->ChangeValue(
                "GetSnapshot failed — is per reachable via com?");
            log_line("GetSnapshot failed");
            return;
        }

        for (size_t i = 0; i < rows_.size(); ++i) {
            const auto& r = rows_[i];
            long row = list_->InsertItem(list_->GetItemCount(),
                wxString::FromUTF8(r.config_type.c_str()));
            if (row < 0) continue;
            list_->SetItem(row, 1, short_digest(r.digest));
        }
        list_->Thaw();
        log_line("snapshot: %llu config row(s)",
                 static_cast<unsigned long long>(rows_.size()));
    }

    void show_row(size_t idx) {
        if (idx >= rows_.size()) return;
        const auto& r = rows_[idx];

        wxString meta;
        meta << wxString::FromAscii("config-type: ")
             << wxString::FromUTF8(r.config_type.c_str())
             << wxString::FromAscii("\n");
        meta << wxString::FromAscii("digest:      ")
             << wxString::FromUTF8(r.digest.c_str())
             << wxString::FromAscii("\n");
        meta << wxString::Format("size:        %llu bytes",
            static_cast<unsigned long long>(r.config.size()));
        detail_meta_->ChangeValue(meta);

        // Decode the RAW config proto bytes → JSON, keyed by config_type (the
        // registered libtrace_decoder type name). This is the same decode path
        // the old etcd panel used for /theia/config/* values; per now returns
        // the (digest, config) split, so no "<digest>\0<proto>" unframing here.
        std::string json;
        if (decoder_.available() &&
            decoder_.decode(r.config_type, r.config, json)) {
            value_text_->ChangeValue(wxString::FromUTF8(json.data(), json.size()));
            btn_copy_->Enable(true);
            return;
        }

        // Decode failed / unknown type → text/hex auto-detect fallback.
        const std::string& body = r.config;
        const bool is_textual = looks_like_json(body) || looks_like_text(body);
        wxString rendered;
        if (is_textual) {
            rendered = wxString::FromUTF8(body.data(), body.size());
            if (rendered.IsEmpty() && !body.empty()) rendered = render_hex(body);
        } else {
            rendered = render_hex(body);
        }
        value_text_->ChangeValue(rendered);
        btn_copy_->Enable(true);
    }

    // ----------- handlers --------------------------------------------

    void on_refresh(wxCommandEvent&)        { refresh_rows(); }
    void on_view_change(wxCommandEvent&)    { refresh_rows(); }

    void on_row_selected(wxListEvent& evt) {
        show_row(static_cast<size_t>(evt.GetIndex()));
    }

    void on_copy_type(wxCommandEvent&) {
        long sel = list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (sel < 0) return;
        wxString t = list_->GetItemText(sel, 0);
        if (wxTheClipboard->Open()) {
            wxTheClipboard->SetData(new wxTextDataObject(t));
            wxTheClipboard->Close();
            log_line("copied: %s", t.c_str().AsChar());
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

void EtcdPanel::set_snapshot_callback(SnapshotCallback cb) {
    impl_->set_snapshot_callback(std::move(cb));
}

}  // namespace sup_gui
