// Some of the ideas in this implementation were inspired by the following: 
// https://levelup.gitconnected.com/organizing-gpu-work-with-directed-acyclic-graphs-f3fd5f2c2af3

#include "RenderGraph.h"
#include "RendererCore.h"
#include "CommandList.h"
#include "../Support/Task.h"
#include "../App/Timer.h"
#include "../Utility/Utility.h"
#include <algorithm>
#include <xxHash/xxhash.h>
#include <ImGui/imnodes.h>

#ifndef NDEBUG
#include "../App/Log.h"
#include <string>
#endif

using namespace ZetaRay;
using namespace ZetaRay::Support;
using namespace ZetaRay::Util;
using namespace ZetaRay::Core;
using namespace ZetaRay::Core::Direct3DUtil;

namespace
{
    const char* GetResStateName(D3D12_RESOURCE_STATES s)
    {
        switch (s)
        {
        case D3D12_RESOURCE_STATE_COMMON:
            return "COMMON_OR_PRESENT";
        case D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER:
            return "VERTEX_AND_CONSTANT_BUFFER";
        case D3D12_RESOURCE_STATE_INDEX_BUFFER:
            return "INDEX_BUFFER";
        case D3D12_RESOURCE_STATE_RENDER_TARGET:
            return "RENDER_TARGET";
        case D3D12_RESOURCE_STATE_UNORDERED_ACCESS:
            return "UNORDERED_ACCESS";
        case D3D12_RESOURCE_STATE_DEPTH_WRITE:
            return "DEPTH_WRITE";
        case D3D12_RESOURCE_STATE_DEPTH_READ:
            return "DEPTH_READ";
        case D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:
            return "NON_PIXEL_SHADER_RESOURCE";
        case D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE:
            return "PIXEL_SHADER_RESOURCE";
        case D3D12_RESOURCE_STATE_COPY_DEST:
            return "COPY_DEST";
        case D3D12_RESOURCE_STATE_COPY_SOURCE:
            return "COPY_SOURCE";
        case D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE:
            return "RAYTRACING_ACCELERATION_STRUCTURE";
        case D3D12_RESOURCE_STATE_GENERIC_READ:
            return "GENERIC_READ";
        case D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE:
            return "ALL_SHADER_RESOURCE";
        default:
            return "UNKNOWN";
        }
    }

    struct Edge
    {
        int E0;
        int E1;
    };
}

//--------------------------------------------------------------------------------------
// AggregateRenderNode
//--------------------------------------------------------------------------------------

void RenderGraph::AggregateRenderNode::Append(const RenderNode& node, int mappedGpeDepIdx, 
    bool forceSeparate)
{
    Assert(IsAsyncCompute == (node.Type == RENDER_NODE_TYPE::ASYNC_COMPUTE), 
        "All the nodes in an AggregateRenderNode must have the same type.");
    Assert(Dlgs.empty() || node.NodeBatchIdx == BatchIdx, 
        "All the nodes in an AggregateRenderNode must have the same batch index.");
    Assert(!forceSeparate || Dlgs.empty(), 
        "Aggregate nodes with forceSeparate flag can't have more than one task.");
    Assert(!node.HasUnsupportedBarrier || node.Type == RENDER_NODE_TYPE::ASYNC_COMPUTE, 
        "Invalid condition.");

    Barriers.append_range(node.Barriers.begin(), node.Barriers.end());
    Dlgs.push_back(node.Dlg);
    BatchIdx = node.NodeBatchIdx;
    ForceSeparate = forceSeparate;
    GpuDepIdx.Val = Math::Max(GpuDepIdx.Val, mappedGpeDepIdx);
    HasUnsupportedBarrier = !HasUnsupportedBarrier ? node.HasUnsupportedBarrier : true;

    int base = Dlgs.size() > 1 ? (int)strlen(Name) : 0;

    if (base)
    {
        Name[base] = '_';
        base++;
    }

    int numBytesToCopy = Math::Min(MAX_NAME_LENGTH - base - 1, (int)strlen(node.Name));
    Assert(numBytesToCopy, "bug");

    memcpy(Name + base, node.Name, numBytesToCopy);
    Name[base + numBytesToCopy] = '\0';
}

//--------------------------------------------------------------------------------------
// RenderGraph
//--------------------------------------------------------------------------------------

void RenderGraph::Shutdown()
{
    m_frameResources.free_memory();

    for (int i = 0; i < MAX_NUM_RENDER_PASSES; i++)
    {
        m_renderNodes[i].Inputs.free_memory();
        m_renderNodes[i].Outputs.free_memory();
        m_renderNodes[i].Barriers.free_memory();
    }
}

void RenderGraph::Reset()
{
    //m_frameResources.clear();
    m_frameResources.resize(MAX_NUM_RESOURCES);

    // Sort the frame resources so that window-dependent ones come after window-independent ones
    auto it = std::partition(m_frameResources.begin(), m_frameResources.begin() + m_prevFramesNumResources,
        [](ResourceMetadata& res)
        {
            return res.IsWindowSizeDependent == false;
        });

    const size_t numRemaining = it - m_frameResources.begin();

    for (size_t i = numRemaining; i < m_prevFramesNumResources; i++)
        m_frameResources[i].Reset();

    std::sort(m_frameResources.begin(), m_frameResources.begin() + numRemaining,
        [](ResourceMetadata& lhs, ResourceMetadata& rhs)
        {
            return lhs.ID < rhs.ID;
        });

    m_prevFramesNumResources = (int)numRemaining;
    m_lastResIdx = (int)numRemaining;

    // Reset the render nodes
    const int numNodes = m_currRenderPassIdx.load(std::memory_order_relaxed);

    for (int currNode = 0; currNode < numNodes; currNode++)
        m_renderNodes[currNode].Reset();

    m_aggregateNodes.free_memory();
    m_currRenderPassIdx.store(0, std::memory_order_relaxed);
}

void RenderGraph::RemoveResource(uint64_t path)
{
    Assert(!m_inBeginEndBlock, "Invalid call.");
    const int pos = FindFrameResource(path, 0, m_prevFramesNumResources - 1);

    if (pos != -1)
    {
        m_lastResIdx.fetch_sub(1, std::memory_order_relaxed);
        m_frameResources[pos].Reset();

        // Insertion sort
        for (int i = pos; i < m_prevFramesNumResources; i++)
            m_frameResources[i] = ZetaMove(m_frameResources[i + 1]);
    }
}

