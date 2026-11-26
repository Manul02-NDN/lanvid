// macOS Cocoa + Metal native GPU window host for LanVid apps.
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>

#include "common_ui/app_host.hpp"
#include "common_ui/theme.hpp"

#include <filesystem>

#include "common/clock.hpp"
#include "imgui.h"
#include "imgui_impl_metal.h"
#include "imgui_impl_osx.h"

namespace {

struct MetalHostState {
    NSWindow* window = nil;
    MTKView* view = nil;
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> cmd_queue = nil;
    bool should_close = false;
    bool fullscreen = false;
};

class MetalDynamicTexture final : public lv::ui::IGpuTexture {
public:
    explicit MetalDynamicTexture(id<MTLDevice> dev) : device_(dev) {}
    ~MetalDynamicTexture() override { release(); }

    bool update(const uint8_t* pixels, int width, int height) override {
        if (!device_ || !pixels || width <= 0 || height <= 0) return false;
        if (!tex_ || w_ != width || h_ != height) {
            release();
            MTLTextureDescriptor* desc =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                   width:NSUInteger(width)
                                                                  height:NSUInteger(height)
                                                               mipmapped:NO];
            desc.usage = MTLTextureUsageShaderRead;
#if TARGET_CPU_ARM64
            desc.storageMode = MTLStorageModeShared;
#else
            desc.storageMode = MTLStorageModeManaged;
#endif
            tex_ = [device_ newTextureWithDescriptor:desc];
            if (!tex_) return false;
            w_ = width;
            h_ = height;
        }
        const MTLRegion region = MTLRegionMake2D(0, 0, NSUInteger(width), NSUInteger(height));
        [tex_ replaceRegion:region mipmapLevel:0 withBytes:pixels bytesPerRow:NSUInteger(width) * 4];
        return true;
    }

    ImTextureID imgui_id() const override {
        return tex_ ? static_cast<ImTextureID>(reinterpret_cast<uintptr_t>((__bridge void*)tex_)) : ImTextureID(0);
    }
    int width() const override { return w_; }
    int height() const override { return h_; }
    bool wants_rgba() const override { return false; }

private:
    void release() {
        if (tex_) {
            [tex_ release];
            tex_ = nil;
        }
        w_ = h_ = 0;
    }

    id<MTLDevice> device_ = nil;
    id<MTLTexture> tex_ = nil;
    int w_ = 0, h_ = 0;
};

bool host_is_fs(void* ctx) { return static_cast<MetalHostState*>(ctx)->fullscreen; }

void host_set_fs(void* ctx, bool fs) {
    auto* st = static_cast<MetalHostState*>(ctx);
    if (!st->window || st->fullscreen == fs) return;
    [st->window toggleFullScreen:nil];
    st->fullscreen = fs;
}

lv::ui::IGpuTexture* host_create_tex(void* ctx) {
    return new MetalDynamicTexture(static_cast<MetalHostState*>(ctx)->device);
}

void host_destroy_tex(void*, lv::ui::IGpuTexture* tex) { delete tex; }

void load_fonts(ImGuiIO& io) {
    static ImVector<ImWchar> ranges;
    if (ranges.empty()) {
        static const ImWchar extra[] = {0x2010, 0x2027, 0x2030, 0x203A, 0x2190, 0x21FF, 0x2212, 0x2212,
                                        0x2500, 0x25FF, 0x2600, 0x26FF, 0x2713, 0x2717, 0};
        ImFontGlyphRangesBuilder b;
        b.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
        b.AddRanges(extra);
        b.BuildRanges(&ranges);
    }
    const char* candidates[] = {
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
    };
    for (const char* p : candidates) {
        if (std::filesystem::exists(p)) {
            io.Fonts->AddFontFromFileTTF(p, 16.5f, nullptr, ranges.Data);
            return;
        }
    }
    io.Fonts->AddFontDefault();
}

}  // namespace

@interface LVWindowDelegate : NSObject <NSWindowDelegate>
@property (nonatomic, assign) MetalHostState* state;
@end

@implementation LVWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    if (self.state) self.state->should_close = true;
    return YES;
}
- (void)windowWillClose:(NSNotification*)notification {
    (void)notification;
    if (self.state) self.state->should_close = true;
}
@end

