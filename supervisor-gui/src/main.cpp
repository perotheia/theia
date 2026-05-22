// supervisor-gui — entry point.
//
// Loads the per-rig machines.yaml (output of `artheia gui emit <rig>`)
// and hands the endpoint list to MainFrame. Without --machines-yaml,
// falls back to a localhost-only list so the single-supervisor demo
// runs unconfigured against services/com on 127.0.0.1:7700.

#include "sup_gui/machines.h"
#include "sup_gui/main_frame.h"

#include <google/protobuf/stubs/common.h>
#include <wx/app.h>
#include <wx/cmdline.h>
#include <wx/wx.h>

#include <cstdio>
#include <utility>

namespace {

class SupervisorGuiApp : public wxApp {
public:
    bool OnInit() override {
        if (!wxApp::OnInit()) return false;
        GOOGLE_PROTOBUF_VERIFY_VERSION;
        SetAppName("supervisor-gui");

        std::vector<sup_gui::MachineEndpoint> machines;
        if (!machines_yaml_.empty()) {
            machines = sup_gui::load_machines_yaml(
                std::string(machines_yaml_.utf8_str()));
            if (machines.empty()) {
                std::fprintf(stderr,
                    "supervisor-gui: '%s' produced no machines; "
                    "falling back to localhost:7700\n",
                    machines_yaml_.utf8_str().data());
            }
        }
        if (machines.empty()) machines = sup_gui::default_machines();

        auto* frame = new sup_gui::MainFrame(std::move(machines));
        frame->Show(true);
        return true;
    }

    int OnExit() override {
        google::protobuf::ShutdownProtobufLibrary();
        return 0;
    }

    void OnInitCmdLine(wxCmdLineParser& parser) override {
        static const wxCmdLineEntryDesc desc[] = {
            { wxCMD_LINE_OPTION, "m", "machines-yaml",
              "Path to machines.yaml (from `artheia gui emit`)",
              wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
            { wxCMD_LINE_NONE }
        };
        parser.SetDesc(desc);
    }

    bool OnCmdLineParsed(wxCmdLineParser& parser) override {
        parser.Found("machines-yaml", &machines_yaml_);
        return true;
    }

private:
    wxString machines_yaml_;
};

}  // namespace

wxIMPLEMENT_APP(SupervisorGuiApp);
