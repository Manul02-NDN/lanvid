// LanVid Studio + LanVid Grid (L2 · 256ch) + DSP Server — Standalone One-Click Windows Setup Wizard
// Zero external runtime dependencies (pure Win32 + COM + D3D11 + Winsock2 + embedded ZIP payload).
//
// Wizard Modes:
//   Page 0 (Mode Selection):
//     1. Автоматичне встановлення (Рекомендовано) — 1-click full ecosystem deployment
//     2. Ручний вибір (Вибіркове встановлення)    — interactive component selection page
//   Page 1 (Custom Components):
//     - LanVid Studio (обов'язково, locked)
//     - LanVid Grid DSP Server + фонова служба автозапуску LanVidGridDSP
//     - LanVid Virtual ASIO Driver (64ch)
//     - Npcap Driver (Layer 2 Grid 256ch)
//     - OBS Studio Plugin (obs-lanvid.dll)
//     - Правила Windows Firewall та QoS DSCP=46
//     - Профіль живлення High Performance
//     - Ярлик на Робочому столі
//   Page 2 (Installation Progress & Launch)
//
// CLI flags:
//   /S | --silent | --auto        Silent automatic full installation
//   --custom                      Open wizard directly on the custom components page
//   /COMPONENTS=core,dsp_server,… Select specific components (works with /S or interactive)
//   --check-only                  Run pre-flight environment diagnostics and exit 0

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <d3d11.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <urlmon.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "advapi32.lib")

#define IDR_LANVID_PAYLOAD 101

namespace {

namespace fs = std::filesystem;

enum WizardPage {
    PAGE_MODE = 0,
    PAGE_CUSTOM = 1,
    PAGE_PROGRESS = 2,
};

// Control IDs
constexpr int IDC_TITLE = 1000;
constexpr int IDC_SUBTITLE = 1001;
// Page 0: Mode selection
constexpr int IDC_RAD_AUTO = 1010;
constexpr int IDC_DESC_AUTO = 1011;
constexpr int IDC_RAD_CUSTOM = 1012;
constexpr int IDC_DESC_CUSTOM = 1013;
constexpr int IDC_PREFLIGHT_SUMMARY = 1014;
// Page 1: Custom components
constexpr int IDC_CHK_CORE = 1020;
constexpr int IDC_CHK_AUDIO_STUDIO = 1028;
constexpr int IDC_CHK_VIDEO_STUDIO = 1029;
constexpr int IDC_CHK_DSP_SVC = 1021;
constexpr int IDC_CHK_ASIO = 1022;
constexpr int IDC_CHK_NPCAP = 1023;
constexpr int IDC_CHK_OBS = 1024;
constexpr int IDC_CHK_FIREWALL = 1025;
constexpr int IDC_CHK_HIGH_PERF = 1026;
constexpr int IDC_CHK_DESKTOP = 1027;
// Page 2: Progress
constexpr int IDC_LOG = 1030;
constexpr int IDC_PROGRESS = 1031;
constexpr int IDC_CHK_LAUNCH = 1032;
// Navigation buttons
constexpr int IDC_BTN_BACK = 1040;
constexpr int IDC_BTN_NEXT = 1041;
constexpr int IDC_BTN_CLOSE = 1042;

struct PreflightReport {
    int running_procs = 0;
    bool port_5000_busy = false;
    uint16_t chosen_port = 5000;
    bool vc_redist_ok = false;
    bool d3d11_ok = false;
    bool npcap_ok = false;
    bool obs_found = false;
    bool is_admin = false;
    fs::path install_dir;
};

struct InstallOptions {
    bool auto_mode = true;
    bool core = true;              // always required
    bool audio_studio = true;      // LanVid Audio (lanvid_audio_studio.exe)
    bool video_studio = true;      // LanVid Video (lanvid_video_studio.exe)
    bool dsp_service = true;
    bool virtual_audio = true;
    bool grid_npcap = true;
    bool obs_plugin = true;
    bool firewall_qos = true;
    bool high_perf_power = false;
    bool desktop = true;
    bool silent = false;
};

HWND g_hwnd = nullptr;
HWND g_title = nullptr;
HWND g_sub = nullptr;
// Page 0 controls
HWND g_rad_auto = nullptr;
HWND g_desc_auto = nullptr;
HWND g_rad_custom = nullptr;
HWND g_desc_custom = nullptr;
HWND g_preflight_box = nullptr;
// Page 1 controls
HWND g_chk_core = nullptr;
HWND g_chk_audio_studio = nullptr;
HWND g_chk_video_studio = nullptr;
HWND g_chk_dsp = nullptr;
HWND g_chk_asio = nullptr;
HWND g_chk_npcap = nullptr;
HWND g_chk_obs = nullptr;
HWND g_chk_fw = nullptr;
HWND g_chk_highperf = nullptr;
HWND g_chk_desktop = nullptr;
// Page 2 controls
HWND g_log = nullptr;
HWND g_prog = nullptr;
HWND g_chk_launch = nullptr;
// Navigation
HWND g_btn_back = nullptr;
HWND g_btn_next = nullptr;
HWND g_btn_close = nullptr;

PreflightReport g_rep{};
InstallOptions g_cli_opt{};
WizardPage g_page = PAGE_MODE;
bool g_start_on_custom = false;
bool g_installed = false;

std::wstring to_lower_copy(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

bool parse_cli_components(const std::wstring& cmd_line, InstallOptions& opt) {
    const std::wstring lower = to_lower_copy(cmd_line);
    const std::wstring key = L"/components=";
    size_t pos = lower.find(key);
    if (pos == std::wstring::npos) return false;
    pos += key.size();
    std::wstring val;
    if (pos < cmd_line.size() && cmd_line[pos] == L'"') {
        ++pos;
        size_t end = cmd_line.find(L'"', pos);
        val = cmd_line.substr(pos, end == std::wstring::npos ? std::wstring::npos : end - pos);
    } else {
        size_t end = cmd_line.find_first_of(L" \t", pos);
        val = cmd_line.substr(pos, end == std::wstring::npos ? std::wstring::npos : end - pos);
    }

    opt.auto_mode = false;
    opt.core = true;  // mandatory
    opt.audio_studio = false;
    opt.video_studio = false;
    opt.dsp_service = false;
    opt.virtual_audio = false;
    opt.grid_npcap = false;
    opt.obs_plugin = false;
    opt.firewall_qos = false;
    opt.high_perf_power = false;

    std::wstringstream ss(to_lower_copy(val));
    std::wstring item;
    while (std::getline(ss, item, L',')) {
        if (item == L"core" || item == L"studio") opt.core = true;
        else if (item == L"audio_studio" || item == L"audio") opt.audio_studio = true;
        else if (item == L"video_studio" || item == L"video") opt.video_studio = true;
        else if (item == L"dsp_server" || item == L"dsp") opt.dsp_service = true;
        else if (item == L"virtual_audio" || item == L"asio") opt.virtual_audio = true;
        else if (item == L"grid_l2_npcap" || item == L"npcap" || item == L"grid_l2") opt.grid_npcap = true;
        else if (item == L"obs_plugin" || item == L"obs") opt.obs_plugin = true;
        else if (item == L"firewall" || item == L"qos" || item == L"firewall_qos" || item == L"net_opt")
            opt.firewall_qos = true;
        else if (item == L"high_perf_power" || item == L"high_perf" || item == L"power")
            opt.high_perf_power = true;
    }
    return true;
}

void append_log(const std::wstring& line) {
    if (!g_log) return;
    int len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, len, len);
    std::wstring txt = line + L"\r\n";
    SendMessageW(g_log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(txt.c_str()));
}

bool is_elevated() {
    BOOL admin = FALSE;
    PSID grp = nullptr;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                 0, 0, 0, 0, 0, 0, &grp)) {
        CheckTokenMembership(nullptr, grp, &admin);
        FreeSid(grp);
    }
    return admin == TRUE;
}

