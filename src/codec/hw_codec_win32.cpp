#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <codecapi.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_3.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "codec/video_codec_common.hpp"
#include "common/cpu_features.hpp"

using Microsoft::WRL::ComPtr;

namespace lv {

namespace {

// Ref-counted Media Foundation + COM initializer safe across threads.
class MfSessionGuard {
public:
    MfSessionGuard() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com_inited_ = SUCCEEDED(hr);
        hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        mf_inited_ = SUCCEEDED(hr);
    }
    ~MfSessionGuard() {
        if (mf_inited_) MFShutdown();
        if (com_inited_) CoUninitialize();
    }
    bool ok() const { return mf_inited_; }

private:
    bool com_inited_ = false;
    bool mf_inited_ = false;
};

std::string wide_to_utf8(const wchar_t* ws) {
    if (!ws || !*ws) return {};
    int need = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return {};
    std::string out(size_t(need - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, out.data(), need, nullptr, nullptr);
    return out;
}

GUID codec_subtype(VideoCodecId codec) {
    return (codec == VideoCodecId::kH265) ? MFVideoFormat_HEVC : MFVideoFormat_H264;
}

std::string activate_friendly_name(IMFActivate* act) {
    if (!act) return "MF MFT";
    wchar_t* name = nullptr;
    UINT32 len = 0;
    if (SUCCEEDED(act->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &len)) && name) {
        std::string s = wide_to_utf8(name);
        CoTaskMemFree(name);
        return s;
    }
    return "MF MFT";
}

struct MftCandidate {
    ComPtr<IMFActivate> activate;
    std::string name;
    bool is_hardware = false;
    bool is_async = false;
    bool has_luid = false;
    LUID adapter_luid{};
};

std::vector<MftCandidate> enumerate_mfts(GUID category, const MFT_REGISTER_TYPE_INFO* in_type,
                                         const MFT_REGISTER_TYPE_INFO* out_type,
                                         bool prefer_hardware) {
    std::vector<MftCandidate> result;

    auto collect = [&](UINT32 flags, bool mark_hw) {
        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        HRESULT hr = MFTEnumEx(category, flags, in_type, out_type, &activates, &count);
        if (FAILED(hr) || !activates) return;
        for (UINT32 i = 0; i < count; ++i) {
            if (!activates[i]) continue;
            MftCandidate c;
            c.activate.Attach(activates[i]);
            c.name = activate_friendly_name(c.activate.Get());
            UINT32 tflags = 0;
            c.activate->GetUINT32(MF_TRANSFORM_FLAGS_Attribute, &tflags);
            c.is_hardware = mark_hw || ((tflags & MFT_ENUM_FLAG_HARDWARE) != 0);
            c.is_async = (tflags & MFT_ENUM_FLAG_ASYNCMFT) != 0;

            UINT32 blob_sz = 0;
            if (SUCCEEDED(c.activate->GetBlob(MFT_ENUM_ADAPTER_LUID,
                                              reinterpret_cast<UINT8*>(&c.adapter_luid),
                                              sizeof(LUID), &blob_sz)) &&
                blob_sz == sizeof(LUID)) {
                c.has_luid = true;
            }

            bool dup = false;
            for (const auto& ex : result) {
                if (ex.name == c.name && ex.is_hardware == c.is_hardware) {
                    dup = true;
                    break;
                }
            }
            if (!dup) result.push_back(std::move(c));
        }
        CoTaskMemFree(activates);
    };

    if (prefer_hardware) {
        collect(MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, true);
    }
    collect(MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_LOCALMFT |
                MFT_ENUM_FLAG_TRANSCODE_ONLY | MFT_ENUM_FLAG_SORTANDFILTER,
            false);
    if (!prefer_hardware) {
        collect(MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, true);
    }
    return result;
}

ComPtr<IDXGIAdapter1> find_adapter_by_luid(const LUID& luid) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) return nullptr;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapters1(i, &adapter)) || !adapter) break;  // any error, not only NOT_FOUND
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.AdapterLuid.LowPart == luid.LowPart &&
            desc.AdapterLuid.HighPart == luid.HighPart) {
            return adapter;
        }
    }
    return nullptr;
}

// Picks the highest-capability hardware DXGI adapter with video support.
// Respects explicit MFT_ENUM_ADAPTER_LUID when provided; otherwise ranks hardware adapters by
// D3D feature level (e.g. DX12_1 VCN3+/QSV/NVENC over older DX12_0 cards) and Unified Memory (UMA)
// for minimum CPU<->GPU texture transfer latency.
ComPtr<IDXGIAdapter1> select_best_video_adapter(const LUID* preferred_luid) {
    if (preferred_luid) {
        if (auto exact = find_adapter_by_luid(*preferred_luid)) return exact;
    }

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) return nullptr;

    const D3D_FEATURE_LEVEL probe_lvls[] = {
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    ComPtr<IDXGIAdapter1> best;
    uint64_t best_score = 0;

    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> ad;
        if (FAILED(factory->EnumAdapters1(i, &ad)) || !ad) break;  // any error, not only NOT_FOUND
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(ad->GetDesc1(&desc))) continue;
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

        ComPtr<ID3D11Device> probe_dev;
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        HRESULT hr = D3D11CreateDevice(
            ad.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, probe_lvls, 4,
            D3D11_SDK_VERSION, &probe_dev, &fl, nullptr);
        if (FAILED(hr) || !probe_dev) continue;

        ComPtr<ID3D11VideoDevice> vdev;
        if (FAILED(probe_dev.As(&vdev)) || !vdev) continue;

        D3D11_FEATURE_DATA_D3D11_OPTIONS2 opt2{};
        probe_dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &opt2, sizeof(opt2));

        // Higher feature level dominates (newer hardware video engine); UMA breaks ties for low-copy CPU<->GPU.
        const uint64_t score = (uint64_t(fl) << 8) | (opt2.UnifiedMemoryArchitecture ? 16ull : 0ull);
        if (!best || score > best_score) {
            best = ad;
            best_score = score;
        }
    }
    return best;
}

bool create_d3d11_manager(const LUID* preferred_luid, ComPtr<ID3D11Device>& dev,
                          ComPtr<ID3D11DeviceContext>& ctx, ComPtr<IMFDXGIDeviceManager>& mgr) {
    UINT reset_token = 0;
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL fl{};
    const UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    ComPtr<IDXGIAdapter1> adapter = select_best_video_adapter(preferred_luid);

    HRESULT hr = E_FAIL;
    if (adapter) {
        hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, 4,
                               D3D11_SDK_VERSION, &dev, &fl, &ctx);
    }
    if (FAILED(hr) || !dev) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 4,
                               D3D11_SDK_VERSION, &dev, &fl, &ctx);
    }
    if (FAILED(hr) || !dev) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 4, D3D11_SDK_VERSION,
                               &dev, &fl, &ctx);
    }
    if (FAILED(hr) || !dev) return false;

    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(dev.As(&mt)) && mt) mt->SetMultithreadProtected(TRUE);

    hr = MFCreateDXGIDeviceManager(&reset_token, &mgr);
    if (FAILED(hr) || !mgr) return false;
    hr = mgr->ResetDevice(dev.Get(), reset_token);
    return SUCCEEDED(hr);
}

void set_codec_bool(ICodecAPI* api, const GUID& prop, bool val) {
    if (!api) return;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = val ? VARIANT_TRUE : VARIANT_FALSE;
    api->SetValue(&prop, &v);
}

void set_codec_u32(ICodecAPI* api, const GUID& prop, uint32_t val) {
    if (!api) return;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = val;
    api->SetValue(&prop, &v);
}

// ---------------------------------------------------------------- GPU Video Processor: RGB -> NV12
// 4-slot ring pool of GPU textures eliminating CPU color conversion and pipeline sync stalls.

class D3D11RgbToNv12Processor {
public:
    static constexpr int kPoolSize = 4;

