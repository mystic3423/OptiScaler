#pragma once
#include "DLSSDFeature.h"
#include <upscalers/IFeature_Dx12.h>
#include <shaders/rcas/RCAS_Dx12.h>
#include <string>
#include "DLSSDTiling.h"
#include <wrl/client.h>

class DLSSDFeatureDx12 : public DLSSDFeature, public IFeature_Dx12
{
  private:
    // Debug copies and immutable descriptors survive submitted work and DRS.
    struct DebugTexture
    {
        size_t tile;
        std::string key;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    };
    struct DebugDescriptors
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> source, destination;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    };
    std::vector<DebugTexture> _debugTextures;
    std::vector<DebugDescriptors> _debugDescriptors;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> _debugCopyRoot;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> _debugCopyPipeline;
    bool InitDebugCopy();
    ID3D12Resource* DebugTextureFor(size_t tile, const char* key, DXGI_FORMAT format,
                                  unsigned int width, unsigned int height, bool output);
    bool CopyDebugInput(ID3D12GraphicsCommandList* commands, const DLSSDTiling::CopyScope::Input& input,
                        ID3D12Resource* destination, const DLSSTile& tile);
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
        DLSSTiling::EvaluationScope<ID3D12Resource> createScope(p);
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
        p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, tiles[0].renderW);
        p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, RenderHeight());
        p->Set(NVSDK_NGX_Parameter_OutWidth, tiles[0].evalOutW);
        p->Set(NVSDK_NGX_Parameter_OutHeight, TargetHeight());
        p->Set(NVSDK_NGX_Parameter_PerfQualityValue, _tileQuality);
        p->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, _tileCreateFlags);
        p->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, DLSSDTiling::DebugIsolatedTextures ? 0 : 1);
        bool ok = true;
        for (size_t i = 0; i < tiles.size(); ++i)
        {
            NVSDK_NGX_Handle* handle = nullptr;
            const auto result = create(&handle);
            if (result != NVSDK_NGX_Result_Success || handle == nullptr ||
                std::find(_croppedTileHandles.begin(), _croppedTileHandles.end(), handle) != _croppedTileHandles.end() ||
                std::find(_tileHandles.begin(), _tileHandles.end(), handle) != _tileHandles.end())
            {
                LOG_ERROR("Expanded RR tile {} creation failed: {:X}", i, (unsigned int) result);
                if (handle != nullptr &&
                    std::find(_croppedTileHandles.begin(), _croppedTileHandles.end(), handle) == _croppedTileHandles.end() &&
                    std::find(_tileHandles.begin(), _tileHandles.end(), handle) == _tileHandles.end())
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
            LOG_INFO("Created experimental integer-crop RR tiles: {} -> {} pixels per tile",
                     tiles[0].renderW, tiles[0].evalOutW);
        return ok;
    }

    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> _cropOutputs;
    ID3D12Resource* CropOutput(ID3D12Resource* output, unsigned int width, unsigned int height);
    bool EvaluateTiles(ID3D12GraphicsCommandList* commands, NVSDK_NGX_Parameter* parameters);
    bool ValidateTileInputs(NVSDK_NGX_Parameter* parameters);
    unsigned int _diagnosticWidth = 0, _diagnosticHeight = 0;
    unsigned int _diagnosticSnapshots = 0;
    bool _loggedEvaluationFailure = false;
    void LogDiagnostics(NVSDK_NGX_Parameter* parameters, const char* phase, bool resources);

  protected:
    bool InitDLSSD(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters);

  public:
    bool InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;
    bool EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;

    feature_version Version() override { return DLSSDFeature::Version(); }
    Upscaler GetUpscalerType() const final { return DLSSDFeature::GetUpscalerType(); }
    API Api() const override { return IFeature_Dx12::Api(); }
    bool CallsUpscalerEndByItself() override { return IFeature_Dx12::CallsUpscalerEndByItself(); }

    bool IsWithDx12() override { return false; }

    DLSSDFeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters);
    ~DLSSDFeatureDx12();
};
