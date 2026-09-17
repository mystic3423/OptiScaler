#pragma once
#include <upscalers/IFeature_Dx11.h>
#include "DLSSFeature.h"
#include <string>
#include <wrl/client.h>

class DLSSFeatureDx11 : public DLSSFeature, public IFeature_Dx11
{
  private:
    // Cache by output format/size until feature destruction (also safe for
    // deferred contexts whose command lists still reference an older output).
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> _cropOutputs;
    ID3D11Texture2D* CropOutput(ID3D11Resource* output, unsigned int width, unsigned int height);
  protected:
  public:
    bool InitInternal(ID3D11DeviceContext* InContext, NVSDK_NGX_Parameter* InParameters) override;
    bool EvaluateInternal(ID3D11DeviceContext* InDeviceContext, NVSDK_NGX_Parameter* InParameters) override;

    feature_version Version() override { return DLSSFeature::Version(); }
    Upscaler GetUpscalerType() const final { return DLSSFeature::GetUpscalerType(); }
    API Api() const override { return IFeature_Dx11::Api(); }
    bool CallsUpscalerEndByItself() override { return IFeature_Dx11::CallsUpscalerEndByItself(); }

    bool IsWithDx12() override { return false; }

    DLSSFeatureDx11(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters);
    ~DLSSFeatureDx11();
};
