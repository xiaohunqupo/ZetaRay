#include "GBufferRT.h"
#include <Core/CommandList.h>
#include <Support/Param.h>
#include <App/Filesystem.h>
#include <Scene/SceneCore.h>

using namespace ZetaRay::Core;
using namespace ZetaRay::Core::Direct3DUtil;
using namespace ZetaRay::RenderPass;
using namespace ZetaRay::Math;
using namespace ZetaRay::Scene;
using namespace ZetaRay::RT;
using namespace ZetaRay::Support;
using namespace ZetaRay::App;
using namespace ZetaRay::Util;

//--------------------------------------------------------------------------------------
// GBufferRT
//--------------------------------------------------------------------------------------

GBufferRT::GBufferRT()
    : RenderPassBase(NUM_CBV, NUM_SRV, NUM_UAV, NUM_GLOBS, NUM_CONSTS)
{
    // frame constants
    m_rootSig.InitAsCBV(0, 0, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
        GlobalResource::FRAME_CONSTANTS_BUFFER);

    // root constants
    m_rootSig.InitAsConstants(1, NUM_CONSTS, 1);

    // BVH
    m_rootSig.InitAsBufferSRV(2, 0, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
        GlobalResource::RT_SCENE_BVH_CURR);

    // mesh buffer
    m_rootSig.InitAsBufferSRV(3, 1, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
        GlobalResource::RT_FRAME_MESH_INSTANCES_CURR);

    // scene VB
    m_rootSig.InitAsBufferSRV(4, 2, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
        GlobalResource::SCENE_VERTEX_BUFFER);

    // scene IB
    m_rootSig.InitAsBufferSRV(5, 3, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
        GlobalResource::SCENE_INDEX_BUFFER);

    // material buffer
    m_rootSig.InitAsBufferSRV(6, 4, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
        GlobalResource::MATERIAL_BUFFER);

    // pick buffer
    m_rootSig.InitAsBufferUAV(7, 0, 0,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE, nullptr, true);
}

void GBufferRT::InitPSOs()
{
    constexpr D3D12_ROOT_SIGNATURE_FLAGS flags =
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    auto samplers = App::GetRenderer().GetStaticSamplers();
    RenderPassBase::InitRenderPass("GBuffer", flags, samplers);

    for (int i = 0; i < (int)GBUFFER_SHADER::COUNT; i++)
        m_psoLib.CompileComputePSO(i, m_rootSigObj.Get(), COMPILED_CS[i]);
}

void GBufferRT::Init()
{
    InitPSOs();

    memset(&m_cbLocal, 0, sizeof(m_cbLocal));
    m_cbLocal.PickedPixelX = UINT16_MAX;

    //ParamVariant p1;
    //p1.InitEnum("Renderer", "G-Buffer", "Mipmap Selection", fastdelegate::MakeDelegate(this, &GBufferRT::MipmapSelectionCallback),
    //    DefaultParamVals::TextureFilter, (int)TextureFilter::COUNT, m_localCB.MipmapSelection);
    //App::AddParam(p1);

    m_pickedInstance = GpuMemory::GetDefaultHeapBuffer("PickIdx", sizeof(uint32), false, true);
    m_readbackBuffer = GpuMemory::GetReadbackHeapBuffer(sizeof(uint32));

    App::AddShaderReloadHandler("GBuffer", fastdelegate::MakeDelegate(this, &GBufferRT::ReloadShader));
}

void GBufferRT::Render(CommandList& cmdList)
{
    Assert(cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
        cmdList.GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Invalid downcast");
    ComputeCmdList& computeCmdList = static_cast<ComputeCmdList&>(cmdList);

    auto& renderer = App::GetRenderer();
    auto& gpuTimer = renderer.GetGpuTimer();
    const uint32_t w = renderer.GetRenderWidth();
    const uint32_t h = renderer.GetRenderHeight();

    computeCmdList.PIXBeginEvent("G-Buffer");
    const uint32_t queryIdx = gpuTimer.BeginQuery(computeCmdList, "G-Buffer");

    computeCmdList.SetRootSignature(m_rootSig, m_rootSigObj.Get());

    const uint32_t dispatchDimX = CeilUnsignedIntDiv(w, GBUFFER_RT_GROUP_DIM_X);
    const uint32_t dispatchDimY = CeilUnsignedIntDiv(h, GBUFFER_RT_GROUP_DIM_Y);

    m_cbLocal.DispatchDimX = (uint16_t)dispatchDimX;
    m_cbLocal.DispatchDimY = (uint16_t)dispatchDimY;
    m_cbLocal.NumGroupsInTile = GBUFFER_RT_TILE_WIDTH * m_cbLocal.DispatchDimY;

    const bool hasPick = m_cbLocal.PickedPixelX != UINT16_MAX;

    if (hasPick)
    {
        auto barrier = BufferBarrier(m_pickedInstance.Resource(),
            D3D12_BARRIER_SYNC_NONE,
            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
            D3D12_BARRIER_ACCESS_NO_ACCESS,
            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
        computeCmdList.ResourceBarrier(barrier);

        m_rootSig.SetRootUAV(7, m_pickedInstance.GpuVA());
    }

    m_rootSig.SetRootConstants(0, sizeof(m_cbLocal) / sizeof(DWORD), &m_cbLocal);
    m_rootSig.End(computeCmdList);

    computeCmdList.SetPipelineState(m_psoLib.GetPSO(0));
    computeCmdList.Dispatch(dispatchDimX, dispatchDimY, 1);

    if (hasPick)
    {
        auto syncWrite = BufferBarrier(m_pickedInstance.Resource(),
            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
            D3D12_BARRIER_SYNC_COPY,
            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
            D3D12_BARRIER_ACCESS_COPY_SOURCE);
        computeCmdList.ResourceBarrier(syncWrite);

        computeCmdList.CopyBufferRegion(m_readbackBuffer.Resource(),
            0,
            m_pickedInstance.Resource(),
            0,
            sizeof(uint32));
    }

    gpuTimer.EndQuery(computeCmdList, queryIdx);
    cmdList.PIXEndEvent();
}

void GBufferRT::ReloadShader()
{
    m_psoLib.Reload((int)GBUFFER_SHADER::GBUFFER, m_rootSigObj.Get(), 
        "GBuffer\\GBufferRT_Inline.hlsl");
}