    bool init(ID3D11Device* dev, ID3D11DeviceContext* ctx, int width, int height,
              PixelOrder order) {
        reset();
        if (!dev || !ctx || width < 64 || height < 64) return false;

        if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&vdev_))) || !vdev_) return false;
        if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&vctx_))) || !vctx_) return false;

        dev_ = dev;
        ctx_ = ctx;
        width_ = width;
        height_ = height;
        order_ = order;
        const DXGI_FORMAT rgb_fmt =
            (order == PixelOrder::kBGRA) ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cdesc{};
        cdesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cdesc.InputFrameRate = {60, 1};
        cdesc.InputWidth = UINT(width_);
        cdesc.InputHeight = UINT(height_);
        cdesc.OutputFrameRate = {60, 1};
        cdesc.OutputWidth = UINT(width_);
        cdesc.OutputHeight = UINT(height_);
        cdesc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;

        if (FAILED(vdev_->CreateVideoProcessorEnumerator(&cdesc, &vpenum_)) || !vpenum_) {
            reset();
            return false;
        }
        if (FAILED(vdev_->CreateVideoProcessor(vpenum_.Get(), 0, &vp_)) || !vp_) {
            reset();
            return false;
        }

        // Disable unnecessary driver filters (deinterlace/denoise/sharpen) for minimum GPU latency.
        vctx_->VideoProcessorSetStreamAutoProcessingMode(vp_.Get(), 0, FALSE);
        vctx_->VideoProcessorSetStreamFrameFormat(vp_.Get(), 0,
                                                  D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);

        ComPtr<ID3D11VideoContext1> vctx1;
        if (SUCCEEDED(vctx_.As(&vctx1)) && vctx1) {
            vctx1->VideoProcessorSetStreamColorSpace1(vp_.Get(), 0,
                                                      DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
            vctx1->VideoProcessorSetOutputColorSpace1(vp_.Get(),
                                                      DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601);
        } else {
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_cs{};
            in_cs.Usage = 0;
            in_cs.RGB_Range = 0;  // Full range [0..255]
            in_cs.YCbCr_Matrix = 0;  // BT.601
            in_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
            vctx_->VideoProcessorSetStreamColorSpace(vp_.Get(), 0, &in_cs);

            D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_cs{};
            out_cs.Usage = 0;
            out_cs.RGB_Range = 0;
            out_cs.YCbCr_Matrix = 0;  // BT.601
            out_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
            vctx_->VideoProcessorSetOutputColorSpace(vp_.Get(), &out_cs);
        }

        RECT rc{0, 0, width_, height_};
        vctx_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, &rc);
        vctx_->VideoProcessorSetStreamDestRect(vp_.Get(), 0, TRUE, &rc);
        vctx_->VideoProcessorSetOutputTargetRect(vp_.Get(), TRUE, &rc);

        // Allocate 4-slot ring pool: DYNAMIC upload texture (WRITE_DISCARD = zero stall) +
        // DEFAULT GPU RGB texture (for drivers that require DEFAULT on VideoProcessorInputView) +
        // output NV12 GPU texture (BIND_RENDER_TARGET | BIND_VIDEO_ENCODER).
        direct_dynamic_in_ = true;
        for (int i = 0; i < kPoolSize; ++i) {
            D3D11_TEXTURE2D_DESC idesc{};
            idesc.Width = UINT(width_);
            idesc.Height = UINT(height_);
            idesc.MipLevels = 1;
            idesc.ArraySize = 1;
            idesc.Format = rgb_fmt;
            idesc.SampleDesc.Count = 1;
            idesc.Usage = D3D11_USAGE_DYNAMIC;
            idesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            idesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

            if (FAILED(dev_->CreateTexture2D(&idesc, nullptr, &upload_rgb_[i]))) {
                reset();
                return false;
            }

            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
            ivd.FourCC = 0;
            ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            ivd.Texture2D.MipSlice = 0;
            ivd.Texture2D.ArraySlice = 0;

            if (direct_dynamic_in_ &&
                SUCCEEDED(vdev_->CreateVideoProcessorInputView(
                    upload_rgb_[i].Get(), vpenum_.Get(), &ivd, &in_views_[i]))) {
                gpu_rgb_[i] = upload_rgb_[i];
            } else {
                direct_dynamic_in_ = false;
                D3D11_TEXTURE2D_DESC gdesc = idesc;
                gdesc.Usage = D3D11_USAGE_DEFAULT;
                gdesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                gdesc.CPUAccessFlags = 0;
                HRESULT htex = dev_->CreateTexture2D(&gdesc, nullptr, &gpu_rgb_[i]);
                if (FAILED(htex)) {
                    gdesc.BindFlags = 0;
                    htex = dev_->CreateTexture2D(&gdesc, nullptr, &gpu_rgb_[i]);
                }
                if (FAILED(htex) ||
                    FAILED(vdev_->CreateVideoProcessorInputView(
                        gpu_rgb_[i].Get(), vpenum_.Get(), &ivd, &in_views_[i]))) {
                    reset();
                    return false;
                }
            }

            // Output NV12 GPU surface (prefer BIND_RENDER_TARGET | BIND_VIDEO_ENCODER, fallback RT).
            D3D11_TEXTURE2D_DESC odesc{};
            odesc.Width = UINT(width_);
            odesc.Height = UINT(height_);
            odesc.MipLevels = 1;
            odesc.ArraySize = 1;
            odesc.Format = DXGI_FORMAT_NV12;
            odesc.SampleDesc.Count = 1;
            odesc.Usage = D3D11_USAGE_DEFAULT;
            odesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER;
            HRESULT hr = dev_->CreateTexture2D(&odesc, nullptr, &nv12_tex_[i]);
            if (FAILED(hr)) {
                odesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                hr = dev_->CreateTexture2D(&odesc, nullptr, &nv12_tex_[i]);
            }
            if (FAILED(hr)) {
                odesc.BindFlags = D3D11_BIND_RENDER_TARGET;
                hr = dev_->CreateTexture2D(&odesc, nullptr, &nv12_tex_[i]);
            }
            if (FAILED(hr)) {
                reset();
                return false;
            }

            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
            ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            ovd.Texture2D.MipSlice = 0;
            if (FAILED(vdev_->CreateVideoProcessorOutputView(nv12_tex_[i].Get(), vpenum_.Get(),
                                                             &ovd, &out_views_[i]))) {
                reset();
                return false;
            }
        }

        pool_idx_ = 0;
        ready_ = true;
        return true;
    }

    // Uploads `img` into next ring slot via WRITE_DISCARD and runs GPU BGRA/RGBA -> NV12 blit.
    // Returns the resulting GPU NV12 texture ready for MFCreateDXGISurfaceBuffer.
    ID3D11Texture2D* convert(const Image& img) {
        if (!ready_) return nullptr;
        if (img.width != width_ || img.height != height_ || img.order != order_) {
            if (!init(dev_, ctx_, img.width, img.height, img.order)) return nullptr;
        }

        const int slot = pool_idx_++ & (kPoolSize - 1);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(ctx_->Map(upload_rgb_[slot].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            return nullptr;
        }
        const size_t row_bytes = size_t(width_) * 4;
        auto* dst = static_cast<uint8_t*>(mapped.pData);
        if (mapped.RowPitch == UINT(img.stride) && img.stride == row_bytes) {
            std::memcpy(dst, img.data.data(), row_bytes * size_t(height_));
        } else {
            for (int y = 0; y < height_; ++y) {
                std::memcpy(dst + size_t(y) * size_t(mapped.RowPitch), img.row(y), row_bytes);
            }
        }
        ctx_->Unmap(upload_rgb_[slot].Get(), 0);

        if (!direct_dynamic_in_) {
            ctx_->CopyResource(gpu_rgb_[slot].Get(), upload_rgb_[slot].Get());
        }

        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = in_views_[slot].Get();

        if (FAILED(vctx_->VideoProcessorBlt(vp_.Get(), out_views_[slot].Get(), 0, 1, &stream))) {
            return nullptr;
        }
        ctx_->Flush();
        return nv12_tex_[slot].Get();
    }

    // Direct zero-copy GPU path when caller already has a D3D11 BGRA/RGBA texture (e.g. DXGI capture).
    ID3D11Texture2D* convert_gpu_texture(ID3D11Texture2D* src_rgb_tex) {
        if (!ready_ || !src_rgb_tex) return nullptr;
        const int slot = pool_idx_++ & (kPoolSize - 1);

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11VideoProcessorInputView> tmp_iv;
        if (FAILED(vdev_->CreateVideoProcessorInputView(src_rgb_tex, vpenum_.Get(), &ivd,
                                                        &tmp_iv))) {
            return nullptr;
        }
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = tmp_iv.Get();
        if (FAILED(vctx_->VideoProcessorBlt(vp_.Get(), out_views_[slot].Get(), 0, 1, &stream))) {
            return nullptr;
        }
        ctx_->Flush();
        return nv12_tex_[slot].Get();
    }

    bool ready() const { return ready_; }

    void reset() {
        ready_ = false;
        for (int i = 0; i < kPoolSize; ++i) {
            out_views_[i].Reset();
            nv12_tex_[i].Reset();
            in_views_[i].Reset();
            gpu_rgb_[i].Reset();
            upload_rgb_[i].Reset();
        }
        vp_.Reset();
        vpenum_.Reset();
        vctx_.Reset();
        vdev_.Reset();
        dev_ = nullptr;
        ctx_ = nullptr;
    }

private:
    ID3D11Device* dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    ComPtr<ID3D11VideoDevice> vdev_;
    ComPtr<ID3D11VideoContext> vctx_;
    ComPtr<ID3D11VideoProcessorEnumerator> vpenum_;
    ComPtr<ID3D11VideoProcessor> vp_;

    std::array<ComPtr<ID3D11Texture2D>, kPoolSize> upload_rgb_;
    std::array<ComPtr<ID3D11Texture2D>, kPoolSize> gpu_rgb_;
    std::array<ComPtr<ID3D11VideoProcessorInputView>, kPoolSize> in_views_;
    std::array<ComPtr<ID3D11Texture2D>, kPoolSize> nv12_tex_;
    std::array<ComPtr<ID3D11VideoProcessorOutputView>, kPoolSize> out_views_;

    int width_ = 0;
    int height_ = 0;
    PixelOrder order_ = PixelOrder::kBGRA;
    bool direct_dynamic_in_ = true;
    int pool_idx_ = 0;
    bool ready_ = false;
};