void RenderGraph::RemoveResources(Util::Span<uint64_t> paths)
{
    Assert(!m_inBeginEndBlock, "Invalid call.");
    int numRemoved = 0;

    for (auto p : paths)
    {
        int pos = FindFrameResource(p, 0, m_prevFramesNumResources - 1);

        if (pos != -1)
        {
            m_frameResources[pos].Reset();
            numRemoved++;
        }
    }

    std::sort(m_frameResources.begin(), m_frameResources.begin() + m_prevFramesNumResources,
        [](ResourceMetadata& lhs, ResourceMetadata& rhs)
        {
            return lhs.ID < rhs.ID;
        });

    m_lastResIdx.fetch_sub(numRemoved, std::memory_order_relaxed);
}

void RenderGraph::BeginFrame()
{
    Assert(!m_inBeginEndBlock && !m_inPreRegister, "Invalid call.");
    m_prevFramesNumResources = m_lastResIdx.load(std::memory_order_relaxed);

    const int numNodes = m_currRenderPassIdx.load(std::memory_order_relaxed);
    //m_numPassesPrevFrame = numNodes;
    m_currRenderPassIdx.store(0, std::memory_order_relaxed);

    // Reset the producers
    for (auto& rm : m_frameResources)
    {
        rm.CurrProdIdx.store(0, std::memory_order_relaxed);

        for (int i = 0; i < MAX_NUM_PRODUCERS; i++)
            rm.Producers[i].Val = INVALID_NODE_HANDLE;
    }

    // Reset the render nodes
    for (int currNode = 0; currNode < MAX_NUM_RENDER_PASSES; currNode++)
        m_renderNodes[currNode].Reset();

    m_aggregateNodes.free_memory();
    m_inBeginEndBlock = true;
    m_inPreRegister = true;
}

int RenderGraph::FindFrameResource(uint64_t key, int beg, int end)
{
    if (end - beg == 0)
        return -1;

    end = (end == -1) ? m_lastResIdx.load(std::memory_order_relaxed) - 1 : end;
    auto idx = BinarySearch(Span(m_frameResources), key, 
        [](const ResourceMetadata& r) {return r.ID; }, beg, end);
    return (int)idx;
}

RenderNodeHandle RenderGraph::RegisterRenderPass(const char* name, RENDER_NODE_TYPE t, 
    fastdelegate::FastDelegate1<CommandList&> dlg, bool forceSeparateCmdList)
{
    Assert(m_inBeginEndBlock && m_inPreRegister, "Invalid call.");
    int h = m_currRenderPassIdx.fetch_add(1, std::memory_order_relaxed);
    Assert(h < MAX_NUM_RENDER_PASSES, "Number of render passes exceeded MAX_NUM_RENDER_PASSES");

    m_renderNodes[h].Reset(name, t, dlg, forceSeparateCmdList);

    return RenderNodeHandle(h);
}

void RenderGraph::RegisterResource(ID3D12Resource* res, uint64_t path, 
    D3D12_RESOURCE_STATES initState, bool isWindowSizeDependent)
{
    Assert(m_inBeginEndBlock && m_inPreRegister, "Invalid call.");
    Assert(res == nullptr || path > DUMMY_RES::COUNT, 
        "resource path ID can't take special value %llu", path);

    const int prevPos = FindFrameResource(path, 0, m_prevFramesNumResources - 1);

    // Existing resource
    if (prevPos != -1)
    {
        if(m_frameResources[prevPos].Res != res)
            m_frameResources[prevPos].Reset(path, res, initState, isWindowSizeDependent);

        return;
    }

    // New resource
    int pos = m_lastResIdx.fetch_add(1, std::memory_order_relaxed);
    Assert(pos < MAX_NUM_RESOURCES, "Number of resources exceeded MAX_NUM_RESOURCES");

    m_frameResources[pos].Reset(path, res, initState, isWindowSizeDependent);
}

void RenderGraph::MoveToPostRegister()
{
    Assert(m_inBeginEndBlock && m_inPreRegister, "Invalid call.");
    const int numResources = m_lastResIdx.load(std::memory_order_relaxed);

    // Sort the frame resources so that binary search can be performed
    std::sort(m_frameResources.begin(), m_frameResources.begin() + numResources,
        [](ResourceMetadata& lhs, ResourceMetadata& rhs)
        {
            return lhs.ID < rhs.ID;
        });

#ifndef NDEBUG
    for (int i = 0; i < numResources - 1; i++)
    {
        if (m_frameResources[i].ID == m_frameResources[i + 1].ID)
        {
            char name[64] = { '\0' };
            UINT n = sizeof(name);
            m_frameResources[i].Res->GetPrivateData(WKPDID_D3DDebugObjectName, &n, name);

            Assert(false, "Duplicate entries for resource %s.", name);
        }
    }
#endif

    m_inPreRegister = false;
}

void RenderGraph::AddInput(RenderNodeHandle h, uint64_t pathID, 
    D3D12_RESOURCE_STATES expectedState)
{
    Assert(m_inBeginEndBlock && !m_inPreRegister, "Invalid call.");
    Assert(h.IsValid(), "Invalid handle");
    Assert(h.Val < m_currRenderPassIdx.load(std::memory_order_relaxed), "Invalid handle");
    Assert(expectedState & Constants::READ_STATES, "Invalid read state.");

    // Defer checking for invalid states until later on
    m_renderNodes[h.Val].Inputs.emplace_back(pathID, expectedState);
}

void RenderGraph::AddOutput(RenderNodeHandle h, uint64_t pathID, 
    D3D12_RESOURCE_STATES expectedState)
{
    Assert(m_inBeginEndBlock && !m_inPreRegister, "Invalid call.");
    Assert(h.IsValid(), "Invalid handle");
    Assert(h.Val < m_currRenderPassIdx.load(std::memory_order_relaxed), "Invalid handle");
    Assert(expectedState & Constants::WRITE_STATES, "Invalid write state.");
    Assert(m_renderNodes[h.Val].Type != RENDER_NODE_TYPE::ASYNC_COMPUTE || 
        !(expectedState & Constants::INVALID_COMPUTE_STATES),
        "state transition to %u is not supported on an async-compute command list.", 
        expectedState);

    m_renderNodes[h.Val].Outputs.emplace_back(pathID, expectedState);

    const size_t idx = FindFrameResource(pathID);
    Assert(idx != size_t(-1), "Invalid resource path %llu.", pathID);

    const int prodIdx = m_frameResources[idx].CurrProdIdx.fetch_add(1, std::memory_order_relaxed);
    Assert(prodIdx < MAX_NUM_PRODUCERS, 
        "Number of producers for each resource can't exceed MAX_NUM_PRODUCERS");
//    Assert(prodIdx == 0 || m_renderNodes[m_frameResources[idx].Producers[prodIdx - 1].Val].Type == m_renderNodes[h.Val].Type,
//        "All the producers need to have the same type.");

    m_frameResources[idx].Producers[prodIdx] = h;
}

