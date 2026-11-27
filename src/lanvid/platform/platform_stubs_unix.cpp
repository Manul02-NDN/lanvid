// Clean UNIX stubs for Windows-specific capture/audio backends so Linux/macOS builds cleanly
// with --source pattern, --source file (raw/PPM/Y4M), and optional SDL2 preview.

#include "platform/audio_capture.hpp"
#include "platform/audio_playback.hpp"
#include "platform/camera_capture.hpp"
#include "platform/screen_capture.hpp"

namespace lv {

struct ScreenCapture::Impl {};
std::vector<MonitorInfo> list_monitors() { return {}; }
ScreenCapture::ScreenCapture() = default;
ScreenCapture::~ScreenCapture() = default;
bool ScreenCapture::open(int, CaptureBackend) { return false; }
void ScreenCapture::close() {}
int ScreenCapture::native_width() const { return 0; }
int ScreenCapture::native_height() const { return 0; }
const char* ScreenCapture::active_backend_name() const { return "none"; }
bool ScreenCapture::capture(Image&, int) { return false; }

struct CameraCapture::Impl {};
std::vector<CameraInfo> list_cameras() { return {}; }
CameraCapture::CameraCapture() = default;
CameraCapture::~CameraCapture() = default;
bool CameraCapture::open(int, int, int, int) { return false; }
void CameraCapture::close() {}
int CameraCapture::native_width() const { return 0; }
int CameraCapture::native_height() const { return 0; }
std::string CameraCapture::device_name() const { return {}; }
bool CameraCapture::grab(Image&, int) { return false; }

struct AudioCapture::Impl {};
std::vector<AudioDeviceInfo> list_audio_devices(bool) { return {}; }
AudioCapture::AudioCapture() = default;
AudioCapture::~AudioCapture() = default;
bool AudioCapture::start(AudioCaptureMode, int, uint16_t, ChunkCallback) { return false; }
void AudioCapture::stop() {}
bool AudioCapture::is_running() const { return false; }
std::string AudioCapture::device_name() const { return {}; }

struct AudioPlayback::Impl {};
AudioPlayback::AudioPlayback() = default;
AudioPlayback::~AudioPlayback() = default;
bool AudioPlayback::start(AudioJitterBuffer*) { return false; }
void AudioPlayback::stop() {}
bool AudioPlayback::is_running() const { return false; }
std::string AudioPlayback::device_name() const { return {}; }

}  // namespace lv
