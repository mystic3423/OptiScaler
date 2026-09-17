#pragma once

// -----------------------------------------------------------------------------
// Split-frame DLSS tiling.
//
// Works around the DLSS Super Resolution D3D12 limit that rejects an output
// width above 8192 (NVSDK_NGX_Result_FAIL_InvalidParameter / 0xBAD00005).
// Instead of one feature at 11520x2160, create N features of 11520/N wide and
// point each at a horizontal subrect of the SAME game-supplied color / depth /
// motion-vector textures, writing into subrects of the SAME output texture.
//
// No tile copies and no scratch allocations: NGX's subrect parameters do the
// addressing. Because every tile reads the untouched full-frame resources, the
// motion vectors stay in the full-frame coordinate space, so MV.Scale.X/Y are
// passed through unchanged.
//
// Phase 1 is a hard split with zero overlap. Tiles are aligned to physical
// monitor boundaries so each seam falls behind a bezel.
// -----------------------------------------------------------------------------

#include <vector>
#include <cstdlib>
#include <Windows.h>

// Self-sufficient: don't rely on DLSSFeature.h's include order.
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>

struct DLSSTile
{
    unsigned int renderX;  // subrect base X into the game's render-res inputs
    unsigned int renderW;
    unsigned int outX;     // subrect base X into the game's output texture
    unsigned int outW;
};

namespace DLSSTiling
{
// OPTI_DLSS_TILES=3 enables a 3-way split. Unset / 1 keeps stock behaviour.
inline unsigned int TileCountFromEnv()
{
    return 3;
    char buf[16] = {};
    DWORD n = GetEnvironmentVariableA("OPTI_DLSS_TILES", buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf))
        return 1;

    int v = atoi(buf);
    if (v < 1)
        return 1;
    if (v > 8)
        return 8;
    return static_cast<unsigned int>(v);
}

// Equal split. Returns false if the split is not exact in both render and
// output space: a fractional tile would desync the render and output subrects,
// which NGX has no way to express.
//
// Triple 4K divides cleanly: 11520/3 = 3840 out, and at Quality 7680/3 = 2560
// render, at Performance 5760/3 = 1920. Bezel-corrected Surround does NOT
// divide cleanly - check your actual Surround width before trusting this.
inline bool BuildTiles(unsigned int renderW, unsigned int outW, unsigned int tileCount,
                       std::vector<DLSSTile>& outTiles)
{
    outTiles.clear();

    if (tileCount <= 1)
        return false;
    if (renderW % tileCount != 0 || outW % tileCount != 0)
        return false;

    const unsigned int tileRenderW = renderW / tileCount;
    const unsigned int tileOutW = outW / tileCount;

    for (unsigned int i = 0; i < tileCount; i++)
        outTiles.push_back({ tileRenderW * i, tileRenderW, tileOutW * i, tileOutW });

    return true;
}

// EVERY input texture DLSS accepts carries its own subrect base, and all of
// them index the SAME full-frame resources. Offsetting only color, depth and
// motion vectors leaves a tile reading the reactive/bias mask, translucency,
// transparency layers and disocclusion mask from the frame's LEFT EDGE - which
// is accidentally correct for tile 0 (whose offset is 0) and wrong for every
// other tile. That mis-sampled mask tells DLSS which pixels to trust from
// history, so getting it wrong shows up as motion blur and aliasing on every
// tile except the first.
//
// Setting a base whose texture the game never supplied is harmless: DLSS keys
// off the resource pointer, not the subrect.
//
// Y bases are deliberately untouched - this is a horizontal split, so every
// tile keeps whatever vertical base the game set.
// isMotionVector marks the textures that live at DISPLAY resolution whenever
// the game does not set NVSDK_NGX_DLSS_Feature_Flags_MVLowRes. Those need the
// tile's OUTPUT offset, not its render offset - and the two coincide only for
// tile 0, which is why a display-res-MV game looks correct on the first tile
// and smears on every other one.
struct SubrectParam
{
    const char* name;
    bool isMotionVector;
};

