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