void RenderGraph::Build(TaskSet& ts)
{
    Assert(m_inBeginEndBlock && !m_inPreRegister, "Invalid call.");
    m_inBeginEndBlock = false;

    const int numNodes = m_currRenderPassIdx.load(std::memory_order_relaxed);
    Assert(numNodes > 0, "no render nodes");

    for (int i = 0; i < numNodes; i++)
        m_renderNodes[i].Indegree = (int16)m_renderNodes[i].Inputs.size();

    SmallVector<RenderNodeHandle, App::FrameAllocator> adjacentTailNodes[MAX_NUM_RENDER_PASSES];

    // Add the graph edges. For each input of node N, add an edge from 
    // that input's producer node (previously populated by AddOutput) to N
    for (int currNode = 0; currNode < numNodes; currNode++)
    {
        RenderNode& node = m_renderNodes[currNode];

        for (Dependency& input : node.Inputs)
        {
            const size_t idx = FindFrameResource(input.ResID);
            Assert(idx != size_t(-1), "Resource ID %u was not found.", input.ResID);

            const int16 numProducers = m_frameResources[idx].CurrProdIdx.load(std::memory_order_relaxed);

            // Null resources or resources that were produced in prior frames
            if (numProducers == 0)
            {
                node.Indegree--;
                Assert(node.Indegree >= 0, "Invalid indegree for node %s.", node.Name);
            }
            // Each producer needs to decrement the dependency counter
            else
                node.Indegree += numProducers - 1;    // -1 to avoid double counting

            for (int prod = 0; prod < numProducers; prod++)
            {
                const int prodHandle = m_frameResources[idx].Producers[prod].Val;

                // Workaround for when resource is set as both input and output for some node, otherwise there'd be a cycle
                if (currNode == prodHandle)
                {
                    node.Indegree--;

                    const int numOutputs = (int)node.Outputs.size();
                    Assert(numOutputs > 0, "invalid graph.");

                    // For pass P, resource R is ping ponged between input & output and may appear as both 
                    // an input and output of P, with possibly different states. Since barriers are executed "prior" 
                    // to recording, this scenario can't be handled. As a workaround, the render graph takes cares of 
                    // transitioning R into its input state, while further transitions (ping-ponging) for R inside P 
                    // must be handled manually. R's state must be restored to its input state, otherwise actual state 
                    // and render graph's state go out of sync.
                    for (int i = 0; i < numOutputs; i++)
                    {
                        if (node.Outputs[i].ResID == input.ResID)
                        {
                            node.OutputMask |= (1 << i);
                            break;
                        }
                    }
                }
                else
                    adjacentTailNodes[prodHandle].push_back(RenderNodeHandle(currNode));
            }
        }
    }

    Sort(adjacentTailNodes);

    // At this point "m_frameResources[_].Producers" is invalid since "m_renderNodes" 
    // was sorted. "mapping" must be used instead.
    InsertResourceBarriers();
    JoinRenderNodes();
    MergeSmallNodes();
    BuildTaskGraph(ts);

#ifndef NDEBUG
    //Log();
#endif
}

