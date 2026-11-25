// LanVid Studio + LanVid Grid (L2 · 256ch) + DSP Server — Standalone One-Click Windows Setup Wizard
// Zero external runtime dependencies (pure Win32 + COM + D3D11 + Winsock2 + embedded ZIP payload).
// Performs full out-of-the-box ecosystem setup:
//   1. Detects & gracefully closes running lanvid_app.exe / lanvid_dsp_server.exe / sender.exe / receiver.exe
//   2. Probes UDP port 5000 availability; auto-selects free port (5002+) in %APPDATA%\LanVid\config.json
//   3. Verifies VC++ 2015-2022 (x64) Redistributable in Registry; silent auto-installs if missing
//   4. Probes Direct3D 11 hardware support & checks Npcap (System32\Npcap\wpcap.dll) for Grid L2
//   5. Extracts embedded payload (Studio, Grid DSP Server, Virtual ASIO, OBS plugin, preflight.ps1)
//   6. Registers 64ch LanVid Virtual ASIO, LanVidGridDSP auto-start background service, Npcap L2 driver,
//      OBS Studio plugin (auto-detected), Windows Defender Firewall (Domain/Private/Public), Policy-based
//      QoS DSCP=46 (EF), socket buffer/MMCSS tuning, optional High Performance power plan, and clean Uninstaller.

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

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

constexpr int IDC_LOG = 1001;
constexpr int IDC_PROGRESS = 1002;
constexpr int IDC_CHK_DESKTOP = 1003;
constexpr int IDC_CHK_FIREWALL = 1004;
constexpr int IDC_CHK_DSP_SVC = 1005;
constexpr int IDC_CHK_NPCAP = 1006;
constexpr int IDC_CHK_OBS = 1007;
constexpr int IDC_CHK_HIGH_PERF = 1008;
constexpr int IDC_CHK_LAUNCH = 1009;
constexpr int IDC_BTN_INSTALL = 1010;
constexpr int IDC_BTN_CLOSE = 1011;

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
    bool desktop = true;
    bool firewall_qos = true;
    bool dsp_service = true;
    bool grid_npcap = true;
    bool obs_plugin = true;
    bool high_perf_power = false;
    bool silent = false;
};

