# libjobs

A cross-platform, header-only compute-shader-styled job dispatching API written in C++.

## Features

* A compute-shader-styled job dispatching API
  * Dispatch `x` number of jobs across `y` number of threads
    * Local Invocation Index
    * Local Invocation Count
    * Global Invocation Index
    * Workgroup Id
    * Shared Memory
      * Across a workgroup
    * (You already know the Global Invocation Count yourself, so you can capture it)
* Execute a single function asynchronously
* Easily wait for a single job or multiple set of jobs to complete
* Fire and forget functions or dispatches
* Jobs are lambda functions with capture support
* Entirely thread safe api
* Help with work on this thread while you wait with `JobRunner::waitAndWork`
* Scope safety
  * When a `JobRunner` goes out of scope, all pending jobs will be completed before shutting down
    * This is done without tracking jobs/watchers inside the `JobRunner`
* Cross platform thread API
  * Thread naming
  * Thread priorities
  * Thread yielding
  * Thread pinning
    * All of this is configurable and supported by the job runner
    * Exposed for use on normal `std::thread`s also in `libjobs::platform`

## License

libjobs is licensed under zlib/libpng.

## Requirements

**libjobs** *requires* a compiler with support for C++20.

## How to use

Clone or download this repository, and include the `libjobs.h` header into your project.

All functions/classes/etc in the library are in the `libjobs` namespace.

## Examples

Create a ``libjobs::JobRunner`` like so:
```cpp
libjobs::JobRunner<256> runner(libjobs::platform::threadCount() - 1);
```
where 256 = the maximum number of queued jobs,
and the first argument represents the maximum number of threads the runner can use.

In this case we leave the main thread free.

### Throwaway async!
```cpp
// Throwaway async!
runner.execute([](libjobs::JobContext&& ctx) -> bool {
  printf("Asynchronous frog!\n");
  return true;
});
```
Will output:
``Asynchronous frog!``

### Counting to 100!
```cpp
libjobs::JobWatcher watcher;
std::atomic<uint32_t> counter = 0;
runner.dispatch(watcher, [&counter](libjobs::JobContext&& ctx) -> bool {
  counter++;
  return true;
}, 100, runner.threadCount());
watcher.wait();
printf("All %u of us counted to %u!\n", runner.threadCount(), counter.load());
```
Will output:
``All 23 of us counted to 100!`` (where 23 == the number of threads on the JobRunner)

### Progress counting!
```cpp
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
```
Will output:
```
Loading: 0/100 complete!
Loading: 1/100 complete!
Loading: 2/100 complete!
Loading: 3/100 complete!
Loading: 4/100 complete!
Loading: 5/100 complete!
Loading: 6/100 complete!
Loading: 7/100 complete!
Loading: 8/100 complete!
[...]
Loading: 97/100 complete!
Loading: 98/100 complete!
Loading: 99/100 complete!
Loading: 100/100 complete!
```

### Cool context information!

```cpp
std::mutex waitingMutex;
bool printedExample = false;
libjobs::JobWatcher watcher;
const uint32_t globalInvocationCount = 128;
runner.dispatch(watcher, [&printedExample, &waitingMutex, globalInvocationCount](libjobs::JobContext&& ctx) -> bool {
  std::unique_lock lock(waitingMutex);
  if (printedExample)
    return true;

  printedExample = true;
  printf("localInvocationIndex:  %u\n", ctx.localInvocationIndex);
  printf("localInvocationCount:  %u\n", ctx.localInvocationCount);
  printf("globalInvocationIndex: %u\n", ctx.globalInvocationIndex);
  // You pass in the global invocation count (jobCount) yourself,
  // so we don't expose this in the context, but you can easily
  // just capture it.
  printf("globalInvocationCount: %u\n", globalInvocationCount);
  printf("workgroupId:           %u\n", ctx.workgroupId);
  printf("sharedMemory:          %p\n", ctx.sharedMemory);
  return true;
}, globalInvocationCount, runner.threadCount(), 128);
watcher.wait();
```
Will output:
```
localInvocationIndex:  0
localInvocationCount:  23
globalInvocationIndex: 0
globalInvocationCount: 128
workgroupId:           0
sharedMemory:          0x7ff997372cc0
```
where 23 is the number of threads on the job runner that we passed in for the group size.

*(note: do not assume `groupSize == localInvocationCount`, it is not guaranteed if the `groupSize` does not divide into the `jobCount`)*

### Scope safety!

```cpp
// This job will be completed and waited upon when the
// runner goes out of scope.
printf("Running a job that waits a bit and waiting on its destruction.\n");
runner.dispatch([](libjobs::JobContext&& ctx) -> bool {
  using namespace std::chrono_literals;
  std::this_thread::sleep_for(2s);
  printf("I finished waiting, goodbye!\n");
  return true;
}, 1, 1);
return 0;
```
Will output:
```
Running a job that waits a bit and waiting on its destruction.
[ 2 seconds of time... ]
I finished waiting, goodbye!
```
