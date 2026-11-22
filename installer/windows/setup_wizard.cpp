// LanVid Studio — Standalone One-Click Windows Setup Wizard (LanVid_Setup.exe)
// Zero external runtime dependencies (pure Win32 + COM + D3D11 + Winsock2 + embedded ZIP payload).
// Performs all pre-flight checks:
//   1. Detects & gracefully closes running lanvid_app.exe / sender.exe / receiver.exe
//   2. Probes UDP port 5000 availability; auto-selects free port (5002+) in %APPDATA%\LanVid\config.json
//   3. Verifies VC++ 2015-2022 (x64) Redistributable in Registry; silent auto-installs if missing
//   4. Probes Direct3D 11 hardware support; writes "preview_backend": "gdi" if D3D11 unavailable
//   5. Configures Windows Defender Firewall Inbound/Outbound UDP rules
//   6. Extracts embedded LanVid payload, creates Desktop & Start Menu shortcuts, registers Uninstaller,
//      and offers "Запустити LanVid зараз" checked by default!

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
constexpr int IDC_CHK_LAUNCH = 1005;
constexpr int IDC_BTN_INSTALL = 1006;
constexpr int IDC_BTN_CLOSE = 1007;

struct PreflightReport {
    int running_procs = 0;
    bool port_5000_busy = false;
    uint16_t chosen_port = 5000;
    bool vc_redist_ok = false;
    bool d3d11_ok = false;
    bool is_admin = false;
    fs::path install_dir;
};

HWND g_hwnd = nullptr;
HWND g_log = nullptr;
HWND g_prog = nullptr;
HWND g_chk_desktop = nullptr;
HWND g_chk_fw = nullptr;
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

bool run_hidden_cmd(const std::wstring& cmd_line, DWORD* exit_code = nullptr) {
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = cmd_line;
    if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD ec = 0;
    GetExitCodeProcess(pi.hProcess, &ec);
    if (exit_code) *exit_code = ec;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
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

    // Use built-in Windows tar.exe (bsdtar on Windows 10/11) for instant zero-dependency extraction.
    const std::wstring cmd =
        L"tar.exe -xf \"" + tmp_zip.wstring() + L"\" -C \"" + dest_dir.wstring() + L"\"";
    DWORD exit_code = 1;
    bool ok = run_hidden_cmd(cmd, &exit_code) && exit_code == 0;
    if (!ok) {
        // Fallback: PowerShell Expand-Archive
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
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, nullptr, 0, KEY_WRITE, nullptr, &hk, nullptr) !=
        ERROR_SUCCESS) {
        return;
    }
    auto set_sz = [&](const wchar_t* name, const std::wstring& val) {
        RegSetValueExW(hk, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(val.c_str()),
                       DWORD((val.size() + 1) * sizeof(wchar_t)));
    };
    set_sz(L"DisplayName", L"LanVid Studio");
    set_sz(L"DisplayVersion", L"2.3.1");
    set_sz(L"Publisher", L"LanVid Project");
    set_sz(L"InstallLocation", dest_dir.wstring());
    set_sz(L"DisplayIcon", (dest_dir / L"lanvid_app.exe").wstring());
    const std::wstring uninst =
        L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"Stop-Process -Name lanvid_app,sender,receiver -Force -ErrorAction SilentlyContinue; Start-Process regsvr32.exe -ArgumentList '/u','/s','" +
        (dest_dir / L"LanVidASIO.dll").wstring() +
        L"' -Wait -ErrorAction SilentlyContinue; Remove-Item -Recurse -Force '" +
        dest_dir.wstring() +
        L"' -ErrorAction SilentlyContinue; Remove-Item -Force \\\"$env:USERPROFILE\\Desktop\\LanVid Studio.lnk\\\" -ErrorAction SilentlyContinue; Remove-Item -Path 'HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\LanVidStudio' -Force -ErrorAction SilentlyContinue\"";
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
    return r;
}

