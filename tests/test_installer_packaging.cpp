// Dry-run validator for cross-platform LanVid installers (Windows, macOS, Linux & Release CI).
// Verifies syntax invariants (LF-only Unix shell scripts, balanced Inno Setup/XML sections),
// Auto ("Рекомендовано") vs Custom ("Ручний вибір") installation modes, and guarantees every
// full-ecosystem component is wired across all platforms:
//   - LanVid Studio + Core + lanvid_dsp_server + Virtual Audio (ASIO/CoreAudio HAL/JACK)
//   - Layer 2 permissions/drivers (Windows Npcap, macOS ChmodBPF access_bpf, Linux setcap cap_net_raw)
//   - Background DSP auto-start (LanVidGridDSP / com.lanvid.dspserver.plist / lanvid-dsp.service)
//   - OBS Studio auto-install plugin (obs-lanvid + locales)
//   - Firewall & QoS DSCP rules + clean uninstallation
#include "mini_test.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

fs::path repo_root() {
#if defined(LV_REPO_ROOT)
    fs::path p(LV_REPO_ROOT);
    if (fs::exists(p / "CMakeLists.txt")) return p;
#endif
    fs::path cur = fs::current_path();
    for (int i = 0; i < 6; ++i) {
        if (fs::exists(cur / "CMakeLists.txt") && fs::exists(cur / "installer")) return cur;
        if (!cur.has_parent_path()) break;
        cur = cur.parent_path();
    }
    return fs::current_path();
}

bool read_file_raw(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return !out.empty();
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

void check_all_tokens(const std::string& label, const std::string& text, const std::vector<const char*>& tokens) {
    for (const char* tok : tokens) {
        CHECK_MSG(contains(text, tok), label + " missing required token: " + tok);
    }
}

void check_unix_shell_script(const std::string& label, const std::string& text) {
    CHECK_MSG(text.rfind("#!/", 0) == 0, label + " must start with a POSIX shebang (#!/)");
    CHECK_MSG(text.find('\r') == std::string::npos, label + " contains CRLF (\\r) line endings — must be LF!");
}

}  // namespace

TEST(installer_windows_ecosystem_validation) {
    const fs::path root = repo_root();

    std::string iss, preflight, build_ps1, wizard;
    REQUIRE(read_file_raw(root / "installer/windows/lanvid.iss", iss));
    REQUIRE(read_file_raw(root / "installer/windows/preflight.ps1", preflight));
    REQUIRE(read_file_raw(root / "installer/windows/build_windows_setup.ps1", build_ps1));
    REQUIRE(read_file_raw(root / "installer/windows/setup_wizard.cpp", wizard));

    // 1. Inno Setup script sections, Auto/Custom [Types], [Components], [Tasks], [Run], [UninstallRun]
    check_all_tokens("lanvid.iss", iss, {
        "[Setup]", "[Types]", "[Components]", "[Tasks]", "[Files]", "[Icons]", "[Run]", "[UninstallRun]",
        "Name: \"auto\"", "Name: \"custom\"", "Flags: iscustom",
        "Types: auto custom", "Types: auto",
        "Name: \"core\"", "Name: \"dsp_server\"", "Name: \"virtual_audio\"",
        "Name: \"grid_l2_npcap\"", "Name: \"obs_plugin\"",
        "lanvid_app.exe", "lanvid_core.dll", "lanvid_dsp_server.exe",
        "LanVidASIO.dll", "regserver 64bit", "obs-lanvid.dll", "en-US.ini", "uk-UA.ini",
        "-ConfigureFirewall", "-ConfigureQoS", "-TuneNetwork", "-EnableHighPerfPower",
        "-InstallDspService", "-InstallNpcap", "-InstallObsPlugin",
        "-Uninstall", "LanVidGridDSP", "regsvr32.exe", "/u /s"
    });

    // 2. PowerShell preflight configurator (Firewall, QoS DSCP=46, socket buffers, DSP, Npcap, OBS, Uninstall)
    check_all_tokens("preflight.ps1", preflight, {
        "Domain,Private,Public", "5000-5035", "5353",
        "lanvid_app.exe", "lanvid_dsp_server.exe",
        "HKLM:\\SOFTWARE\\Policies\\Microsoft\\Windows\\QoS", "DSCP Value", "\"46\"",
        "DefaultReceiveWindow", "DefaultSendWindow", "NetworkThrottlingIndex", "powercfg.exe",
        "LanVidGridDSP", "Register-ScheduledTask",
        "System32\\Npcap\\wpcap.dll", "/winpcap_mode=yes /S", "sc.exe start npcap",
        "HKLM:\\SOFTWARE\\OBS Studio", "obs-plugins\\64bit", "obs-lanvid.dll", "en-US.ini", "uk-UA.ini",
        "regsvr32.exe", "Remove-NetFirewallRule", "-Uninstall"
    });

    // 3. Packaging script & standalone Win32 setup wizard (Auto + Custom pages + /S + /COMPONENTS=)
    check_all_tokens("build_windows_setup.ps1", build_ps1, {
        "lanvid_app.exe", "lanvid_core.dll", "lanvid_dsp_server.exe", "LanVidASIO.dll",
        "preflight.ps1", "obs-lanvid.dll", "npcap-*.exe", "lanvid.iss", "/COMPONENTS="
    });
    check_all_tokens("setup_wizard.cpp", wizard, {
        "PAGE_MODE", "PAGE_CUSTOM", "PAGE_PROGRESS",
        "IDC_RAD_AUTO", "IDC_RAD_CUSTOM", "IDC_CHK_CORE", "IDC_CHK_ASIO",
        "BS_AUTORADIOBUTTON", "/components=", "/s", "--auto", "--custom", "--check-only",
        "lanvid_dsp_server.exe", "LanVidASIO.dll", "preflight.ps1",
        "-ConfigureFirewall", "-ConfigureQoS", "-InstallDspService",
        "-InstallNpcap", "-InstallObsPlugin", "-Uninstall"
    });

    std::printf("    [installer-win] lanvid.iss (auto/custom) + preflight.ps1 + build_windows_setup.ps1 + setup_wizard.cpp OK\n");
}

