---
id: Cache_persistence
title: Cache persistence
---

Cachelib supports *persisting the cache across process restarts*. This is useful when you want to restart your binary that contains a  cache and not lose the cache upon restart.  Cache persistence only works when you restart your process in the same machine. This does not provide persistence across machines.

Unlike a DB where persistent state is stored in a non-volatile medium like Disk or SSD, cachelib persists its cache by retaining the memory associated with the cache across processes. Hence using a persistent cache&mdash;even the size of hundreds of GB&mdash;does not impact the cache shutdown or creation, which is very quick.

## Create a persistent cache

To make your cache persistent in memory, when creating the cache, you need to use the appropriate constructor of type `ShareMemNew` or `SharedMemAttach`. To support persistence, you  need to provide a directory location (cache path) for cachelib to manage the persistence. This will be used to store some *small metadata* about the cache instance that will help us restore the cache. This directory path also uniquely identifies the instance of the cache and only one cache instance can be active at any given time corresponding to a given path.


```cpp
Cache::Config config;
config.setCacheSize(/* size of cache in bytes */);
config.enableCachePersistence(/* directory for shared memory related metadata */);

Cache cache(Cache::SharedMemNew, config);
// ... Use the cache
```

## Shut down a persistent cache

To make sure that your cache is saved in the correct state, *ensure that you have drained all accesses to your cache* and call the `shutdown()` API in your process shutdown path. This ensures that cachelib saves all the relevant information to restore the cache.


```cpp
// Save the metadata and shut down after this point. After that,
// accessing the cache content results in undefined behavior.
//
// Note: If you use cache admin, you must destroy it before calling shutdown
//       (e.g., cacheAdmin->reset();)
auto res = cache.shutDown();
if (res == Cache::ShutDownStatus::kSuccess) {
   // Successfully shut down the cache. Can attempt recovering.
} else {
   // Failure.
}
```


`ShutDownRes` contains three possible states, which can be used to identify the state of the cache upon shutting down:


```cpp
enum class ShutDownRes { kSuccess = 0, kFileDeleted, kFailedWrite }
```


`kSuccess` means the metadata has been successfully saved. `kFileDeleted` indicates that the cache directory was deleted while the cache was running. And `kFailedWrite` indicates any other system error that prevented cachelib from saving the metadata.

## Restore a persistent cache

On start up, try to use the following constructor to attach to your previous instance of cache. Upon failure, you can create a new one like before. A common pattern that many cachelib users do is to try to always attach on startup; if attach fails, which will throw an exception, catch the exception and then try to create a new cache. Also, please note that the pools should not be added again if the attach has been succeeded.


```cpp
Cache::Config config;
config.setCacheSize(/* this must be the same size you specified before */);
config.enableCachePersistence(
    /* this must be the same cache directory you specified before */);

bool attached = false;
std::unique_ptr<Cache> cache;
try {
  cache = std::make_unique<Cache>(Cache::SharedMemAttach, config);
  // Cache is now restored
  attached = true;
} catch (const std::exception& ex) {
  // Attaching failed. Create a new one but make sure that
  // the old cache is destroyed before creating a new one.
  // This allows us to release any held resources (such as
  // open file descriptors and associated fcntl locks).
  cache.reset();
  std::cerr << "Couldn't attach to cache: " << ex.what() << std::endl;
  cache = std::make_unique<Cache>(Cache::SharedMemNew, config);
}

if (!attached) {
  // Add pool only if the cache is not restored above
  cache->addPool("default", cache->getCacheMemoryStats().ramCacheSize);
}
```


## Back the cache with huge pages (HugeTLB)

