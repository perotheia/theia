// Diag panel — DoIP/UDS over com (DiagView.SendUds).
//
// A small operator form: pick a canned UDS read (or type raw hex) → Send →
// see the response (positive vs NRC, the bytes, a friendly decode of the common
// ones). com's DiagView proxies the request to the diag FC's UdsRouter; no DoIP
// TCP or TIPC client needed. Request/response — main_frame wires send_cb to the
// focused machine's GrpcClient::send_uds.

#include "sup_gui/panels.h"

#include <wx/wx.h>
#include <wx/choice.h>
#include <wx/textctrl.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace sup_gui {

namespace {

// A canned UDS request: a label + the raw request bytes.
struct Canned { const char* label; std::vector<uint8_t> uds; };

const std::vector<Canned>& canned() {
    static const std::vector<Canned> v = {
        {"Read VIN (22 F190)",            {0x22, 0xF1, 0x90}},
        {"Read SW version (22 F195)",     {0x22, 0xF1, 0x95}},
        {"Read part number (22 F187)",    {0x22, 0xF1, 0x87}},
        {"Fault-log count (22 FDFF)",     {0x22, 0xFD, 0xFF}},
        {"Fault-log entry #0 (22 FD00)",  {0x22, 0xFD, 0x00}},
        {"ReadDTC by mask (19 02 FF)",    {0x19, 0x02, 0xFF}},
        {"Default session (10 01)",       {0x10, 0x01}},
    };
    return v;
}

std::string to_hex(const std::string& b) {
    std::string s;
    char tmp[4];
    for (unsigned char c : b) { std::snprintf(tmp, sizeof(tmp), "%02X ", c); s += tmp; }
    if (!s.empty()) s.pop_back();
    return s;
}

// Parse a hex string ("22 F1 90" or "22F190") into bytes. Ignores whitespace.
bool from_hex(const std::string& in, std::string& out) {
    std::string h;
    for (char c : in) if (!std::isspace(static_cast<unsigned char>(c))) h += c;
    if (h.size() % 2 != 0) return false;
    out.clear();
    for (size_t i = 0; i < h.size(); i += 2) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hi = nib(h[i]), lo = nib(h[i+1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return true;
}

// A friendly one-line decode of the common responses.
std::string decode(const std::string& resp, bool is_nrc) {
    if (resp.empty()) return "(empty response)";
    const unsigned char sid = static_cast<unsigned char>(resp[0]);
    if (is_nrc || sid == 0x7F) {
        // 7F <req-sid> <nrc>
        if (resp.size() >= 3)
            return wxString::Format("NRC: service 0x%02X rejected, code 0x%02X",
                static_cast<unsigned char>(resp[1]),
                static_cast<unsigned char>(resp[2])).ToStdString();
        return "NRC (negative response)";
    }
    // 0x22 positive = 0x62; 0x19 = 0x59; 0x10 = 0x50
    if (sid == 0x62 && resp.size() > 3) {
        // 62 <did-hi> <did-lo> <data...> — try to render trailing ASCII (VIN etc.)
        std::string ascii;
        bool printable = true;
        for (size_t i = 3; i < resp.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(resp[i]);
            if (c >= 0x20 && c < 0x7F) ascii += static_cast<char>(c);
            else { printable = false; break; }
        }
        if (printable && !ascii.empty()) return "DID data: \"" + ascii + "\"";
        return "DID data (binary)";
    }
    if (sid == 0x59) return "DTC report (positive)";
    if (sid == 0x50) return "session started (positive)";
    return "positive response";
}

}  // namespace

struct DiagPanelImpl {
    wxChoice*     canned_choice{nullptr};
    wxTextCtrl*   addr_input{nullptr};
    wxTextCtrl*   hex_input{nullptr};
    wxTextCtrl*   result{nullptr};
    DiagPanel::SendCallback send_cb;
};

DiagPanel::DiagPanel(wxWindow* parent)
    : wxPanel(parent), impl_(new DiagPanelImpl()) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    auto* box = new wxStaticBoxSizer(wxVERTICAL, this, "Diagnostics (DoIP/UDS via com)");

    // Row 1: canned read + ECU address.
    auto* r1 = new wxBoxSizer(wxHORIZONTAL);
    r1->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Read:"),
            0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    impl_->canned_choice = new wxChoice(box->GetStaticBox(), wxID_ANY);
    for (const auto& c : canned()) impl_->canned_choice->Append(c.label);
    impl_->canned_choice->SetSelection(0);
    r1->Add(impl_->canned_choice, 1, wxRIGHT, 8);
    r1->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "ECU addr:"),
            0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    impl_->addr_input = new wxTextCtrl(box->GetStaticBox(), wxID_ANY, "0",
        wxDefaultPosition, wxSize(70, -1));
    r1->Add(impl_->addr_input, 0);
    box->Add(r1, 0, wxEXPAND | wxALL, 4);

    // Row 2: raw UDS hex + Send.
    auto* r2 = new wxBoxSizer(wxHORIZONTAL);
    r2->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "or raw UDS hex:"),
            0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    impl_->hex_input = new wxTextCtrl(box->GetStaticBox(), wxID_ANY, "",
        wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    impl_->hex_input->SetHint("e.g. 22 F1 90  (overrides the dropdown)");
    r2->Add(impl_->hex_input, 1, wxRIGHT, 8);
    auto* send = new wxButton(box->GetStaticBox(), wxID_ANY, "Send");
    r2->Add(send, 0);
    box->Add(r2, 0, wxEXPAND | wxALL, 4);

    sizer->Add(box, 0, wxEXPAND | wxALL, 4);

    impl_->result = new wxTextCtrl(this, wxID_ANY, "",
        wxDefaultPosition, wxDefaultSize,
        wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
    impl_->result->SetFont(wxFont(wxFontInfo().Family(wxFONTFAMILY_TELETYPE)));
    sizer->Add(impl_->result, 1, wxEXPAND | wxALL, 4);

    SetSizer(sizer);

    auto do_send = [this](wxCommandEvent&) {
        if (!impl_->send_cb) {
            impl_->result->AppendText("(no machine connected)\n");
            return;
        }
        // raw hex overrides the dropdown when non-empty
        std::string uds;
        std::string raw = impl_->hex_input->GetValue().ToStdString();
        std::string label;
        if (!raw.empty()) {
            if (!from_hex(raw, uds)) {
                impl_->result->AppendText("bad hex: " + raw + "\n");
                return;
            }
            label = raw;
        } else {
            const auto& c = canned()[impl_->canned_choice->GetSelection()];
            uds.assign(c.uds.begin(), c.uds.end());
            label = c.label;
        }
        long addr = 0;
        impl_->addr_input->GetValue().ToLong(&addr);

        auto r = impl_->send_cb(static_cast<uint32_t>(addr), uds);
        wxString line;
        if (!r.ok) {
            line = wxString::Format("→ %s\n   FAILED (diag unreachable / timeout)\n\n",
                                    label.c_str());
        } else {
            line = wxString::Format("→ %s\n   resp: %s\n   %s\n\n",
                                    label.c_str(), to_hex(r.uds).c_str(),
                                    decode(r.uds, r.is_nrc).c_str());
        }
        impl_->result->AppendText(line);
    };
    send->Bind(wxEVT_BUTTON, do_send);
    impl_->hex_input->Bind(wxEVT_TEXT_ENTER, do_send);
}

DiagPanel::~DiagPanel() { delete impl_; }

void DiagPanel::set_send_callback(SendCallback cb) {
    impl_->send_cb = std::move(cb);
}

}  // namespace sup_gui
