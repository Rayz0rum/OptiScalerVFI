#pragma once

// The colour codec, on Direct3D 11.
//
// This is the D3D12 codec's twin: same shader, same constants, same three modes. Only the binding
// model differs, and D3D11's is the simpler one -- views instead of a descriptor heap, a constant
// buffer instead of root constants, no resource states to track.
//
// The shader itself is not copied. `codec::kShaderSource` in DlssNr_Codec.h is the single source for
// both, compiled here as cs_5_0 rather than cs_5_1 -- 5.1's additions (descriptor spaces, unbounded
// arrays) are not used by it, and 5.0 is what D3D11 accepts. If that shader ever grows a 5.1-only
// construct, this compile is where it will show up, immediately and at startup.

#include <d3d11.h>
#include <d3dcompiler.h>

#include <unordered_map>

#include "DlssNr_Codec.h"

namespace codec
{

// D3D11 cannot view a typeless resource either, so the typed member of the same family is substituted.
// Kept separate from the D3D12 side's table because that one lives in an anonymous namespace in its
// own translation unit; the two describe the same families and should be changed together.
inline DXGI_FORMAT TypedFormat11(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        // An sRGB view cannot back a typed UAV, and the shader applies the transfer function itself.
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        return f;
    }
}

class Codec11
{
  public:
    bool ensure(ID3D11Device* device)
    {
        if (shader_ != nullptr)
            return true;

        if (device == nullptr)
            return false;

        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;

        if (FAILED(D3DCompile(kShaderSource, strlen(kShaderSource), nullptr, nullptr, nullptr, "main",
                              "cs_5_0", 0, 0, &code, &errors)))
        {
            if (errors != nullptr)
                errors->Release();

            if (code != nullptr)
                code->Release();

            return false;
        }

        if (errors != nullptr)
            errors->Release();

        HRESULT hr = device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                                 &shader_);
        code->Release();

        if (FAILED(hr))
            return false;

        // Rounded up to a 16-byte multiple: D3D11 rejects a constant buffer that is not.
        D3D11_BUFFER_DESC cb = {};
        cb.ByteWidth = (sizeof(Params) + 15) & ~15u;
        cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        if (FAILED(device->CreateBuffer(&cb, nullptr, &constants_)))
        {
            release();
            return false;
        }

        D3D11_SAMPLER_DESC samp = {};
        samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samp.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samp.MaxLOD = D3D11_FLOAT32_MAX;

        if (FAILED(device->CreateSamplerState(&samp, &sampler_)))
        {
            release();
            return false;
        }

