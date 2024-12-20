#include "ThreadPool.h"
#include "../App/Log.h"

using namespace ZetaRay::Support;
using namespace ZetaRay::App;
using namespace ZetaRay::Util;

namespace
{
    ZetaInline int FindThreadIdx(Span<ZETA_THREAD_ID_TYPE> threadIds)
    {
        int idx = -1;

        for (int i = 0; i < (int)threadIds.size(); i++)
        {
            if (threadIds[i] == GetCurrentThreadId())
            {
                idx = i;
                break;
            }
        }

        return idx;
    }
}

//--------------------------------------------------------------------------------------
// ThreadPool
//--------------------------------------------------------------------------------------

void ThreadPool::Init(int poolSize, int totalNumThreads, const wchar_t* threadNamePrefix, 
    THREAD_PRIORITY priority)
{
    m_threadPoolSize = poolSize;
    m_totalNumThreads = totalNumThreads;

    // Tokens below have to conisder that threads outside this thread pool
    // (e.g. the main thread) may also insert tasks and occasionally execute 
    // tasks (for example when trying to pump the queue to empty it)

    // Consumers
    {
        // +1 for main therad
        uintptr_t curr = reinterpret_cast<uintptr_t>(m_consumerTokensMem);
        for (int i = 0; i < m_totalNumThreads; i++)
        {
            new (reinterpret_cast<void*>(curr)) moodycamel::ConsumerToken(m_taskQueue);
            curr += sizeof(moodycamel::ConsumerToken);
        }

        m_consumerTokens = reinterpret_cast<moodycamel::ConsumerToken*>(m_consumerTokensMem);
    }

    // Producers
    {
        // +1 for main therad
        uintptr_t curr = reinterpret_cast<uintptr_t>(m_producerTokensMem);
        for (int i = 0; i < m_totalNumThreads; i++)
        {
            new (reinterpret_cast<void*>(curr)) moodycamel::ProducerToken(m_taskQueue);
            curr += sizeof(moodycamel::ProducerToken);
        }

        m_producerTokens = reinterpret_cast<moodycamel::ProducerToken*>(m_producerTokensMem);
    }

    for (int i = 0; i < m_threadPoolSize; i++)
    {
        m_threadPool[i] = std::thread(&ThreadPool::WorkerThread, this);
        m_threadIDs[i] = App::GetThreadID(m_threadPool[i].native_handle());

        wchar_t buffer[32];
        //swprintf(buff, L"ZetaWorker_%d", i);
        swprintf(buffer, L"%ls_%d", threadNamePrefix, i);
        App::SetThreadDesc(m_threadPool[i].native_handle(), buffer);

        App::SetThreadPriority(m_threadPool[i].native_handle(), priority);
    }
}

void ThreadPool::Start(Span<ZETA_THREAD_ID_TYPE> threadIDs)
{
    Assert(threadIDs.size() == m_totalNumThreads, "these must match");

    for (int i = 0; i < threadIDs.size(); i++)
        m_allThreadIds[i] = threadIDs[i];

    m_start.store(true, std::memory_order_release);
}

void ThreadPool::Shutdown()
{
    // Relaxed since Enqueue has a release op
    m_shutdown.store(true, std::memory_order_relaxed);

    // Upon observing shutdown flag to be true, all the threads are going to exit

    for (int i = 0; i < m_threadPoolSize; i++)
    {
        Task t("NoOp", TASK_PRIORITY::NORMAL, []() {});
        Enqueue(ZetaMove(t));
    }

    for (int i = 0; i < m_threadPoolSize; i++)
        m_threadPool[i].join();
}

void ThreadPool::Enqueue(Task&& task)
{
    const int idx = FindThreadIdx(Span(m_allThreadIds, m_totalNumThreads));
    Assert(idx != -1, "Thread ID was not found");

    bool memAllocFailed = m_taskQueue.enqueue(m_producerTokens[idx], ZetaMove(task));
    Assert(memAllocFailed, "moodycamel::ConcurrentQueue couldn't allocate memory.");

    m_numTasksToFinishTarget.fetch_add(1, std::memory_order_relaxed);
    m_numTasksInQueue.fetch_add(1, std::memory_order_release);
}