CacheLib can back the *slab memory (main cache)* and the *access-container hash tables* with [HugeTLB](https://docs.kernel.org/admin-guide/mm/hugetlbpage.html) pages to lower TLB pressure. Each side is configured independently: `enableSlabHugePages` controls slab memory, `enableAccessHugePages` controls both access-container hash tables (main and chained-item), and `enableHugePages` is a shorthand that enables both with the same size. All of them take a huge-page size that the kernel supports (see available sizes in the subdirectories of `/sys/kernel/mm/hugepages`) and share a single `hugetlbfs` mount directory:

```cpp
Cache::Config config;
config.setCacheSize(/* size of cache in bytes */);
config.enableCachePersistence(/* directory for shared memory related metadata */);
// Back slabs and hash tables with 2MB huge pages.
config.enableHugePages(cachelib::PageSize(2 * 1024 * 1024));
```

```cpp
// Back only the hash tables with 2MB huge pages; slabs stay on normal pages.
config.enableAccessHugePages(cachelib::PageSize(2 * 1024 * 1024));
```

Backing only the access containers is useful on CPUs with a small number of huge-page TLB slots: the hash tables sit on the latency-sensitive lookup path, so covering them with huge pages while leaving the much larger slab memory on normal pages spends the limited TLB entries where they matter most.

The HugeTLB pool must be provisioned out-of-band by reserving pages via `/proc/sys/vm/nr_hugepages`, `hugeadm` or [kernel command line args](https://docs.kernel.org/admin-guide/mm/hugetlbpage.html). CacheLib only draws from the already-reserved pool. SysV and POSIX shared memory fail cache creation if there aren't enough huge pages available. Heap-backed (non-persistent) and temporary-shared-memory caches instead map anonymous HugeTLB pages that succeed at `mmap` time even with an empty pool and then fault with `SIGBUS` on first touch, so always size the reserved pool before enabling huge pages there.

Huge pages are supported for SysV shared memory (the default), POSIX shared memory, temporary shared memory (cache with memory monitoring enabled but no persistence), and plain (non-persistent) heap memory with no memory monitoring. For POSIX shared memory you must also pass a mounted `hugetlbfs` directory as the second argument; cache creation throws if it is empty:

```cpp
config.enableCachePersistence(/* cache directory */)
    .usePosixForShm()
    .enableHugePages(cachelib::PageSize(2 * 1024 * 1024), "/mnt/hugetlbfs");
```

For a persistent cache, the slab huge-page size, the access huge-page size, and — for POSIX shared memory — the `hugetlbfs` mount directory are all part of the persisted metadata. A warm roll requires all of these to match the previous run; any change forces a cold roll, where CacheLib removes the old shared-memory segments and starts with an empty cache:

| Change across restart | Result |
|---|---|
| Same slab size, access size, mount dir, backing | Warm roll |
| Slab size changed (including enabled/disabled flips) | Cold roll |
| Access size changed (including enabled/disabled flips) | Cold roll |
| `hugetlbfs` mount dir changed (POSIX) | Cold roll |
| `hugetlbfs` mount dir changed (SysV) | Warm roll (mount dir is ignored) |
| Backing type changed (e.g. SysV to POSIX) | Startup failure (see below) |

The cold roll on a page-size change is deliberate, even for SysV shared memory where attach would otherwise succeed: SysV ignores page configuration at attach time, so without the explicit check a warm roll would silently keep the old segments on their creation-time backing while the new configuration claims otherwise. Failing attach guarantees that changing the configuration actually changes the backing — enabling huge pages on a warm cache recreates the segments on huge pages instead of being a silent no-op — and keeps the exported cache config truthful. POSIX behaves the same way necessarily (segments live at different paths), so the rule is uniform across backings: page configuration is part of cache identity.

Changing the backing type does not cold-roll: `ShmManager` rejects the attach with `invalid_argument` before clearing the persisted metadata, so both attach and create-new fail on every restart until `cacheDir` is removed.

Huge pages are not supported with cross-host cache persistence. They're also not supported with memory monitoring when the *slab* huge page size is bigger than the slab size - for example, you can't punch a slab-sized hole in a 1GB huge page. (The access huge-page size is exempt: hash tables are never `madvise`d away, so access-only huge pages work with memory monitoring regardless of size.)

## Keep POSIX segments in a directory

By default POSIX shared memory segments live in `/dev/shm`. To keep them as files in another directory, for example a tmpfs volume that outlives a container restart, call `usePosixShmDirectory(dir)` instead of `usePosixForShm()`. The directory must exist and should be on tmpfs; a disk filesystem would write the cache back to disk. The directory is not saved with the cache: if it differs from the previous run, `SharedMemAttach` throws as if there were no saved cache, and segment files in a previous custom directory are left for you to delete. Huge-page segments still use their `hugetlbfs` mount. Cross-host persistence does not support this option yet.

## Drop persistent cache

Sometimes you would like your cache to be not persistent when you restart your process. There are two ways to accomplish this:

1. Calling the `SharedMemNew` constructor will always create a new instance and blow away the old one.
2. Indicate to cachelib by blowing away the cache directory that you passed in the `Config`. This will cause `SharedMemAttach` to throw an exception.

If NVM cache is used, it will be dropped along with the RAM cache if `CacheAllocatorConfig.dropNvmCacheOnShmNew` is set to `true`. Otherwise or if you want to drop NVM cache separately, there are two ways to accomplish it.

1. Remove `NvmCacheState` file in `cacheDir`

2. Create (touch) a file named `IceRoll` in `cacheDir`

## Change configs

When you attach to an existing cache, cachelib will try to incorporate the config changes; however not all configs can be changed while attaching. Notice these two important points:

* The size of the cache is immutable unless you drop the previous instance.

## Apply best practices

If you haven't done so, *please consider adding a try-catch block in your main or any top level code that will be working with cachelib API*. This is because cachelib APIs can throw exceptions, and when it comes to persisting states, we expect the stack to be properly unwound to ensure the state is not corrupted. Uncaught exception does not unwind the stack properly and can lead to state corruption for cachelib. For more information, see [Is stack unwinding with exceptions guaranteed by C++ standard?](https://stackoverflow.com/questions/39962999/is-stack-unwinding-with-exceptions-guaranteed-by-c-standard) on StackOverflow.

## Common failure scenarios

When using persistent setup, CacheLib locks the path to ensure only one instance can operate corresponding to a given cache path. When you see failures to startup due to *"Can not lock shm metadata file: Device or resource busy"*, this usually means there are more than one processes attempting to have operate a cache with the same path. To resolve this, look for zombie processes or other processes that might be still running with an active cachelib instance corresponding to the same cache path.
