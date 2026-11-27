#include "midi/virtual_midi.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace cl {
namespace {

uint64_t now_us() {
    using namespace std::chrono;
    return uint64_t(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

std::string wide_to_utf8(const wchar_t* ws) {
    if (!ws || !*ws) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring out(size_t(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

std::string to_lower_ascii(std::string s) {
    for (char& c : s) {
        c = char(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool contains_ci(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return false;
    return to_lower_ascii(haystack).find(to_lower_ascii(needle)) != std::string::npos;
}

// Finds WinMM MIDI input device index by exact name (or optional case-insensitive substring).
int find_winmm_in(const std::string& name, bool exact = true) {
    const UINT n = midiInGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        MIDIINCAPSW caps{};
        if (midiInGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
        const std::string dev = wide_to_utf8(caps.szPname);
        if (exact ? (dev == name) : contains_ci(dev, name)) return int(i);
    }
    return -1;
}

// Finds WinMM MIDI output device index by exact name (or optional case-insensitive substring).
int find_winmm_out(const std::string& name, bool exact = true) {
    const UINT n = midiOutGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        MIDIOUTCAPSW caps{};
        if (midiOutGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
        const std::string dev = wide_to_utf8(caps.szPname);
        if (exact ? (dev == name) : contains_ci(dev, name)) return int(i);
    }
    return -1;
}

// Locates Windows MIDI Services CLI (`midi.exe`) via registry or default install path.
std::wstring find_windows_midi_cli() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows MIDI Services\\Desktop App SDK Runtime",
                      0, KEY_READ | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
        wchar_t buf[MAX_PATH] = {};
        DWORD sz = sizeof(buf);
        DWORD type = 0;
        const LONG r = RegQueryValueExW(key, L"MidiConsole", nullptr, &type,
                                        reinterpret_cast<LPBYTE>(buf), &sz);
        RegCloseKey(key);
        if (r == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && buf[0] != L'\0') {
            if (GetFileAttributesW(buf) != INVALID_FILE_ATTRIBUTES) {
                return std::wstring(buf);
            }
        }
    }
    const wchar_t* fallback = L"C:\\Program Files\\Windows MIDI Services\\Tools\\midi.exe";
    if (GetFileAttributesW(fallback) != INVALID_FILE_ATTRIBUTES) {
        return std::wstring(fallback);
    }
    return {};
}

// Executes a command silently (CREATE_NO_WINDOW) and captures stdout/stderr.
bool run_hidden_process(const std::wstring& cmdline, std::string* output, DWORD timeout_ms = 6000) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) return false;
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;

    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = cmdline;
    const BOOL ok = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(write_pipe);
    if (!ok) {
        CloseHandle(read_pipe);
        return false;
    }

    std::string captured;
    char buf[512];
    DWORD read_bytes = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            if (ReadFile(read_pipe, buf, sizeof(buf), &read_bytes, nullptr) && read_bytes > 0) {
                captured.append(buf, buf + read_bytes);
            }
        } else {
            const DWORD wait = WaitForSingleObject(pi.hProcess, 20);
            if (wait == WAIT_OBJECT_0) {
                while (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                    if (!ReadFile(read_pipe, buf, sizeof(buf), &read_bytes, nullptr) || read_bytes == 0) break;
                    captured.append(buf, buf + read_bytes);
                }
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                TerminateProcess(pi.hProcess, 1);
                break;
            }
        }
    }

    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(read_pipe);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (output) *output = std::move(captured);
    return exit_code == 0;
}

std::string sanitize_cli_arg(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        if (c < 0x20 || c == '"' || c == '\\' || c == '%' || c == '&' || c == '|' || c == '^' ||
            c == '<' || c == '>') {
            continue;
        }
        out.push_back(char(c));
    }
    return out.empty() ? std::string("CrossLink MIDI") : out.substr(0, 64);
}

// Extracts `{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}` from `midi loopback create` output.
std::string parse_association_guid(const std::string& text) {
    const size_t pos = text.find("Association Id");
    const size_t start_search = (pos != std::string::npos) ? pos : 0;
    const size_t lb = text.find('{', start_search);
    if (lb == std::string::npos) return {};
    const size_t rb = text.find('}', lb);
    if (rb == std::string::npos || rb - lb < 32 || rb - lb > 48) return {};
    std::string guid = text.substr(lb, rb - lb + 1);
    for (size_t i = 1; i + 1 < guid.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(guid[i]);
        if (!std::isxdigit(c) && c != '-') return {};
    }
    return guid;
}

// Dynamic teVirtualMIDI64.dll ABI (Tobias Erichsen's virtual MIDI driver, optional fallback).
struct _VM_MIDI_PORT;
using LPVM_MIDI_PORT = _VM_MIDI_PORT*;
using LPVM_MIDI_DATA_CB = void(CALLBACK*)(LPVM_MIDI_PORT midiPort, LPBYTE midiDataBytes,
                                          DWORD length, DWORD_PTR dwCallbackInstance);
using FnVmCreatePortEx2 = LPVM_MIDI_PORT(CALLBACK*)(LPCWSTR portName, LPVM_MIDI_DATA_CB callback,
                                                    DWORD_PTR dwCallbackInstance,
                                                    DWORD maxSysexLength, DWORD flags);
using FnVmClosePort = void(CALLBACK*)(LPVM_MIDI_PORT midiPort);
using FnVmSendData = BOOL(CALLBACK*)(LPVM_MIDI_PORT midiPort, LPBYTE midiDataBytes, DWORD length);

constexpr DWORD kTeVmFlagParseRx = 1;
constexpr DWORD kTeVmFlagParseTx = 2;

}  // namespace

struct VirtualMidiPort::Impl {
    mutable std::mutex mu;
    std::mutex cb_mu;
    std::atomic<bool> alive{false};
    VirtualMidiStatus st{};
    Callback cb;