// ---------------------------------------------------------------- GPU Video Processor: NV12 -> RGB
// Hardware DXVA2/D3D11 NV12 -> BGRA/RGBA conversion + cropping for MfVideoDecoder.

class D3D11Nv12ToRgbProcessor {
public:
    bool convert(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* nv12_tex,
                 UINT array_slice, int src_w, int src_h, int display_w, int display_h,
                 PixelOrder order, Image& out_image) {
        if (!dev || !ctx || !nv12_tex || display_w <= 0 || display_h <= 0) return false;

        if (!ensure_init(dev, ctx, src_w, src_h, display_w, display_h, order)) return false;

        // Look up or create cached input view for (nv12_tex, array_slice).
        const uint64_t key = (reinterpret_cast<uintptr_t>(nv12_tex) << 8) ^ uint64_t(array_slice);
        auto it = in_view_cache_.find(key);
        ID3D11VideoProcessorInputView* in_view = nullptr;
        if (it != in_view_cache_.end()) {
            in_view = it->second.Get();
        } else {
            if (in_view_cache_.size() > 32) in_view_cache_.clear();
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
            ivd.FourCC = 0;
            ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            ivd.Texture2D.MipSlice = 0;
            ivd.Texture2D.ArraySlice = array_slice;
            ComPtr<ID3D11VideoProcessorInputView> iv;
            if (FAILED(vdev_->CreateVideoProcessorInputView(nv12_tex, vpenum_.Get(), &ivd, &iv))) {
                return false;
            }
            in_view = iv.Get();
            in_view_cache_.emplace(key, std::move(iv));
        }

        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = in_view;

        if (FAILED(vctx_->VideoProcessorBlt(vp_.Get(), out_view_.Get(), 0, 1, &stream))) {
            return false;
        }

        if (out_image.width != display_w || out_image.height != display_h ||
            out_image.order != order) {
            out_image.allocate(display_w, display_h, order);
        }

        ctx_->CopyResource(staging_rgb_.Get(), out_rgb_.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(ctx_->Map(staging_rgb_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;

        const auto* src = static_cast<const uint8_t*>(mapped.pData);
        const size_t row_bytes = size_t(display_w) * 4;
        if (mapped.RowPitch == UINT(out_image.stride) && out_image.stride == row_bytes) {
            std::memcpy(out_image.data.data(), src, row_bytes * size_t(display_h));
        } else {
            for (int y = 0; y < display_h; ++y) {
                std::memcpy(out_image.row(y), src + size_t(y) * size_t(mapped.RowPitch), row_bytes);
            }
        }
        ctx_->Unmap(staging_rgb_.Get(), 0);
        return true;
    }

    void reset() {
        in_view_cache_.clear();
        staging_rgb_.Reset();
        out_view_.Reset();
        out_rgb_.Reset();
        vp_.Reset();
        vpenum_.Reset();
        vctx_.Reset();
        vdev_.Reset();
        dev_ = nullptr;
        ctx_ = nullptr;
        src_w_ = src_h_ = dst_w_ = dst_h_ = 0;
    }

private:
    bool ensure_init(ID3D11Device* dev, ID3D11DeviceContext* ctx, int src_w, int src_h, int dst_w,
                     int dst_h, PixelOrder order) {
        if (vp_ && dev_ == dev && src_w_ == src_w && src_h_ == src_h && dst_w_ == dst_w &&
            dst_h_ == dst_h && order_ == order) {
            return true;
        }
        reset();
        if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&vdev_))) || !vdev_) return false;
        if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&vctx_))) || !vctx_) return false;

        dev_ = dev;
        ctx_ = ctx;
        src_w_ = src_w;
        src_h_ = src_h;
        dst_w_ = dst_w;
        dst_h_ = dst_h;
        order_ = order;

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cdesc{};
        cdesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cdesc.InputFrameRate = {60, 1};
        cdesc.InputWidth = UINT(src_w_);
        cdesc.InputHeight = UINT(src_h_);
        cdesc.OutputFrameRate = {60, 1};
        cdesc.OutputWidth = UINT(dst_w_);
        cdesc.OutputHeight = UINT(dst_h_);
        cdesc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;

        if (FAILED(vdev_->CreateVideoProcessorEnumerator(&cdesc, &vpenum_)) || !vpenum_) {
            reset();
            return false;
        }
        if (FAILED(vdev_->CreateVideoProcessor(vpenum_.Get(), 0, &vp_)) || !vp_) {
            reset();
            return false;
        }

        vctx_->VideoProcessorSetStreamAutoProcessingMode(vp_.Get(), 0, FALSE);
        vctx_->VideoProcessorSetStreamFrameFormat(vp_.Get(), 0,
                                                  D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);

        ComPtr<ID3D11VideoContext1> vctx1;
        if (SUCCEEDED(vctx_.As(&vctx1)) && vctx1) {
            vctx1->VideoProcessorSetStreamColorSpace1(vp_.Get(), 0,
                                                      DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601);
            vctx1->VideoProcessorSetOutputColorSpace1(vp_.Get(),
                                                      DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        } else {
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_cs{};
            in_cs.Usage = 0;
            in_cs.RGB_Range = 0;
            in_cs.YCbCr_Matrix = 0;  // BT.601
            in_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
            vctx_->VideoProcessorSetStreamColorSpace(vp_.Get(), 0, &in_cs);

            D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_cs{};
            out_cs.Usage = 0;
            out_cs.RGB_Range = 0;  // Full [0..255]
            out_cs.YCbCr_Matrix = 0;
            out_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
            vctx_->VideoProcessorSetOutputColorSpace(vp_.Get(), &out_cs);
        }

        RECT crop{0, 0, dst_w_, dst_h_};
        vctx_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, &crop);
        vctx_->VideoProcessorSetStreamDestRect(vp_.Get(), 0, TRUE, &crop);
        vctx_->VideoProcessorSetOutputTargetRect(vp_.Get(), TRUE, &crop);

        const DXGI_FORMAT fmt =
            (order_ == PixelOrder::kBGRA) ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;

        D3D11_TEXTURE2D_DESC odesc{};
        odesc.Width = UINT(dst_w_);
        odesc.Height = UINT(dst_h_);
        odesc.MipLevels = 1;
        odesc.ArraySize = 1;
        odesc.Format = fmt;
        odesc.SampleDesc.Count = 1;
        odesc.Usage = D3D11_USAGE_DEFAULT;
        odesc.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(dev_->CreateTexture2D(&odesc, nullptr, &out_rgb_))) {
            reset();
            return false;
        }

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ovd.Texture2D.MipSlice = 0;
        if (FAILED(vdev_->CreateVideoProcessorOutputView(out_rgb_.Get(), vpenum_.Get(), &ovd,
                                                         &out_view_))) {
            reset();
            return false;
        }

        D3D11_TEXTURE2D_DESC sdesc = odesc;
        sdesc.Usage = D3D11_USAGE_STAGING;
        sdesc.BindFlags = 0;
        sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev_->CreateTexture2D(&sdesc, nullptr, &staging_rgb_))) {
            reset();
            return false;
        }
        return true;
    }

    ID3D11Device* dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    ComPtr<ID3D11VideoDevice> vdev_;
    ComPtr<ID3D11VideoContext> vctx_;
    ComPtr<ID3D11VideoProcessorEnumerator> vpenum_;
    ComPtr<ID3D11VideoProcessor> vp_;
    ComPtr<ID3D11Texture2D> out_rgb_;
    ComPtr<ID3D11VideoProcessorOutputView> out_view_;
    ComPtr<ID3D11Texture2D> staging_rgb_;
    std::unordered_map<uint64_t, ComPtr<ID3D11VideoProcessorInputView>> in_view_cache_;

    int src_w_ = 0;
    int src_h_ = 0;
    int dst_w_ = 0;
    int dst_h_ = 0;
    PixelOrder order_ = PixelOrder::kBGRA;
};

