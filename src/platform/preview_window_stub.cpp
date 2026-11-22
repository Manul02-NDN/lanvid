// Headless preview stub for UNIX systems built without SDL2.

#include "platform/preview_window.hpp"

namespace lv {

namespace {

class StubPreviewWindow final : public IPreviewWindow {
public:
    bool open(int, int, const std::string&, PreviewCallbacks) override { return false; }
    void close() override {}
    bool is_open() const override { return false; }
    void present(const Image&) override {}
    void set_title(const std::string&) override {}
    const char* renderer_name() const override { return "none"; }
};

}  // namespace

std::unique_ptr<IPreviewWindow> create_preview_window(bool) {
    return std::make_unique<StubPreviewWindow>();
}

}  // namespace lv
