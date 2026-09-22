#pragma once

#include "../dlss/DLSSTiling.h"
#include <array>
#include <string>
#include <cstring>

namespace DLSSDTiling
{
// Temporary diagnostic only. Do not carry this copy path into the SR branch or
// treat it as the final RR implementation.
inline constexpr bool DebugIsolatedTextures = true;
// Optimization experiment: keep the proven isolated inputs/camera, but write
// divisible tiles directly to the game's output. Set true for reference 68165B43.
// Uneven integer-crop tiles still require an intermediate output.
inline constexpr bool DebugPrivateOutput = false;
// Residual skin/shadow-transition control: restore color/depth copies too.
// False restores A483E43E's direct color/depth; keep MV/material copies enabled.
inline constexpr bool DebugCopyCoreInputs = true;
inline constexpr bool DebugCopyMotionVectors = true;
// Skin/shadow-transition diagnostic: restore B0973BAD's copied material guides.
// Whether direct albedos + normals/roughness caused the artifact is unconfirmed.
inline constexpr bool DebugCopyMaterialGuides = true;
inline bool CopyInput(const char* key)
{
    if (std::strcmp(key, "Color") == 0 || std::strcmp(key, "Depth") == 0)
        return DebugCopyCoreInputs;
    if (std::strcmp(key, "MotionVectors") == 0)
        return DebugCopyCoreInputs || DebugCopyMotionVectors;
    if (std::strcmp(key, "DLSS.Input.DiffuseAlbedo") == 0 ||
        std::strcmp(key, "DLSS.Input.SpecularAlbedo") == 0 ||
        std::strcmp(key, "GBuffer.Normals") == 0 || std::strcmp(key, "GBuffer.Roughness") == 0)
        return DebugCopyMaterialGuides;
    return true;
}
inline constexpr char DebugCopyShader[] = R"(
Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Destination : register(u0);
cbuffer Region : register(b0) { uint2 Origin; uint2 Size; };
[numthreads(8, 8, 1)]
void CSMain(uint3 pixel : SV_DispatchThreadID)
{
    if (all(pixel.xy < Size))
        Destination[pixel.xy] = Source.Load(int3(pixel.xy + Origin, 0));
}
)";
// RR-only names from NVIDIA's nvsdk_ngx_defs_dlssd.h. Keep this table separate
// from SR: these guides all use render coordinates, including packed roughness.
struct Guide { const char* resource; const char* base; };
inline constexpr Guide coreInputs[] = {
    { "Color", "DLSS.Input.Color" },
    { "Depth", "DLSS.Input.Depth" },
    { "MotionVectors", "DLSS.Input.MV" },
    { "DLSS.Input.Bias.Current.Color.Mask", "DLSS.Input.Bias.Current.Color" },
    { "TransparencyMask", "DLSS.Input.Translucency" },
    { "DLSS.TransparencyLayer", "DLSS.TransparencyLayer" },
    { "DLSS.TransparencyLayerOpacity", "DLSS.TransparencyLayerOpacity" },
    { "DLSS.TransparencyLayerMvecs", "DLSS.TransparencyLayerMvecs" },
    { "DLSS.DisocclusionMask", "DLSS.DisocclusionMask" },
};
inline constexpr Guide guides[] = {
    { "DLSS.Input.DiffuseAlbedo", "DLSS.Input.DiffuseAlbedo" },
    { "DLSS.Input.SpecularAlbedo", "DLSS.Input.SpecularAlbedo" },
    { "GBuffer.Normals", "DLSS.Input.Normals" },
    { "GBuffer.Roughness", "DLSS.Input.Roughness" },
    { "DLSSD.Alpha", "DLSSD.Alpha" },
    { "DLSSD.ReflectedAlbedo", "DLSSD.ReflectedAlbedo" },
    { "DLSSD.ColorBeforeParticles", "DLSSD.ColorBeforeParticles" },
    { "DLSSD.ColorAfterParticles", "DLSSD.ColorAfterParticles" },
    { "DLSSD.ColorBeforeTransparency", "DLSSD.ColorBeforeTransparency" },
    { "DLSSD.ColorAfterTransparency", "DLSSD.ColorAfterTransparency" },
    { "DLSSD.ColorBeforeFog", "DLSSD.ColorBeforeFog" },
    { "DLSSD.ColorAfterFog", "DLSSD.ColorAfterFog" },
    { "DLSSD.ScreenSpaceSubsurfaceScatteringGuide", "DLSSD.ScreenSpaceSubsurfaceScatteringGuide" },
    { "DLSSD.ColorBeforeScreenSpaceSubsurfaceScattering", "DLSSD.ColorBeforeScreenSpaceSubsurfaceScattering" },
    { "DLSSD.ColorAfterScreenSpaceSubsurfaceScattering", "DLSSD.ColorAfterScreenSpaceSubsurfaceScattering" },
    { "DLSSD.ScreenSpaceRefractionGuide", "DLSSD.ScreenSpaceRefractionGuide" },
    { "DLSSD.ColorBeforeScreenSpaceRefraction", "DLSSD.ColorBeforeScreenSpaceRefraction" },
    { "DLSSD.ColorAfterScreenSpaceRefraction", "DLSSD.ColorAfterScreenSpaceRefraction" },
    { "DLSSD.DepthOfFieldGuide", "DLSSD.DepthOfFieldGuide" },
    { "DLSSD.ColorBeforeDepthOfField", "DLSSD.ColorBeforeDepthOfField" },
    { "DLSSD.ColorAfterDepthOfField", "DLSSD.ColorAfterDepthOfField" },
    { "DLSSD.DiffuseHitDistance", "DLSSD.DiffuseHitDistance" },
    { "DLSSD.SpecularHitDistance", "DLSSD.SpecularHitDistance" },
    { "DLSSD.DiffuseRayDirection", "DLSSD.DiffuseRayDirection" },
    { "DLSSD.SpecularRayDirection", "DLSSD.SpecularRayDirection" },
    { "DLSSD.DiffuseRayDirectionHitDistance", "DLSSD.DiffuseRayDirectionHitDistance" },
    { "DLSSD.SpecularRayDirectionHitDistance", "DLSSD.SpecularRayDirectionHitDistance" },
    { "DLSSD.ResponsivityMask", "DLSSD.ResponsivityMask" },
};