void RenderGraph::BuildTaskGraph(Support::TaskSet& ts)
{
    // Task-level dependency cases:
    // 
    // 1. From nodes with batchIdx i to nodes with batchIdx i + 1
    // 2. From gpuDep(node) to node

    // GPU dependency & unsupported barriers:
    // 
    //  - If C has an unsupported barrier, add a barrier Task T immediately before
    // the tasks from batch index B where B = C.batchIdx
    //  - Remove C's GPU dependency (if any), then add a GPU dependency from T to C

    for (int i = 0; i < m_aggregateNodes.size(); i++)
    {
        m_aggregateNodes[i].TaskH = ts.EmplaceTask(m_aggregateNodes[i].Name, [this, i]()
            {
                auto& renderer = App::GetRenderer();

                ComputeCmdList* cmdList = nullptr;
                AggregateRenderNode& aggregateNode = m_aggregateNodes[i];

                if (aggregateNode.MergeStart)
                {
                    Assert(m_mergedCmdLists[aggregateNode.MergedCmdListIdx] == nullptr, 
                        "Merged command list should be initially NULL.");
                    m_mergedCmdLists[aggregateNode.MergedCmdListIdx] = 
                        static_cast<ComputeCmdList*>(renderer.GetGraphicsCmdList());;
                    cmdList = m_mergedCmdLists[aggregateNode.MergedCmdListIdx];
                }
                else if (aggregateNode.MergedCmdListIdx != -1)
                {
                    cmdList = m_mergedCmdLists[aggregateNode.MergedCmdListIdx];
                    Assert(cmdList, "Merged command list should've been initializeda at this point.");
                }
                else
                {
                    if (!aggregateNode.IsAsyncCompute)
                        cmdList = static_cast<ComputeCmdList*>(renderer.GetGraphicsCmdList());
                    else
                        cmdList = renderer.GetComputeCmdList();
                }

#ifndef NDEBUG
                cmdList->SetName(aggregateNode.Name);
#endif

                if (aggregateNode.HasUnsupportedBarrier)
                {
                    CommandList* barrierCmdList = renderer.GetGraphicsCmdList();
                    GraphicsCmdList& directCmdList = static_cast<GraphicsCmdList&>(*barrierCmdList);
#ifndef NDEBUG
                    directCmdList.SetName("Barrier");
#endif
                    directCmdList.ResourceBarrier(aggregateNode.Barriers.data(), 
                        (UINT)aggregateNode.Barriers.size());
                    uint64_t f = renderer.ExecuteCmdList(barrierCmdList);

                    renderer.WaitForDirectQueueOnComputeQueue(f);
                }
                else if (!aggregateNode.Barriers.empty())
                {
                    cmdList->ResourceBarrier(aggregateNode.Barriers.begin(), 
                        (UINT)aggregateNode.Barriers.size());
                }

                // Record
                for(auto dlg : aggregateNode.Dlgs)
                    dlg(*cmdList);

                // Wait for possible GPU fence
                if (!aggregateNode.HasUnsupportedBarrier && aggregateNode.GpuDepIdx.Val != -1)
                {
                    uint64_t f = m_aggregateNodes[aggregateNode.GpuDepIdx.Val].CompletionFence;
                    Assert(f != UINT64_MAX, "GPU hasn't finished executing.");

                    if (aggregateNode.IsAsyncCompute)
                        renderer.WaitForDirectQueueOnComputeQueue(f);
                    else
                        renderer.WaitForComputeQueueOnDirectQueue(f);
                }

                if (aggregateNode.IsLast)
                {
                    auto& gpuTimer = renderer.GetGpuTimer();
                    gpuTimer.EndFrame(*cmdList);
                }

                // submit
                if (aggregateNode.MergedCmdListIdx == -1 || aggregateNode.MergeEnd)
                {
                    aggregateNode.CompletionFence = renderer.ExecuteCmdList(cmdList);

                    if (aggregateNode.MergeEnd)
                    {
                        m_mergedCmdLists[aggregateNode.MergedCmdListIdx] = nullptr;

                        int curr = i - 1;
                        while (m_aggregateNodes[curr].MergedCmdListIdx == aggregateNode.MergedCmdListIdx)
                        {
                            m_aggregateNodes[curr].CompletionFence = aggregateNode.CompletionFence;
                            curr--;
                        }
                    }
                }

                if (m_submissionWaitObj && aggregateNode.IsLast)
                {
                    m_submissionWaitObj->Notify();
                    m_submissionWaitObj = nullptr;
                }
            });
    }

    for (int i = 0; i < (int)m_aggregateNodes.size() - 1; i++)
    {
        const int currBatchIdx = m_aggregateNodes[i].BatchIdx;

        for (int j = i + 1; j < (int)m_aggregateNodes.size(); j++)
        {
            const int nextBatchIdx = m_aggregateNodes[j].BatchIdx;

            if (nextBatchIdx > currBatchIdx + 1)
                break;

            if (nextBatchIdx == currBatchIdx + 1)
                ts.AddOutgoingEdge(m_aggregateNodes[i].TaskH, m_aggregateNodes[j].TaskH);

            if(nextBatchIdx == currBatchIdx && m_aggregateNodes[j].ForceSeparate)
                ts.AddOutgoingEdge(m_aggregateNodes[i].TaskH, m_aggregateNodes[j].TaskH);
        }
    }
}

void RenderGraph::Sort(Span<SmallVector<RenderNodeHandle, App::FrameAllocator>> adjacentTailNodes)
{
    const int numNodes = m_currRenderPassIdx.load(std::memory_order_relaxed);
    RenderNodeHandle sorted[MAX_NUM_RENDER_PASSES];
    int currIdx = 0;

    // Move all the nodes with zero indegree to sorted
    for (int currNode = 0; currNode < numNodes; currNode++)
    {
        RenderNode& node = m_renderNodes[currNode];

        if (node.Indegree == 0)
        {
            // when batchIdx is zero there are no dependencies
            sorted[currIdx++] = RenderNodeHandle(currNode);
            node.NodeBatchIdx = 0;
        }
    }

    Assert(currIdx > 0, "Graph is not a DAG- no node with 0 dependencies.");

    // Topological sort
    for (int currNode = 0; currNode < numNodes; currNode++)
    {
        Assert(sorted[currNode].IsValid(), "invalid handle");
        const int currHandle = sorted[currNode].Val;

        for (RenderNodeHandle adjacent : adjacentTailNodes[currHandle])
        {
            if (--m_renderNodes[adjacent.Val].Indegree == 0)
                sorted[currIdx++] = adjacent;
        }
    }

    Assert(numNodes == currIdx, "Graph is not a DAG");

    // Length of the longest path for every node in DAG
    for (int i = 0; i < numNodes; i++)
    {
        RenderNodeHandle currHandle = sorted[i];

        for (RenderNodeHandle adjacent : adjacentTailNodes[currHandle.Val])
        {
            m_renderNodes[adjacent.Val].NodeBatchIdx = Math::Max(
                m_renderNodes[currHandle.Val].NodeBatchIdx + 1,
                m_renderNodes[adjacent.Val].NodeBatchIdx);
        }
    }

    std::sort(sorted, sorted + numNodes, 
        [this](const RenderNodeHandle& lhs, const RenderNodeHandle& rhs)
        {
            return m_renderNodes[lhs.Val].NodeBatchIdx < m_renderNodes[rhs.Val].NodeBatchIdx;
        });

    // Producer Handle to sorted array index mapping.
    // Producer handles were specified using the unsorted index. This maps those
    // to sorted order as subsequent processing uses the sorted one:
    // 
    //        original: [0, 1, 2, 3, 4, 5]
    //        sorted:   [3, 2, 1, 4, 0, 5]
    //        mapping:  [4, 2, 1, 0, 3, 5]
    //
    // e.g. Producer handle 0 is now located at mapping[0] = 4
    for (int currNode = 0; currNode < numNodes; currNode++)
        m_mapping[sorted[currNode].Val] = RenderNodeHandle(currNode);

    // Shuffle
    RenderNode tempRenderNodes[MAX_NUM_RENDER_PASSES];

    for (int currNode = 0; currNode < numNodes; currNode++)
        tempRenderNodes[currNode] = ZetaMove(m_renderNodes[sorted[currNode].Val]);

    for (int currNode = 0; currNode < numNodes; currNode++)
        m_renderNodes[currNode] = ZetaMove(tempRenderNodes[currNode]);

    // TODO can be avoided
//    std::sort(m_renderNodes, m_renderNodes + numNodes, [this](const RenderNode& lhs, const RenderNode& rhs)
//        {
//            return lhs.BatchIdx < rhs.BatchIdx;
//        });
}