TEST(installer_macos_ecosystem_validation) {
    const fs::path root = repo_root();

    std::string sdk_pkg, studio_pkg, postinst, dist_xml;
    REQUIRE(read_file_raw(root / "sdk/apple/build_macos_pkg.sh", sdk_pkg));
    REQUIRE(read_file_raw(root / "installer/macos/build_studio_pkg.sh", studio_pkg));
    REQUIRE(read_file_raw(root / "sdk/apple/installer/scripts/postinstall", postinst));
    REQUIRE(read_file_raw(root / "sdk/apple/installer/distribution.xml", dist_xml));

    check_unix_shell_script("sdk/apple/build_macos_pkg.sh", sdk_pkg);
    check_unix_shell_script("installer/macos/build_studio_pkg.sh", studio_pkg);
    check_unix_shell_script("sdk/apple/installer/scripts/postinstall", postinst);

    check_all_tokens("sdk/apple/build_macos_pkg.sh", sdk_pkg, {
        "LanVid Studio.app", "/usr/local/bin/lanvid_dsp_server", "/usr/local/lib/liblanvid_core.dylib",
        "LanVidAudio.driver",
        "com.lanvid.pkg.studio", "com.lanvid.pkg.dspserver", "com.lanvid.pkg.chmodbpf", "com.lanvid.pkg.obsplugin",
        "com.lanvid.dspserver.plist", "<key>KeepAlive</key>", "<key>RunAtLoad</key>",
        "com.lanvid.chmod-bpf.plist", "access_bpf", "/dev/bpf*", "dseditgroup",
        "obs-lanvid.plugin", "Library/Application Support/obs-studio",
        "socketfilterfw", "--add", "--unblockapp"
    });

    check_all_tokens("installer/macos/build_studio_pkg.sh", studio_pkg, {
        "customize=\"allow\"",
        "com.lanvid.pkg.studio", "com.lanvid.pkg.dspserver", "com.lanvid.pkg.chmodbpf", "com.lanvid.pkg.obsplugin",
        "LanVid Studio.app", "lanvid_dsp_server", "liblanvid_core.dylib", "LanVidAudio.driver",
        "com.lanvid.dspserver.plist", "com.lanvid.chmod-bpf.plist", "access_bpf", "/dev/bpf*",
        "obs-lanvid.plugin", "socketfilterfw", "--add", "--unblockapp"
    });

    // macOS Standard Install (all checked) vs Customize (optional modules toggleable, Studio mandatory enabled="false")
    check_all_tokens("sdk/apple/installer/distribution.xml", dist_xml, {
        "<installer-gui-script", "</installer-gui-script>", "customize=\"allow\"",
        "choice_studio", "enabled=\"false\"", "com.lanvid.pkg.studio", "lanvid-studio.pkg",
        "choice_dspserver", "com.lanvid.pkg.dspserver", "lanvid-dspserver.pkg",
        "choice_chmodbpf", "com.lanvid.pkg.chmodbpf", "lanvid-chmodbpf.pkg",
        "choice_obsplugin", "com.lanvid.pkg.obsplugin", "lanvid-obsplugin.pkg",
        "choice_hal", "com.lanvid.audio.hal"
    });

    // Postinstall must conditionally skip disabled Customize components
    check_all_tokens("sdk/apple/installer/scripts/postinstall", postinst, {
        "/Applications/LanVid Studio.app", "/usr/local/bin/lanvid_dsp_server",
        "com.lanvid.chmod-bpf.plist", "com.lanvid.dspserver.plist",
        "Customize mode", "socketfilterfw", "--add", "--unblockapp"
    });

    std::printf("    [installer-mac] build_macos_pkg.sh + build_studio_pkg.sh + distribution.xml (Standard/Customize) + postinstall OK (LF clean)\n");
}