// Row-major, row-vector NGX/Streamline projection: clip.x' = scale*clip.x +
// shift*clip.w. This maps the tile's interval of full-frame NDC onto [-1,1].
// Origins are relative to the active frame, not texture allocation/base offsets.
inline std::array<float, 16> TileProjection(const float* full, unsigned int fullWidth,
                                          unsigned int origin, unsigned int width)
{
    std::array<float, 16> result;
    std::memcpy(result.data(), full, sizeof(float) * 16);
    const float scale = static_cast<float>(fullWidth) / width;
    const float shift = (static_cast<float>(fullWidth) - 2.0f * origin - width) / width;
    for (unsigned int row = 0; row < 4; ++row)
        result[row * 4] = full[row * 4] * scale + full[row * 4 + 3] * shift;
    return result;
}

// Captures resources AND both bases before any per-tile changes. Scope lifetime
// covers all tiles; destruction restores caller parameters after any early exit.
class CopyScope
{
    NVSDK_NGX_Parameter* _params;
    static constexpr const char* dimensions[] = { "Width", "Height", "OutWidth", "OutHeight" };
    unsigned int _dimensions[4] {};
  public:
    struct Input
    {
        const char* key;
        std::string xKey, yKey;
        ID3D12Resource* source;
        unsigned int x = 0, y = 0;
    };
    std::vector<Input> inputs;
    explicit CopyScope(NVSDK_NGX_Parameter* p) : _params(p)
    {
        auto capture = [&](const Guide& guide) {
            ID3D12Resource* resource = nullptr;
            if (p->Get(guide.resource, &resource) != NVSDK_NGX_Result_Success || !resource)
                return;
            Input input { guide.resource, std::string(guide.base) + ".Subrect.Base.X",
                          std::string(guide.base) + ".Subrect.Base.Y", resource };
            p->Get(input.xKey.c_str(), &input.x);
            p->Get(input.yKey.c_str(), &input.y);
            inputs.push_back(input);
        };
        for (const auto& guide : coreInputs) capture(guide);
        for (const auto& guide : guides) capture(guide);
        for (size_t i = 0; i < 4; ++i) p->Get(dimensions[i], &_dimensions[i]);
    }
    void Bind(const Input& input, ID3D12Resource* isolated)
    {
        _params->Set(input.key, isolated);
        _params->Set(input.xKey.c_str(), 0u);
        _params->Set(input.yKey.c_str(), 0u);
    }
    void Dimensions(unsigned int w, unsigned int h, unsigned int outW, unsigned int outH)
    {
        const unsigned int values[] = { w, h, outW, outH };
        for (size_t i = 0; i < 4; ++i) _params->Set(dimensions[i], values[i]);
    }
    ~CopyScope()
    {
        for (const auto& input : inputs)
        {
            _params->Set(input.key, input.source);
            _params->Set(input.xKey.c_str(), input.x);
            _params->Set(input.yKey.c_str(), input.y);
        }
        for (size_t i = 0; i < 4; ++i) _params->Set(dimensions[i], _dimensions[i]);
    }
    CopyScope(const CopyScope&) = delete;
    CopyScope& operator=(const CopyScope&) = delete;
};

class EvaluationScope
{
    NVSDK_NGX_Parameter* _params;
    struct Base { std::string x; unsigned int value = 0; bool active = false; };
    std::array<Base, sizeof(guides) / sizeof(guides[0])> _bases;
    void* _projection = nullptr;
    std::array<float, 16> _tileProjection {};

  public:
    // Preserve the game's camera for the shared-texture/subrect experiment.
    // A mathematically valid crop is not proof that NGX expects tile-local NDC.
    explicit EvaluationScope(NVSDK_NGX_Parameter* params, bool cropProjection = false) : _params(params)
    {
        for (size_t i = 0; i < _bases.size(); ++i)
        {
            auto& base = _bases[i];
            base.x = std::string(guides[i].base) + ".Subrect.Base.X";
            ID3D12Resource* resource = nullptr;
            base.active = params->Get(guides[i].resource, &resource) == NVSDK_NGX_Result_Success && resource;
            params->Get(base.x.c_str(), &base.value);
        }
        if (cropProjection)
            params->Get("ViewToClipMatrix", &_projection);
    }
    ~EvaluationScope()
    {
        for (const auto& base : _bases)
            if (base.active)
                _params->Set(base.x.c_str(), base.value);
        if (_projection)
            _params->Set("ViewToClipMatrix", _projection);
    }
    void Apply(const DLSSTile& tile, unsigned int fullWidth)
    {
        for (const auto& base : _bases)
            if (base.active)
                _params->Set(base.x.c_str(), base.value + tile.renderX);
        if (_projection)
        {
            _tileProjection = TileProjection(static_cast<const float*>(_projection), fullWidth,
                                            tile.renderX, tile.renderW);
            _params->Set("ViewToClipMatrix", static_cast<void*>(_tileProjection.data()));
        }
    }
    EvaluationScope(const EvaluationScope&) = delete;
    EvaluationScope& operator=(const EvaluationScope&) = delete;
};
}
