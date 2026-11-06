---
id: pool_rebalance_strategy
title: Pool rebalance strategy
---

## When do you need pool rebalancing?

If your cachelib use case always allocates objects of a single size, then
rebalancing is almost always not required for you. Rebalancing of cache
becomes *important only when you store variable sized objects* in cache and
your workload's footprint of access across these objects can potentially
change over time. Often, when you cache objects of variable size, the
distribution of `find()` and `allocate()` across object sizes would vary over
time. This can leave slabs assigned to allocation classes that no longer match
the current workload. For example, imagine you had a cache of 30 GB and store
objects of size 100 bytes, 500 bytes, and 1,000 bytes, each occupying 10 GB
when warmed up. When your application workload changes over time, the optimal
sizes for these objects could vary as well, requiring more memory for one
object size than another. Without pool rebalancing, this kind of workload
change would usually result in metrics like eviction age and hit ratios being
sub-optimal over time.

Cachelib offers several rebalancing strategies to offset this behavior by
asking the cache to restructure the underlying memory allocated among objects
of different sizes.

## How does it work?

Internally, cachelib divides memory into 4 MB slabs and assigns slabs to
allocation classes within a pool. Each allocation class serves a specific item
size range and has its own eviction container. Pool rebalancing is an
intra-pool operation: cachelib releases one slab from a victim allocation class
and either gives it to a receiver allocation class in the same pool or returns
it to the pool's free slab list.

The pool rebalancer is a periodic background worker. For each regular pool, a
worker run asks the configured strategy to pick a victim allocation class and,
when the strategy supports it, a receiver allocation class. Cachelib then
releases one slab from the victim and either gives it to the receiver in the
same pool or returns it to the pool's free slab list. Active items on the
selected slab are either copied into new allocations through the
`enableMovingOnSlabRelease()` callback (Cachelib refers to this as *moving*),
or evicted if they cannot be relocated. Whenever possible, use a move callback
via `enableMovingOnSlabRelease()` so the remaining live items on a released slab
are relocated instead of evicted.

A successful strategy action moves one slab, or 4 MB, per pool per interval, so
with multiple regular pools the total movement scales linearly with pool count.

Slab release runs asynchronously, but it is still real cache work. It may run
user move callbacks, execute evictions, and wait for outstanding item handles
on the selected slab before memory can be freed. Keep item handles short-lived
so eviction and slab rebalancing are not blocked. `slabRebalanceTimeout`
controls how long slab release waits while marking an item as moving before it
aborts; the default is 10 minutes, and `0` waits forever.

## Enabling pool rebalancing

`CacheAllocatorConfig` has pool rebalancing enabled by default with the base
`RebalanceStrategy` and a 1 second interval. The base strategy only reacts to
allocation failures: it picks the allocation class with recent allocation
failures as the receiver, and picks a victim from the allocation class with the
most slabs when the strategy did not choose a victim.

To disable pool rebalancing, set a null strategy or a zero interval:

```cpp
config.enablePoolRebalancing(nullptr, std::chrono::seconds{0});
```

To override the default behavior, specify these parameters:

1. **Strategy** for re-evaluating metrics about your cache and figuring out a
   rebalancing action
2. **Interval** of executing the rebalancing
3. **Allocation-failure wakeup behavior** (optional). By default, allocation
   failures wake the rebalancer before the next scheduled interval. Pass `true`
   as the third argument to `enablePoolRebalancing()` to disable that forced
   wakeup.

For example:

```cpp
auto rebalanceStrategy =
  std::make_shared<cachelib::LruTailAgeStrategy>(rebalanceConfig);

config.enablePoolRebalancing(
  rebalanceStrategy,
  std::chrono::seconds(kRebalanceIntervalSecs),
  false // keep allocation-failure forced wakeups enabled
);
```

You can also override the rebalance strategy for a specific pool by passing a
strategy to `addPool()` or by calling `overridePoolRebalanceStrategy()`.