std::vector<DWORD> find_lanvid_pids() {
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W pe{sizeof(pe)};
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"lanvid_app.exe") == 0 ||
                _wcsicmp(pe.szExeFile, L"lanvid_dsp_server.exe") == 0 ||
                _wcsicmp(pe.szExeFile, L"sender.exe") == 0 ||
                _wcsicmp(pe.szExeFile, L"receiver.exe") == 0) {
                if (pe.th32ProcessID != GetCurrentProcessId()) pids.push_back(pe.th32ProcessID);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pids;
}

void close_running_lanvid() {
    for (DWORD pid : find_lanvid_pids()) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        if (h) {
            TerminateProcess(h, 0);
            WaitForSingleObject(h, 2000);
            CloseHandle(h);
        }
    }
}

bool can_bind_udp_port(uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = htons(port);
    const bool ok = (bind(s, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) == 0);
    closesocket(s);
    return ok;
}

bool check_vc_redist_x64() {
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\x64",
                      0, KEY_READ | KEY_WOW64_64KEY, &hk) != ERROR_SUCCESS) {
        return false;
    }
    DWORD installed = 0, major = 0, minor = 0, sz = sizeof(DWORD);
    RegQueryValueExW(hk, L"Installed", nullptr, nullptr, reinterpret_cast<LPBYTE>(&installed), &sz);
    sz = sizeof(DWORD);
    RegQueryValueExW(hk, L"Major", nullptr, nullptr, reinterpret_cast<LPBYTE>(&major), &sz);
    sz = sizeof(DWORD);
    RegQueryValueExW(hk, L"Minor", nullptr, nullptr, reinterpret_cast<LPBYTE>(&minor), &sz);
    RegCloseKey(hk);
    return (installed == 1 && major >= 14 && minor >= 30);
}

bool check_d3d11_hardware() {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                   &dev, &fl, &ctx);
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    return SUCCEEDED(hr);
}

bool check_npcap_installed() {
    wchar_t sysdir[MAX_PATH] = {};
    if (!GetSystemDirectoryW(sysdir, MAX_PATH)) return false;
    std::error_code ec;
    return fs::exists(fs::path(sysdir) / L"Npcap" / L"wpcap.dll", ec);
}

bool check_obs_installed() {
    std::error_code ec;
    if (fs::exists(L"C:\\Program Files\\obs-studio", ec)) return true;
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\OBS Studio", 0, KEY_READ | KEY_WOW64_64KEY, &hk) ==
        ERROR_SUCCESS) {
        RegCloseKey(hk);
        return true;
    }
    return false;
}

