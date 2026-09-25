#include <pch.h>
#include "DLSSFeature_Dx12.h"
#include <dxgi1_4.h>
#include <Config.h>

ID3D12Resource* DLSSFeatureDx12::CropOutput(ID3D12Resource* output, unsigned int width, unsigned int height)
{
    const auto original = output->GetDesc();
    for (const auto& cached : _cropOutputs)
    {
        const auto desc = cached->GetDesc();
        if (desc.Width == width && desc.Height == height && desc.Format == original.Format)
            return cached.Get();
    }
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = original.Format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    Microsoft::WRL::ComPtr<ID3D12Resource> scratch;
    const auto result = Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&scratch));
    if (FAILED(result))
    {
        LOG_ERROR("Integer-crop DLSS output allocation failed: {:X}", (unsigned int) result);
        return nullptr;
    }
    _cropOutputs.push_back(scratch);
    return scratch.Get();
}

bool DLSSFeatureDx12::InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    if (IsInited())
        return true;

    return InitDLSS(InCommandList, InParameters);
}

bool DLSSFeatureDx12::InitDLSS(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    if (NVNGXProxy::NVNGXModule() == nullptr)
    {
        LOG_ERROR("nvngx.dll not loaded!");
        return false;
    }

    if (!_dlssInited)
    {
        _dlssInited = NVNGXProxy::InitDx12(Device);

        if (!_dlssInited)
            return false;

        _moduleLoaded =
            (NVNGXProxy::D3D12_Init_ProjectID() != nullptr || NVNGXProxy::D3D12_Init_Ext() != nullptr) &&
            (NVNGXProxy::D3D12_Shutdown() != nullptr || NVNGXProxy::D3D12_Shutdown1() != nullptr) &&
            (NVNGXProxy::D3D12_GetParameters() != nullptr || NVNGXProxy::D3D12_AllocateParameters() != nullptr) &&
            NVNGXProxy::D3D12_DestroyParameters() != nullptr && NVNGXProxy::D3D12_CreateFeature() != nullptr &&
            NVNGXProxy::D3D12_ReleaseFeature() != nullptr && NVNGXProxy::D3D12_EvaluateFeature() != nullptr;

        // delay between init and create feature
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    LOG_INFO("Creating DLSS feature");

    if (NVNGXProxy::D3D12_CreateFeature() == nullptr)
    {
        LOG_ERROR("_CreateFeature is nullptr");
        return false;
    }

    ProcessInitParams(InParameters);

    _tileOutputHeight = TargetHeight();
    InParameters->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &_tileCreateFlags);
    InParameters->Get(NVSDK_NGX_Parameter_PerfQualityValue, &_tileQuality);

    // Create the direct-output set at nominal integer dimensions. On an
    // uneven first frame this set is retained for later divisible frames;
    // evaluation creates the expanded set and uses the full active input.
    // Keep ordinary-sized outputs on the single-feature path.
    const unsigned int tileCount = TargetWidth() < 8192 ? 1 : DLSSTiling::TileCountFromEnv();
    _tiles.clear();
    _tileHandles.clear();

    if (tileCount > 1 && !DLSSTiling::BuildTiles((RenderWidth() / tileCount) * tileCount, TargetWidth(), tileCount, _tiles))
    {
        LOG_ERROR("Tiling requested ({} tiles) but {}x -> {}x cannot form a valid output layout; falling back to single feature",
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
            NVSDK_NGX_Result nvResult;
            {
                ScopedSkipHeapCapture skipHeapCapture {};
                nvResult = NVNGXProxy::D3D12_CreateFeature()(InCommandList, NVSDK_NGX_Feature_SuperSampling,
                                                             InParameters, &handle);
            }

            if (nvResult != NVSDK_NGX_Result_Success)
            {
                LOG_ERROR("_CreateFeature tile {} result: {:X}", i, (unsigned int) nvResult);

                for (auto* h : _tileHandles)
                    if (h != nullptr && NVNGXProxy::D3D12_ReleaseFeature() != nullptr)
                        NVNGXProxy::D3D12_ReleaseFeature()(h);

                _tileHandles.clear();
                _tiles.clear();
                tilesOk = false;
                break;
            }

            LOG_INFO("_CreateFeature tile {} ({}px wide): Success", i, _tiles[i].outW);
            _tileHandles.push_back(handle);
        }

        // Restore the whole-frame values the game expects to read back.
        InParameters->Set(NVSDK_NGX_Parameter_Width, RenderWidth());
        InParameters->Set(NVSDK_NGX_Parameter_Height, RenderHeight());
        InParameters->Set(NVSDK_NGX_Parameter_OutWidth, TargetWidth());
        InParameters->Set(NVSDK_NGX_Parameter_OutHeight, TargetHeight());

        if (!tilesOk)
            return false;

        _p_dlssHandle = _tileHandles[0];
    }
    else
    {
        _p_dlssHandle = &_dlssHandle;

        NVSDK_NGX_Result nvResult;
        {
            ScopedSkipHeapCapture skipHeapCapture {};

            nvResult = NVNGXProxy::D3D12_CreateFeature()(InCommandList, NVSDK_NGX_Feature_SuperSampling, InParameters,
                                                         &_p_dlssHandle);
        }

        if (nvResult != NVSDK_NGX_Result_Success)
        {
            LOG_ERROR("_CreateFeature result: {0:X}", (unsigned int) nvResult);
            return false;
        }

        LOG_INFO("_CreateFeature result: NVSDK_NGX_Result_Success");
    }

    ReadVersion();

    SetInit(true);
    return true;
}