**Reclaiming first from mostly-free allocation classes:**
`poolRebalancerFreeAllocThreshold` controls a `PoolRebalancer`-level pre-pass
that runs before the strategy (see below) on every interval. Any class whose
`freeAllocs / allocsPerSlab` (slab's worth of free space) exceeds the threshold
is eligible, and the eligible class with the most free space releases one slab
back to the pool with no receiver. `0` (the default) disables it; `1` means "at
least two slab's worth of free slots". Unlike the strategy path, it does not
require the pool to be full.

### Picking a strategy

Cachelib offers a few pre-packaged strategies for rebalancing that you can pick
from. They differ by what they try to optimize for based on traditional wisdom
of large scale caches like social graph caches and general purpose look-aside
key value caches. These are good defaults to start with, but you can also come
up with your own implementation if you have other goals.

#### Base strategy

`RebalanceStrategy` is the default strategy. It does not optimize hit rate or
eviction-age fairness. It only helps allocation classes that have seen
allocation failures since the previous rebalancer run. This is a conservative
default that helps a class get at least one slab when it cannot allocate or
evict within its current allocation class.

#### LRU Tail Age

`LruTailAgeStrategy` is a fair policy that tries to make objects of different
sizes get similar eviction ages in cache. For example, in steady state for your
cache, you could have 100 byte objects getting a 1 hour lifetime while 1,000
byte objects get a 30 minute lifetime. This strategy picks the allocation class
with the oldest projected eviction age as the victim and the allocation class
with the youngest eviction age as the receiver.

You can configure the following parameters in `LruTailAgeStrategy::Config`:

* `tailAgeDifferenceRatio`
This defines how tight the tail age of various object sizes you want them to be.
Setting it to 0.1 means that you don't want the min and max age to differ by
more than 10%.

* `minTailAgeDifference`
This specifies a threshold of how big the actual diff ratio should be to warrant
a rebalancing. For example, your min and max might be more than 10% off, but the
real difference is insignificant.

* `minSlabs`
This specifies the minimum amount of memory in slabs that specific object size
can not go below while rebalancing. Keep in mind that this is specified in slabs
and not in bytes.

* `numSlabsFreeMem`
  If an allocation class has more than this many slabs worth of free memory
  (i.e. `numSlabsFreeMem * Slab::kSize` bytes) and has not recently evicted,
  it is prioritized as a victim. If it holds no live items (e.g. it was
  emptied by TTL expiry or removals), it gives up a slab without the
  `tailAgeDifferenceRatio` and `minTailAgeDifference` checks, since its tail
  age of 0 would always fail them.

* `slabProjectionLength`
  How many slabs worth of items to project when computing the victim's
  projected tail age.

* `queueSelector`
  Which eviction-age queue to use: hot, warm, or cold. Not every eviction
  policy has separate hot, warm, and cold queues; when it does not, the policy
  defines how these values map to its available eviction-age stats.

* `getWeight`
  An optional weight function for weighted tail age. When this is set,
  `tailAgeDifferenceRatio` and `minTailAgeDifference` are ignored.

For example:

```cpp
cachelib::LruTailAgeStrategy::Config cfg(ratio, kLruTailAgeStrategyMinSlabs);
cfg.slabProjectionLength = 0; // don't project or estimate tail age
cfg.numSlabsFreeMem = 10;     // ok to have ~40 MB free memory in unused allocations
auto rebalanceStrategy = std::make_shared<cachelib::LruTailAgeStrategy>(cfg);

// every 5 seconds, re-evaluate the eviction ages and rebalance the cache.
config.enablePoolRebalancing(std::move(rebalanceStrategy), std::chrono::seconds(5));
```

#### Hits per slab

`HitsPerSlabStrategy` tries to optimize the overall hit ratio rather than
ensuring a fairness in the cache eviction age. This should result in a
relatively higher hit ratio. However, it might potentially make your cache
contain more of objects that give hits vs. objects that are expensive to
recompute. For example, the cost of miss on objects is not uniform.

You can configure the following parameters in `HitsPerSlabStrategy::Config`:

* `minDiff`
  The minimum absolute improvement in hits per slab required before a rebalance
  happens.

* `diffRatio`
  The minimum relative improvement required before a rebalance happens. Both
  `minDiff` and `diffRatio` must be satisfied.

* `minSlabs`
  The minimum number of slabs to retain in every allocation class. An
  allocation class with `minSlabs` or fewer slabs cannot be picked as the
  victim.

* `numSlabsFreeMem` and `enableVictimByFreeMem`
  When enabled, prioritize allocation classes with more than
  `numSlabsFreeMem` slab equivalents of free memory as victims.

* `minLruTailAge`
  Require a victim to have at least this eviction age, and prioritize receivers
  below this eviction age. Use this to preserve some eviction-age fairness
  while optimizing hits.

* `maxLruTailAge`
  Prefer victims above this eviction age and receivers below this eviction age.
  If no receiver satisfies the limit, the strategy falls back to the hits-based
  choice.

* `updateHitsOnEveryAttempt`
  Update the hit-count baseline on every rebalance attempt, even if no slab is
  moved. By default, the baseline is updated after successful rebalances.

* `getWeight`
  An optional weight function to bias hit-per-slab values. Higher weights make
  an allocation class more likely to receive slabs and less likely to donate.

* `classIdTargetEvictionAge`
  Optional per-class target eviction ages. Victims must meet their target, and
  receivers below target are prioritized.

#### Marginal hits

This strategy ensures that the marginal hits (estimated by the hits in the tail
part of LRU) across different object sizes are similar. Unlike hit based
strategy which counts for historical count of hits across the entire cache, this
tracks which objects could marginally benefit from getting more memory. To
enable this,  you need to use the MM2Q eviction policy and enable tail hits
tracking (`Allocator::Config::enableTailHitsTracking()`).

You can configure the following parameters in `MarginalHitsStrategy::Config`:

* `movingAverageParam`
  The smoothing parameter used for marginal-hit rankings.

* `minSlabs`
  The minimum number of slabs to retain in every allocation class. An
  allocation class with `minSlabs` or fewer slabs cannot be picked as the
  victim.

* `maxFreeMemSlabs`
  An allocation class can be picked as the receiver only when its free memory
  is below this many slab equivalents.

#### Free memory

This strategy frees a slab from an allocation class that satisfies all of the
following requirements:
- this allocation class has total slabs above `minSlabs`
- this allocation class has more than `numFreeSlabs` slab equivalents of total
free memory
- this allocation class has the most total free memory among all non-evicting
(i.e. no eviction is currently happening) allocation classes in the pool

Note: this strategy does not specify a target allocation class to receive the
freed slab.  You can configure the following parameters in
`FreeMemStrategy::Config`:
* `minSlabs`
The minimum number of slabs to retain in every allocation class. Default is 1.
* `numFreeSlabs`
The free-memory threshold, in slab equivalents. Default is 3.
* `maxUnAllocatedSlabs`
FreeMem strategy will not rebalance anything if the number of free slabs in this
pool is more than this number. Default is 1000.

### Writing your own strategy

In addition, if you have some application specific context on how you can
improve your cache, you can implement your own strategy and pass it to cachelib
for rebalancing. Your rebalancing strategy will have to extend the type
`RebalanceStrategy` and implement the following two methods that define where to
take memory from and where to give more memory to:


```cpp
virtual RebalanceContext pickVictimAndReceiverImpl(
  const CacheBase& /*cache*/,
  PoolId /*pid*/,
  const PoolStats& /*poolStats*/
) {
  return {};
}

virtual ClassId pickVictimImpl(
  const CacheBase& /*cache*/,
  PoolId /*pid*/,
  const PoolStats& /*poolStats*/
) {
  return Slab::kInvalidClassId;
}
```