// ---------------------------------------------------------------- Media Foundation Encoder

class MfVideoEncoder final : public IVideoEncoder {
public:
    explicit MfVideoEncoder(VideoCodecId codec) : codec_(codec) {}

    ~MfVideoEncoder() override { shutdown(); }

    bool init(const VideoEncoderConfig& cfg) override {
        shutdown();
        if (!mf_.ok()) return false;
        cfg_ = cfg;
        width_ = cfg.width & ~1;
        height_ = cfg.height & ~1;
        if (width_ < 64 || height_ < 64) return false;
        fps_num_ = uint32_t(std::clamp(int(cfg.fps + 0.5), 1, 240));
        bitrate_kbps_ = std::clamp<uint32_t>(cfg.bitrate_kbps, 250u, 100000u);
        keyint_ = std::clamp<uint32_t>(cfg.keyframe_interval, 1u, 600u);

        MFT_REGISTER_TYPE_INFO out_info{MFMediaType_Video, codec_subtype(codec_)};
        const auto candidates =
            enumerate_mfts(MFT_CATEGORY_VIDEO_ENCODER, nullptr, &out_info, cfg.prefer_hardware);

        for (const auto& cand : candidates) {
            if (try_init_candidate(cand)) return true;
            shutdown();
        }
        return false;
    }

    bool encode_image(const Image& img, EncodedVideoPacket& out) override {
        if (!mft_ || img.width != width_ || img.height != height_) return false;

        // Fast Path: GPU D3D11 Video Processor (BGRA/RGBA -> NV12) + MFCreateDXGISurfaceBuffer!
        if (use_d3d_surfaces_ && d3d_dev_ && d3d_ctx_) {
            if (!rgb_to_nv12_.ready()) {
                rgb_to_nv12_.init(d3d_dev_.Get(), d3d_ctx_.Get(), width_, height_, img.order);
            }
            if (ID3D11Texture2D* nv12_tex = rgb_to_nv12_.convert(img)) {
                ComPtr<IMFSample> sample;
                ComPtr<IMFMediaBuffer> buf;
                if (SUCCEEDED(MFCreateSample(&sample)) &&
                    SUCCEEDED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12_tex, 0,
                                                        FALSE, &buf))) {
                    ComPtr<IMF2DBuffer> b2d;
                    DWORD len = DWORD(size_t(width_) * size_t(height_) * 3 / 2);
                    if (SUCCEEDED(buf.As(&b2d)) && b2d) b2d->GetContiguousLength(&len);
                    buf->SetCurrentLength(len);
                    sample->AddBuffer(buf.Get());
                    if (encode_sample(sample.Get(), out)) return true;
                }
                // If hardware MFT rejected DXGI surface buffer, fall back cleanly to CPU buffer.
                use_d3d_surfaces_ = false;
                rgb_to_nv12_.reset();
            }
        }

        // Safe CPU fallback (for software MFTs or drivers without Video Processor support).
        const size_t nv12_size = size_t(width_) * size_t(height_) * 3 / 2;
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(MFCreateSample(&sample)) || FAILED(MFCreateMemoryBuffer(DWORD(nv12_size), &buf))) {
            return false;
        }
        BYTE* dst = nullptr;
        if (FAILED(buf->Lock(&dst, nullptr, nullptr)) || !dst) return false;
        packed_rgb_to_nv12(img, dst, width_, dst + size_t(width_) * size_t(height_), width_);
        buf->Unlock();
        buf->SetCurrentLength(DWORD(nv12_size));
        sample->AddBuffer(buf.Get());
        return encode_sample(sample.Get(), out);
    }

    bool encode_yuv420(const Yuv420View& yuv, int width, int height,
                       EncodedVideoPacket& out) override {
        if (!mft_ || width != width_ || height != height_) return false;
        const size_t nv12_size = size_t(width_) * size_t(height_) * 3 / 2;
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(MFCreateSample(&sample)) || FAILED(MFCreateMemoryBuffer(DWORD(nv12_size), &buf))) {
            return false;
        }
        BYTE* dst = nullptr;
        if (FAILED(buf->Lock(&dst, nullptr, nullptr)) || !dst) return false;
        yuv420_to_nv12(yuv, width_, height_, dst, width_, dst + size_t(width_) * size_t(height_),
                       width_);
        buf->Unlock();
        buf->SetCurrentLength(DWORD(nv12_size));
        sample->AddBuffer(buf.Get());
        return encode_sample(sample.Get(), out);
    }

    bool set_bitrate_kbps(uint32_t bitrate_kbps) override {
        bitrate_kbps = std::clamp<uint32_t>(bitrate_kbps, 250u, 100000u);
        if (bitrate_kbps == bitrate_kbps_) return true;
        bitrate_kbps_ = bitrate_kbps;
        if (!codec_api_) return false;
        set_codec_u32(codec_api_.Get(), CODECAPI_AVEncCommonMeanBitRate, bitrate_kbps_ * 1000u);
        set_codec_u32(codec_api_.Get(), CODECAPI_AVEncCommonMaxBitRate,
                      uint32_t(uint64_t(bitrate_kbps_) * 2000u));
        return true;
    }

    void force_keyframe() override { force_idr_.store(true, std::memory_order_release); }

    VideoCodecId codec_id() const override { return codec_; }
    std::string backend_name() const override { return backend_name_; }
    bool is_hardware() const override { return is_hardware_; }

