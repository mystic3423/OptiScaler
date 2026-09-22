#include <pch.h>
#include "DLSSFeature_Dx11.h"
#include <Config.h>

#include <dxgi.h>

ID3D11Texture2D* DLSSFeatureDx11::CropOutput(ID3D11Resource* output, unsigned int width, unsigned int height)
{
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (FAILED(output->QueryInterface(IID_PPV_ARGS(&texture))))
        return nullptr;
    D3D11_TEXTURE2D_DESC original {};
    texture->GetDesc(&original);
    for (const auto& cached : _cropOutputs)
    {
        D3D11_TEXTURE2D_DESC desc {};
        cached->GetDesc(&desc);
        if (desc.Width == width && desc.Height == height && desc.Format == original.Format)
            return cached.Get();
    }
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = original.Format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> scratch;
    const auto result = Device->CreateTexture2D(&desc, nullptr, &scratch);
    if (FAILED(result))
    {
        LOG_ERROR("Integer-crop DLSS output allocation failed: {:X}", (unsigned int) result);
        return nullptr;
    }
    _cropOutputs.push_back(scratch);
    return scratch.Get();
}

bool DLSSFeatureDx11::InitInternal(ID3D11DeviceContext* InContext, NVSDK_NGX_Parameter* InParameters)
{
    if (NVNGXProxy::NVNGXModule() == nullptr)
    {
        LOG_ERROR("nvngx.dll not loaded!");

        SetInit(false);
        return false;
    }

    NVSDK_NGX_Result nvResult;
    bool initResult = false;

    do
    {
        if (!_dlssInited)
        {
            _dlssInited = NVNGXProxy::InitDx11(Device);

            if (!_dlssInited)
                return false;

            _moduleLoaded =
                (NVNGXProxy::D3D11_Init_ProjectID() != nullptr || NVNGXProxy::D3D11_Init_Ext() != nullptr) &&
                (NVNGXProxy::D3D11_Shutdown() != nullptr || NVNGXProxy::D3D11_Shutdown1() != nullptr) &&
                (NVNGXProxy::D3D11_GetParameters() != nullptr || NVNGXProxy::D3D11_AllocateParameters() != nullptr) &&
                NVNGXProxy::D3D11_DestroyParameters() != nullptr && NVNGXProxy::D3D11_CreateFeature() != nullptr &&
                NVNGXProxy::D3D11_ReleaseFeature() != nullptr && NVNGXProxy::D3D11_EvaluateFeature() != nullptr;

            // delay between init and create feature
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        LOG_INFO("Creating DLSS feature");

        if (NVNGXProxy::D3D11_CreateFeature() == nullptr)
        {
            LOG_ERROR("NVNGXProxy::D3D11_CreateFeature is nullptr");
            break;
        }

        ProcessInitParams(InParameters);

        _tileOutputHeight = TargetHeight();
        InParameters->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &_tileCreateFlags);
        InParameters->Get(NVSDK_NGX_Parameter_PerfQualityValue, &_tileQuality);

        // Create the direct-output set at nominal integer dimensions. On an
        // uneven first frame this set is retained for later divisible frames;
        // evaluation creates the expanded set and uses the full active input.
        const unsigned int tileCount = DLSSTiling::TileCountFromEnv();
        _tiles.clear();
        _tileHandles.clear();

        if (tileCount > 1 && !DLSSTiling::BuildTiles((RenderWidth() / tileCount) * tileCount, TargetWidth(), tileCount, _tiles))
        {
            LOG_ERROR("Tiling requested ({} tiles) but {}x -> {}x cannot form a valid output layout; "
                      "falling back to single feature",
                      tileCount, RenderWidth(), TargetWidth());
        }

        if (!_tiles.empty())
        {
            LOG_INFO("Tiled DLSS: {} tiles, render {}x{} -> target {}x{} ({} per tile)", _tiles.size(), RenderWidth(),
                     RenderHeight(), TargetWidth(), TargetHeight(), _tiles[0].outW);

            // Must be set at create time or the output subrect base is ignored.
            InParameters->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 1);

            bool tilesOk = true;

            for (size_t i = 0; i < _tiles.size(); i++)
            {
                // Each feature is sized to its tile, not to the whole frame.
                InParameters->Set(NVSDK_NGX_Parameter_Width, _tiles[i].renderW);
                InParameters->Set(NVSDK_NGX_Parameter_Height, RenderHeight());
                InParameters->Set(NVSDK_NGX_Parameter_OutWidth, _tiles[i].outW);
                InParameters->Set(NVSDK_NGX_Parameter_OutHeight, TargetHeight());

                NVSDK_NGX_Handle* handle = nullptr;
                nvResult = NVNGXProxy::D3D11_CreateFeature()(InContext, NVSDK_NGX_Feature_SuperSampling, InParameters,
                                                             &handle);

                if (nvResult != NVSDK_NGX_Result_Success)
                {
                    LOG_ERROR("NVNGXProxy::D3D11_CreateFeature tile {} result: {:X}", i, (unsigned int) nvResult);

                    for (auto* h : _tileHandles)
                        if (h != nullptr && NVNGXProxy::D3D11_ReleaseFeature() != nullptr)
                            NVNGXProxy::D3D11_ReleaseFeature()(h);

                    _tileHandles.clear();
                    _tiles.clear();
                    tilesOk = false;
                    break;
                }

                LOG_INFO("NVNGXProxy::D3D11_CreateFeature tile {} ({}px wide): Success", i, _tiles[i].outW);
                _tileHandles.push_back(handle);
            }

            // Restore the whole-frame values the game expects to read back.
            InParameters->Set(NVSDK_NGX_Parameter_Width, RenderWidth());
            InParameters->Set(NVSDK_NGX_Parameter_Height, RenderHeight());
            InParameters->Set(NVSDK_NGX_Parameter_OutWidth, TargetWidth());
            InParameters->Set(NVSDK_NGX_Parameter_OutHeight, TargetHeight());

            if (!tilesOk)
                break;

            _p_dlssHandle = _tileHandles[0];
        }
        else
        {
            _p_dlssHandle = &_dlssHandle;
            nvResult = NVNGXProxy::D3D11_CreateFeature()(InContext, NVSDK_NGX_Feature_SuperSampling, InParameters,
                                                         &_p_dlssHandle);

            if (nvResult != NVSDK_NGX_Result_Success)
            {
                LOG_ERROR("NVNGXProxy::D3D11_CreateFeature result: {0:X}", (unsigned int) nvResult);
                break;
            }
        }

        ReadVersion();

        initResult = true;

    } while (false);

    SetInit(initResult);

    return initResult;
}