bool DLSSFeatureDx12::EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    if (!_moduleLoaded)
    {
        LOG_ERROR("nvngx.dll or _nvngx.dll is not loaded!");
        return false;
    }

    NVSDK_NGX_Result nvResult;

    if (NVNGXProxy::D3D12_EvaluateFeature() == nullptr)
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
        DLSSTiling::EvaluationScope<ID3D12Resource> scope(InParameters);
        ID3D12Resource* scratch = nullptr;
        if (cropped)
        {
            if (scope.output == nullptr)
                return false;
            const auto desc = scope.output->GetDesc();
            if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 ||
                desc.SampleDesc.Count != 1 || desc.MipLevels != 1)
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
                ScopedSkipHeapCapture skipHeapCapture {};
                return NVNGXProxy::D3D12_CreateFeature()(InCommandList, NVSDK_NGX_Feature_SuperSampling,
                                                          InParameters, handle);
            }, [](NVSDK_NGX_Handle* handle) { NVNGXProxy::D3D12_ReleaseFeature()(handle); }))
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
                InParameters->Set(NVSDK_NGX_Parameter_Output, static_cast<ID3D12Resource*>(scratch));
                InParameters->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0u);
                InParameters->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0u);
            }
            nvResult = NVNGXProxy::D3D12_EvaluateFeature()(InCommandList, handles[i], InParameters, nullptr);
            if (nvResult != NVSDK_NGX_Result_Success)
            {
                LOG_ERROR("_EvaluateFeature tile {} result: {:X}", i, (unsigned int) nvResult);
                return false;
            }
            if (cropped)
            {
                // NGX output is UAV, as in the existing postprocessing path.
                ResourceBarrier(InCommandList, scratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                D3D12_RESOURCE_STATE_COPY_SOURCE);
                ResourceBarrier(InCommandList, scope.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                D3D12_RESOURCE_STATE_COPY_DEST);
                D3D12_TEXTURE_COPY_LOCATION source {}, destination {};
                source.pResource = scratch;
                source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                destination.pResource = scope.output;
                destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                const D3D12_BOX box { tile.cropX, 0, 0, tile.cropX + tile.outW, TargetHeight(), 1 };
                InCommandList->CopyTextureRegion(&destination, scope.base.outX + tile.outX,
                                                scope.base.outY, 0, &source, &box);
                ResourceBarrier(InCommandList, scope.output, D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                ResourceBarrier(InCommandList, scratch, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
        }
        _lastTilesCropped = cropped;
        _lastTileRenderW = RenderWidth();
        _lastTileRenderH = RenderHeight();
        _tileHistoryValid = true;
    }
    else
    {
        nvResult = NVNGXProxy::D3D12_EvaluateFeature()(InCommandList, _p_dlssHandle, InParameters, NULL);

        if (nvResult != NVSDK_NGX_Result_Success)
        {
            LOG_ERROR("_EvaluateFeature result: {0:X}", (unsigned int) nvResult);
            return false;
        }
    }

    _frameCount++;

    return true;
}

void DLSSFeatureDx12::Shutdown(ID3D12Device* InDevice)
{
    if (_dlssInited)
    {
        if (NVNGXProxy::D3D12_Shutdown() != nullptr)
            NVNGXProxy::D3D12_Shutdown()();
        else if (NVNGXProxy::D3D12_Shutdown1() != nullptr)
            NVNGXProxy::D3D12_Shutdown1()(InDevice);
    }

    DLSSFeature::Shutdown();
}

DLSSFeatureDx12::DLSSFeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters)
    : IFeature(InHandleId, InParameters), IFeature_Dx12(InHandleId, InParameters), DLSSFeature(InHandleId, InParameters)
{
    if (NVNGXProxy::NVNGXModule() == nullptr)
    {
        LOG_INFO("nvngx.dll not loaded, now loading");
        NVNGXProxy::InitNVNGX();
    }

    LOG_INFO("binding complete!");
}

DLSSFeatureDx12::~DLSSFeatureDx12()
{
    if (State::Instance().isShuttingDown)
        return;

    if (NVNGXProxy::D3D12_ReleaseFeature() == nullptr)
        return;

    for (auto* handle : _croppedTileHandles)
        if (handle != nullptr)
            NVNGXProxy::D3D12_ReleaseFeature()(handle);
    _croppedTileHandles.clear();

    if (!_tileHandles.empty())
    {
        for (auto* h : _tileHandles)
            if (h != nullptr)
                NVNGXProxy::D3D12_ReleaseFeature()(h);

        _tileHandles.clear();
        _p_dlssHandle = nullptr;
    }
    else if (_p_dlssHandle != nullptr)
    {
        NVNGXProxy::D3D12_ReleaseFeature()(_p_dlssHandle);
    }
}