private:
    bool try_init_candidate(const MftCandidate& cand) {
        ComPtr<IMFTransform> mft;
        HRESULT hr = cand.activate->ActivateObject(IID_PPV_ARGS(&mft));
        if (FAILED(hr) || !mft) return false;

        ComPtr<IMFAttributes> attrs;
        bool is_async = cand.is_async;
        bool d3d_aware = false;
        if (SUCCEEDED(mft->GetAttributes(&attrs)) && attrs) {
            UINT32 async_val = 0;
            if (SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &async_val)) && async_val) {
                is_async = true;
                attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            }
            UINT32 d3d11_val = 0;
            if (SUCCEEDED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &d3d11_val)) && d3d11_val) {
                d3d_aware = true;
            }
            attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
            attrs->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
        }

        ComPtr<IMFMediaEventGenerator> evgen;
        if (is_async) {
            if (FAILED(mft.As(&evgen)) || !evgen) return false;
        }

        bool d3d_bound = false;
        if (cand.is_hardware && d3d_aware) {
            const LUID* luid_ptr = cand.has_luid ? &cand.adapter_luid : nullptr;
            if (create_d3d11_manager(luid_ptr, d3d_dev_, d3d_ctx_, dxgi_mgr_)) {
                if (SUCCEEDED(mft->ProcessMessage(
                        MFT_MESSAGE_SET_D3D_MANAGER,
                        reinterpret_cast<ULONG_PTR>(dxgi_mgr_.Get())))) {
                    d3d_bound = true;
                }
            }
        }

        // Resolve stream IDs (usually 0, 0).
        DWORD in_id = 0, out_id = 0;
        if (FAILED(mft->GetStreamIDs(1, &in_id, 1, &out_id))) {
            in_id = 0;
            out_id = 0;
        }

        // Configure ICodecAPI BEFORE setting types where supported (especially HW MFTs).
        ComPtr<ICodecAPI> api;
        mft.As(&api);
        configure_codec_api(api.Get());

        // 1. Set Output Type (H.264 / HEVC)
        ComPtr<IMFMediaType> out_type;
        if (FAILED(MFCreateMediaType(&out_type))) return false;
        out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out_type->SetGUID(MF_MT_SUBTYPE, codec_subtype(codec_));
        out_type->SetUINT32(MF_MT_AVG_BITRATE, bitrate_kbps_ * 1000u);
        out_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(out_type.Get(), MF_MT_FRAME_SIZE, UINT32(width_), UINT32(height_));
        MFSetAttributeRatio(out_type.Get(), MF_MT_FRAME_RATE, fps_num_, 1);
        MFSetAttributeRatio(out_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (codec_ == VideoCodecId::kH264) {
            out_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
        } else {
            out_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH265VProfile_Main_420_8);
        }

        hr = mft->SetOutputType(out_id, out_type.Get(), 0);
        if (FAILED(hr)) {
            out_type->DeleteItem(MF_MT_MPEG2_PROFILE);
            hr = mft->SetOutputType(out_id, out_type.Get(), 0);
            if (FAILED(hr)) return false;
        }

        // 2. Set Input Type (NV12)
        ComPtr<IMFMediaType> in_type;
        if (FAILED(MFCreateMediaType(&in_type))) return false;
        in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        in_type->SetUINT32(MF_MT_DEFAULT_STRIDE, UINT32(width_));
        in_type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT601);
        in_type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        MFSetAttributeSize(in_type.Get(), MF_MT_FRAME_SIZE, UINT32(width_), UINT32(height_));
        MFSetAttributeRatio(in_type.Get(), MF_MT_FRAME_RATE, fps_num_, 1);
        MFSetAttributeRatio(in_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

        hr = mft->SetInputType(in_id, in_type.Get(), 0);
        if (FAILED(hr)) {
            bool ok_in = false;
            for (DWORD i = 0; i < 16; ++i) {
                ComPtr<IMFMediaType> avail;
                if (FAILED(mft->GetInputAvailableType(in_id, i, &avail))) break;
                GUID sub{};
                if (SUCCEEDED(avail->GetGUID(MF_MT_SUBTYPE, &sub)) && sub == MFVideoFormat_NV12) {
                    MFSetAttributeSize(avail.Get(), MF_MT_FRAME_SIZE, UINT32(width_), UINT32(height_));
                    MFSetAttributeRatio(avail.Get(), MF_MT_FRAME_RATE, fps_num_, 1);
                    if (SUCCEEDED(mft->SetInputType(in_id, avail.Get(), 0))) {
                        ok_in = true;
                        break;
                    }
                }
            }
            if (!ok_in) return false;
        }

        // Re-apply low-latency ICodecAPI properties after media types are set.
        configure_codec_api(api.Get());

        // Cache sequence header if exposed on output type.
        ComPtr<IMFMediaType> cur_out;
        if (SUCCEEDED(mft->GetOutputCurrentType(out_id, &cur_out)) && cur_out) {
            UINT32 blob_sz = 0;
            if (SUCCEEDED(cur_out->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &blob_sz)) && blob_sz > 4) {
                std::vector<uint8_t> seq(blob_sz);
                if (SUCCEEDED(cur_out->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, seq.data(), blob_sz,
                                               &blob_sz))) {
                    const auto nalus = annexb_scan_nalus(seq.data(), seq.size(), codec_);
                    if (!nalus.empty()) cached_params_ = std::move(seq);
                }
            }
        }

        mft->GetOutputStreamInfo(out_id, &out_stream_info_);
        hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        if (FAILED(hr) && hr != E_NOTIMPL) return false;
        hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        if (FAILED(hr) && hr != E_NOTIMPL) return false;

        // Pre-initialize D3D11 Video Processor ring pool before frame 0 (default BGRA).
        use_d3d_surfaces_ = false;
        if (d3d_bound && d3d_dev_ && d3d_ctx_) {
            if (rgb_to_nv12_.init(d3d_dev_.Get(), d3d_ctx_.Get(), width_, height_,
                                  PixelOrder::kBGRA)) {
                use_d3d_surfaces_ = true;
            }
        }

        mft_ = mft;
        codec_api_ = api;
        event_gen_ = evgen;
        in_stream_id_ = in_id;
        out_stream_id_ = out_id;
        is_async_ = is_async;
        is_hardware_ = cand.is_hardware;
        pending_need_input_ = 0;
        frame_index_ = 0;
        force_idr_.store(true, std::memory_order_relaxed);
        backend_name_ = cand.name +
                        (use_d3d_surfaces_ ? " (D3D11 HW)" : (is_hardware_ ? " (HW)" : " (SW)"));
        return true;
    }

    void configure_codec_api(ICodecAPI* api) {
        if (!api) return;
        set_codec_bool(api, CODECAPI_AVLowLatencyMode, true);
        VARIANT rc;
        VariantInit(&rc);
        rc.vt = VT_UI4;
        rc.ulVal = eAVEncCommonRateControlMode_LowDelayVBR;
        if (FAILED(api->SetValue(&CODECAPI_AVEncCommonRateControlMode, &rc))) {
            rc.ulVal = eAVEncCommonRateControlMode_PeakConstrainedVBR;
            if (FAILED(api->SetValue(&CODECAPI_AVEncCommonRateControlMode, &rc))) {
                rc.ulVal = eAVEncCommonRateControlMode_CBR;
                api->SetValue(&CODECAPI_AVEncCommonRateControlMode, &rc);
            }
        }
        set_codec_u32(api, CODECAPI_AVEncCommonMeanBitRate, bitrate_kbps_ * 1000u);
        set_codec_u32(api, CODECAPI_AVEncCommonMaxBitRate, uint32_t(uint64_t(bitrate_kbps_) * 2000u));
        set_codec_u32(api, CODECAPI_AVEncMPVDefaultBPictureCount, 0u);
        set_codec_u32(api, CODECAPI_AVEncVideoMaxNumRefFrame, 1u);
        set_codec_u32(api, CODECAPI_AVEncMPVGOPSize, keyint_);
        set_codec_u32(api, CODECAPI_AVEncCommonQualityVsSpeed, 0u);
    }

    bool pull_output_sample(EncodedVideoPacket& out) {
        MFT_OUTPUT_DATA_BUFFER odb{};
        odb.dwStreamID = out_stream_id_;
        ComPtr<IMFSample> alloc_sample;
        const bool mft_provides =
            (out_stream_info_.dwFlags &
             (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        if (!mft_provides) {
            if (FAILED(MFCreateSample(&alloc_sample))) return false;
            ComPtr<IMFMediaBuffer> mb;
            const DWORD cap = std::max<DWORD>(out_stream_info_.cbSize, DWORD(width_ * height_));
            if (FAILED(MFCreateMemoryBuffer(cap, &mb))) return false;
            alloc_sample->AddBuffer(mb.Get());
            odb.pSample = alloc_sample.Get();
        }

        DWORD status = 0;
        HRESULT hr = mft_->ProcessOutput(0, 1, &odb, &status);
        if (odb.pEvents) odb.pEvents->Release();

        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            mft_->GetOutputStreamInfo(out_stream_id_, &out_stream_info_);
            return pull_output_sample(out);
        }
        if (FAILED(hr) || !odb.pSample) {
            if (mft_provides && odb.pSample) odb.pSample->Release();
            return false;
        }

        ComPtr<IMFSample> out_sample;
        if (mft_provides) out_sample.Attach(odb.pSample);
        else out_sample = alloc_sample;

        UINT32 clean_pt = 0;
        bool is_kf = SUCCEEDED(out_sample->GetUINT32(MFSampleExtension_CleanPoint, &clean_pt)) &&
                     clean_pt != 0;

        ComPtr<IMFMediaBuffer> contig;
        if (FAILED(out_sample->ConvertToContiguousBuffer(&contig)) || !contig) return false;
        BYTE* ptr = nullptr;
        DWORD max_len = 0, cur_len = 0;
        if (FAILED(contig->Lock(&ptr, &max_len, &cur_len)) || !ptr || cur_len == 0) return false;

        out.data.assign(ptr, ptr + cur_len);
        contig->Unlock();

        bool has_cfg = false;
        annexb_normalize_access_unit(codec_, out.data, cached_params_, is_kf, has_cfg);
        out.is_keyframe = is_kf;
        out.has_codec_config = has_cfg;
        return !out.data.empty();
    }

    bool encode_sample(IMFSample* sample, EncodedVideoPacket& out) {
        out.data.clear();
        out.is_keyframe = false;
        out.has_codec_config = false;

        const LONGLONG dur_100ns = 10000000LL / LONGLONG(fps_num_);
        sample->SetSampleTime(LONGLONG(frame_index_) * dur_100ns);
        sample->SetSampleDuration(dur_100ns);
        ++frame_index_;

        const bool want_idr = force_idr_.exchange(false, std::memory_order_acq_rel);
        if (want_idr && codec_api_) {
            set_codec_u32(codec_api_.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1u);
        }

        if (is_async_) {
            for (int guard = 0; guard < 16 && pending_need_input_ <= 0; ++guard) {
                ComPtr<IMFMediaEvent> ev;
                if (FAILED(event_gen_->GetEvent(0, &ev)) || !ev) return false;
                MediaEventType met = MEUnknown;
                ev->GetType(&met);
                if (met == METransformNeedInput) {
                    ++pending_need_input_;
                } else if (met == METransformHaveOutput) {
                    EncodedVideoPacket stray;
                    pull_output_sample(stray);
                }
            }
            if (pending_need_input_ <= 0) return false;
            --pending_need_input_;

            if (FAILED(mft_->ProcessInput(in_stream_id_, sample, 0))) return false;

            for (int guard = 0; guard < 32; ++guard) {
                ComPtr<IMFMediaEvent> ev;
                if (FAILED(event_gen_->GetEvent(0, &ev)) || !ev) return false;
                MediaEventType met = MEUnknown;
                ev->GetType(&met);
                if (met == METransformHaveOutput) {
                    return pull_output_sample(out);
                } else if (met == METransformNeedInput) {
                    ++pending_need_input_;
                }
            }
            return false;
        }

        // Synchronous MFT path
        HRESULT hr = mft_->ProcessInput(in_stream_id_, sample, 0);
        if (hr == MF_E_NOTACCEPTING) {
            pull_output_sample(out);
            hr = mft_->ProcessInput(in_stream_id_, sample, 0);
        }
        if (FAILED(hr)) return false;

        if (pull_output_sample(out)) return true;

        if (frame_index_ <= 2) {
            sample->SetSampleTime(LONGLONG(frame_index_) * dur_100ns);
            ++frame_index_;
            if (SUCCEEDED(mft_->ProcessInput(in_stream_id_, sample, 0))) {
                return pull_output_sample(out);
            }
        }
        return false;
    }

    void shutdown() {
        if (mft_) {
            mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        }
        rgb_to_nv12_.reset();
        use_d3d_surfaces_ = false;
        event_gen_.Reset();
        codec_api_.Reset();
        mft_.Reset();
        dxgi_mgr_.Reset();
        d3d_ctx_.Reset();
        d3d_dev_.Reset();
        cached_params_.clear();
        pending_need_input_ = 0;
    }

    MfSessionGuard mf_;
    VideoCodecId codec_;
    VideoEncoderConfig cfg_{};
    int width_ = 0;
    int height_ = 0;
    uint32_t fps_num_ = 60;
    uint32_t bitrate_kbps_ = 8000;
    uint32_t keyint_ = 120;

    ComPtr<ID3D11Device> d3d_dev_;
    ComPtr<ID3D11DeviceContext> d3d_ctx_;
    ComPtr<IMFDXGIDeviceManager> dxgi_mgr_;
    D3D11RgbToNv12Processor rgb_to_nv12_;
    bool use_d3d_surfaces_ = false;

    ComPtr<IMFTransform> mft_;
    ComPtr<ICodecAPI> codec_api_;
    ComPtr<IMFMediaEventGenerator> event_gen_;
    DWORD in_stream_id_ = 0;
    DWORD out_stream_id_ = 0;
    MFT_OUTPUT_STREAM_INFO out_stream_info_{};
    bool is_async_ = false;
    bool is_hardware_ = false;
    int pending_need_input_ = 0;
    uint64_t frame_index_ = 0;
    std::atomic<bool> force_idr_{true};
    std::string backend_name_;
    std::vector<uint8_t> cached_params_;
};

