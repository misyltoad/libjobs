#pragma once

#include <condition_variable>
#include <algorithm>
#include <atomic>
#include <vector>
#include <array>
#include <sstream>
#include <limits>
#include <thread>
#include <string_view>
#include <cstring>
#include <immintrin.h>
#include <functional>
#include <memory>

#include <cstdint>
#ifdef _WIN32
  #include <malloc.h>
  #define libjobs_alloca(x) _alloca(x)
#else
  #include <alloca.h>
  #define libjobs_alloca(x) alloca(x)
#endif

namespace libjobs {

  namespace platform {
    enum class ThreadPriority : int32_t {
      Lowest      = -2,
      BelowNormal = -1,
      Normal      =  0,
      AboveNormal =  1,
      Highest     =  2
    };

    inline void yield() noexcept {
      #if defined(_WIN32)
      YieldProcessor();
      #elif defined(SSE2) || defined(__SSE2__)
      _mm_pause();
      #else
      #warning No yield implementation, using std::this_thread::yield()
      std::this_thread::yield();
      #endif
    }

    inline void setThreadName(std::thread& thread, std::string_view name) noexcept {
      #ifdef _WIN32
      auto wideName = std::wstring(name.begin(), name.end());
      ::SetThreadDescription(thread.native_handle(), wideName.c_str());
      #else
      std::array<char, 16> posixName = {};
      std::strncpy(posixName.data(), name.data(), 15);
      ::pthread_setname_np(thread.native_handle(), posixName.data());
      #endif
    }

    inline void setThreadPriority(std::thread& thread, ThreadPriority priority) noexcept {
      #ifdef _WIN32
      ::SetThreadPriority(thread.native_handle(), static_cast<int32_t>(priority));
      #else
      ::sched_param param = {};
      int32_t policy;
      switch (priority) {
        case ThreadPriority::Highest:
          policy = SCHED_FIFO;
          param.sched_priority = sched_get_priority_max(SCHED_FIFO);
          break;
        case ThreadPriority::AboveNormal:
          policy = SCHED_FIFO;
          param.sched_priority = sched_get_priority_max(SCHED_FIFO) / 2;
          break;
        default:
        case ThreadPriority::Normal:
          policy = SCHED_OTHER;
          break;
        case ThreadPriority::BelowNormal:
          policy = SCHED_BATCH;
          break;
        case ThreadPriority::Lowest:
          policy = SCHED_IDLE;
          break;
      }

      ::pthread_setschedparam(thread.native_handle(), policy, &param);
      #endif
    }

    inline void pinThread(std::thread& thread, uint32_t index) noexcept {
      #ifdef _WIN32
      DWORD mask = 1 << index;
      ::SetThreadAffinityMask(thread.native_handle(), mask);
      #else
      cpu_set_t cpuset;
      CPU_ZERO(&cpuset);
      CPU_SET(index, &cpuset);
      ::pthread_setaffinity_np(thread.native_handle(), sizeof(cpu_set_t), &cpuset);
      #endif
    }

    inline uint32_t threadCount() noexcept {
      return std::thread::hardware_concurrency();
    }
  }
  using platform::ThreadPriority;

  template <typename T, size_t Capacity>
  class RingBuffer {
  public:
    inline void push_back(const T& item) noexcept {
      while (!try_push_back(item))
        platform::yield();
    }

    inline bool try_push_back(const T& item) noexcept {
      std::unique_lock lock(m_mutex);
      if (full())
        return false;

      m_data[m_head] = item;
      m_head         = next(m_head);
      return true;
    }

    inline bool pop_front(T& item) noexcept {
      std::unique_lock lock(m_mutex);
      if (empty())
        return false;

      item   = m_data[m_tail];
      m_tail = next(m_tail);
      return true;
    }

    constexpr size_t capacity() const noexcept { return Capacity; }

  private:
    // Not thread safe, internally used.
    [[nodiscard]] constexpr bool full()  const noexcept { return next(m_head) == m_tail; }
    [[nodiscard]] constexpr bool empty() const noexcept { return m_tail == m_head; }

    [[nodiscard]] constexpr size_t next(size_t index) const noexcept { return (index + 1) % Capacity; }

    std::array<T, Capacity> m_data = {};
    size_t                  m_head = 0;
    size_t                  m_tail = 0;
    std::mutex              m_mutex;
  };

  struct JobContext {
    uint32_t localInvocationIndex;  // The job index relative to the group, Akin to gl_LocalInvocationIndex, SV_GroupIndex.
    uint32_t localInvocationCount;  // The number of invocations for this group.
    uint32_t globalInvocationIndex; // The job index, akin to gl_GlobalInvocationID, SV_DispatchThreadID.
    uint32_t workgroupId;           // The group index relative to the global invocation, akin to gl_WorkGroupID, SV_GroupID.
    void*    sharedMemory;

    constexpr bool isFirstJobInGroup() const noexcept { return localInvocationIndex == 0; }
    constexpr bool isLastJobInGroup()  const noexcept { return localInvocationIndex == localInvocationCount - 1; }
  };

  using JobFunction = std::function<bool(JobContext&&)  >;

  class JobWatcher {
  public:
    [[nodiscard]] 
    inline bool busy() const noexcept { return m_counter.load() > 0; }
    inline void wait() const noexcept { while(busy()) platform::yield(); }
  protected:
    template <size_t MaxJobCount> friend class JobRunner;
    inline void newJob()      noexcept { m_counter.fetch_add(1, std::memory_order_acquire); }
    inline void jobComplete() noexcept { m_counter.fetch_sub(1, std::memory_order_release); }
  private:
    std::atomic<uint32_t> m_counter = { 0u };
  };