static constexpr SubrectParam kInputSubrects[] = {
    { NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, false },
    { NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, false },
    { NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, true },
    { NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_X, false },
    { NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_X, false },
    { NVSDK_NGX_Parameter_DLSS_TransparencyLayer_Subrect_Base_X, false },
    { NVSDK_NGX_Parameter_DLSS_TransparencyLayerOpacity_Subrect_Base_X, false },
    { NVSDK_NGX_Parameter_DLSS_TransparencyLayerMvecs_Subrect_Base_X, true },
    { NVSDK_NGX_Parameter_DLSS_DisocclusionMask_Subrect_Base_X, false },
};

static constexpr size_t kInputSubrectCount = sizeof(kInputSubrects) / sizeof(kInputSubrects[0]);

// The game's own subrect values must be restored after each tiled evaluate:
// OptiScaler reads DLSS.Render.Subrect.Dimensions.* back in
// IFeature::GetRenderResolution(), so leaving a tile's value behind would make
// the next frame think the render resolution shrank.
struct SubrectBackup
{
    unsigned int inX[kInputSubrectCount] = {};
    unsigned int outX = 0;
    unsigned int subW = 0, subH = 0;
    bool hadSubDims = false;
};

inline void Save(NVSDK_NGX_Parameter* p, SubrectBackup& b)
{
    for (size_t i = 0; i < kInputSubrectCount; i++)
    {
        // Get may leave the value untouched when the parameter was never set,
        // so an unsupplied texture is pinned to 0 and restores to 0.
        if (p->Get(kInputSubrects[i].name, &b.inX[i]) != NVSDK_NGX_Result_Success)
            b.inX[i] = 0;
    }

    if (p->Get(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, &b.outX) != NVSDK_NGX_Result_Success)
        b.outX = 0;

    b.hadSubDims = p->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &b.subW) ==
                       NVSDK_NGX_Result_Success &&
                   p->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &b.subH) ==
                       NVSDK_NGX_Result_Success;

    // A game that never sets the render subrect dimensions still has to get
    // whole-frame values back, because IFeature::GetRenderResolution() reads
    // these FIRST and treats them as the render resolution. Leaving a tile's
    // value behind makes the next feature init believe the game is rendering
    // at one third width - which then fails the divisibility check and builds
    // a single feature sized to one tile.
    if (!b.hadSubDims)
    {
        if (p->Get(NVSDK_NGX_Parameter_Width, &b.subW) != NVSDK_NGX_Result_Success)
            b.subW = 0;
        if (p->Get(NVSDK_NGX_Parameter_Height, &b.subH) != NVSDK_NGX_Result_Success)
            b.subH = 0;
    }
}

inline void Restore(NVSDK_NGX_Parameter* p, const SubrectBackup& b)
{
    for (size_t i = 0; i < kInputSubrectCount; i++)
        p->Set(kInputSubrects[i].name, b.inX[i]);

    p->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, b.outX);

    // Unconditional: see the note in Save(). Skipping this is what let a tile's
    // dimensions leak into the next feature creation.
    if (b.subW != 0 && b.subH != 0)
    {
        p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, b.subW);
        p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, b.subH);
    }
}

// Point one evaluate at a single tile. Offsets are ADDED to whatever base the
// game already supplied, so a game that itself renders into a subrect of a
// larger atlas still works.
//
// lowResMV comes from the game's MVLowRes feature flag (IFeature::LowResMV()).
// When it is false the motion vectors are at display resolution and need the
// output offset instead of the render offset.
inline void ApplyTile(NVSDK_NGX_Parameter* p, const SubrectBackup& base, const DLSSTile& t, unsigned int renderH,
                      bool lowResMV)
{
    const unsigned int mvOffset = lowResMV ? t.renderX : t.outX;

    for (size_t i = 0; i < kInputSubrectCount; i++)
        p->Set(kInputSubrects[i].name, base.inX[i] + (kInputSubrects[i].isMotionVector ? mvOffset : t.renderX));

    p->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, base.outX + t.outX);

    p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, t.renderW);
    p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, renderH);
}
} // namespace DLSSTiling