bool DLSSFeatureDx11::EvaluateInternal(ID3D11DeviceContext* InDeviceContext, NVSDK_NGX_Parameter* InParameters)
{
    if (!_moduleLoaded)
    {
        LOG_ERROR("nvngx.dll or _nvngx.dll is not loaded!");
        return false;
    }

    NVSDK_NGX_Result nvResult;

    if (NVNGXProxy::D3D11_EvaluateFeature() == nullptr)
    {
        LOG_ERROR("_EvaluateFeature is nullptr");
        return false;
    }

    ProcessEvaluateParams(InParameters);

    if (!_tileHandles.empty())
    {
        const bool previousValid = _tileHistoryValid;
        _tileHistoryValid = false; // Any early return forces a reset on the next complete frame.
        std::vector<DLSSTile> frameTiles;
        if (RenderHeight() == 0 || TargetHeight() != _tileOutputHeight ||
            TargetWidth() != _tiles[0].outW * _tileHandles.size() ||
            !DLSSTiling::BuildFrameTiles(RenderWidth(), TargetWidth(),
                                        static_cast<unsigned int>(_tileHandles.size()), frameTiles))
        {
            LOG_ERROR("Invalid current tiled DLSS geometry: {}x{} -> {}x{}",
                      RenderWidth(), RenderHeight(), TargetWidth(), TargetHeight());
            return false;
        }

        const bool cropped = frameTiles[0].evalOutW != frameTiles[0].outW;
        const bool reset = !previousValid || cropped != _lastTilesCropped ||
                           (cropped && (RenderWidth() != _lastTileRenderW || RenderHeight() != _lastTileRenderH));
        DLSSTiling::EvaluationScope<ID3D11Resource> scope(InParameters);
        ID3D11Texture2D* scratch = nullptr;
        if (cropped)
        {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> outputTexture;
            if (scope.output == nullptr || FAILED(scope.output->QueryInterface(IID_PPV_ARGS(&outputTexture))))
                return false;
            D3D11_TEXTURE2D_DESC desc {};
            outputTexture->GetDesc(&desc);
            if (desc.ArraySize != 1 || desc.SampleDesc.Count != 1 || desc.MipLevels != 1 ||
                desc.Usage != D3D11_USAGE_DEFAULT)
                return false;
            if (static_cast<uint64_t>(scope.base.outX) + TargetWidth() > desc.Width ||
                static_cast<uint64_t>(scope.base.outY) + TargetHeight() > desc.Height)
            {
                LOG_ERROR("Integer-crop DLSS destination subrect exceeds output texture");
                return false;
            }
            scratch = CropOutput(scope.output, frameTiles[0].evalOutW, TargetHeight());
            if (scratch == nullptr)
                return false;
            if (!EnsureCroppedTileHandles(InParameters, frameTiles, [&](NVSDK_NGX_Handle** handle) {
                return NVNGXProxy::D3D11_CreateFeature()(InDeviceContext, NVSDK_NGX_Feature_SuperSampling,
                                                          InParameters, handle);
            }, [](NVSDK_NGX_Handle* handle) { NVNGXProxy::D3D11_ReleaseFeature()(handle); }))
                return false;
        }

        const auto& handles = cropped ? _croppedTileHandles : _tileHandles;
        for (size_t i = 0; i < handles.size(); ++i)
        {
            const auto& tile = frameTiles[i];
            DLSSTiling::ApplyTile(InParameters, scope.base, tile, RenderHeight(), LowResMV());
            InParameters->Set(NVSDK_NGX_Parameter_Reset, reset ? 1 : scope.base.reset);
            if (cropped)
            {
                InParameters->Set(NVSDK_NGX_Parameter_Output, static_cast<ID3D11Resource*>(scratch));
                InParameters->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0u);
                InParameters->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0u);
            }
            nvResult = NVNGXProxy::D3D11_EvaluateFeature()(InDeviceContext, handles[i], InParameters, nullptr);
            if (nvResult != NVSDK_NGX_Result_Success)
            {
                LOG_ERROR("_EvaluateFeature tile {} result: {:X}", i, (unsigned int) nvResult);
                return false;
            }
            if (cropped)
            {
                const D3D11_BOX box { tile.cropX, 0, 0, tile.cropX + tile.outW, TargetHeight(), 1 };
                InDeviceContext->CopySubresourceRegion(scope.output, 0, scope.base.outX + tile.outX,
                                                       scope.base.outY, 0, scratch, 0, &box);
            }
        }
        _lastTilesCropped = cropped;
        _lastTileRenderW = RenderWidth();
        _lastTileRenderH = RenderHeight();
        _tileHistoryValid = true;
    }
    else
    {
        nvResult = NVNGXProxy::D3D11_EvaluateFeature()(InDeviceContext, _p_dlssHandle, InParameters, NULL);

        if (nvResult != NVSDK_NGX_Result_Success)
        {
            LOG_ERROR("_EvaluateFeature result: {0:X}", (unsigned int) nvResult);
            return false;
        }
    }

    LOG_TRACE("_EvaluateFeature ok!");

    return true;
}