fs::path default_install_dir(bool admin) {
    wchar_t* path = nullptr;
    fs::path base;
    if (admin && SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &path))) {
        base = fs::path(path) / L"LanVid";
        CoTaskMemFree(path);
    } else if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) {
        base = fs::path(path) / L"Programs" / L"LanVid";
        CoTaskMemFree(path);
    } else {
        base = L"C:\\LanVid";
    }
    return base;
}

void write_initial_config(uint16_t listen_port, bool d3d11_ok) {
    wchar_t* appdata = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) return;
    fs::path dir = fs::path(appdata) / L"LanVid";
    CoTaskMemFree(appdata);
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path cfg_path = dir / L"config.json";
    if (fs::exists(cfg_path)) return;  // preserve existing user settings on upgrade

    std::ofstream out(cfg_path, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out << "{\n"
        << "  \"dest_ip\": \"127.0.0.1\",\n"
        << "  \"dest_port\": " << listen_port << ",\n"
        << "  \"listen_port\": " << listen_port << ",\n"
        << "  \"preview_backend\": \"" << (d3d11_ok ? "d3d11" : "gdi") << "\",\n"
        << "  \"quality\": 80,\n"
        << "  \"fps\": 60,\n"
        << "  \"volume\": 1.0\n"
        << "}\n";
}

bool create_shortcut(const fs::path& lnk_path, const fs::path& target_exe, const fs::path& work_dir,
                     const wchar_t* desc) {
    IShellLinkW* psl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                reinterpret_cast<void**>(&psl))) ||
        !psl) {
        return false;
    }
    psl->SetPath(target_exe.c_str());
    psl->SetWorkingDirectory(work_dir.c_str());
    psl->SetDescription(desc);

    IPersistFile* ppf = nullptr;
    bool ok = false;
    if (SUCCEEDED(psl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&ppf))) && ppf) {
        std::error_code ec;
        fs::create_directories(lnk_path.parent_path(), ec);
        ok = SUCCEEDED(ppf->Save(lnk_path.c_str(), TRUE));
        ppf->Release();
    }
    psl->Release();
    return ok;
}

bool run_hidden_cmd(const std::wstring& cmd_line, DWORD* exit_code = nullptr, DWORD timeout_ms = 90000) {
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = cmd_line;
    if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, timeout_ms);
    DWORD ec = 0;
    GetExitCodeProcess(pi.hProcess, &ec);
    if (exit_code) *exit_code = ec;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool run_elevated_cmd(const std::wstring& exe, const std::wstring& params, DWORD* exit_code = nullptr) {
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        return false;
    }
    WaitForSingleObject(sei.hProcess, 90000);
    DWORD ec = 0;
    GetExitCodeProcess(sei.hProcess, &ec);
    if (exit_code) *exit_code = ec;
    CloseHandle(sei.hProcess);
    return true;
}

bool extract_embedded_payload(const fs::path& dest_dir) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_LANVID_PAYLOAD), RT_RCDATA);
    if (!res) return false;
    HGLOBAL mem = LoadResource(nullptr, res);
    if (!mem) return false;
    const void* data = LockResource(mem);
    const DWORD size = SizeofResource(nullptr, res);
    if (!data || size == 0) return false;

    std::error_code ec;
    fs::create_directories(dest_dir, ec);
    const fs::path tmp_zip = fs::temp_directory_path(ec) / L"lanvid_setup_payload.zip";
    {
        std::ofstream out(tmp_zip, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(static_cast<const char*>(data), std::streamsize(size));
    }

    const std::wstring cmd =
        L"tar.exe -xf \"" + tmp_zip.wstring() + L"\" -C \"" + dest_dir.wstring() + L"\"";
    DWORD exit_code = 1;
    bool ok = run_hidden_cmd(cmd, &exit_code) && exit_code == 0;
    if (!ok) {
        const std::wstring ps =
            L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"Expand-Archive -Force -Path '" +
            tmp_zip.wstring() + L"' -DestinationPath '" + dest_dir.wstring() + L"'\"";
        ok = run_hidden_cmd(ps, &exit_code) && exit_code == 0;
    }
    fs::remove(tmp_zip, ec);
    return ok;
}

void register_uninstaller(const fs::path& dest_dir) {
    HKEY hk = nullptr;
    const wchar_t* subkey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\LanVidStudio";
    HKEY root = g_rep.is_admin ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    if (RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_WRITE, nullptr, &hk, nullptr) != ERROR_SUCCESS) {
        if (RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, nullptr, 0, KEY_WRITE, nullptr, &hk, nullptr) !=
            ERROR_SUCCESS) {
            return;
        }
    }
    auto set_sz = [&](const wchar_t* name, const std::wstring& val) {
        RegSetValueExW(hk, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(val.c_str()),
                       DWORD((val.size() + 1) * sizeof(wchar_t)));
    };
    set_sz(L"DisplayName", L"LanVid Studio + Grid L2");
    set_sz(L"DisplayVersion", L"2.6.0");
    set_sz(L"Publisher", L"LanVid Project");
    set_sz(L"InstallLocation", dest_dir.wstring());
    set_sz(L"DisplayIcon", (dest_dir / L"lanvid_app.exe").wstring());
    const std::wstring preflight = (dest_dir / L"preflight.ps1").wstring();
    const std::wstring uninst =
        L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \""
        L"if (Test-Path '" + preflight + L"') { & '" + preflight + L"' -InstallDir '" + dest_dir.wstring() +
        L"' -Uninstall }; "
        L"Remove-Item -Recurse -Force '" + dest_dir.wstring() + L"' -ErrorAction SilentlyContinue; "
        L"Remove-Item -Force \\\"$env:USERPROFILE\\Desktop\\LanVid Studio.lnk\\\" -ErrorAction SilentlyContinue; "
        L"Remove-Item -Path 'HKLM:\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\LanVidStudio' -Force -ErrorAction SilentlyContinue; "
        L"Remove-Item -Path 'HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\LanVidStudio' -Force -ErrorAction SilentlyContinue\"";
    set_sz(L"UninstallString", uninst);
    RegCloseKey(hk);
}