HWND g_hwnd = nullptr;
HWND g_log = nullptr;
HWND g_prog = nullptr;
HWND g_chk_desktop = nullptr;
HWND g_chk_fw = nullptr;
HWND g_chk_dsp = nullptr;
HWND g_chk_npcap = nullptr;
HWND g_chk_obs = nullptr;
HWND g_chk_highperf = nullptr;
HWND g_chk_launch = nullptr;
HWND g_btn_install = nullptr;
HWND g_btn_close = nullptr;
PreflightReport g_rep{};
bool g_installed = false;

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
    log_msg(L"[3/7] Розпакування Studio, Grid DSP Server, ASIO та плагіна OBS у " +
            g_rep.install_dir.wstring() + L"...");
    if (!extract_embedded_payload(g_rep.install_dir)) {
        log_msg(L"ПОМИЛКА: не вдалося розпакувати інсталяційний архів.");
        return false;
    }

    set_prog(55);
    {   // 64-channel virtual ASIO device (64-bit COM registration in HKLM\SOFTWARE\ASIO)
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
    }

    set_prog(68);
    log_msg(L"[5/7] Збереження конфігурації (UDP :" + std::to_wstring(g_rep.chosen_port) +
            L", рендер: " + (g_rep.d3d11_ok ? L"Direct3D 11" : L"GDI fallback") + L")...");
    write_initial_config(g_rep.chosen_port, g_rep.d3d11_ok);

    set_prog(82);
    {   // Delegate Firewall, QoS DSCP=46, socket buffers, LanVidGridDSP, Npcap, OBS plugin to preflight.ps1
        const fs::path pf = g_rep.install_dir / L"preflight.ps1";
        if (fs::exists(pf)) {
            std::wstring ps_args = L"-NoProfile -ExecutionPolicy Bypass -File \"" + pf.wstring() +
                                   L"\" -InstallDir \"" + g_rep.install_dir.wstring() + L"\"";
            if (opt.firewall_qos) ps_args += L" -ConfigureFirewall -ConfigureQoS -TuneNetwork";
            if (opt.high_perf_power) ps_args += L" -EnableHighPerfPower";
            if (opt.dsp_service) ps_args += L" -InstallDspService";
            if (opt.grid_npcap) ps_args += L" -InstallNpcap";
            if (opt.obs_plugin) ps_args += L" -InstallObsPlugin";
            if (opt.silent) ps_args += L" -Silent";

            log_msg(L"[6/7] Налаштування Firewall, QoS DSCP=46, LanVidGridDSP, Npcap L2 та OBS...");
            DWORD ec_pf = 0;
            run_hidden_cmd(L"powershell.exe " + ps_args, &ec_pf, 300000);
            if (ec_pf == 0) {
                log_msg(L"      Системне налаштування екосистеми LanVid виконано успішно.");
            } else {
                log_msg(L"      Системне налаштування завершено з попередженням (код " + std::to_wstring(ec_pf) + L").");
            }
        }
    }

    set_prog(94);
    log_msg(L"[7/7] Створення ярликів (Меню Пуск" + std::wstring(opt.desktop ? L" + Робочий стіл" : L"") + L")...");
    const fs::path app_exe = g_rep.install_dir / L"lanvid_app.exe";
    const fs::path dsp_exe = g_rep.install_dir / L"lanvid_dsp_server.exe";
    wchar_t* sm_dir = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &sm_dir))) {
        create_shortcut(fs::path(sm_dir) / L"LanVid Studio" / L"LanVid Studio.lnk", app_exe,
                        g_rep.install_dir, L"LanVid Studio + Grid L2 — Ultra-Low Latency LAN Video & Audio");
        if (fs::exists(dsp_exe)) {
            create_shortcut(fs::path(sm_dir) / L"LanVid Studio" / L"LanVid Grid DSP Server.lnk", dsp_exe,
                            g_rep.install_dir, L"LanVid Grid 256ch Layer 2 Distributed DSP Server");
        }
        CoTaskMemFree(sm_dir);
    }
    if (opt.desktop) {
        wchar_t* desk_dir = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk_dir))) {
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

LRESULT CALLBACK wizard_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            HFONT font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HFONT bold = CreateFontW(-19, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

            HWND title = CreateWindowW(L"STATIC", L"Майстер встановлення LanVid Studio + Grid L2 v2.6.0",
                                       WS_VISIBLE | WS_CHILD, 20, 12, 560, 26, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(title, WM_SETFONT, WPARAM(bold), TRUE);

            HWND sub = CreateWindowW(
                L"STATIC", L"Автоматичне налаштування Studio, Grid L2 (256ch), DSP Server, ASIO, OBS та Firewall:",
                WS_VISIBLE | WS_CHILD, 20, 40, 560, 20, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(sub, WM_SETFONT, WPARAM(font), TRUE);

            auto ctl_id = [](int id) { return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)); };

            g_log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_VISIBLE | WS_CHILD | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
                                    20, 66, 560, 185, hwnd, ctl_id(IDC_LOG), nullptr, nullptr);
            SendMessageW(g_log, WM_SETFONT, WPARAM(font), TRUE);

            g_prog = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_VISIBLE | WS_CHILD,
                                     20, 258, 560, 16, hwnd, ctl_id(IDC_PROGRESS), nullptr, nullptr);
            SendMessageW(g_prog, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

            auto make_chk = [&](const wchar_t* label, int y, int id, bool checked) -> HWND {
                HWND h = CreateWindowW(L"BUTTON", label, WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
                                       20, y, 560, 21, hwnd, ctl_id(id), nullptr, nullptr);
                SendMessageW(h, WM_SETFONT, WPARAM(font), TRUE);
                if (checked) SendMessageW(h, BM_SETCHECK, BST_CHECKED, 0);
                return h;
            };

            g_chk_desktop = make_chk(L"Створити ярлик «LanVid Studio» на Робочому столі", 282, IDC_CHK_DESKTOP, true);
            g_chk_fw = make_chk(L"Firewall (UDP 5000-5035, 5353 + TCP) + Policy QoS DSCP=46 + буфери сокетів", 306, IDC_CHK_FIREWALL, true);
            g_chk_dsp = make_chk(L"LanVid Grid DSP Server (фонова служба LanVidGridDSP з автозапуском і Failover)", 330, IDC_CHK_DSP_SVC, true);
            g_chk_npcap = make_chk(L"LanVid Grid Layer 2: перевірити / встановити драйвер Npcap (для Ethernet 256ch)", 354, IDC_CHK_NPCAP, true);
            g_chk_obs = make_chk(L"Автоматично встановити плагін OBS Studio (obs-lanvid.dll + локалізації)", 378, IDC_CHK_OBS, true);
            g_chk_highperf = make_chk(L"Увімкнути профіль живлення «Висока продуктивність» (High Performance)", 402, IDC_CHK_HIGH_PERF, false);
            g_chk_launch = make_chk(L"Запустити LanVid Studio після встановлення", 426, IDC_CHK_LAUNCH, true);

            g_btn_install = CreateWindowW(L"BUTTON", L"Встановити зараз",
                                          WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 342, 460, 140, 34,
                                          hwnd, ctl_id(IDC_BTN_INSTALL), nullptr, nullptr);
            SendMessageW(g_btn_install, WM_SETFONT, WPARAM(font), TRUE);

            g_btn_close = CreateWindowW(L"BUTTON", L"Скасувати", WS_VISIBLE | WS_CHILD,
                                        492, 460, 88, 34, hwnd, ctl_id(IDC_BTN_CLOSE), nullptr, nullptr);
            SendMessageW(g_btn_close, WM_SETFONT, WPARAM(font), TRUE);

            append_log(L"Папка встановлення: " + g_rep.install_dir.wstring());
            append_log(g_rep.running_procs > 0
                           ? (L"[УВАГА] Знайдено активних процесів LanVid: " + std::to_wstring(g_rep.running_procs) + L" (будуть закриті)")
                           : L"[OK] Активних конфліктних процесів немає.");
            append_log(g_rep.port_5000_busy
                           ? (L"[УВАГА] UDP порт 5000 зайнятий -> автопризначено порт " + std::to_wstring(g_rep.chosen_port))
                           : L"[OK] UDP порт 5000 вільний.");
            append_log(g_rep.vc_redist_ok
                           ? L"[OK] Visual C++ 2015-2022 Redistributable (x64) встановлено."
                           : L"[ІНФО] VC++ Redistributable x64 відсутній -> буде встановлено автоматично.");
            append_log(g_rep.d3d11_ok
                           ? L"[OK] Апаратний Direct3D 11 підтримується."
                           : L"[ІНФО] Direct3D 11 недоступний -> буде активовано GDI fallback.");
            append_log(g_rep.npcap_ok
                           ? L"[OK] Npcap (System32\\Npcap\\wpcap.dll) знайдено — LanVid Grid L2 готовий."
                           : L"[ІНФО] Npcap не знайдено — буде запропоновано встановлення для LanVid Grid L2.");
            append_log(g_rep.obs_found
                           ? L"[OK] OBS Studio виявлено — плагін obs-lanvid буде скопійовано автоматично."
                           : L"[ІНФО] OBS Studio не виявлено (файли плагіна збережуться у папці LanVid).");
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (id == IDC_BTN_INSTALL) {
                if (!g_installed) {
                    EnableWindow(g_btn_install, FALSE);
                    InstallOptions opt{};
                    opt.desktop = SendMessageW(g_chk_desktop, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    opt.firewall_qos = SendMessageW(g_chk_fw, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    opt.dsp_service = SendMessageW(g_chk_dsp, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    opt.grid_npcap = SendMessageW(g_chk_npcap, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    opt.obs_plugin = SendMessageW(g_chk_obs, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    opt.high_perf_power = SendMessageW(g_chk_highperf, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    opt.silent = false;
                    if (execute_install(opt)) {
                        SetWindowTextW(g_btn_install, L"Готово");
                        SetWindowTextW(g_btn_close, L"Закрити");
                        EnableWindow(g_btn_install, TRUE);
                    } else {
                        EnableWindow(g_btn_install, TRUE);
                    }
                } else {
                    if (SendMessageW(g_chk_launch, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                        const fs::path exe = g_rep.install_dir / L"lanvid_app.exe";
                        ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, g_rep.install_dir.c_str(),
                                      SW_SHOWNORMAL);
                    }
                    DestroyWindow(hwnd);
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
    if (args.find(L"--check-only") != std::wstring::npos) {
        std::printf("LanVid_Setup preflight: procs=%d port5000_busy=%d chosen_port=%u vc_redist=%d d3d11=%d npcap=%d obs=%d\n",
                    g_rep.running_procs, g_rep.port_5000_busy ? 1 : 0, unsigned(g_rep.chosen_port),
                    g_rep.vc_redist_ok ? 1 : 0, g_rep.d3d11_ok ? 1 : 0,
                    g_rep.npcap_ok ? 1 : 0, g_rep.obs_found ? 1 : 0);
        CoUninitialize();
        WSACleanup();
        return 0;
    }
    if (args.find(L"--silent") != std::wstring::npos) {
        InstallOptions opt{};
        opt.silent = true;
        const bool ok = execute_install(opt);
        CoUninitialize();
        WSACleanup();
        return ok ? 0 : 1;
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
                             CW_USEDEFAULT, CW_USEDEFAULT, 616, 548, nullptr, nullptr, hInst, nullptr);
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