void ThreadPool::Enqueue(TaskSet&& ts)
{
    Assert(ts.IsFinalized(), "Given TaskSet is not finalized.");

    m_numTasksToFinishTarget.fetch_add(ts.GetSize(), std::memory_order_relaxed);
    m_numTasksInQueue.fetch_add(ts.GetSize(), std::memory_order_release);
    auto tasks = ts.GetTasks();

    const int idx = FindThreadIdx(Span(m_allThreadIds, m_totalNumThreads));
    Assert(idx != -1, "Thread ID was not found");

    bool memAllocFailed = m_taskQueue.enqueue_bulk(m_producerTokens[idx], 
        std::make_move_iterator(tasks.data()), tasks.size());
    Assert(memAllocFailed, "moodycamel::ConcurrentQueue couldn't allocate memory.");
}

void ThreadPool::PumpUntilEmpty()
{
    const int idx = FindThreadIdx(Span(m_allThreadIds, m_totalNumThreads));
    Assert(idx != -1, "Thread ID was not found");

    const ZETA_THREAD_ID_TYPE tid = GetCurrentThreadId();
    Task task;

    // "try_dequeue()" returning false doesn't guarantee that queue is empty
    while (m_numTasksInQueue.load(std::memory_order_acquire) != 0)
    {
        if (m_taskQueue.try_dequeue(m_consumerTokens[idx], task))
        {
            m_numTasksInQueue.fetch_sub(1, std::memory_order_relaxed);

            const int taskHandle = task.GetSignalHandle();

            // Block if this task depends on other unfinished tasks
            App::WaitForAdjacentHeadNodes(taskHandle);

            task.DoTask();

            // Signal dependent tasks that this task has finished
            auto adjacencies = task.GetAdjacencies();
            if (adjacencies.size() > 0)
                App::SignalAdjacentTailNodes(adjacencies);

            m_numTasksFinished.fetch_add(1, std::memory_order_release);
        }
    }
}

bool ThreadPool::TryFlush()
{
    const bool success = m_numTasksFinished.load(std::memory_order_acquire) == 
        m_numTasksToFinishTarget.load(std::memory_order_acquire);
    if (!success)
    {
        PumpUntilEmpty();
    }
    else
    {
        // Reset the counters
        m_numTasksFinished.store(0, std::memory_order_relaxed);
        m_numTasksToFinishTarget.store(0, std::memory_order_relaxed);
    }

    return success;
}

void ThreadPool::WorkerThread()
{
    while (!m_start.load(std::memory_order_acquire));

    const ZETA_THREAD_ID_TYPE tid = GetCurrentThreadId();
    LOG_UI(INFO, "Thread %u waiting for tasks...\n", tid);

    const int idx = FindThreadIdx(Span(m_allThreadIds, m_totalNumThreads));
    Assert(idx != -1, "Thread ID was not found");

    while (true)
    {
        Task task;

        // Exit
        if (m_shutdown.load(std::memory_order_acquire))
            break;

        // block if there aren't any tasks
        m_taskQueue.wait_dequeue(m_consumerTokens[idx], task);
        m_numTasksInQueue.fetch_sub(1, std::memory_order_acquire);

        const int taskHandle = task.GetSignalHandle();

        // Block if this task has unfinished dependencies
        if(task.GetPriority() != TASK_PRIORITY::BACKGROUND)
            App::WaitForAdjacentHeadNodes(taskHandle);

        task.DoTask();

        // Signal dependent tasks that this task has finished
        if (task.GetPriority() != TASK_PRIORITY::BACKGROUND)
        {
            auto adjacencies = task.GetAdjacencies();
            if (adjacencies.size() > 0)
                App::SignalAdjacentTailNodes(adjacencies);
        }

        m_numTasksFinished.fetch_add(1, std::memory_order_release);
    }

    LOG_UI(INFO, "Thread %u exiting...\n", tid);
}