PreflightReport run_preflight_scan() {
    PreflightReport r;
    r.is_admin = is_elevated();
    r.install_dir = default_install_dir(r.is_admin);
    r.running_procs = int(find_lanvid_pids().size());
    r.port_5000_busy = !can_bind_udp_port(5000);
    r.chosen_port = 5000;
    if (r.port_5000_busy) {
        for (uint16_t p = 5002; p <= 5100; p += 2) {
            if (can_bind_udp_port(p)) {
                r.chosen_port = p;
                break;
            }
        }
    }
    r.vc_redist_ok = check_vc_redist_x64();
    r.d3d11_ok = check_d3d11_hardware();
    r.npcap_ok = check_npcap_installed();
    r.obs_found = check_obs_installed();
    return r;
}

bool execute_install(const InstallOptions& opt) {
    auto log_msg = [&](const std::wstring& s) {
        if (!opt.silent) append_log(s);
    };
    auto set_prog = [&](int pct) {
        if (!opt.silent && g_prog) SendMessageW(g_prog, PBM_SETPOS, pct, 0);
    };

    log_msg(opt.auto_mode
                ? L"Режим: Автоматичне встановлення (Рекомендовано — повна екосистема)"
                : L"Режим: Ручний вибір (Вибіркове встановлення обраних компонентів)");

    set_prog(10);
    if (g_rep.running_procs > 0) {
        log_msg(L"[1/7] Закриття активних процесів LanVid / DSP Server...");
        close_running_lanvid();
    } else {
        log_msg(L"[1/7] Конфліктів процесів не виявлено.");
    }

    set_prog(22);
    if (!g_rep.vc_redist_ok) {
        log_msg(L"[2/7] Встановлення Visual C++ 2015-2022 Redistributable (x64)...");
        std::error_code ec;
        const fs::path vc_tmp = fs::temp_directory_path(ec) / L"vc_redist.x64.exe";
        if (SUCCEEDED(URLDownloadToFileW(nullptr, L"https://aka.ms/vs/17/release/vc_redist.x64.exe",
                                         vc_tmp.c_str(), 0, nullptr))) {
            DWORD ec_vc = 0;
            run_hidden_cmd(L"\"" + vc_tmp.wstring() + L"\" /install /quiet /norestart", &ec_vc);
            fs::remove(vc_tmp, ec);
        }
    } else {
        log_msg(L"[2/7] Visual C++ 2015-2022 Redistributable (x64): OK.");
    }

    set_prog(40);
    log_msg(L"[3/7] Розпакування файлів у " + g_rep.install_dir.wstring() + L"...");
    if (!extract_embedded_payload(g_rep.install_dir)) {
        log_msg(L"ПОМИЛКА: не вдалося розпакувати інсталяційний архів.");
        return false;
    }

    // Remove unselected optional binaries/staging if user explicitly unchecked them in custom mode
    std::error_code rem_ec;
    if (!opt.audio_studio) {
        fs::remove(g_rep.install_dir / L"lanvid_audio_studio.exe", rem_ec);
    }
    if (!opt.video_studio) {
        fs::remove(g_rep.install_dir / L"lanvid_video_studio.exe", rem_ec);
    }
    if (!opt.dsp_service) {
        fs::remove(g_rep.install_dir / L"lanvid_dsp_server.exe", rem_ec);
    }

    set_prog(55);
    if (opt.virtual_audio) {
        const std::wstring asio = (g_rep.install_dir / L"LanVidASIO.dll").wstring();
        const std::wstring regsvr = L"C:\\Windows\\System32\\regsvr32.exe";
        const std::wstring reg_args = L"/s \"" + asio + L"\"";
        DWORD ec_reg = 1;
        bool registered = false;
        if (fs::exists(asio)) {
            if (g_rep.is_admin) {
                registered = run_hidden_cmd(L"\"" + regsvr + L"\" " + reg_args, &ec_reg) && ec_reg == 0;
            } else if (!opt.silent) {
                registered = run_elevated_cmd(regsvr, reg_args, &ec_reg) && ec_reg == 0;
            }
        }
        if (registered) {
            log_msg(L"[4/7] ASIO-драйвер «LanVid Virtual ASIO» (64 In / 64 Out) зареєстровано.");
        } else {
            log_msg(L"[4/7] ASIO-драйвер: реєстрація пропущена (потрібні права Адміністратора).");
        }
    } else {
        log_msg(L"[4/7] ASIO-драйвер «LanVid Virtual ASIO»: пропущено користувачем.");
    }

    set_prog(68);
    log_msg(L"[5/7] Збереження конфігурації (UDP :" + std::to_wstring(g_rep.chosen_port) +
            L", рендер: " + (g_rep.d3d11_ok ? L"Direct3D 11" : L"GDI fallback") + L")...");
    write_initial_config(g_rep.chosen_port, g_rep.d3d11_ok);

    set_prog(82);
    {
        const fs::path pf = g_rep.install_dir / L"preflight.ps1";
        if (fs::exists(pf) &&
            (opt.firewall_qos || opt.high_perf_power || opt.dsp_service || opt.grid_npcap || opt.obs_plugin)) {
            std::wstring ps_args = L"-NoProfile -ExecutionPolicy Bypass -File \"" + pf.wstring() +
                                   L"\" -InstallDir \"" + g_rep.install_dir.wstring() + L"\"";
            if (opt.firewall_qos) ps_args += L" -ConfigureFirewall -ConfigureQoS -TuneNetwork";
            if (opt.high_perf_power) ps_args += L" -EnableHighPerfPower";
            if (opt.dsp_service) ps_args += L" -InstallDspService";
            if (opt.grid_npcap) ps_args += L" -InstallNpcap";
            if (opt.obs_plugin) ps_args += L" -InstallObsPlugin";
            if (opt.silent) ps_args += L" -Silent";

            log_msg(L"[6/7] Виконання системного налаштування обраних модулів...");
            DWORD ec_pf = 0;
            run_hidden_cmd(L"powershell.exe " + ps_args, &ec_pf, 300000);
            if (ec_pf == 0) {
                log_msg(L"      Системне налаштування екосистеми LanVid виконано успішно.");
            } else {
                log_msg(L"      Системне налаштування завершено з попередженням (код " + std::to_wstring(ec_pf) + L").");
            }
        } else {
            log_msg(L"[6/7] Додаткові системні модулі пропущено згідно з вибором користувача.");
        }
    }

    set_prog(94);
    log_msg(L"[7/7] Створення ярликів (Меню Пуск" + std::wstring(opt.desktop ? L" + Робочий стіл" : L"") + L")...");
    const fs::path app_exe = g_rep.install_dir / L"lanvid_app.exe";
    const fs::path audio_exe = g_rep.install_dir / L"lanvid_audio_studio.exe";
    const fs::path video_exe = g_rep.install_dir / L"lanvid_video_studio.exe";
    const fs::path dsp_exe = g_rep.install_dir / L"lanvid_dsp_server.exe";
    wchar_t* sm_dir = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &sm_dir))) {
        if (opt.audio_studio && fs::exists(audio_exe)) {
            create_shortcut(fs::path(sm_dir) / L"LanVid Studio" / L"LanVid Audio.lnk", audio_exe,
                            g_rep.install_dir, L"LanVid Audio — AoIP, Patchbay 64x64, Personal Monitor & Grid L2");
        }
        if (opt.video_studio && fs::exists(video_exe)) {
            create_shortcut(fs::path(sm_dir) / L"LanVid Studio" / L"LanVid Video.lnk", video_exe,
                            g_rep.install_dir, L"LanVid Video — Cameras, Tally, Director Intercom/IFB & OBS");
        }
        create_shortcut(fs::path(sm_dir) / L"LanVid Studio" / L"LanVid Studio.lnk", app_exe,
                        g_rep.install_dir, L"LanVid Studio + Grid L2 — Ultra-Low Latency LAN Video & Audio");
        if (opt.dsp_service && fs::exists(dsp_exe)) {
            create_shortcut(fs::path(sm_dir) / L"LanVid Studio" / L"LanVid Grid DSP Server.lnk", dsp_exe,
                            g_rep.install_dir, L"LanVid Grid 256ch Layer 2 Distributed DSP Server");
        }
        CoTaskMemFree(sm_dir);
    }
    if (opt.desktop) {
        wchar_t* desk_dir = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk_dir))) {
            if (opt.audio_studio && fs::exists(audio_exe)) {
                create_shortcut(fs::path(desk_dir) / L"LanVid Audio.lnk", audio_exe, g_rep.install_dir,
                                L"LanVid Audio — AoIP, Patchbay 64x64, Personal Monitor & Grid L2");
            }
            if (opt.video_studio && fs::exists(video_exe)) {
                create_shortcut(fs::path(desk_dir) / L"LanVid Video.lnk", video_exe, g_rep.install_dir,
                                L"LanVid Video — Cameras, Tally, Director Intercom/IFB & OBS");
            }
            create_shortcut(fs::path(desk_dir) / L"LanVid Studio.lnk", app_exe, g_rep.install_dir,
                            L"LanVid Studio + Grid L2 — Ultra-Low Latency LAN Video & Audio");
            CoTaskMemFree(desk_dir);
        }
    }
    register_uninstaller(g_rep.install_dir);

    set_prog(100);
    log_msg(L"=== ВСТАНОВЛЕННЯ ЕКОСИСТЕМИ LANVID ЗАВЕРШЕНО УСПІШНО! ===");
    g_installed = true;
    return true;
}