namespace lv::ui {

int run_desktop_app(const WindowSpec& spec, IDesktopApp& app) {
    @autoreleasepool {
        NSApplication* nsapp = [NSApplication sharedApplication];
        [nsapp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [nsapp finishLaunching];

        MetalHostState host{};
        host.device = MTLCreateSystemDefaultDevice();
        if (!host.device) {
            return 1;
        }
        host.cmd_queue = [host.device newCommandQueue];

        const NSRect rect = NSMakeRect(0, 0, CGFloat(spec.width), CGFloat(spec.height));
        const NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
        host.window = [[NSWindow alloc] initWithContentRect:rect
                                                  styleMask:style
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO];
        [host.window setTitle:[NSString stringWithUTF8String:(spec.window_title ? spec.window_title : "LanVid")]];
        [host.window center];

        LVWindowDelegate* del = [[LVWindowDelegate alloc] init];
        del.state = &host;
        [host.window setDelegate:del];

        host.view = [[MTKView alloc] initWithFrame:rect device:host.device];
        host.view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
        host.view.clearColor = MTLClearColorMake(0.929, 0.914, 0.996, 1.0);
        host.view.paused = YES;
        host.view.enableSetNeedsDisplay = NO;
        [host.window setContentView:host.view];
        [host.window makeKeyAndOrderFront:nil];
        if (spec.smoke_test_ms <= 0) {
            [nsapp activateIgnoringOtherApps:YES];
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr;
        load_fonts(io);
        apply_theme(false);

        ImGui_ImplOSX_Init(host.view);
        ImGui_ImplMetal_Init(host.device);

        PlatformHost ph{};
        ph.renderer_name = "Cocoa / Metal";
        ph.is_fullscreen = host_is_fs;
        ph.set_fullscreen = host_set_fs;
        ph.create_texture = host_create_tex;
        ph.destroy_texture = host_destroy_tex;
        ph.ctx = &host;
        app.set_platform_host(ph);
        app.init();

        const uint64_t t0 = lv::now_us();
        while (!host.should_close) {
            @autoreleasepool {
                NSEvent* ev = nil;
                while ((ev = [nsapp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:[NSDate distantPast]
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES]) != nil) {
                    [nsapp sendEvent:ev];
                }
                if (spec.smoke_test_ms > 0 &&
                    lv::now_us() - t0 >= uint64_t(spec.smoke_test_ms) * 1000ull) {
                    break;
                }

                const NSSize pt_sz = host.view.bounds.size;
                const CGFloat scale = host.window.backingScaleFactor > 0.0 ? host.window.backingScaleFactor : 1.0;
                host.view.drawableSize = CGSizeMake(pt_sz.width * scale, pt_sz.height * scale);
                io.DisplaySize = ImVec2(float(pt_sz.width), float(pt_sz.height));
                io.DisplayFramebufferScale = ImVec2(float(scale), float(scale));

                id<CAMetalDrawable> drawable = [(CAMetalLayer*)host.view.layer nextDrawable];
                MTLRenderPassDescriptor* rpd = host.view.currentRenderPassDescriptor;
                if (drawable && rpd) {
                    const auto& pal = theme_palette();
                    rpd.colorAttachments[0].clearColor =
                        MTLClearColorMake(pal.bg_bot_right.x, pal.bg_bot_right.y, pal.bg_bot_right.z, 1.0);
                    rpd.colorAttachments[0].loadAction = MTLLoadActionClear;

                    id<MTLCommandBuffer> cmd = [host.cmd_queue commandBuffer];
                    ImGui_ImplMetal_NewFrame(rpd);
                    ImGui_ImplOSX_NewFrame(host.view);
                    ImGui::NewFrame();

                    app.render_frame();

                    ImGui::Render();
                    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rpd];
                    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cmd, enc);
                    [enc endEncoding];
                    [cmd presentDrawable:drawable];
                    [cmd commit];
                } else {
                    ImGui_ImplOSX_NewFrame(host.view);
                    ImGui::NewFrame();
                    app.render_frame();
                    ImGui::EndFrame();
                    [NSThread sleepForTimeInterval:0.016];
                }
            }
        }

        app.shutdown();
        ImGui_ImplMetal_Shutdown();
        ImGui_ImplOSX_Shutdown();
        ImGui::DestroyContext();

        [host.view release];
        [host.window setDelegate:nil];
        [del release];
        [host.window release];
        [host.cmd_queue release];
        [host.device release];
    }
    return 0;
}

}  // namespace lv::ui
