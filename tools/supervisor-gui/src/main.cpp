// supervisor-gui — entry point.
//
// Loads the per-rig machines.json (output of `artheia gui emit <rig>`)
// and hands the endpoint list to MainFrame. Without --machines-json,
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

        // Route wx log messages to stderr instead of the default modal
        // wxLogGui dialog. wxLogGui's DoLogRecord walks parent windows
        // looking for a status bar — that path crashed when called from
        // a panel without a stable frame parent (Phase 4b SIGSEGV in
        // EtcdPanelImpl::refresh_keys → wxLogStatus → TryBefore).
        // An operator GUI logs to terminal, not pop-ups; this is also
        // friendlier under `theia-supervisor-gui &` in a shell.
        delete wxLog::SetActiveTarget(new wxLogStderr());

        // Endpoint resolution order:
        //   1. --machines-json    (flat file)
        //   2. --manifest-dir     (per-machine layout)
        //   3. autodiscover       (well-known locations of either)
        //   4. localhost:7700     (last-resort fallback)
        std::vector<sup_gui::MachineEndpoint> machines;
        if (!machines_json_.empty()) {
            machines = sup_gui::load_machines_json(
                std::string(machines_json_.utf8_str()));
            if (machines.empty()) {
                std::fprintf(stderr,
                    "supervisor-gui: '%s' produced no machines\n",
                    machines_json_.utf8_str().data());
            }
        }
        if (machines.empty() && !manifest_dir_.empty()) {
            machines = sup_gui::load_manifest_dir(
                std::string(manifest_dir_.utf8_str()));
            if (machines.empty()) {
                std::fprintf(stderr,
                    "supervisor-gui: '%s' produced no machines\n",
                    manifest_dir_.utf8_str().data());
            }
        }
        if (machines.empty()) machines = sup_gui::autodiscover_machines();
        if (machines.empty()) {
            std::fprintf(stderr,
                "supervisor-gui: no machines.json or manifest dir found; "
                "falling back to localhost:7700\n");
            machines = sup_gui::default_machines();
        }

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
            { wxCMD_LINE_OPTION, "m", "machines-json",
              "Path to flat machines.json (from `artheia gui emit`).",
              wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
            { wxCMD_LINE_OPTION, "d", "manifest-dir",
              "Path to dist/manifest/ (output of "
              "`artheia generate-manifest`). Reads index.json + each "
              "<machine>/machine.json. Skips kind=host machines.",
              wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
            { wxCMD_LINE_NONE }
        };
        parser.SetDesc(desc);
    }

    bool OnCmdLineParsed(wxCmdLineParser& parser) override {
        parser.Found("machines-json", &machines_json_);
        parser.Found("manifest-dir",  &manifest_dir_);
        return true;
    }

private:
    wxString machines_json_;
    wxString manifest_dir_;
};

}  // namespace

wxIMPLEMENT_APP(SupervisorGuiApp);