        device_ = device;
        return true;
    }

    void dispatch(ID3D11DeviceContext* ctx, const Params& constants, ID3D11Resource* source,
                  ID3D11Resource* model, ID3D11Resource* original, ID3D11Resource* target,
                  ID3D11Resource* keep, ID3D11Resource* motion = nullptr)
    {
        if (shader_ == nullptr || ctx == nullptr || source == nullptr || target == nullptr)
            return;

        D3D11_MAPPED_SUBRESOURCE mapped = {};

        if (FAILED(ctx->Map(constants_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return;

        memcpy(mapped.pData, &constants, sizeof(Params));
        ctx->Unmap(constants_, 0);

        // Absent optional inputs resolve to `source` rather than null. The shader indexes all four
        // unconditionally, and an unbound SRV reads as zero, which for the resolve's proxy would mean
        // dividing by a luminance of zero.
        ID3D11ShaderResourceView* srvs[4] = {
            srv(source), srv(model != nullptr ? model : source), srv(original != nullptr ? original : source),
            srv(motion != nullptr ? motion : source)
        };

        ID3D11UnorderedAccessView* uavs[2] = { uav(target), uav(keep != nullptr ? keep : target) };

        for (auto* v : srvs)
        {
            if (v == nullptr)
                return;
        }

        for (auto* v : uavs)
        {
            if (v == nullptr)
                return;
        }

        ctx->CSSetShader(shader_, nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, &constants_);
        ctx->CSSetSamplers(0, 1, &sampler_);
        ctx->CSSetShaderResources(0, 4, srvs);
        ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

        ctx->Dispatch((constants.width + 7) / 8, (constants.height + 7) / 8, 1);

        // Unbound before returning. The game's own rendering follows on this context, and leaving a
        // UAV bound to a texture it is about to use as a render target silently drops its draws.
        ID3D11ShaderResourceView* noSrv[4] = {};
        ID3D11UnorderedAccessView* noUav[2] = {};
        ctx->CSSetShaderResources(0, 4, noSrv);
        ctx->CSSetUnorderedAccessViews(0, 2, noUav, nullptr);
        ctx->CSSetShader(nullptr, nullptr, 0);
    }

    void release()
    {
        for (auto& [res, view] : srvs_)
        {
            if (view != nullptr)
                view->Release();
        }

        for (auto& [res, view] : uavs_)
        {
            if (view != nullptr)
                view->Release();
        }

        srvs_.clear();
        uavs_.clear();

        if (sampler_ != nullptr)
        {
            sampler_->Release();
            sampler_ = nullptr;
        }

        if (constants_ != nullptr)
        {
            constants_->Release();
            constants_ = nullptr;
        }

        if (shader_ != nullptr)
        {
            shader_->Release();
            shader_ = nullptr;
        }

        device_ = nullptr;
    }

    // Drops the views naming a resource that is about to be freed. The cache is keyed by pointer, and
    // a released texture's address is a perfectly good key for the next allocation to land on.
    void forget(ID3D11Resource* res)
    {
        if (auto it = srvs_.find(res); it != srvs_.end())
        {
            if (it->second != nullptr)
                it->second->Release();

            srvs_.erase(it);
        }

        if (auto it = uavs_.find(res); it != uavs_.end())
        {
            if (it->second != nullptr)
                it->second->Release();

            uavs_.erase(it);
        }
    }

    ~Codec11() { release(); }

  private:
    static DXGI_FORMAT formatOf(ID3D11Resource* res)
    {
        ID3D11Texture2D* tex = nullptr;

        if (FAILED(res->QueryInterface(IID_PPV_ARGS(&tex))) || tex == nullptr)
            return DXGI_FORMAT_UNKNOWN;

        D3D11_TEXTURE2D_DESC desc = {};
        tex->GetDesc(&desc);
        tex->Release();

        return TypedFormat11(desc.Format);
    }

    ID3D11ShaderResourceView* srv(ID3D11Resource* res)
    {
        if (res == nullptr)
            return nullptr;

        if (auto it = srvs_.find(res); it != srvs_.end())
            return it->second;

        D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
        desc.Format = formatOf(res);
        desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipLevels = 1;

        ID3D11ShaderResourceView* view = nullptr;

        if (desc.Format == DXGI_FORMAT_UNKNOWN || device_ == nullptr ||
            FAILED(device_->CreateShaderResourceView(res, &desc, &view)))
        {
            return nullptr;
        }

        srvs_[res] = view;
        return view;
    }

    ID3D11UnorderedAccessView* uav(ID3D11Resource* res)
    {
        if (res == nullptr)
            return nullptr;

        if (auto it = uavs_.find(res); it != uavs_.end())
            return it->second;

        D3D11_UNORDERED_ACCESS_VIEW_DESC desc = {};
        desc.Format = formatOf(res);
        desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

        ID3D11UnorderedAccessView* view = nullptr;

        if (desc.Format == DXGI_FORMAT_UNKNOWN || device_ == nullptr ||
            FAILED(device_->CreateUnorderedAccessView(res, &desc, &view)))
        {
            return nullptr;
        }

        uavs_[res] = view;
        return view;
    }

    ID3D11Device* device_ = nullptr;
    ID3D11ComputeShader* shader_ = nullptr;
    ID3D11Buffer* constants_ = nullptr;
    ID3D11SamplerState* sampler_ = nullptr;

    std::unordered_map<ID3D11Resource*, ID3D11ShaderResourceView*> srvs_;
    std::unordered_map<ID3D11Resource*, ID3D11UnorderedAccessView*> uavs_;
};

} // namespace codec
