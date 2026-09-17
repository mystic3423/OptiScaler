#pragma once

#include "SysUtils.h"
#include <proxies/NVNGX_Proxy.h>
#include <upscalers/IFeature.h>
#include "DLSSTiling.h"

class DLSSFeature : public virtual IFeature
{
  private:
    feature_version _version = { 0, 0, 0 };

  protected:
    NVSDK_NGX_Handle _dlssHandle = {};
    NVSDK_NGX_Handle* _p_dlssHandle = nullptr;
    inline static bool _dlssInited = false;

    // Split-frame tiling. _tileHandles is empty when tiling is off, in which
    // case _p_dlssHandle is the single feature and behaviour is stock.
    std::vector<NVSDK_NGX_Handle*> _tileHandles;
    std::vector<DLSSTile> _tiles;
    std::vector<NVSDK_NGX_Handle*> _croppedTileHandles;
    unsigned int _tileOutputHeight = 0;
    int _tileCreateFlags = 0;
    unsigned int _tileQuality = 0;
    unsigned int _lastTileRenderW = 0, _lastTileRenderH = 0;
    bool _lastTilesCropped = false;
    bool _tileHistoryValid = false;

    // Keep a second set only after the first uneven frame. Reusing both sets
    // avoids repeated creation as dynamic resolution crosses multiples of 3.
    template <typename Create, typename Release>
    bool EnsureCroppedTileHandles(NVSDK_NGX_Parameter* p, const std::vector<DLSSTile>& tiles,
                                  Create create, Release release)
    {
        if (!_croppedTileHandles.empty())
            return true;
        unsigned int width = 0, height = 0, outWidth = 0, outHeight = 0, quality = 0;
        int flags = 0, subrects = 0;
        p->Get(NVSDK_NGX_Parameter_Width, &width);
        p->Get(NVSDK_NGX_Parameter_Height, &height);
        p->Get(NVSDK_NGX_Parameter_OutWidth, &outWidth);
        p->Get(NVSDK_NGX_Parameter_OutHeight, &outHeight);
        p->Get(NVSDK_NGX_Parameter_PerfQualityValue, &quality);
        p->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags);
        p->Get(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, &subrects);
        p->Set(NVSDK_NGX_Parameter_Width, tiles[0].renderW);
        p->Set(NVSDK_NGX_Parameter_Height, RenderHeight());
        p->Set(NVSDK_NGX_Parameter_OutWidth, tiles[0].evalOutW);
        p->Set(NVSDK_NGX_Parameter_OutHeight, TargetHeight());
        p->Set(NVSDK_NGX_Parameter_PerfQualityValue, _tileQuality);
        p->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, _tileCreateFlags);
        p->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 1);
        bool ok = true;
        for (size_t i = 0; i < tiles.size(); ++i)
        {
            NVSDK_NGX_Handle* handle = nullptr;
            const auto result = create(&handle);
            if (result != NVSDK_NGX_Result_Success || handle == nullptr)
            {
                LOG_ERROR("Expanded DLSS tile {} creation failed: {:X}", i, (unsigned int) result);
                if (handle != nullptr)
                    release(handle);
                for (auto* h : _croppedTileHandles)
                    release(h);
                _croppedTileHandles.clear();
                ok = false;
                break;
            }
            _croppedTileHandles.push_back(handle);
        }
        p->Set(NVSDK_NGX_Parameter_Width, width);
        p->Set(NVSDK_NGX_Parameter_Height, height);
        p->Set(NVSDK_NGX_Parameter_OutWidth, outWidth);
        p->Set(NVSDK_NGX_Parameter_OutHeight, outHeight);
        p->Set(NVSDK_NGX_Parameter_PerfQualityValue, quality);
        p->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, flags);
        p->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, subrects);
        if (ok)
            LOG_INFO("Created experimental integer-crop DLSS tiles: {} -> {} pixels per tile",
                     tiles[0].renderW, tiles[0].evalOutW);
        return ok;
    }

    void ProcessEvaluateParams(NVSDK_NGX_Parameter* InParameters);
    void ProcessInitParams(NVSDK_NGX_Parameter* InParameters);
    void ReadVersion();

    static void Shutdown();
    float GetSharpness(const NVSDK_NGX_Parameter* InParameters);

  public:
    feature_version Version() override { return feature_version { _version.major, _version.minor, _version.patch }; }
    Upscaler GetUpscalerType() const override { return Upscaler::DLSS; }

    DLSSFeature(unsigned int handleId, NVSDK_NGX_Parameter* InParameters);

    ~DLSSFeature();
};