TEST(installer_linux_and_release_ci_validation) {
    const fs::path root = repo_root();

    std::string build_linux, install_linux, release_yml;
    REQUIRE(read_file_raw(root / "installer/linux/build_linux_packages.sh", build_linux));
    REQUIRE(read_file_raw(root / "installer/linux/install_lanvid.sh", install_linux));
    REQUIRE(read_file_raw(root / ".github/workflows/release.yml", release_yml));

    check_unix_shell_script("installer/linux/build_linux_packages.sh", build_linux);
    check_unix_shell_script("installer/linux/install_lanvid.sh", install_linux);

    check_all_tokens("installer/linux/build_linux_packages.sh", build_linux, {
        "lanvid_studio", "lanvid_dsp_server", "liblanvid_core.so", "ldconfig",
        "setcap cap_net_raw,cap_net_admin=eip /usr/local/bin/lanvid_studio",
        "setcap cap_net_raw,cap_net_admin=eip /usr/local/bin/lanvid_dsp_server",
        "/etc/systemd/system/lanvid-dsp.service",
        "systemctl daemon-reload", "systemctl enable --now lanvid-dsp.service",
        "/usr/lib/x86_64-linux-gnu/obs-plugins/obs-lanvid.so",
        ".config/obs-studio/plugins/obs-lanvid/bin/64bit",
        "5000:5035/udp", "5353/udp", "DEBIAN/prerm"
    });

    check_all_tokens("installer/linux/install_lanvid.sh", install_linux, {
        "--auto", "--custom", "--check-only", "ask_yn",
        "[1]", "[2]", "INSTALL_MODE=\"auto\"", "INSTALL_MODE=\"custom\"",
        "INSTALL_DSP", "INSTALL_SETCAP", "INSTALL_OBS", "INSTALL_FIREWALL",
        "lanvid_studio", "lanvid_dsp_server", "liblanvid_core.so", "ldconfig",
        "setcap cap_net_raw,cap_net_admin=eip",
        "/etc/systemd/system/lanvid-dsp.service",
        "systemctl daemon-reload", "systemctl enable --now lanvid-dsp.service",
        "obs-lanvid.so", "obs-plugins", "5000:5035/udp", "5353/udp"
    });

    check_all_tokens(".github/workflows/release.yml", release_yml, {
        "build_windows_setup.ps1", "preflight.ps1 -CheckOnly", "lanvid_dsp_server.exe",
        "build_linux_packages.sh", "lanvid-studio_", "LanVid-Linux-x86_64.run",
        "build_studio_pkg.sh", "build_macos_pkg.sh", "lanvid_dsp_server"
    });

    std::printf("    [installer-linux+ci] build_linux_packages.sh + install_lanvid.sh (auto/custom/check-only) + release.yml OK (LF clean)\n");
}