  template <size_t MaxJobCount>
  class JobRunner {
  public:
    JobRunner(
      uint32_t         maxThreads   = std::numeric_limits<uint32_t>::max(),
      std::string_view threadPrefix = "libjobs",
      ThreadPriority   priority     = ThreadPriority::Normal,
      bool             pinToCore    = false) {
      uint32_t numThreads = std::min(platform::threadCount(), maxThreads);

      for (uint32_t i = 0; i < numThreads; i++) {
        auto thread = std::thread([this, &mutex = m_wakeMutex, &condition = m_wakeCondition] {
          for (;;) {
            WorkerState state = work();
            if (state == WorkerState::Work) {
              continue;
            } else if (state == WorkerState::Sleep) {
              auto lock = std::unique_lock(mutex);
              condition.wait(lock);
              continue;
            } else if (state == WorkerState::Terminate) {
              break;
            }
          }
        });

        std::string threadName;
        {
          std::stringstream ss;
          ss << threadPrefix << "_" << i;
          threadName = ss.str();
        }
        platform::setThreadName(thread, threadName);
        platform::setThreadPriority(thread, priority);
        if (pinToCore)
          platform::pinThread(thread, i);

        m_threads.emplace_back(std::move(thread));
      }
    }

    ~JobRunner() {
      // As each thread dies, it won't be able to take another terminate job.
      JobWatcher terminateWatcher;
      dispatch(terminateWatcher, [](JobContext&& ctx) { return false; }, threadCount(), 1);
      terminateWatcher.wait();

      for (auto& thread : m_threads)
        thread.join();
    }

    inline void execute(JobWatcher& watcher, JobFunction function) noexcept {
      watcher.newJob();

      JobDesc desc = {
        .watcher               = &watcher,
        .function              = function,
        .workgroupId           = 0,
        .localInvocationOffset = 0,
        .localInvocationCount  = 1,
        .sharedMemorySize      = 0
      };

      m_queue.push_back(desc);
      m_wakeCondition.notify_one();
    }

    inline void execute(JobFunction function) noexcept {
      execute(m_throwawayWatcher, function);
    }

    inline void dispatch(JobWatcher& watcher, JobFunction function, uint32_t jobCount, uint32_t groupSize, uint32_t sharedMemorySize = 0) noexcept {
      if (jobCount == 0 || groupSize == 0)
        return;

      uint32_t groupCount = (jobCount + groupSize - 1) / groupSize;

      for (uint32_t i = 0; i < groupCount; i++) {
        watcher.newJob();

        JobDesc desc = {
          .watcher               = &watcher,
          .function              = function,
          .workgroupId           = i,
          .localInvocationOffset = i * groupSize,
          .localInvocationCount  = std::min(groupSize, jobCount),
          .sharedMemorySize      = sharedMemorySize
        };
        jobCount -= groupSize;

        m_queue.push_back(desc);
      }

      m_wakeCondition.notify_all();
    }

    inline void dispatch(JobFunction function, uint32_t jobCount, uint32_t groupSize, uint32_t sharedMemorySize = 0) noexcept {
      dispatch(m_throwawayWatcher, function, jobCount, groupSize, sharedMemorySize);
    }

    inline uint32_t threadCount() const { return uint32_t(m_threads.size()); }

    inline void waitAndWork(JobWatcher& watcher) noexcept {
      while (watcher.busy()) {
        if (work() != WorkerState::Work)
          platform::yield();
      }
    }

    inline void synchronise() noexcept {
      std::atomic<uint32_t> counter = { 0 };
      dispatch([&counter](JobContext&& ctx) {
        counter++;
        return true;
      }, threadCount(), 1);

      while (counter != threadCount())
        platform::yield();
    }

    inline void barrier() noexcept {
      auto counter = std::make_shared<std::atomic<uint32_t>>(0);
      dispatch([counter, threadCount = threadCount()](JobContext&& ctx) {
        ++*counter;

        while (*counter != threadCount)
          platform::yield();

        return true;
      }, threadCount(), 1);
    }

  private:
    struct JobDesc {
      JobWatcher* watcher;
      JobFunction function;
      uint32_t    workgroupId;
      uint32_t    localInvocationOffset;
      uint32_t    localInvocationCount;
      uint32_t    sharedMemorySize;
    };

    enum class WorkerState {
      Work,
      Sleep,
      Terminate
    };

    [[nodiscard]]
    inline WorkerState work() noexcept {
      JobDesc desc;

      if (!m_queue.pop_front(desc))
        return WorkerState::Sleep;

      void* sharedMemory = libjobs_alloca(desc.sharedMemorySize);

      for (uint32_t i = 0; i < desc.localInvocationCount; i++) {
        JobContext ctx = {
          .localInvocationIndex  = i,
          .localInvocationCount  = desc.localInvocationCount,
          .globalInvocationIndex = i + desc.localInvocationOffset,
          .workgroupId           = desc.workgroupId,
          .sharedMemory          = sharedMemory
        };

        bool shouldTerminate = !desc.function(std::move(ctx));

        if (shouldTerminate) {
          desc.watcher->jobComplete();
          return WorkerState::Terminate;
        }
      }
      desc.watcher->jobComplete();

      return WorkerState::Work;
    }

    std::vector<std::thread>         m_threads;
    std::mutex                       m_wakeMutex;
    std::condition_variable          m_wakeCondition;
    RingBuffer<JobDesc, MaxJobCount> m_queue;

    JobWatcher                       m_throwawayWatcher;
  };

}
