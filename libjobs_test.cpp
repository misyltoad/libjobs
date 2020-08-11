#include "libjobs.h"

#include <atomic>
#include <chrono>

int main(int argc, char** argv) {
  libjobs::JobRunner<256> runner(libjobs::platform::threadCount() - 1);

  {
    // Throwaway async!
    runner.execute([](libjobs::JobContext&& ctx) -> bool {
      printf("Asynchronous frog!\n");
      return true;
    });
  }

  {
    // Counting to 100!
    libjobs::JobWatcher watcher;
    std::atomic<uint32_t> counter = 0;
    runner.dispatch(watcher, [&counter](libjobs::JobContext&& ctx) -> bool {
      counter++;
      return true;
    }, 100, runner.threadCount());
    watcher.wait();
    printf("All %u of us counted to %u!\n", runner.threadCount(), counter.load());
  }

  // Progress counting!
  {
    libjobs::JobWatcher watcher;
    std::atomic<uint32_t> counter = 0;
    const uint32_t globalInvocationCount = 100;
    runner.dispatch(watcher, [&counter](libjobs::JobContext&& ctx) -> bool {
      using namespace std::chrono_literals;
      std::this_thread::sleep_for(10ms);
      counter++;
      return true;
    }, globalInvocationCount, runner.threadCount());

    uint32_t lastCounter = std::numeric_limits<uint32_t>::max();
    while (watcher.busy()) {
      uint32_t counterValue = counter.load();
      if (lastCounter != counterValue)
        printf("Loading: %u/%u complete!\n", counterValue, globalInvocationCount);
      lastCounter = counterValue;
      libjobs::platform::yield();
    }
    uint32_t counterValue = counter.load();
    if (lastCounter != counterValue)
      printf("Loading: %u/%u complete!\n", counterValue, globalInvocationCount);
  }

  {
    // Cool information available in the context
    std::mutex waitingMutex;
    bool printedExample = false;
    libjobs::JobWatcher watcher;
    const uint32_t globalInvocationCount = 128;
    runner.dispatch(watcher, [&printedExample, &waitingMutex](libjobs::JobContext&& ctx) -> bool {
      std::unique_lock lock(waitingMutex);
      if (printedExample)
        return true;

      printedExample = true;
      printf("localInvocationIndex:  %u\n", ctx.localInvocationIndex);
      printf("localInvocationCount:  %u\n", ctx.localInvocationCount);
      printf("globalInvocationIndex: %u\n", ctx.globalInvocationIndex);
      // You pass in the global invocation count (jobCount) yourself,
      // so we don't expose this in the context, but you can easily
      // just capture it, or take advantage if it is const of the
      // implicit capture.
      printf("globalInvocationCount: %u\n", globalInvocationCount);
      printf("workgroupId:           %u\n", ctx.workgroupId);
      printf("sharedMemory:          %p\n", ctx.sharedMemory);
      return true;
    }, globalInvocationCount, runner.threadCount(), 128);
    watcher.wait();
  }


  {
    // This job will be completed and waited upon when the
    // runner goes out of scope.
    printf("Running a job that waits a bit and waiting on its destruction.\n");
    runner.dispatch([](libjobs::JobContext&& ctx) -> bool {
      using namespace std::chrono_literals;
      std::this_thread::sleep_for(2s);
      printf("I finished waiting, goodbye!\n");
      return true;
    }, 1, 1);
  }

  return 0;
}