    // Backend 1 & 3: WinMM handles (connected to internal endpoint of Windows MIDI Services
    // loopback pair, or directly to an existing WinMM loopback port).
    HMIDIIN h_in = nullptr;
    HMIDIOUT h_out = nullptr;
    std::wstring midi_cli_path;
    std::string association_id;  // Non-empty only when we created the loopback pair ourselves.

    // Backend 2: teVirtualMIDI64.dll
    HMODULE tevm_dll = nullptr;
    LPVM_MIDI_PORT tevm_port = nullptr;
    FnVmClosePort tevm_close = nullptr;
    FnVmSendData tevm_send = nullptr;

    static void CALLBACK winmm_in_proc(HMIDIIN, UINT wMsg, DWORD_PTR dwInstance,
                                       DWORD_PTR dwParam1, DWORD_PTR) {
        if (wMsg != MIM_DATA) return;
        auto* self = reinterpret_cast<Impl*>(dwInstance);
        if (!self || !self->alive.load(std::memory_order_acquire)) return;
        Callback fn;
        {
            std::lock_guard<std::mutex> clk(self->cb_mu);
            if (!self->alive.load(std::memory_order_relaxed) || !self->cb) return;
            fn = self->cb;
        }
        const uint8_t status = uint8_t(dwParam1 & 0xFFu);
        const uint8_t d1 = uint8_t((dwParam1 >> 8) & 0xFFu);
        const uint8_t d2 = uint8_t((dwParam1 >> 16) & 0xFFu);
        fn(status, d1, d2, now_us());
    }

    static void CALLBACK tevm_rx_proc(LPVM_MIDI_PORT, LPBYTE data, DWORD len, DWORD_PTR instance) {
        auto* self = reinterpret_cast<Impl*>(instance);
        if (!self || !data || len == 0 || !self->alive.load(std::memory_order_acquire)) return;
        if (data[0] == 0xF0u) return;  // SysEx is not a 3-byte short message
        Callback fn;
        {
            std::lock_guard<std::mutex> clk(self->cb_mu);
            if (!self->alive.load(std::memory_order_relaxed) || !self->cb) return;
            fn = self->cb;
        }
        const uint64_t ts = now_us();
        const uint8_t status = data[0];
        const uint8_t d1 = len >= 2 ? data[1] : 0;
        const uint8_t d2 = len >= 3 ? data[2] : 0;
        fn(status, d1, d2, ts);
    }