DLSSFeatureDx11::DLSSFeatureDx11(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters)
    : IFeature(InHandleId, InParameters), IFeature_Dx11(InHandleId, InParameters), DLSSFeature(InHandleId, InParameters)
{
    if (NVNGXProxy::NVNGXModule() == nullptr)
    {
        LOG_INFO("nvngx.dll not loaded, now loading");
        NVNGXProxy::InitNVNGX();
    }

    LOG_INFO("binding complete!");
}

DLSSFeatureDx11::~DLSSFeatureDx11()
{
    if (State::Instance().isShuttingDown)
        return;

    if (NVNGXProxy::D3D11_ReleaseFeature() == nullptr)
        return;

    for (auto* handle : _croppedTileHandles)
        if (handle != nullptr)
            NVNGXProxy::D3D11_ReleaseFeature()(handle);
    _croppedTileHandles.clear();

    if (!_tileHandles.empty())
    {
        for (auto* h : _tileHandles)
            if (h != nullptr)
                NVNGXProxy::D3D11_ReleaseFeature()(h);

        _tileHandles.clear();
        _p_dlssHandle = nullptr;
    }
    else if (_p_dlssHandle != nullptr)
    {
        NVNGXProxy::D3D11_ReleaseFeature()(_p_dlssHandle);
    }
}