void show_page(WizardPage p) {
    g_page = p;
    auto set_vis = [](HWND h, bool show) {
        if (h) ShowWindow(h, show ? SW_SHOW : SW_HIDE);
    };

    const bool p0 = (p == PAGE_MODE);
    const bool p1 = (p == PAGE_CUSTOM);
    const bool p2 = (p == PAGE_PROGRESS);

    set_vis(g_rad_auto, p0);
    set_vis(g_desc_auto, p0);
    set_vis(g_rad_custom, p0);
    set_vis(g_desc_custom, p0);
    set_vis(g_preflight_box, p0);

    set_vis(g_chk_core, p1);
    set_vis(g_chk_audio_studio, p1);
    set_vis(g_chk_video_studio, p1);
    set_vis(g_chk_dsp, p1);
    set_vis(g_chk_asio, p1);
    set_vis(g_chk_npcap, p1);
    set_vis(g_chk_obs, p1);
    set_vis(g_chk_fw, p1);
    set_vis(g_chk_highperf, p1);
    set_vis(g_chk_desktop, p1);

    set_vis(g_log, p2);
    set_vis(g_prog, p2);
    set_vis(g_chk_launch, p2);

    if (p0) {
        SetWindowTextW(g_sub, L"Оберіть режим встановлення LanVid Studio + Grid L2:");
        set_vis(g_btn_back, false);
        const bool is_auto = (SendMessageW(g_rad_auto, BM_GETCHECK, 0, 0) == BST_CHECKED);
        SetWindowTextW(g_btn_next, is_auto ? L"Встановити" : L"Далі >");
        SetWindowTextW(g_btn_close, L"Скасувати");
        EnableWindow(g_btn_next, TRUE);
    } else if (p1) {
        SetWindowTextW(g_sub, L"Ручний вибір (Вибіркове встановлення) — позначте потрібні модулі:");
        set_vis(g_btn_back, true);
        SetWindowTextW(g_btn_next, L"Встановити");
        SetWindowTextW(g_btn_close, L"Скасувати");
        EnableWindow(g_btn_next, TRUE);
    } else {
        SetWindowTextW(g_sub, L"Виконання встановлення та налаштування системи:");
        set_vis(g_btn_back, false);
        SetWindowTextW(g_btn_next, g_installed ? L"Готово" : L"Встановлення...");
        SetWindowTextW(g_btn_close, g_installed ? L"Закрити" : L"Скасувати");
    }
}