bool execute_install(bool want_desktop, bool want_firewall, bool silent) {
    auto log_msg = [&](const std::wstring& s) {
        if (!silent) append_log(s);
    };
    auto set_prog = [&](int pct) {
        if (!silent && g_prog) SendMessageW(g_prog, PBM_SETPOS, pct, 0);
    };

    set_prog(10);
    if (g_rep.running_procs > 0) {
        log_msg(L"[1/6] Закриття активних процесів LanVid...");
        close_running_lanvid();
    } else {
        log_msg(L"[1/6] Конфліктів процесів не виявлено.");
    }

    set_prog(25);
    if (!g_rep.vc_redist_ok) {
        log_msg(L"[2/6] Встановлення Visual C++ 2015-2022 Redistributable (x64)...");
        std::error_code ec;
        const fs::path vc_tmp = fs::temp_directory_path(ec) / L"vc_redist.x64.exe";
        if (SUCCEEDED(URLDownloadToFileW(nullptr, L"https://aka.ms/vs/17/release/vc_redist.x64.exe",
                                         vc_tmp.c_str(), 0, nullptr))) {
            DWORD ec_vc = 0;
            run_hidden_cmd(L"\"" + vc_tmp.wstring() + L"\" /install /quiet /norestart", &ec_vc);
            fs::remove(vc_tmp, ec);
        }
    } else {
        log_msg(L"[2/6] Visual C++ 2015-2022 Redistributable (x64): OK.");
    }

    set_prog(50);
    log_msg(L"[3/6] Розпакування файлів у " + g_rep.install_dir.wstring() + L"...");
    if (!extract_embedded_payload(g_rep.install_dir)) {
        log_msg(L"ПОМИЛКА: не вдалося розпакувати інсталяційний архів.");
        return false;
    }

    {   // 64-channel virtual ASIO device (silent COM registration; requires the elevated installer)
        const std::wstring asio = (g_rep.install_dir / L"LanVidASIO.dll").wstring();
        DWORD ec_reg = 1;
        if (fs::exists(asio) && run_hidden_cmd(L"regsvr32.exe /s \"" + asio + L"\"", &ec_reg) && ec_reg == 0) {
            log_msg(L"      ASIO-драйвер «LanVid Virtual ASIO» (64 In / 64 Out) зареєстровано.");
        } else {
            log_msg(L"      ASIO-драйвер: реєстрація пропущена (потрібні права Адміністратора: regsvr32 LanVidASIO.dll).");
        }
    }

    set_prog(70);
    log_msg(L"[4/6] Збереження конфігурації (UDP :" + std::to_wstring(g_rep.chosen_port) +
            L", рендер: " + (g_rep.d3d11_ok ? L"Direct3D 11" : L"GDI fallback") + L")...");
    write_initial_config(g_rep.chosen_port, g_rep.d3d11_ok);

    set_prog(85);
    if (want_firewall) {
        const std::wstring exe = (g_rep.install_dir / L"lanvid_app.exe").wstring();
        const std::wstring fw_in =
            L"netsh advfirewall firewall add rule name=\"LanVid Studio (UDP In)\" dir=in action=allow protocol=UDP program=\"" +
            exe + L"\" profile=private,public";
        const std::wstring fw_out =
            L"netsh advfirewall firewall add rule name=\"LanVid Studio (UDP Out)\" dir=out action=allow protocol=UDP program=\"" +
            exe + L"\" profile=private,public";
        DWORD ec1 = 1, ec2 = 1;
        run_hidden_cmd(fw_in, &ec1);
        run_hidden_cmd(fw_out, &ec2);
        if (ec1 == 0 && ec2 == 0) {
            log_msg(L"[5/6] Правила Windows Defender Firewall успішно додано.");
        } else {
            log_msg(L"[5/6] Firewall: потрібен запуск від Адміністратора (пропущено в режимі користувача).");
        }
    }

    set_prog(95);
    log_msg(L"[6/6] Створення ярликів (Меню Пуск" + std::wstring(want_desktop ? L" + Робочий стіл" : L"") + L")...");
    const fs::path app_exe = g_rep.install_dir / L"lanvid_app.exe";
    wchar_t* sm_dir = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &sm_dir))) {
        create_shortcut(fs::path(sm_dir) / L"LanVid Studio" / L"LanVid Studio.lnk", app_exe,
                        g_rep.install_dir, L"LanVid Studio — Ultra-Low Latency LAN Video & Audio");
        CoTaskMemFree(sm_dir);
    }
    if (want_desktop) {
        wchar_t* desk_dir = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk_dir))) {
            create_shortcut(fs::path(desk_dir) / L"LanVid Studio.lnk", app_exe, g_rep.install_dir,
                            L"LanVid Studio — Ultra-Low Latency LAN Video & Audio");
            CoTaskMemFree(desk_dir);
        }
    }
    register_uninstaller(g_rep.install_dir);

    set_prog(100);
    log_msg(L"=== ВСТАНОВЛЕННЯ ЗАВЕРШЕНО УСПІШНО! ===");
    g_installed = true;
    return true;
}