void RenderGraph::InsertResourceBarriers()
{
    const int numNodes = m_currRenderPassIdx.load(std::memory_order_relaxed);

    // Using ordering imposed by sorted, largest index of the node on the Direct/Compute queue with which 
    // a compute/Direct node has already synced (see case b below). Note that this is an index into "sorted"
    // not a Handle
    int lastDirQueueHandle = 0;
    int lastComputeQueueHandle = 0;

    // Helper to return last*Idx based on the node type
    auto getLastSyncedIdx = [&lastDirQueueHandle, &lastComputeQueueHandle](RENDER_NODE_TYPE t)
    {
        return t == RENDER_NODE_TYPE::ASYNC_COMPUTE ? &lastDirQueueHandle : 
            &lastComputeQueueHandle;
    };

    // Workflow:
    // 
    // 1. For each input resource R:
    // 
    //     - if R.state != expected --> add a barrier (e.g. RTV to SRV)
    //     - if stateBefore(== R.state) is unsupported --> set hasUnsupportedBarriers
    //     - if producer is on a different queue, add a gpu sync, but only if an earlier
    //       task hasn't synced already (see cases below)
    //
    // 2. For each output resource R:
    // 
    //         - if R.state != expected --> add a barrier (e.g. SRV to UAV)
    //         - if stateBefore(== R.state) is unsupported --> set hasUnsupportedBarriers

    // Iterate by execution order (i.e. sorted by batch index)
    for (int currNode = 0; currNode < numNodes; currNode++)
    {
        RenderNode& node = m_renderNodes[currNode];
        const bool isAsyncCompute = node.Type == RENDER_NODE_TYPE::ASYNC_COMPUTE;
        RenderNodeHandle largestProducerSortedHandle;    // i.e. index in sorted (execution) order

        //
        // Inputs
        //
        for (Dependency& currInputRes : node.Inputs)
        {
            if (currInputRes.ResID < DUMMY_RES::COUNT)
                continue;

            const size_t inputFrameResIdx = FindFrameResource(currInputRes.ResID);
            Assert(inputFrameResIdx != size_t(-1), "Resource %llu was not found.", currInputRes.ResID);
            const D3D12_RESOURCE_STATES inputResState = m_frameResources[inputFrameResIdx].State;

            if (!(inputResState & currInputRes.ExpectedState))
            {
                // Unsupported stateAfter should've been caught earlier
                node.HasUnsupportedBarrier = node.HasUnsupportedBarrier || 
                    (isAsyncCompute && (inputResState & Constants::INVALID_COMPUTE_STATES));
                node.Barriers.push_back(TransitionBarrier(m_frameResources[inputFrameResIdx].Res,
                    inputResState,
                    currInputRes.ExpectedState));

                // Update resource state
                m_frameResources[inputFrameResIdx].State = currInputRes.ExpectedState;
            }

            // If the input producer is on a different command queue, a GPU cross-queue sync is required.
            // (numbers correspond to index in the execution order)
            // 
            // Cases:
            //
            // a. 5 only needs to sync with 4 and 7.
            //
            //        Queue1      1------> 3 ------> 5
            //                                       |
            //                    |--------|----------
            //        Queue2      2 -----> 4 ------> 6
            //
            //           
            // b. since 4 has synced with 1, 6 no longer needs to sync with 1.
            //
            //        Queue1      1------> 2 -----> 3
            //                    |-----------------
            //                    |                 |
            //        Queue2      4 -----> 5 -----> 6

            // Find the largest producer batch index (case a)
            const int numProducers = m_frameResources[inputFrameResIdx].CurrProdIdx.load(std::memory_order_relaxed);
            //Assert(m_frameResources[inputFrameResIdx].Res == nullptr || 
            //    numProducers > 0, "No producer for resource %llu", m_frameResources[inputFrameResIdx].ID);

            for (int i = 0; i < numProducers; i++)
            {
                RenderNodeHandle unsortedHandle = m_frameResources[inputFrameResIdx].Producers[i];
                RenderNodeHandle sortedHandle = m_mapping[unsortedHandle.Val];
                const bool producerOnDifferentQueue = 
                    (isAsyncCompute && m_renderNodes[sortedHandle.Val].Type != RENDER_NODE_TYPE::ASYNC_COMPUTE) ||
                    (!isAsyncCompute && m_renderNodes[sortedHandle.Val].Type == RENDER_NODE_TYPE::ASYNC_COMPUTE);

                if (producerOnDifferentQueue)
                {
                    Assert(m_renderNodes[sortedHandle.Val].NodeBatchIdx < node.NodeBatchIdx, "Invalid graph");
                    // case a
                    largestProducerSortedHandle.Val = Math::Max(largestProducerSortedHandle.Val, sortedHandle.Val);
                }
            }
        }

        // Case b
        if (largestProducerSortedHandle.Val != -1 && *getLastSyncedIdx(node.Type) < largestProducerSortedHandle.Val)
        {
            *getLastSyncedIdx(node.Type) = largestProducerSortedHandle.Val;
            node.GpuDepSourceIdx = largestProducerSortedHandle;
        }

        //
        // Outputs
        //
        int i = 0;

        for (Dependency& currOutputRes : node.Outputs)
        {
            if (currOutputRes.ResID < DUMMY_RES::COUNT)
                continue;

            const bool skipBarrier = ((1 << i++) & node.OutputMask);

            const size_t outputFrameResIdx = FindFrameResource(currOutputRes.ResID);
            Assert(outputFrameResIdx != size_t(-1), "Resource %llu was not found.", currOutputRes.ResID);
            const D3D12_RESOURCE_STATES outputResState = m_frameResources[outputFrameResIdx].State;

            if (!skipBarrier && !(m_frameResources[outputFrameResIdx].State & currOutputRes.ExpectedState))
            {
                // Unsupported resourceAfter should've been caught earlier
                node.HasUnsupportedBarrier = node.HasUnsupportedBarrier || 
                    (isAsyncCompute && (outputResState & Constants::INVALID_COMPUTE_STATES));
                node.Barriers.push_back(TransitionBarrier(m_frameResources[outputFrameResIdx].Res,
                    outputResState,
                    currOutputRes.ExpectedState));
            }

            // Update the resource state
            m_frameResources[outputFrameResIdx].State = currOutputRes.ExpectedState;
        }
    }

    // Temporary solution; assumes that "someone" will transition backbuffer to Present state
    int idx = FindFrameResource(App::GetRenderer().GetCurrentBackBuffer().ID());
    //Assert(idx != -1, "Current backbuffer was not found in frame resources");
    if(idx != -1)
        m_frameResources[idx].State = D3D12_RESOURCE_STATE_PRESENT;
}