InstallOptions read_custom_checkboxes() {
    InstallOptions opt{};
    opt.auto_mode = false;
    opt.core = true;
    opt.audio_studio = SendMessageW(g_chk_audio_studio, BM_GETCHECK, 0, 0) == BST_CHECKED;
    opt.video_studio = SendMessageW(g_chk_video_studio, BM_GETCHECK, 0, 0) == BST_CHECKED;
    opt.dsp_service = SendMessageW(g_chk_dsp, BM_GETCHECK, 0, 0) == BST_CHECKED;
    opt.virtual_audio = SendMessageW(g_chk_asio, BM_GETCHECK, 0, 0) == BST_CHECKED;
    opt.grid_npcap = SendMessageW(g_chk_npcap, BM_GETCHECK, 0, 0) == BST_CHECKED;
    opt.obs_plugin = SendMessageW(g_chk_obs, BM_GETCHECK, 0, 0) == BST_CHECKED;
    opt.firewall_qos = SendMessageW(g_chk_fw, BM_GETCHECK, 0, 0) == BST_CHECKED;
    opt.high_perf_power = SendMessageW(g_chk_highperf, BM_GETCHECK, 0, 0) == BST_CHECKED;
    opt.desktop = SendMessageW(g_chk_desktop, BM_GETCHECK, 0, 0) == BST_CHECKED;
    opt.silent = false;
    return opt;
}

void start_install_ui(const InstallOptions& opt) {
    show_page(PAGE_PROGRESS);
    EnableWindow(g_btn_next, FALSE);
    UpdateWindow(g_hwnd);
    if (execute_install(opt)) {
        SetWindowTextW(g_btn_next, L"Готово");
        SetWindowTextW(g_btn_close, L"Закрити");
        EnableWindow(g_btn_next, TRUE);
    } else {
        SetWindowTextW(g_btn_next, L"Повторити");
        EnableWindow(g_btn_next, TRUE);
    }
}