    bool open_winmm_pair(int in_idx, int out_idx) {
        if (in_idx >= 0) {
            if (midiInOpen(&h_in, UINT(in_idx), reinterpret_cast<DWORD_PTR>(&Impl::winmm_in_proc),
                           reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION) == MMSYSERR_NOERROR) {
                midiInStart(h_in);
            } else {
                h_in = nullptr;
            }
        }
        if (out_idx >= 0) {
            if (midiOutOpen(&h_out, UINT(out_idx), 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
                h_out = nullptr;
            }
        }
        if (!h_in && !h_out) return false;
        return true;
    }

    bool try_windows_midi_services(const std::string& daw_name) {
        const std::string internal_name = daw_name + " Internal";

        // 1) Reuse existing loopback pair if already present (e.g., saved in config).
        int in_idx = find_winmm_in(internal_name, true);
        int out_idx = find_winmm_out(internal_name, true);
        int daw_in_idx = find_winmm_in(daw_name, true);
        int daw_out_idx = find_winmm_out(daw_name, true);

        if (in_idx >= 0 && out_idx >= 0 && daw_in_idx >= 0 && daw_out_idx >= 0) {
            if (open_winmm_pair(in_idx, out_idx)) {
                st.open = true;
                st.backend = VirtualMidiBackend::kWindowsMidiServices;
                st.daw_port_name = daw_name;
                st.internal_port_name = internal_name;
                st.backend_name = "Windows MIDI Services Loopback";
                st.status_text = daw_name + " (Активний — оберіть у REAPER / DAW -> MIDI Devices)";
                return true;
            }
        }

        // 2) Create a runtime loopback pair via Windows MIDI Services CLI (`midi.exe`).
        midi_cli_path = find_windows_midi_cli();
        if (midi_cli_path.empty()) return false;

        const std::string safe_daw = sanitize_cli_arg(daw_name);
        const std::string safe_internal = sanitize_cli_arg(internal_name);
        const std::wstring cmd = L"\"" + midi_cli_path + L"\" loopback create --name-a \"" +
                                 utf8_to_wide(safe_internal) + L"\" --name-b \"" +
                                 utf8_to_wide(safe_daw) + L"\"";
        std::string cli_out;
        if (!run_hidden_process(cmd, &cli_out, 6000)) {
            return false;
        }
        association_id = parse_association_guid(cli_out);

        // Wait up to 2.5 seconds for WinMM enumeration to pick up the new endpoints.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
        while (std::chrono::steady_clock::now() < deadline) {
            in_idx = find_winmm_in(internal_name, true);
            out_idx = find_winmm_out(internal_name, true);
            daw_in_idx = find_winmm_in(daw_name, true);
            daw_out_idx = find_winmm_out(daw_name, true);
            if (in_idx >= 0 && out_idx >= 0 && daw_in_idx >= 0 && daw_out_idx >= 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        if (in_idx < 0 || out_idx < 0) {
            cleanup_loopback_endpoint();
            return false;
        }
        if (!open_winmm_pair(in_idx, out_idx)) {
            cleanup_loopback_endpoint();
            return false;
        }

        st.open = true;
        st.backend = VirtualMidiBackend::kWindowsMidiServices;
        st.daw_port_name = daw_name;
        st.internal_port_name = internal_name;
        st.backend_name = "Windows MIDI Services Loopback";
        st.status_text = daw_name + " (Активний — оберіть у REAPER / DAW -> MIDI Devices)";
        return true;
    }

    bool try_te_virtual_midi(const std::string& daw_name) {
        tevm_dll = LoadLibraryExW(L"teVirtualMIDI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!tevm_dll) tevm_dll = LoadLibraryExW(L"teVirtualMIDI.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!tevm_dll) return false;

        auto create_fn = reinterpret_cast<FnVmCreatePortEx2>(
            GetProcAddress(tevm_dll, "virtualMIDICreatePortEx2"));
        tevm_close = reinterpret_cast<FnVmClosePort>(
            GetProcAddress(tevm_dll, "virtualMIDIClosePort"));
        tevm_send = reinterpret_cast<FnVmSendData>(
            GetProcAddress(tevm_dll, "virtualMIDISendData"));
        if (!create_fn || !tevm_close || !tevm_send) {
            FreeLibrary(tevm_dll);
            tevm_dll = nullptr;
            tevm_close = nullptr;
            tevm_send = nullptr;
            return false;
        }

        const std::wstring wname = utf8_to_wide(daw_name);
        tevm_port = create_fn(wname.c_str(), &Impl::tevm_rx_proc, reinterpret_cast<DWORD_PTR>(this),
                              65535, kTeVmFlagParseRx | kTeVmFlagParseTx);
        if (!tevm_port) {
            FreeLibrary(tevm_dll);
            tevm_dll = nullptr;
            tevm_close = nullptr;
            tevm_send = nullptr;
            return false;
        }

        st.open = true;
        st.backend = VirtualMidiBackend::kTeVirtualMidi;
        st.daw_port_name = daw_name;
        st.internal_port_name = daw_name;
        st.backend_name = "teVirtualMIDI";
        st.status_text = daw_name + " (teVirtualMIDI — оберіть у REAPER / DAW -> MIDI Devices)";
        return true;
    }

    bool try_existing_winmm_loopback(const std::string& daw_name) {
        // Fallback: detect any user-installed virtual loopback port ("CrossLink MIDI" or "loopMIDI").
        const char* candidates[] = {daw_name.c_str(), "loopMIDI"};
        for (const char* pat : candidates) {
            const int in_idx = find_winmm_in(pat, false);
            const int out_idx = find_winmm_out(pat, false);
            if (in_idx >= 0 || out_idx >= 0) {
                if (open_winmm_pair(in_idx, out_idx)) {
                    std::string found_name = pat;
                    if (out_idx >= 0) {
                        MIDIOUTCAPSW caps{};
                        if (midiOutGetDevCapsW(UINT(out_idx), &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
                            found_name = wide_to_utf8(caps.szPname);
                        }
                    }
                    st.open = true;
                    st.backend = VirtualMidiBackend::kWinMmLoopback;
                    st.daw_port_name = found_name;
                    st.internal_port_name = found_name;
                    st.backend_name = "WinMM Loopback";
                    st.status_text = found_name + " (Активний — оберіть у REAPER / DAW -> MIDI Devices)";
                    return true;
                }
            }
        }
        return false;
    }

    void cleanup_loopback_endpoint() {
        if (!association_id.empty() && !midi_cli_path.empty()) {
            const std::wstring cmd = L"\"" + midi_cli_path +
                                     L"\" loopback remove --association-id \"" +
                                     utf8_to_wide(association_id) + L"\"";
            run_hidden_process(cmd, nullptr, 4000);
        }
        association_id.clear();
    }

    void close_locked() {
        {
            std::lock_guard<std::mutex> clk(cb_mu);
            alive.store(false, std::memory_order_release);
            cb = nullptr;
        }
        if (h_in) {
            midiInStop(h_in);
            midiInReset(h_in);
            midiInClose(h_in);
            h_in = nullptr;
        }
        if (h_out) {
            midiOutReset(h_out);
            midiOutClose(h_out);
            h_out = nullptr;
        }
        if (tevm_port && tevm_close) {
            tevm_close(tevm_port);
            tevm_port = nullptr;
        }
        if (tevm_dll) {
            FreeLibrary(tevm_dll);
            tevm_dll = nullptr;
            tevm_close = nullptr;
            tevm_send = nullptr;
        }
        cleanup_loopback_endpoint();
        st = VirtualMidiStatus{};
    }
};

VirtualMidiPort::VirtualMidiPort() : impl_(std::make_unique<Impl>()) {}
VirtualMidiPort::~VirtualMidiPort() { close(); }

bool VirtualMidiPort::open(const std::string& daw_port_name, Callback on_from_daw) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->close_locked();
    {
        std::lock_guard<std::mutex> clk(impl_->cb_mu);
        impl_->cb = std::move(on_from_daw);
        impl_->alive.store(true, std::memory_order_release);
    }
    const std::string name = sanitize_cli_arg(daw_port_name.empty() ? "CrossLink MIDI" : daw_port_name);

    if (impl_->try_windows_midi_services(name)) return true;
    if (impl_->try_te_virtual_midi(name)) return true;
    if (impl_->try_existing_winmm_loopback(name)) return true;

    {
        std::lock_guard<std::mutex> clk(impl_->cb_mu);
        impl_->alive.store(false, std::memory_order_release);
        impl_->cb = nullptr;
    }
    impl_->st.open = false;
    impl_->st.backend = VirtualMidiBackend::kNone;
    impl_->st.daw_port_name = name;
    impl_->st.backend_name = "Unavailable";
    impl_->st.status_text =
        "Недоступно (оновіть Windows MIDI Services або встановіть loopMIDI для віртуального порту DAW)";
    return false;
}

void VirtualMidiPort::close() {
    if (!impl_) return;
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->close_locked();
}

bool VirtualMidiPort::is_open() const {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->st.open;
}

bool VirtualMidiPort::send_to_daw(uint8_t status, uint8_t d1, uint8_t d2) {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->st.open) return false;
    if (impl_->h_out) {
        const DWORD msg = DWORD(status) | (DWORD(d1) << 8) | (DWORD(d2) << 16);
        return midiOutShortMsg(impl_->h_out, msg) == MMSYSERR_NOERROR;
    }
    if (impl_->tevm_port && impl_->tevm_send) {
        BYTE msg[3] = {status, d1, d2};
        DWORD len = 3u;
        if (status >= 0xF8u || status == 0xF6u) len = 1u;
        else if ((status & 0xF0u) == 0xC0u || (status & 0xF0u) == 0xD0u || status == 0xF1u || status == 0xF3u) len = 2u;
        return impl_->tevm_send(impl_->tevm_port, msg, len) != FALSE;
    }
    return false;
}

void VirtualMidiPort::send_panic_to_daw() {
    for (uint8_t ch = 0; ch < 16; ++ch) {
        send_to_daw(uint8_t(0xB0u | ch), 123, 0);  // CC 123: All Notes Off
        send_to_daw(uint8_t(0xB0u | ch), 120, 0);  // CC 120: All Sound Off
    }
}

VirtualMidiStatus VirtualMidiPort::status() const {
    if (!impl_) return {};
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->st;
}

}  // namespace cl