void RenderGraph::JoinRenderNodes()
{
    const int numNodes = m_currRenderPassIdx.load(std::memory_order_relaxed);
    m_aggregateNodes.reserve(numNodes);

    int currBatchIdx = 0;
    SmallVector<int, App::FrameAllocator, 16> nonAsyncComputeNodes;
    SmallVector<int, App::FrameAllocator, 16> asyncComputeNodes;

    auto insertAggRndrNode = [this, &nonAsyncComputeNodes, &asyncComputeNodes]()
    {
        Assert(!nonAsyncComputeNodes.empty() || !asyncComputeNodes.empty(), "bug");

        if (!asyncComputeNodes.empty())
        {
            m_aggregateNodes.emplace_back(true);

            bool hasGpuFence = false;
            bool hasUnsupportedBarrier = false;

            for (auto n : asyncComputeNodes)
            {
                const int gpuDep = m_renderNodes[n].GpuDepSourceIdx.Val;

                hasGpuFence = hasGpuFence || (gpuDep != -1);
                hasUnsupportedBarrier = hasUnsupportedBarrier || m_renderNodes[n].HasUnsupportedBarrier;

                const int mappedGpuDepIdx = gpuDep == -1 ? -1 : m_renderNodes[gpuDep].AggNodeIdx;
                Assert(gpuDep == -1 || mappedGpuDepIdx != -1, 
                    "Aggregate node of GPU dependency should come before the dependent node.");

                m_aggregateNodes.back().Append(m_renderNodes[n], mappedGpuDepIdx);
                m_renderNodes[n].AggNodeIdx = (int16)m_aggregateNodes.size() - 1;
            }

            // If there's an async. compute task in this batch that has unsupported barriers,
            // then that task's going to sync with the direct queue immediately before execution,
            // which supersedes any other GPU fence in this joined node.
            m_aggregateNodes.back().GpuDepIdx = hasGpuFence && hasUnsupportedBarrier ?
                RenderNodeHandle(-1) :
                m_aggregateNodes.back().GpuDepIdx;
        }

        if (!nonAsyncComputeNodes.empty())
        {
            m_aggregateNodes.emplace_back(false);

            for (auto n : nonAsyncComputeNodes)
            {
                const int gpuDep = m_renderNodes[n].GpuDepSourceIdx.Val;
                // Map from node index to aggregate node index
                const int mappedGpuDepIdx = gpuDep == -1 ? -1 : m_renderNodes[gpuDep].AggNodeIdx;
                Assert(gpuDep == -1 || mappedGpuDepIdx != -1, 
                    "Aggregate node of GPU dependency should come before the dependent node.");

                m_aggregateNodes.back().Append(m_renderNodes[n], mappedGpuDepIdx);
                m_renderNodes[n].AggNodeIdx = (int16)m_aggregateNodes.size() - 1;
            }
        }
    };

    for (int currNode = 0; currNode < numNodes; currNode++)
    {
        if (m_renderNodes[currNode].NodeBatchIdx != currBatchIdx)
        {
            // When the previous batch contained a single pass that forced a separate 
            // command list, the corresponding aggregate node for that batch has a single
            // pass and was added in the previous iteration. Since there weren't any
            // other passes in that batch, the nodes array are empty.
            if (!nonAsyncComputeNodes.empty() || !asyncComputeNodes.empty())
            {
                insertAggRndrNode();
                nonAsyncComputeNodes.clear();
                asyncComputeNodes.clear();
            }

            currBatchIdx = m_renderNodes[currNode].NodeBatchIdx;
        }

        if (m_renderNodes[currNode].ForceSeparateCmdList)
        {
            m_aggregateNodes.emplace_back(m_renderNodes[currNode].Type == RENDER_NODE_TYPE::ASYNC_COMPUTE);

            const int gpuDep = m_renderNodes[currNode].GpuDepSourceIdx.Val;
            const int mappedGpuDepIdx = gpuDep == -1 ? -1 : m_renderNodes[gpuDep].AggNodeIdx;
            Assert(gpuDep == -1 || mappedGpuDepIdx != -1, 
                "GPU dependency aggregate node should come before the dependent node.");

            m_aggregateNodes.back().Append(m_renderNodes[currNode], mappedGpuDepIdx, true);
            m_renderNodes[currNode].AggNodeIdx = (int16)m_aggregateNodes.size() - 1;

            continue;
        }

        if (m_renderNodes[currNode].Type == RENDER_NODE_TYPE::ASYNC_COMPUTE)
            asyncComputeNodes.push_back(currNode);
        else
            nonAsyncComputeNodes.push_back(currNode);
    }

    insertAggRndrNode();

    m_aggregateNodes.back().IsLast = true;
}

void RenderGraph::MergeSmallNodes()
{
    int currOffset = -1;
    int cmdListIdx = 0;
    int currCount = 0;

    for (int nodeIdx = 0; nodeIdx < (int)m_aggregateNodes.size(); nodeIdx++)
    {
        auto& node = m_aggregateNodes[nodeIdx];

        if (!node.IsAsyncCompute && !node.ForceSeparate && (node.Dlgs.size() == 1))
        {
            node.MergeStart = (currOffset == -1) ? true : false;
            node.MergedCmdListIdx = cmdListIdx;
            currOffset = (currOffset == -1) ? nodeIdx : currOffset;
            currCount++;
        }
        else
        {
            if (currCount)
            {
                auto& prev = m_aggregateNodes[nodeIdx - 1];
                
                if (currCount == 1)
                {
                    Assert(prev.MergeStart && prev.MergedCmdListIdx != -1, "bug");

                    prev.MergeStart = false;
                    prev.MergedCmdListIdx--;
                }
                else
                {
                    prev.MergeEnd = true;
                    cmdListIdx++;
                }
            }

            currCount = 0;
            currOffset = -1;
        }
    }

    if (currCount)
    {
        auto& prev = m_aggregateNodes.back();

        if (currCount == 1)
        {
            Assert(prev.MergeStart && prev.MergedCmdListIdx != -1, "bug");

            prev.MergeStart = false;
            prev.MergedCmdListIdx--;
        }
        else
        {
            prev.MergeEnd = true;
            cmdListIdx++;
        }
    }

    if (cmdListIdx)
        m_mergedCmdLists.resize(cmdListIdx, nullptr);

    //for (int i = 0; i < (int)m_aggregateNodes.size(); i++)
    //{
    //    const auto& node = m_aggregateNodes[i];
    //    if (node.MergeStart)
    //    {
    //        printf("Merge:\n\t");
    //        while (i < (int)m_aggregateNodes.size())
    //        {
    //            if (m_aggregateNodes[i].MergeEnd)
    //                break;
    //            printf("%s (%d), ", m_aggregateNodes[i].Name, m_aggregateNodes[i].MergeCmdListIdx);
    //            i++;
    //        }
    //    }
    //}

#ifndef NDEBUG
    bool inMerged = false;
    currCount = 0;

    for (int i = 0; i < (int)m_aggregateNodes.size(); i++)
    {
        if(inMerged)
            Assert(!m_aggregateNodes[i].MergeStart, "RenderGraph: merge validation failed.");

        if(!inMerged)
            Assert(!m_aggregateNodes[i].MergeEnd, "RenderGraph: merge validation failed.");

        if (m_aggregateNodes[i].MergeStart)
            inMerged = true;

        if (inMerged)
            currCount++;

        if (m_aggregateNodes[i].MergeEnd)
        {
            Assert(!m_aggregateNodes[i].MergeStart, "RenderGraph: merge validation failed.");
            Assert(inMerged, "RenderGraph: merge validation failed.");
            Assert(currCount > 1, "RenderGraph: merge validation failed.");

            inMerged = false;
            currCount = 0;
        }
    }
#endif
}