LRESULT CALLBACK wizard_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            HFONT font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HFONT font_bold = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                          0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HFONT title_font = CreateFontW(-20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                           0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

            auto ctl_id = [](int id) { return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)); };

            g_title = CreateWindowW(L"STATIC", L"Майстер встановлення LanVid Studio + Grid L2 v2.6.0",
                                    WS_VISIBLE | WS_CHILD, 20, 14, 570, 28, hwnd, ctl_id(IDC_TITLE), nullptr, nullptr);
            SendMessageW(g_title, WM_SETFONT, WPARAM(title_font), TRUE);

            g_sub = CreateWindowW(L"STATIC", L"Оберіть режим встановлення LanVid Studio + Grid L2:",
                                  WS_VISIBLE | WS_CHILD, 20, 44, 570, 22, hwnd, ctl_id(IDC_SUBTITLE), nullptr, nullptr);
            SendMessageW(g_sub, WM_SETFONT, WPARAM(font), TRUE);

            // ---- PAGE 0: Mode Selection ----
            g_rad_auto = CreateWindowW(
                L"BUTTON", L"Автоматичне встановлення (Рекомендовано)",
                WS_CHILD | WS_GROUP | BS_AUTORADIOBUTTON, 24, 82, 560, 24, hwnd, ctl_id(IDC_RAD_AUTO), nullptr, nullptr);
            SendMessageW(g_rad_auto, WM_SETFONT, WPARAM(font_bold), TRUE);
            SendMessageW(g_rad_auto, BM_SETCHECK, g_start_on_custom ? BST_UNCHECKED : BST_CHECKED, 0);

            g_desc_auto = CreateWindowW(
                L"STATIC",
                L"У 1 клік встановлює всі компоненти: LanVid Studio, LanVid Grid DSP Server (фонова служба\r\n"
                L"з автозапуском), 64ch Virtual ASIO, Npcap (Layer 2 Ethernet), OBS Studio плагін,\r\n"
                L"правила Firewall, QoS DSCP=46 та оптимізацію мережі без зайвих вікон.",
                WS_CHILD, 44, 108, 540, 56, hwnd, ctl_id(IDC_DESC_AUTO), nullptr, nullptr);
            SendMessageW(g_desc_auto, WM_SETFONT, WPARAM(font), TRUE);

            g_rad_custom = CreateWindowW(
                L"BUTTON", L"Ручний вибір (Вибіркове встановлення)",
                WS_CHILD | BS_AUTORADIOBUTTON, 24, 178, 560, 24, hwnd, ctl_id(IDC_RAD_CUSTOM), nullptr, nullptr);
            SendMessageW(g_rad_custom, WM_SETFONT, WPARAM(font_bold), TRUE);
            SendMessageW(g_rad_custom, BM_SETCHECK, g_start_on_custom ? BST_CHECKED : BST_UNCHECKED, 0);

            g_desc_custom = CreateWindowW(
                L"STATIC",
                L"Відкриває сторінку вибору компонентів, де ви самостійно вмикаєте/вимикаєте DSP Server,\r\n"
                L"Virtual ASIO (64ch), Npcap L2, OBS плагін, правила Firewall/QoS та профіль High Performance.",
                WS_CHILD, 44, 204, 540, 42, hwnd, ctl_id(IDC_DESC_CUSTOM), nullptr, nullptr);
            SendMessageW(g_desc_custom, WM_SETFONT, WPARAM(font), TRUE);

            std::wstring summary =
                L"Папка встановлення: " + g_rep.install_dir.wstring() + L"\r\n" +
                (g_rep.vc_redist_ok ? L"[OK] VC++ 2015-2022 Redistributable (x64)   " : L"[ІНФО] VC++ Redist буде довстановлено   ") +
                (g_rep.d3d11_ok ? L"[OK] Direct3D 11\r\n" : L"[ІНФО] GDI fallback\r\n") +
                (g_rep.npcap_ok ? L"[OK] Npcap (Grid L2 готовий)   " : L"[ІНФО] Npcap буде перевірено/встановлено   ") +
                (g_rep.obs_found ? L"[OK] OBS Studio виявлено" : L"[ІНФО] OBS Studio не знайдено");
            g_preflight_box = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", summary.c_str(),
                WS_CHILD | ES_MULTILINE | ES_READONLY, 24, 266, 560, 82, hwnd, ctl_id(IDC_PREFLIGHT_SUMMARY), nullptr, nullptr);
            SendMessageW(g_preflight_box, WM_SETFONT, WPARAM(font), TRUE);

            // ---- PAGE 1: Custom Components ----
            auto make_chk = [&](const wchar_t* label, int y, int id, bool checked, bool enabled = true) -> HWND {
                HWND h = CreateWindowW(L"BUTTON", label, WS_CHILD | BS_AUTOCHECKBOX,
                                       24, y, 560, 24, hwnd, ctl_id(id), nullptr, nullptr);
                SendMessageW(h, WM_SETFONT, WPARAM(font), TRUE);
                SendMessageW(h, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
                if (!enabled) EnableWindow(h, FALSE);
                return h;
            };

            g_chk_core = make_chk(L"Ядро LanVid (lanvid_core.dll) та базові утиліти (обов'язково)", 72, IDC_CHK_CORE, true, false);
            g_chk_audio_studio = make_chk(L"LanVid Audio (AoIP, Patchbay 64x64, Personal Monitor, Grid L2)", 98, IDC_CHK_AUDIO_STUDIO, g_cli_opt.audio_studio);
            g_chk_video_studio = make_chk(L"LanVid Video (Камери та Tally, Інтерком/IFB, OBS Вивід)", 124, IDC_CHK_VIDEO_STUDIO, g_cli_opt.video_studio);
            g_chk_dsp = make_chk(L"LanVid Grid DSP Server + фонова служба автозапуску LanVidGridDSP", 150, IDC_CHK_DSP_SVC, g_cli_opt.dsp_service);
            g_chk_asio = make_chk(L"LanVid Virtual ASIO Driver (64ch In / 64ch Out)", 176, IDC_CHK_ASIO, g_cli_opt.virtual_audio);
            g_chk_npcap = make_chk(L"Npcap Driver (Layer 2 Grid 256ch Ethernet)", 202, IDC_CHK_NPCAP, g_cli_opt.grid_npcap);
            g_chk_obs = make_chk(L"OBS Studio Plugin (obs-lanvid.dll + локалізації)", 228, IDC_CHK_OBS, g_cli_opt.obs_plugin);
            g_chk_fw = make_chk(L"Правила Windows Firewall та QoS DSCP=46 (EF) + оптимізація сокетів", 254, IDC_CHK_FIREWALL, g_cli_opt.firewall_qos);
            g_chk_highperf = make_chk(L"Профіль живлення High Performance (максимальна стабільність аудіо)", 280, IDC_CHK_HIGH_PERF, g_cli_opt.high_perf_power);
            g_chk_desktop = make_chk(L"Створити ярлики «LanVid Audio» і «LanVid Video» на Робочому столі", 306, IDC_CHK_DESKTOP, g_cli_opt.desktop);

            // ---- PAGE 2: Progress ----
            g_log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
                                    20, 76, 564, 248, hwnd, ctl_id(IDC_LOG), nullptr, nullptr);
            SendMessageW(g_log, WM_SETFONT, WPARAM(font), TRUE);

            g_prog = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD,
                                     20, 332, 564, 18, hwnd, ctl_id(IDC_PROGRESS), nullptr, nullptr);
            SendMessageW(g_prog, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

            g_chk_launch = make_chk(L"Запустити LanVid Studio після встановлення", 360, IDC_CHK_LAUNCH, true);

            // ---- Navigation Buttons ----
            g_btn_back = CreateWindowW(L"BUTTON", L"< Назад", WS_CHILD,
                                       226, 404, 100, 34, hwnd, ctl_id(IDC_BTN_BACK), nullptr, nullptr);
            SendMessageW(g_btn_back, WM_SETFONT, WPARAM(font), TRUE);

            g_btn_next = CreateWindowW(L"BUTTON", L"Встановити",
                                       WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 336, 404, 142, 34,
                                       hwnd, ctl_id(IDC_BTN_NEXT), nullptr, nullptr);
            SendMessageW(g_btn_next, WM_SETFONT, WPARAM(font), TRUE);

            g_btn_close = CreateWindowW(L"BUTTON", L"Скасувати", WS_VISIBLE | WS_CHILD,
                                        488, 404, 96, 34, hwnd, ctl_id(IDC_BTN_CLOSE), nullptr, nullptr);
            SendMessageW(g_btn_close, WM_SETFONT, WPARAM(font), TRUE);

            show_page(g_start_on_custom ? PAGE_CUSTOM : PAGE_MODE);
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (id == IDC_RAD_AUTO || id == IDC_RAD_CUSTOM) {
                const bool is_auto = (SendMessageW(g_rad_auto, BM_GETCHECK, 0, 0) == BST_CHECKED);
                SetWindowTextW(g_btn_next, is_auto ? L"Встановити" : L"Далі >");
            } else if (id == IDC_BTN_BACK) {
                if (g_page == PAGE_CUSTOM) show_page(PAGE_MODE);
            } else if (id == IDC_BTN_NEXT) {
                if (g_page == PAGE_MODE) {
                    const bool is_auto = (SendMessageW(g_rad_auto, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    if (is_auto) {
                        InstallOptions auto_opt{};
                        auto_opt.auto_mode = true;
                        start_install_ui(auto_opt);
                    } else {
                        show_page(PAGE_CUSTOM);
                    }
                } else if (g_page == PAGE_CUSTOM) {
                    start_install_ui(read_custom_checkboxes());
                } else if (g_page == PAGE_PROGRESS) {
                    if (!g_installed) {
                        start_install_ui(read_custom_checkboxes());
                    } else {
                        if (SendMessageW(g_chk_launch, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                            const fs::path exe = g_rep.install_dir / L"lanvid_app.exe";
                            ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, g_rep.install_dir.c_str(),
                                          SW_SHOWNORMAL);
                        }
                        DestroyWindow(hwnd);
                    }
                }
            } else if (id == IDC_BTN_CLOSE) {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int nShow) {
    WSADATA wd{};
    WSAStartup(MAKEWORD(2, 2), &wd);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    InitCommonControls();

    g_rep = run_preflight_scan();

    const std::wstring args = cmdLine ? cmdLine : L"";
    const std::wstring lower_args = to_lower_copy(args);

    if (lower_args.find(L"--check-only") != std::wstring::npos) {
        std::printf("LanVid_Setup preflight: procs=%d port5000_busy=%d chosen_port=%u vc_redist=%d d3d11=%d npcap=%d obs=%d\n",
                    g_rep.running_procs, g_rep.port_5000_busy ? 1 : 0, unsigned(g_rep.chosen_port),
                    g_rep.vc_redist_ok ? 1 : 0, g_rep.d3d11_ok ? 1 : 0,
                    g_rep.npcap_ok ? 1 : 0, g_rep.obs_found ? 1 : 0);
        CoUninitialize();
        WSACleanup();
        return 0;
    }

    const bool has_components = parse_cli_components(args, g_cli_opt);
    const bool silent_flag = (lower_args.find(L"--silent") != std::wstring::npos ||
                              lower_args.find(L"--auto") != std::wstring::npos ||
                              lower_args.find(L"/s") != std::wstring::npos);

    if (silent_flag) {
        g_cli_opt.silent = true;
        if (!has_components) g_cli_opt.auto_mode = true;
        const bool ok = execute_install(g_cli_opt);
        CoUninitialize();
        WSACleanup();
        return ok ? 0 : 1;
    }

    if (has_components || lower_args.find(L"--custom") != std::wstring::npos) {
        g_start_on_custom = true;
    }

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wizard_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hbrBackground = HBRUSH(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"LanVidSetupWizardClass";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"LanVid Studio + Grid L2 — Встановлення",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 620, 492, nullptr, nullptr, hInst, nullptr);
    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    WSACleanup();
    return 0;
}
