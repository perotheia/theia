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
#include "sup_gui/trace_decoder_lib.h"

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
#include <memory>
#include <mutex>
#include <string>
#include <vector>

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
    std::string  msg_type;       // proto message type name (event for STATEM)
    uint32_t     corr_id{};      // pairs Send/Recv/Dispatch
    std::string  raw_payload;    // proto-wire-v3 bytes
    std::string  from_state;     // STATEM only: the state left
    std::string  to_state;       // STATEM only: the state entered
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

// Split a compact JSON object ({"n":2,"label":"x"}) into "key: value" strings,
// one per top-level field, for the Decoded tree leaves. Depth-aware so nested
// objects/arrays + quoted commas/colons don't split mid-value. Best-effort
// pretty-print, not a validating parser — the canonical JSON is shown above it.
std::vector<std::string> _split_json_fields(const std::string& json) {
    std::vector<std::string> out;
    size_t i = 0, n = json.size();
    while (i < n && json[i] != '{') ++i;
    if (i >= n) return out;
    ++i;                                   // past '{'
    std::string field;
    int depth = 0;
    bool in_str = false, esc = false;
    for (; i < n; ++i) {
        char c = json[i];
        if (esc) { field.push_back(c); esc = false; continue; }
        if (in_str) {
            field.push_back(c);
            if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') { in_str = true; field.push_back(c); continue; }
        if (c == '{' || c == '[') { ++depth; field.push_back(c); continue; }
        if (c == '}' || c == ']') {
            if (depth == 0) break;         // closing the top object
            --depth; field.push_back(c); continue;
        }
        if (c == ',' && depth == 0) {
            if (!field.empty()) out.push_back(field);
            field.clear();
            continue;
        }
        field.push_back(c);
    }
    if (!field.empty()) out.push_back(field);
    // Tidy each `"key":value` → `key: value`.
    for (auto& f : out) {
        const size_t colon = f.find(':');
        if (colon != std::string::npos) {
            std::string k = f.substr(0, colon), v = f.substr(colon + 1);
            if (k.size() >= 2 && k.front() == '"' && k.back() == '"')
                k = k.substr(1, k.size() - 2);
            f = k + ": " + v;
        }
    }
    return out;
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
            case 4:
                // STATEM rows have no peer (dst empty) — show the transition
                // (from→to) in the Dst column instead, where it reads well.
                if (!r.to_state.empty())
                    return wxString::FromUTF8(r.from_state.c_str()) + " → " +
                           wxString::FromUTF8(r.to_state.c_str());
                return wxString::FromUTF8(r.dst.c_str(), r.dst.size());
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
    std::vector<size_t>   visible;     // ring indices currently shown
    std::string           current_filter;
    std::mutex            mtx;
    bool                  follow{true};   // auto-scroll to newest (Wireshark)
    TraceDecoderLib       decoder;        // libtrace_decoder.so (payload → JSON)

    // Boolean filter expression. Tokens are `[key:]substring` LEAVES combined
    // with `and` / `or` / `not` and parens — e.g.
    //   msg:GetReply or (src:incrementer and msg:Inc)
    // Precedence: not > and > or. Adjacent leaves with no operator imply AND
    // (back-compat: `src:a msg:b` == `src:a and msg:b`). Empty filter = match
    // all. Parsed once per filter-string change into an AST; matches() walks it
    // per row (cheap — the tree is tiny).
    struct FNode {
        enum Op { Leaf, And, Or, Not } op{Leaf};
        std::string key, sub;                 // Leaf: lower-cased key/substring
        std::unique_ptr<FNode> a, b;          // And/Or: a,b; Not: a
    };
    std::unique_ptr<FNode> filter_ast_;       // null = match all

    static void to_lower(std::string& s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(c));
    }

    // ---- filter tokenizer + recursive-descent parser ----------------------
    // Tokens: "(", ")", "and", "or", "not", or a LEAF string (a run of
    // non-space, non-paren chars). `and/or/not` are reserved words (case-
    // insensitive); anything else is a leaf, even with a `:` in it.
    static std::vector<std::string> tokenize_filter(const std::string& s) {
        std::vector<std::string> toks;
        std::string cur;
        auto flush = [&] { if (!cur.empty()) { toks.push_back(cur); cur.clear(); } };
        for (char c : s) {
            if (c == '(' || c == ')') { flush(); toks.emplace_back(1, c); }
            else if (c == ' ' || c == '\t') flush();
            else cur.push_back(c);
        }
        flush();
        return toks;
    }

    static bool is_kw(const std::string& t, const char* kw) {
        if (t.size() != std::strlen(kw)) return false;
        for (size_t i = 0; i < t.size(); ++i)
            if (std::tolower(t[i]) != kw[i]) return false;
        return true;
    }

    static std::unique_ptr<FNode> make_leaf(const std::string& tok) {
        auto n = std::make_unique<FNode>();
        n->op = FNode::Leaf;
        std::string key, sub = tok;
        size_t colon = tok.find(':');
        if (colon != std::string::npos) { key = tok.substr(0, colon); sub = tok.substr(colon + 1); }
        to_lower(key); to_lower(sub);
        n->key = std::move(key); n->sub = std::move(sub);
        return n;
    }

    // Recursive descent: parse_or → parse_and → parse_not → primary.
    // `i` is the cursor into toks.
    static std::unique_ptr<FNode> parse_primary(
            const std::vector<std::string>& toks, size_t& i) {
        if (i >= toks.size()) return nullptr;
        if (toks[i] == "(") {
            ++i;
            auto inner = parse_or(toks, i);
            if (i < toks.size() && toks[i] == ")") ++i;   // tolerate missing ')'
            return inner;
        }
        if (toks[i] == ")") return nullptr;
        return make_leaf(toks[i++]);
    }
    static std::unique_ptr<FNode> parse_not(
            const std::vector<std::string>& toks, size_t& i) {
        if (i < toks.size() && is_kw(toks[i], "not")) {
            ++i;
            auto n = std::make_unique<FNode>();
            n->op = FNode::Not; n->a = parse_not(toks, i);
            return n;
        }
        return parse_primary(toks, i);
    }
    static std::unique_ptr<FNode> parse_and(
            const std::vector<std::string>& toks, size_t& i) {
        auto lhs = parse_not(toks, i);
        while (i < toks.size() && toks[i] != ")") {
            // explicit `and`, or an IMPLICIT and before the next leaf/`(`/`not`.
            const bool explicit_and = is_kw(toks[i], "and");
            if (explicit_and) ++i;
            else if (is_kw(toks[i], "or")) break;          // `or` binds looser
            auto rhs = parse_not(toks, i);
            if (!rhs) break;
            auto n = std::make_unique<FNode>();
            n->op = FNode::And; n->a = std::move(lhs); n->b = std::move(rhs);
            lhs = std::move(n);
        }
        return lhs;
    }
    static std::unique_ptr<FNode> parse_or(
            const std::vector<std::string>& toks, size_t& i) {
        auto lhs = parse_and(toks, i);
        while (i < toks.size() && is_kw(toks[i], "or")) {
            ++i;
            auto rhs = parse_and(toks, i);
            if (!rhs) break;
            auto n = std::make_unique<FNode>();
            n->op = FNode::Or; n->a = std::move(lhs); n->b = std::move(rhs);
            lhs = std::move(n);
        }
        return lhs;
    }

    // (Re)parse current_filter into filter_ast_. Call on filter change only.
    void parse_filter() {
        const auto toks = tokenize_filter(current_filter);
        size_t i = 0;
        filter_ast_ = toks.empty() ? nullptr : parse_or(toks, i);
    }

    // Evaluate one LEAF against a row (the original key dispatch).
    static bool leaf_matches(const FNode& t, const EventRow& r) {
        auto ci_find = [](const std::string& hay, const std::string& needle) {
            if (needle.empty()) return true;
            std::string h; h.reserve(hay.size());
            for (char c : hay) h.push_back(static_cast<char>(std::tolower(c)));
            return h.find(needle) != std::string::npos;
        };
        if (t.key.empty())
            return ci_find(r.src, t.sub) || ci_find(r.dst, t.sub) ||
                   ci_find(r.msg_type, t.sub);
        if (t.key == "kind") {
            std::string k = kind_name(r.kind); to_lower(k);
            return k.find(t.sub) != std::string::npos;
        }
        if (t.key == "src" || t.key == "node") return ci_find(r.src, t.sub);
        if (t.key == "dst")                    return ci_find(r.dst, t.sub);
        if (t.key == "msg" || t.key == "type") return ci_find(r.msg_type, t.sub);
        if (t.key == "machine")                return ci_find(r.machine, t.sub);
        // Unknown key — treat the whole token as a bare substring so a stray
        // `foo:bar` still does something sensible instead of matching nothing.
        return ci_find(r.src, t.sub) || ci_find(r.dst, t.sub) ||
               ci_find(r.msg_type, t.sub);
    }

    static bool eval(const FNode* n, const EventRow& r) {
        if (!n) return true;
        switch (n->op) {
            case FNode::Leaf: return leaf_matches(*n, r);
            case FNode::Not:  return !eval(n->a.get(), r);
            case FNode::And:  return eval(n->a.get(), r) && eval(n->b.get(), r);
            case FNode::Or:   return eval(n->a.get(), r) || eval(n->b.get(), r);
        }
        return true;
    }

    // Does one row pass the current (already-parsed) filter?
    bool matches(const EventRow& r) const { return eval(filter_ast_.get(), r); }

    // Full rebuild of `visible` from `ring`. Only on filter change / Clear /
    // ring eviction — NOT per record (that was O(N²) and froze under bursts).
    void apply_filter() {  // caller holds mtx
        parse_filter();
        visible.clear();
        for (size_t i = 0; i < ring.size(); ++i)
            if (matches(ring[i])) visible.push_back(i);   // null AST = match all
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
        // STATEM rows: surface the transition explicitly. msg_type is the
        // EVENT that fired it; from_state→to_state is the move.
        if (!r.to_state.empty()) {
            tree->AppendItem(subj, wxString::Format(
                "transition: %s → %s",
                r.from_state.empty() ? "?" : r.from_state.c_str(),
                r.to_state.c_str()));
        }

        // Decoded fields — the spec's centerpiece. libtrace_decoder.so renders
        // the payload to JSON via libprotobuf reflection (same .so tdb/rtdb
        // use); one tree leaf per top-level field. Lazy: only the SELECTED
        // row is decoded. Falls back to the raw hex group when the .so isn't
        // available or the type isn't registered.
        wxTreeItemId decoded_id;
        std::string json;
        if (!r.raw_payload.empty() &&
            decoder.decode(r.msg_type, r.raw_payload, json)) {
            decoded_id = tree->AppendItem(root, "Decoded");
            // The JSON is compact ({"n":2}); show it whole + split top-level
            // "key": value pairs into leaves for grep-ability.
            tree->AppendItem(decoded_id,
                wxString::FromUTF8(json.c_str(), json.size()));
            for (const auto& kv : _split_json_fields(json))
                tree->AppendItem(decoded_id, wxString::FromUTF8(kv.c_str()));
        }

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
        if (decoded_id.IsOk()) tree->Expand(decoded_id);
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
    impl_->filter_input->SetHint(
        "msg:GetReply or (src:incrementer and msg:Inc)");
    impl_->filter_input->SetToolTip(
        "Filter expression (Enter to apply). Keys: src:/node: dst: msg:/type: "
        "kind: machine:; a bare word matches src|dst|msg. Combine with "
        "and / or / not and ( ). Adjacent terms imply AND. "
        "Case-insensitive substring match.");
    top->Add(impl_->filter_input, 1, wxRIGHT, 4);
    auto* clear_btn = new wxButton(this, wxID_ANY, "Clear");
    top->Add(clear_btn, 0, wxRIGHT, 8);
    auto* follow_cb = new wxCheckBox(this, wxID_ANY, "Follow new events");
    follow_cb->SetValue(true);
    top->Add(follow_cb, 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(top, 0, wxEXPAND | wxALL, 4);

    follow_cb->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& e) {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->follow = e.IsChecked();
    });

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
    r.from_state  = tr.from_state();
    r.to_state    = tr.to_state();

    long new_count = 0;
    bool follow = false;
    bool show = false;
    EventRow newest;
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        const bool evicted = impl_->ring.size() >= kRingCapacity;

        // INCREMENTAL filter — O(terms), not O(ring). The new row goes to the
        // back; if it passes the filter, its index is appended to `visible`.
        // (The old code re-scanned the whole ring per record → O(N²) freeze.)
        impl_->ring.push_back(std::move(r));
        const size_t new_idx = impl_->ring.size() - 1;
        show = impl_->matches(impl_->ring.back());   // null AST = match all
        if (show) impl_->visible.push_back(new_idx);
        newest = impl_->ring.back();   // copy for the decode pane (lock-free use)

        // Ring eviction shifts every ring index down by one. Rather than
        // rewrite all of `visible`, drop a now-stale leading entry and
        // decrement the rest — O(visible) once per eviction, amortized O(1)
        // since eviction happens at most once per record at steady state.
        if (evicted) {
            impl_->ring.pop_front();
            if (!impl_->visible.empty() && impl_->visible.front() == 0)
                impl_->visible.erase(impl_->visible.begin());
            for (auto& v : impl_->visible) --v;
        }
        new_count = static_cast<long>(impl_->visible.size());
        follow = impl_->follow;
    }
    impl_->list->SetItemCount(new_count);
    impl_->list->Refresh();
    // Following: yank to the newest visible row AND drive the detail/decode
    // pane to it — otherwise the list scrolls but the bottom pane stays on an
    // old (or empty) selection. Not following: leave the user's scroll +
    // selection alone so they can inspect history during a live stream.
    if (follow && new_count > 0) {
        impl_->list->EnsureVisible(new_count - 1);
        // Drive the decode pane to the newest row. We rebuild directly rather
        // than via SetItemState(SELECTED) — that would re-fire our own
        // ITEM_SELECTED handler (double rebuild) and the virtual list shows
        // the follow tail anyway. Only when the new row passes the filter
        // (else it isn't the visible tail).
        if (show) impl_->rebuild_tree(newest);
    }
}

}  // namespace sup_gui