uint64_t RenderGraph::GetCompletionFence(RenderNodeHandle h)
{
    Assert(h.IsValid(), "invalid handle.");
    Assert(!m_inBeginEndBlock, "invalid call.");
    Assert(!m_inPreRegister, "invalid call.");

    auto mappedIdx = m_mapping[h.Val];
    Assert(mappedIdx.IsValid(), "invalid mapped index");

    auto aggNodeIdx = m_renderNodes[mappedIdx.Val].AggNodeIdx;
    Assert(aggNodeIdx != -1, "render graph hasn't been built yet.");
    // TODO fix
    Assert(m_aggregateNodes[aggNodeIdx].MergedCmdListIdx == -1, 
        "Completion fence for merged command lists is currently unsupported.");
    auto fence = m_aggregateNodes[aggNodeIdx].CompletionFence;
    //Assert(fence != -1, "render node hasn't been submitted yet.");

    return fence;
}

void RenderGraph::SetFrameSubmissionWaitObj(Support::WaitObject& waitObj)
{
    m_submissionWaitObj = &waitObj;
}

uint64_t RenderGraph::GetFrameCompletionFence()
{
    Assert(!m_inBeginEndBlock, "Invalid call.");
    Assert(!m_inPreRegister, "Invalid call.");

    return m_aggregateNodes.back().CompletionFence;
}