// ---------------------------------------------------------------- Media Foundation Decoder

class MfVideoDecoder final : public IVideoDecoder {
public:
    explicit MfVideoDecoder(VideoCodecId codec) : codec_(codec) {}

    ~MfVideoDecoder() override { shutdown(); }

    bool init(int width_hint, int height_hint, PixelOrder output_order) override {
        shutdown();
        if (!mf_.ok()) return false;
        display_w_ = width_hint > 0 ? width_hint : 1920;
        display_h_ = height_hint > 0 ? height_hint : 1080;
        coded_w_ = display_w_;
        coded_h_ = display_h_;
        output_order_ = output_order;

        MFT_REGISTER_TYPE_INFO in_info{MFMediaType_Video, codec_subtype(codec_)};
        const auto candidates = enumerate_mfts(MFT_CATEGORY_VIDEO_DECODER, &in_info, nullptr, true);
        for (const auto& cand : candidates) {
            if (try_init_candidate(cand)) return true;
            shutdown();
        }
        return false;
    }

    bool decode(const uint8_t* data, size_t size, PixelOrder output_order, Image& out_image) override {
        if (!mft_ || !data || size == 0) return false;
        output_order_ = output_order;

        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(MFCreateSample(&sample)) || FAILED(MFCreateMemoryBuffer(DWORD(size), &buf))) {
            return false;
        }
        BYTE* dst = nullptr;
        if (FAILED(buf->Lock(&dst, nullptr, nullptr)) || !dst) return false;
        std::memcpy(dst, data, size);
        buf->Unlock();
        buf->SetCurrentLength(DWORD(size));
        sample->AddBuffer(buf.Get());

        const LONGLONG dur_100ns = 166667LL;  // ~60 fps nominal
        sample->SetSampleTime(LONGLONG(frame_index_) * dur_100ns);
        sample->SetSampleDuration(dur_100ns);
        ++frame_index_;

        if (is_async_) {
            for (int guard = 0; guard < 16 && pending_need_input_ <= 0; ++guard) {
                ComPtr<IMFMediaEvent> ev;
                if (FAILED(event_gen_->GetEvent(0, &ev)) || !ev) return false;
                MediaEventType met = MEUnknown;
                ev->GetType(&met);
                if (met == METransformNeedInput) ++pending_need_input_;
                else if (met == METransformHaveOutput) {
                    pull_decoded_frame(output_order, out_image);
                }
            }
            if (pending_need_input_ <= 0) return false;
            --pending_need_input_;
            if (FAILED(mft_->ProcessInput(in_stream_id_, sample.Get(), 0))) return false;

            for (int guard = 0; guard < 16; ++guard) {
                ComPtr<IMFMediaEvent> ev;
                if (FAILED(event_gen_->GetEvent(0, &ev)) || !ev) return false;
                MediaEventType met = MEUnknown;
                ev->GetType(&met);
                if (met == METransformHaveOutput) {
                    if (pull_decoded_frame(output_order, out_image)) return true;
                } else if (met == METransformNeedInput) {
                    ++pending_need_input_;
                    return false;
                }
            }
            return false;
        }

        HRESULT hr = mft_->ProcessInput(in_stream_id_, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            pull_decoded_frame(output_order, out_image);
            hr = mft_->ProcessInput(in_stream_id_, sample.Get(), 0);
        }
        if (FAILED(hr)) return false;

        bool produced = false;
        for (int guard = 0; guard < 8; ++guard) {
            const int rc = pull_decoded_frame_status(output_order, out_image);
            if (rc == 1) {
                produced = true;
            } else if (rc == 2) {
                continue;
            } else {
                break;
            }
        }
        return produced;
    }

    void reset() override {
        if (mft_) {
            mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
            mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        }
        nv12_to_rgb_.reset();
        pending_need_input_ = 0;
    }

    VideoCodecId codec_id() const override { return codec_; }
    std::string backend_name() const override { return backend_name_; }
    bool is_hardware() const override { return is_hardware_; }