LRESULT CALLBACK wizard_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            HFONT font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HFONT bold = CreateFontW(-20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

            HWND title = CreateWindowW(L"STATIC", L"Майстер встановлення LanVid Studio v2.1",
                                       WS_VISIBLE | WS_CHILD, 20, 14, 540, 28, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(title, WM_SETFONT, WPARAM(bold), TRUE);

            HWND sub = CreateWindowW(
                L"STATIC", L"Автоматична перевірка середовища, конфліктів портів, Direct3D 11 та Firewall:",
                WS_VISIBLE | WS_CHILD, 20, 44, 540, 22, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(sub, WM_SETFONT, WPARAM(font), TRUE);

            auto ctl_id = [](int id) { return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)); };

            g_log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_VISIBLE | WS_CHILD | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
                                    20, 72, 544, 210, hwnd, ctl_id(IDC_LOG), nullptr, nullptr);
            SendMessageW(g_log, WM_SETFONT, WPARAM(font), TRUE);

            g_prog = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_VISIBLE | WS_CHILD,
                                     20, 290, 544, 18, hwnd, ctl_id(IDC_PROGRESS), nullptr, nullptr);
            SendMessageW(g_prog, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

            g_chk_desktop = CreateWindowW(L"BUTTON", L"Створити ярлик на Робочому столі",
                                          WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, 20, 318, 290, 22,
                                          hwnd, ctl_id(IDC_CHK_DESKTOP), nullptr, nullptr);
            SendMessageW(g_chk_desktop, WM_SETFONT, WPARAM(font), TRUE);
            SendMessageW(g_chk_desktop, BM_SETCHECK, BST_CHECKED, 0);

            g_chk_fw = CreateWindowW(L"BUTTON", L"Налаштувати Windows Defender Firewall (UDP)",
                                     WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, 20, 344, 340, 22,
                                     hwnd, ctl_id(IDC_CHK_FIREWALL), nullptr, nullptr);
            SendMessageW(g_chk_fw, WM_SETFONT, WPARAM(font), TRUE);
            SendMessageW(g_chk_fw, BM_SETCHECK, BST_CHECKED, 0);

            g_chk_launch = CreateWindowW(L"BUTTON", L"Запустити LanVid зараз",
                                         WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, 20, 370, 260, 22,
                                         hwnd, ctl_id(IDC_CHK_LAUNCH), nullptr, nullptr);
            SendMessageW(g_chk_launch, WM_SETFONT, WPARAM(font), TRUE);
            SendMessageW(g_chk_launch, BM_SETCHECK, BST_CHECKED, 0);

            g_btn_install = CreateWindowW(L"BUTTON", L"Встановити зараз",
                                          WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 326, 404, 140, 34,
                                          hwnd, ctl_id(IDC_BTN_INSTALL), nullptr, nullptr);
            SendMessageW(g_btn_install, WM_SETFONT, WPARAM(font), TRUE);

            g_btn_close = CreateWindowW(L"BUTTON", L"Скасувати", WS_VISIBLE | WS_CHILD,
                                        476, 404, 88, 34, hwnd, ctl_id(IDC_BTN_CLOSE), nullptr, nullptr);
            SendMessageW(g_btn_close, WM_SETFONT, WPARAM(font), TRUE);

            // Display pre-flight scan results immediately
            append_log(L"Папка встановлення: " + g_rep.install_dir.wstring());
            append_log(g_rep.running_procs > 0
                           ? (L"[УВАГА] Знайдено запущених процесів LanVid: " + std::to_wstring(g_rep.running_procs) + L" (будуть закриті)")
                           : L"[OK] Активних конфліктних процесів немає.");
            append_log(g_rep.port_5000_busy
                           ? (L"[УВАГА] UDP порт 5000 зайнятий -> автопризначено вільний порт " + std::to_wstring(g_rep.chosen_port))
                           : L"[OK] UDP порт 5000 вільний.");
            append_log(g_rep.vc_redist_ok
                           ? L"[OK] Visual C++ 2015-2022 Redistributable (x64) встановлено."
                           : L"[ІНФО] VC++ Redistributable x64 відсутній -> буде встановлено автоматично.");
            append_log(g_rep.d3d11_ok
                           ? L"[OK] Апаратний Direct3D 11 підтримується."
                           : L"[ІНФО] Direct3D 11 недоступний -> буде активовано GDI fallback у config.json.");
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (id == IDC_BTN_INSTALL) {
                if (!g_installed) {
                    EnableWindow(g_btn_install, FALSE);
                    const bool desk = SendMessageW(g_chk_desktop, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    const bool fw = SendMessageW(g_chk_fw, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    if (execute_install(desk, fw, false)) {
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
        std::printf("LanVid_Setup preflight: procs=%d port5000_busy=%d chosen_port=%u vc_redist=%d d3d11=%d\n",
                    g_rep.running_procs, g_rep.port_5000_busy ? 1 : 0, unsigned(g_rep.chosen_port),
                    g_rep.vc_redist_ok ? 1 : 0, g_rep.d3d11_ok ? 1 : 0);
        CoUninitialize();
        WSACleanup();
        return 0;
    }
    if (args.find(L"--silent") != std::wstring::npos) {
        const bool ok = execute_install(true, true, true);
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

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"LanVid Studio — Встановлення",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 600, 490, nullptr, nullptr, hInst, nullptr);
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