void RenderGraph::DebugDrawGraph()
{
    const int numNodes = m_currRenderPassIdx.load(std::memory_order_relaxed);
    const bool needsReorder = m_numPassesLastTimeDrawn != numNodes;

    ImNodes::BeginNodeEditor();

    ImNodes::PushColorStyle(ImNodesCol_TitleBarSelected, IM_COL32(81, 48, 204, 255));

    int batchSize[MAX_NUM_RENDER_PASSES];
    memset(batchSize, 0, sizeof(int) * MAX_NUM_RENDER_PASSES);

    int currBatchIdx = 0;

    // Compute batch sizes
    {
        int currBatchSize = 0;

        for (int currNode = 0; currNode < numNodes; currNode++)
        {
            if (m_renderNodes[currNode].NodeBatchIdx != currBatchIdx)
            {
                batchSize[currBatchIdx] = currBatchSize;

                currBatchSize = 0;
                currBatchIdx = m_renderNodes[currNode].NodeBatchIdx;
            }

            currBatchSize++;
        }

        Assert(currBatchIdx < MAX_NUM_RENDER_PASSES, "out-of-bound write");
        batchSize[currBatchIdx] = currBatchSize;
    }

    const int numBatches = currBatchIdx + 1;
    int currBatchStartPin = 0;
    int currBatchInputPin = 0;
    int currBatchOutputPin = 0;
    currBatchIdx = 0;
    int idxInBatch = 0;
    int numBarriersInBatch = 0;

    for (int currNode = 0; currNode < numNodes; currNode++)
    {
        if (m_renderNodes[currNode].NodeBatchIdx != currBatchIdx)
        {
            const int prevBatchSize = currBatchIdx > 0 ? batchSize[currBatchIdx - 1] : 0;
            const int currBatchSize = batchSize[currBatchIdx];
            const int nextBatchSize = currBatchIdx + 1 < numBatches ? batchSize[currBatchIdx + 1] : 0;

            currBatchIdx = m_renderNodes[currNode].NodeBatchIdx;
            currBatchStartPin += currBatchSize * prevBatchSize + nextBatchSize * currBatchSize;

            currBatchInputPin = 0;
            currBatchOutputPin = 0;
            idxInBatch = 0;
            numBarriersInBatch = 0;
        }

        Assert(currBatchIdx >= 0 && currBatchIdx < numBatches, "out-of-bound access");

        if (m_renderNodes[currNode].Type == RENDER_NODE_TYPE::ASYNC_COMPUTE)
            ImNodes::PushColorStyle(ImNodesCol_TitleBar, IM_COL32(21, 133, 41, 255));
        else if(m_aggregateNodes[m_renderNodes[currNode].AggNodeIdx].MergedCmdListIdx != -1)
            ImNodes::PushColorStyle(ImNodesCol_TitleBar, IM_COL32(15, 51, 109, 255));
        else
            ImNodes::PushColorStyle(ImNodesCol_TitleBar, IM_COL32(155, 21, 41, 255));

        ImNodes::BeginNode(currNode);

        ImNodes::BeginNodeTitleBar();
        ImGui::Text("\t%d. %s, Batch: %d, (GPU dep %d) %s", currNode, m_renderNodes[currNode].Name,
            m_renderNodes[currNode].NodeBatchIdx, m_renderNodes[currNode].GpuDepSourceIdx.Val, 
            m_renderNodes[currNode].Type == RENDER_NODE_TYPE::ASYNC_COMPUTE ? "[Async Compute]" : "");
        ImNodes::EndNodeTitleBar();

#ifndef NDEBUG
        if(m_renderNodes[currNode].Barriers.empty())
            ImGui::Text("");
        else
        {
            for (auto b : m_renderNodes[currNode].Barriers)
            {
                char buff[64] = { '\0' };
                UINT n = sizeof(buff);
                CheckHR(b.Transition.pResource->GetPrivateData(WKPDID_D3DDebugObjectName, &n, buff));

                ImGui::Text("\t\tRes: %s\n\tBefore: %s\nAfter: %s",
                    buff,
                    GetResStateName(b.Transition.StateBefore),
                    GetResStateName(b.Transition.StateAfter));
            }
        }
#else
        ImGui::Text("");
#endif

        const int prevBatchSize = currBatchIdx > 0 ? batchSize[currBatchIdx - 1] : 0;
        const int currBatchSize = batchSize[currBatchIdx];
        const int nextBatchSize = currBatchIdx + 1 < numBatches ? batchSize[currBatchIdx + 1] : 0;

        for (int i = 0; i < prevBatchSize; i++)
        {
            int p = currBatchStartPin + currBatchInputPin;
            ImNodes::BeginInputAttribute(p);
            ImNodes::EndInputAttribute();

            currBatchInputPin++;
        }

        for (int i = 0; i < nextBatchSize; i++)
        {
            int p = currBatchStartPin + currBatchSize * prevBatchSize + currBatchOutputPin;
            ImNodes::BeginOutputAttribute(p);
            ImNodes::EndOutputAttribute();

            currBatchOutputPin++;
        }

        ImNodes::EndNode();
        ImNodes::PopColorStyle();

        if (needsReorder)
        {
            const float x = currBatchIdx * 350.0f;
#ifndef NDEBUG
            const float y = 50.0f + idxInBatch++ * 75.0f + numBarriersInBatch * 60.0f;
#else
            const float y = 50.0f + idxInBatch++ * 75.0f;
#endif

            ImNodes::SetNodeEditorSpacePos(currNode, ImVec2(x, y));

            numBarriersInBatch += (int)m_renderNodes[currNode].Barriers.size();
        }
            //ImNodes::SetNodeScreenSpacePos(currNode, ImVec2(currBatchIdx * 400.0f, 50.0f + idxInBatch++ * 150.0f));
            //ImNodes::SetNodeGridSpacePos(currNode, ImVec2(currBatchIdx * 400.0f, 50.0f + idxInBatch++ * 150.0f));
    }

    currBatchIdx = 0;
    int currEdge = 0;
    currBatchStartPin = 0;
    int batchOutpinStart = 0;
    int nextBatchInpinStart = batchSize[0] * batchSize[1];

    for (int currNode = 0; currNode < numNodes; currNode++)
    {
        if (m_renderNodes[currNode].NodeBatchIdx != currBatchIdx)
        {
            currBatchIdx = m_renderNodes[currNode].NodeBatchIdx;

            const int prevPrevBatchSize = currBatchIdx > 1 ? batchSize[currBatchIdx - 2] : 0;
            const int prevBatchSize = currBatchIdx > 0 ? batchSize[currBatchIdx - 1] : 0;
            const int currBatchSize = batchSize[currBatchIdx];
            const int nextBatchSize = currBatchIdx + 1 < numBatches ? batchSize[currBatchIdx + 1] : 0;

            currBatchStartPin += prevPrevBatchSize * prevBatchSize + prevBatchSize * currBatchSize;
            batchOutpinStart = currBatchStartPin + currBatchSize * prevBatchSize;
            nextBatchInpinStart = batchOutpinStart + nextBatchSize * currBatchSize;

            idxInBatch = 0;
        }

        const int prevBatchSize = currBatchIdx > 0 ? batchSize[currBatchIdx - 1] : 0;
        const int currBatchSize = batchSize[currBatchIdx];
        const int nextBatchSize = currBatchIdx + 1 < numBatches ? batchSize[currBatchIdx + 1] : 0;

        //const int targetPinStart = batchOutpinStart + nextBatchSize * currBatchSize + idxInBatch;

        for (int i = 0; i < nextBatchSize; i++)
        {
            // In between Begin|EndAttribute calls, you can call ImGui
            // UI functions
            //ImGui::Text("out_%d", i);
            const int t = nextBatchInpinStart + i * currBatchSize + idxInBatch;
            ImNodes::Link(currEdge++, batchOutpinStart++, t);
        }

        idxInBatch++;
    }

    ImNodes::PopColorStyle();

    ImNodes::MiniMap(0.3f, ImNodesMiniMapLocation_BottomLeft);
    ImNodes::EndNodeEditor();

    m_numPassesLastTimeDrawn = numNodes;
}

#ifndef NDEBUG
void RenderGraph::Log()
{
    std::string formattedRenderGraph;
    formattedRenderGraph.reserve(2048);

    char temp[256];
    stbsp_snprintf(temp, sizeof(temp), "\nRenderGraph for frame %llu, #batches = %d\n", 
        App::GetTimer().GetTotalFrameCount(), m_aggregateNodes.size());
    formattedRenderGraph += temp;

    int currBatch = 0;
    temp[0] = '\0';

    for (auto& node : m_aggregateNodes)
    {
        stbsp_snprintf(temp, sizeof(temp), "Batch %d\n", currBatch);
        formattedRenderGraph += temp;

        stbsp_snprintf(temp, sizeof(temp), "\t%s (GPU dep %d == %s)\n", node.Name, node.GpuDepIdx.Val, 
            node.GpuDepIdx.Val != -1 ? m_aggregateNodes[node.GpuDepIdx.Val].Name : "None");
        formattedRenderGraph += temp;

        for (auto& b : node.Barriers)
        {
            char buff[64] = { '\0' };
            UINT n = sizeof(buff);
            CheckHR(b.Transition.pResource->GetPrivateData(WKPDID_D3DDebugObjectName, &n, buff));

            stbsp_snprintf(temp, sizeof(temp), "\t\tRes: %s, Before: %s, After: %s\n",
                buff,
                GetResStateName(b.Transition.StateBefore),
                GetResStateName(b.Transition.StateAfter));

            formattedRenderGraph += temp;
        }

        currBatch++;
    }

    formattedRenderGraph += '\n';

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-security"
#endif
    LOG_CONSOLE(formattedRenderGraph.c_str());
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
}
#endif