private:
    bool try_init_candidate(const MftCandidate& cand) {
        ComPtr<IMFTransform> mft;
        HRESULT hr = cand.activate->ActivateObject(IID_PPV_ARGS(&mft));
        if (FAILED(hr) || !mft) return false;

        ComPtr<IMFAttributes> attrs;
        bool is_async = cand.is_async;
        bool d3d_aware = false;
        if (SUCCEEDED(mft->GetAttributes(&attrs)) && attrs) {
            UINT32 async_val = 0;
            if (SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &async_val)) && async_val) {
                is_async = true;
                attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            }
            UINT32 d3d11_val = 0;
            if (SUCCEEDED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &d3d11_val)) && d3d11_val) {
                d3d_aware = true;
            }
            attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
            attrs->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
            attrs->SetUINT32(CODECAPI_AVDecVideoAcceleration_H264, TRUE);
        }

        ComPtr<IMFMediaEventGenerator> evgen;
        if (is_async) {
            if (FAILED(mft.As(&evgen)) || !evgen) return false;
        }

        // Enable DXVA2/D3D11 hardware video acceleration + GPU Video Processor output!
        bool using_d3d_hw = false;
        if (d3d_aware) {
            const LUID* luid_ptr = cand.has_luid ? &cand.adapter_luid : nullptr;
            if (create_d3d11_manager(luid_ptr, d3d_dev_, d3d_ctx_, dxgi_mgr_)) {
                if (SUCCEEDED(mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                                  reinterpret_cast<ULONG_PTR>(dxgi_mgr_.Get())))) {
                    using_d3d_hw = true;
                }
            }
        }

        DWORD in_id = 0, out_id = 0;
        if (FAILED(mft->GetStreamIDs(1, &in_id, 1, &out_id))) {
            in_id = 0;
            out_id = 0;
        }

        ComPtr<ICodecAPI> api;
        if (SUCCEEDED(mft.As(&api)) && api) {
            set_codec_bool(api.Get(), CODECAPI_AVLowLatencyMode, true);
        }

        ComPtr<IMFMediaType> in_type;
        if (FAILED(MFCreateMediaType(&in_type))) return false;
        in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in_type->SetGUID(MF_MT_SUBTYPE, codec_subtype(codec_));
        in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(in_type.Get(), MF_MT_FRAME_SIZE, UINT32(display_w_), UINT32(display_h_));
        if (FAILED(mft->SetInputType(in_id, in_type.Get(), 0))) return false;

        mft_ = mft;
        event_gen_ = evgen;
        in_stream_id_ = in_id;
        out_stream_id_ = out_id;
        if (!select_nv12_output_type()) return false;

        hr = mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        if (FAILED(hr) && hr != E_NOTIMPL) return false;
        hr = mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        if (FAILED(hr) && hr != E_NOTIMPL) return false;

        is_async_ = is_async;
        is_hardware_ = cand.is_hardware || using_d3d_hw;
        pending_need_input_ = 0;
        frame_index_ = 0;
        backend_name_ = cand.name + (is_hardware_ ? " (D3D11 DXVA HW)" : " (SW)");
        return true;
    }

    bool select_nv12_output_type() {
        for (DWORD i = 0; i < 32; ++i) {
            ComPtr<IMFMediaType> mt;
            if (FAILED(mft_->GetOutputAvailableType(out_stream_id_, i, &mt))) break;
            GUID sub{};
            if (SUCCEEDED(mt->GetGUID(MF_MT_SUBTYPE, &sub)) && sub == MFVideoFormat_NV12) {
                if (SUCCEEDED(mft_->SetOutputType(out_stream_id_, mt.Get(), 0))) {
                    update_output_geometry(mt.Get());
                    mft_->GetOutputStreamInfo(out_stream_id_, &out_stream_info_);
                    return true;
                }
            }
        }
        return false;
    }

    void update_output_geometry(IMFMediaType* mt) {
        UINT32 cw = 0, ch = 0;
        if (SUCCEEDED(MFGetAttributeSize(mt, MF_MT_FRAME_SIZE, &cw, &ch)) && cw > 0 && ch > 0) {
            coded_w_ = int(cw);
            coded_h_ = int(ch);
            display_w_ = coded_w_;
            display_h_ = coded_h_;
        }
        MFVideoArea area{};
        UINT32 cb = 0;
        if (SUCCEEDED(mt->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area),
                                  sizeof(area), &cb)) &&
            cb == sizeof(area) && area.Area.cx > 0 && area.Area.cy > 0) {
            display_w_ = int(area.Area.cx);
            display_h_ = int(area.Area.cy);
        } else if (SUCCEEDED(mt->GetBlob(MF_MT_GEOMETRIC_APERTURE, reinterpret_cast<UINT8*>(&area),
                                         sizeof(area), &cb)) &&
                   cb == sizeof(area) && area.Area.cx > 0 && area.Area.cy > 0) {
            display_w_ = int(area.Area.cx);
            display_h_ = int(area.Area.cy);
        }
        UINT32 stride_u32 = 0;
        if (SUCCEEDED(mt->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride_u32)) && int32_t(stride_u32) > 0) {
            default_stride_ = int(int32_t(stride_u32));
        } else {
            default_stride_ = coded_w_;
        }
    }

    bool extract_nv12_from_sample(IMFSample* sample, PixelOrder order, Image& out_image) {
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->GetBufferByIndex(0, &buf)) || !buf) return false;

        // Path 1: Hardware DXVA2/D3D11 NV12 texture -> GPU VideoProcessor (NV12 -> BGRA/RGBA)!
        ComPtr<IMFDXGIBuffer> dxgi_buf;
        if (SUCCEEDED(buf.As(&dxgi_buf)) && dxgi_buf && d3d_dev_ && d3d_ctx_) {
            ComPtr<ID3D11Texture2D> tex;
            UINT subres = 0;
            if (SUCCEEDED(dxgi_buf->GetResource(IID_PPV_ARGS(&tex))) && tex &&
                SUCCEEDED(dxgi_buf->GetSubresourceIndex(&subres))) {
                D3D11_TEXTURE2D_DESC desc{};
                tex->GetDesc(&desc);
                const int w = std::min(display_w_, int(desc.Width));
                const int h = std::min(display_h_, int(desc.Height));

                // When returning a CPU Image on an AVX2 host, reading the compact 1.5 Bpp NV12
                // DXVA surface (3.1 MB at 1080p vs 8.3 MB BGRA) + 0.38 ms AVX2 conversion avoids
                // the cross-engine VCN->3D/VP fence stall and cuts decode latency by ~2-3 ms.
                // On non-AVX2 hosts (or non-NV12 formats such as P010), use ID3D11VideoProcessor.
                const bool prefer_avx2_nv12 =
                    (desc.Format == DXGI_FORMAT_NV12) && cpu_has_avx2() && simd_enabled();
                if (!prefer_avx2_nv12 &&
                    nv12_to_rgb_.convert(d3d_dev_.Get(), d3d_ctx_.Get(), tex.Get(), subres,
                                         int(desc.Width), int(desc.Height), w, h, order,
                                         out_image)) {
                    return true;
                }

                // Compact NV12 DXVA staging readback (1.5 Bpp) + AVX2/SIMD BT.601 -> BGRA/RGBA.
                if (desc.Format == DXGI_FORMAT_NV12) {
                    if (!staging_tex_ || staging_w_ != desc.Width || staging_h_ != desc.Height ||
                        staging_fmt_ != desc.Format) {
                        staging_tex_.Reset();
                        D3D11_TEXTURE2D_DESC sdesc = desc;
                        sdesc.ArraySize = 1;
                        sdesc.MipLevels = 1;
                        sdesc.BindFlags = 0;
                        sdesc.MiscFlags = 0;
                        sdesc.Usage = D3D11_USAGE_STAGING;
                        sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        if (SUCCEEDED(d3d_dev_->CreateTexture2D(&sdesc, nullptr, &staging_tex_))) {
                            staging_w_ = desc.Width;
                            staging_h_ = desc.Height;
                            staging_fmt_ = desc.Format;
                        }
                    }
                    if (staging_tex_) {
                        d3d_ctx_->CopySubresourceRegion(staging_tex_.Get(), 0, 0, 0, 0, tex.Get(),
                                                        subres, nullptr);
                        D3D11_MAPPED_SUBRESOURCE mapped{};
                        if (SUCCEEDED(
                                d3d_ctx_->Map(staging_tex_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                            const uint8_t* y_ptr = static_cast<const uint8_t*>(mapped.pData);
                            const int pitch = int(mapped.RowPitch);
                            const uint8_t* uv_ptr = y_ptr + size_t(desc.Height) * size_t(pitch);
                            nv12_to_packed_rgb(y_ptr, pitch, uv_ptr, pitch, w, h, order, out_image);
                            d3d_ctx_->Unmap(staging_tex_.Get(), 0);
                            return true;
                        }
                    }
                }

                // Fallback to GPU ID3D11VideoProcessor if NV12 staging failed.
                if (nv12_to_rgb_.convert(d3d_dev_.Get(), d3d_ctx_.Get(), tex.Get(), subres,
                                         int(desc.Width), int(desc.Height), w, h, order,
                                         out_image)) {
                    return true;
                }
            }
        }

        // Path 2: IMF2DBuffer::Lock2D (handles padded row pitch cleanly)
        ComPtr<IMF2DBuffer> buf2d;
        if (SUCCEEDED(buf.As(&buf2d)) && buf2d) {
            BYTE* scan0 = nullptr;
            LONG pitch = 0;
            if (SUCCEEDED(buf2d->Lock2D(&scan0, &pitch)) && scan0 && pitch > 0) {
                const uint8_t* uv_ptr = scan0 + size_t(coded_h_) * size_t(pitch);
                nv12_to_packed_rgb(scan0, int(pitch), uv_ptr, int(pitch), display_w_, display_h_,
                                   order, out_image);
                buf2d->Unlock2D();
                return true;
            }
        }

        // Path 3: Flat contiguous IMFMediaBuffer
        ComPtr<IMFMediaBuffer> contig;
        if (FAILED(sample->ConvertToContiguousBuffer(&contig)) || !contig) return false;
        BYTE* ptr = nullptr;
        DWORD max_len = 0, cur_len = 0;
        if (FAILED(contig->Lock(&ptr, &max_len, &cur_len)) || !ptr) return false;
        const int stride = default_stride_ > 0 ? default_stride_ : coded_w_;
        const size_t need = size_t(stride) * size_t(coded_h_) +
                            size_t(stride) * size_t((display_h_ + 1) / 2);
        bool ok = false;
        if (cur_len >= need) {
            const uint8_t* uv_ptr = ptr + size_t(coded_h_) * size_t(stride);
            nv12_to_packed_rgb(ptr, stride, uv_ptr, stride, display_w_, display_h_, order, out_image);
            ok = true;
        }
        contig->Unlock();
        return ok;
    }

    // Returns: 1 = decoded frame, 2 = stream format changed (retry), 0 = need more input / error
    int pull_decoded_frame_status(PixelOrder order, Image& out_image) {
        MFT_OUTPUT_DATA_BUFFER odb{};
        odb.dwStreamID = out_stream_id_;
        ComPtr<IMFSample> alloc_sample;
        const bool mft_provides =
            (out_stream_info_.dwFlags &
             (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        if (!mft_provides) {
            if (FAILED(MFCreateSample(&alloc_sample))) return 0;
            ComPtr<IMFMediaBuffer> mb;
            const DWORD cap = std::max<DWORD>(
                out_stream_info_.cbSize, DWORD(size_t(coded_w_) * size_t(coded_h_) * 2 + 4096));
            if (FAILED(MFCreateMemoryBuffer(cap, &mb))) return 0;
            alloc_sample->AddBuffer(mb.Get());
            odb.pSample = alloc_sample.Get();
        }

        DWORD status = 0;
        HRESULT hr = mft_->ProcessOutput(0, 1, &odb, &status);
        if (odb.pEvents) odb.pEvents->Release();

        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (mft_provides && odb.pSample) odb.pSample->Release();
            return select_nv12_output_type() ? 2 : 0;
        }
        if (FAILED(hr) || !odb.pSample) {
            if (mft_provides && odb.pSample) odb.pSample->Release();
            return 0;
        }

        ComPtr<IMFSample> out_sample;
        if (mft_provides) out_sample.Attach(odb.pSample);
        else out_sample = alloc_sample;

        return extract_nv12_from_sample(out_sample.Get(), order, out_image) ? 1 : 0;
    }

    bool pull_decoded_frame(PixelOrder order, Image& out_image) {
        for (int i = 0; i < 4; ++i) {
            int rc = pull_decoded_frame_status(order, out_image);
            if (rc == 1) return true;
            if (rc == 0) return false;
        }
        return false;
    }

    void shutdown() {
        if (mft_) {
            mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        }
        nv12_to_rgb_.reset();
        staging_tex_.Reset();
        event_gen_.Reset();
        mft_.Reset();
        dxgi_mgr_.Reset();
        d3d_ctx_.Reset();
        d3d_dev_.Reset();
        pending_need_input_ = 0;
    }

    MfSessionGuard mf_;
    VideoCodecId codec_;
    PixelOrder output_order_ = PixelOrder::kBGRA;
    int coded_w_ = 0;
    int coded_h_ = 0;
    int display_w_ = 0;
    int display_h_ = 0;
    int default_stride_ = 0;

    ComPtr<ID3D11Device> d3d_dev_;
    ComPtr<ID3D11DeviceContext> d3d_ctx_;
    ComPtr<IMFDXGIDeviceManager> dxgi_mgr_;
    D3D11Nv12ToRgbProcessor nv12_to_rgb_;
    ComPtr<ID3D11Texture2D> staging_tex_;
    UINT staging_w_ = 0;
    UINT staging_h_ = 0;
    DXGI_FORMAT staging_fmt_ = DXGI_FORMAT_UNKNOWN;

    ComPtr<IMFTransform> mft_;
    ComPtr<IMFMediaEventGenerator> event_gen_;
    DWORD in_stream_id_ = 0;
    DWORD out_stream_id_ = 0;
    MFT_OUTPUT_STREAM_INFO out_stream_info_{};
    bool is_async_ = false;
    bool is_hardware_ = false;
    int pending_need_input_ = 0;
    uint64_t frame_index_ = 0;
    std::string backend_name_;
};

}  // namespace

std::unique_ptr<IVideoEncoder> create_platform_video_encoder(VideoCodecId codec,
                                                             const VideoEncoderConfig& cfg) {
    if (codec != VideoCodecId::kH264 && codec != VideoCodecId::kH265) return nullptr;
    auto enc = std::make_unique<MfVideoEncoder>(codec);
    if (!enc->init(cfg)) return nullptr;
    return enc;
}

std::unique_ptr<IVideoDecoder> create_platform_video_decoder(VideoCodecId codec,
                                                             PixelOrder output_order) {
    if (codec != VideoCodecId::kH264 && codec != VideoCodecId::kH265) return nullptr;
    auto dec = std::make_unique<MfVideoDecoder>(codec);
    if (!dec->init(1920, 1080, output_order)) return nullptr;
    return dec;
}

void query_platform_codecs(std::vector<CodecCapability>& out) {
    MfSessionGuard mf;
    for (VideoCodecId cid : {VideoCodecId::kH264, VideoCodecId::kH265}) {
        CodecCapability cap;
        cap.codec = cid;
        cap.name = (cid == VideoCodecId::kH265) ? "H265" : "H264";
        if (mf.ok()) {
            MFT_REGISTER_TYPE_INFO type_info{MFMediaType_Video, codec_subtype(cid)};
            const auto encs = enumerate_mfts(MFT_CATEGORY_VIDEO_ENCODER, nullptr, &type_info, true);
            if (!encs.empty()) {
                cap.encoder_available = true;
                cap.encoder_hardware = encs.front().is_hardware;
                cap.encoder_backend =
                    encs.front().name + (encs.front().is_hardware ? " (HW)" : " (SW)");
            }
            const auto decs = enumerate_mfts(MFT_CATEGORY_VIDEO_DECODER, &type_info, nullptr, true);
            if (!decs.empty()) {
                cap.decoder_available = true;
                cap.decoder_hardware = decs.front().is_hardware;
                cap.decoder_backend =
                    decs.front().name + (decs.front().is_hardware ? " (HW)" : " (SW)");
            }
        }
        out.push_back(std::move(cap));
    }
}

}  // namespace lv

#endif  // _WIN32
