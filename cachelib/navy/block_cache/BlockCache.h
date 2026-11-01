/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <folly/Expected.h>
#include <folly/Range.h>

#include <memory>
#include <optional>
#include <vector>

#include "cachelib/allocator/nvmcache/NavyConfig.h"
#include "cachelib/allocator/nvmcache/NvmItem.h"
#include "cachelib/common/AtomicCounter.h"
#include "cachelib/common/EventInterface.h"
#include "cachelib/navy/block_cache/Allocator.h"
#include "cachelib/navy/block_cache/CombinedEntryManager.h"
#include "cachelib/navy/block_cache/EvictionPolicy.h"
#include "cachelib/navy/block_cache/Index.h"
#include "cachelib/navy/block_cache/RegionManager.h"
#include "cachelib/navy/common/Device.h"
#include "cachelib/navy/engine/Engine.h"

namespace facebook {
namespace cachelib {
namespace interface {
class FlashCacheComponent;
class FlashCacheItem;
} // namespace interface

namespace navy {
class JobScheduler;

// Constructor can throw but system remains in the valid state. Caller can
// fix parameters and re-run.
class BlockCache final : public Engine {
 public:
  // See CacheProto for details
  struct Config {
    Device* device{};
    NavyPersistParams persistParams{};
    ExpiredCheck checkExpired;
    DestructorCallback destructorCb;
    // Checksum data read/written
    bool checksum{};
    // Offload value checksumming (fused with the value copy on the write
    // path) to Intel DSA via the DTO library when available. Requires
    // checksum to be enabled. No effect when built without DTO support.
    bool checksumOffload{false};
    // Minimum value size to use the offloaded (fused) path on the WRITE path;
    // smaller values use software copy+checksum. Submitting and polling a
    // descriptor costs a few microseconds regardless of size, about what the
    // CPU needs to CRC 16-32 KiB; on a size-diverse (CDN) workload a 16 KiB
    // gate beat 4 KiB by 4% of process CPU and 32 KiB was indistinguishable.
    uint32_t checksumOffloadMinSize{16384};
    // Minimum value size to offload checksum VERIFICATION (lookup, reclaim,
    // reinsertion, cleanup). 0 = same as checksumOffloadMinSize. The read side
    // has no CPU work to overlap with the accelerator, so its break-even size
    // is higher than the fused write; ~UINT32_MAX disables read-side offload.
    uint32_t checksumOffloadReadMinSize{0};
    // Cache-control hint on the fused write-path copy: true steers the value
    // bytes toward the CPU cache (right when in-memory lookup hits or a CPU
    // flush copy read them soon), false keeps them out (right when the next
    // reader is the device's DMA engine, e.g. directFlush/flushCopyOffload).
    bool checksumOffloadCacheControl{true};
    // Region reclaim and cleanup verify every entry of a region. With this on
    // (and checksumOffload) the values are checksummed as DSA Batch
    // descriptors - up to 64 per submission, one completion to poll - instead
    // of one descriptor each; values under checksumOffloadReadMinSize are
    // checksummed on the CPU as before. Cuts reclaim descriptor traffic ~50x
    // at equal CPU (measured neutral on both test workloads), so off by
    // default until a workload shows a gain.
    bool checksumOffloadBatchReclaim{false};
    // Coalesce the write path's fused copy+CRC descriptors across the fibers
    // of a NavyThread into one Batch descriptor. A fiber yields to let
    // siblings join only when the scheduler has runnable fibers. Measured
    // neutral to slightly negative (+2-4% CPU) with 24-48 client threads -
    // siblings are rarely ready - so off by default.
    bool checksumOffloadBatch{false};
    // Values below checksumOffloadMinSize are copied by the CPU at insert and
    // checksummed at region flush in DSA batches with the region's other small
    // values (Region::PendingChecksum; RegionManager::drainPendingChecksums
    // patches EntryDesc::cs before the buffer is written). Implies
    // skipInMemValueVerify. Measured neutral (the copy stays on the CPU), so
    // off by default.
    bool checksumDeferSmall{false};
    // Lookups served from the in-memory region buffer skip the value checksum:
    // the bytes never left DRAM and the header checksum still covers the
    // descriptor. Everything read back from the device is verified as before.
    bool skipInMemValueVerify{false};
    // Base offset and size (in bytes) of cache on the device
    uint64_t cacheBaseOffset{};
    uint64_t cacheSize{};
    // Eviction policy
    std::unique_ptr<EvictionPolicy> evictionPolicy;
    BlockCacheReinsertionConfig reinsertionConfig{};
    // Region size, bytes
    uint64_t regionSize{16 * 1024 * 1024};
    // See AbstractCacheProto::setReadBufferSize
    uint32_t readBufferSize{};
    // Clean region pool size
    uint32_t cleanRegionsPool{1};
    // The number of region_manager threads for reclaim and flush
    uint32_t cleanRegionThreads{1};
    // The fiber stack size of region_manager threads
    uint32_t stackSize{0};
    // Number of in-memory buffers where writes are buffered before flushed
    // on to the device
    uint32_t numInMemBuffers{1};
    // whether ItemDestructor is enabled
    bool itemDestructorEnabled{false};

    std::optional<std::reference_wrapper<LegacyEventTracker>>
        legacyEventTracker;

    // When exceeding the limit, we will not reschedule any flushing job but
    // directly fail it.
    uint16_t inMemBufFlushRetryLimit{10};

    // The effect of priorities will be up to the particular
    // eviction policy. There must be at least one priority.
    // This vector is the number of allocators for each priority. Under the same
    // priority, allocators will be picked by key hash and written into one of
    // the regions.
    std::vector<uint32_t> allocatorsPerPriority{1};

    // whether to remove an item by checking the full key.
    bool preciseRemove{false};
    // Whether region manager's worker threads should flush asynchronously.
    bool regionManagerFlushAsync{false};

    // Whether to use Combined entry block (For index entries and small sized
    // items)
    bool useCombinedEntryBlock{false};

    // Whether to persist and recover eviction policy ordering across restarts.
    // When enabled, the FIFO queue order is preserved, avoiding the hit rate
    // regression caused by resetting the write position on restart.
    //
    // NOTE: Only FifoPolicy currently supports persistence. LruPolicy and
    // SegmentedFifoPolicy gracefully fall back to resetEvictionPolicy() on
    // recover (a WARN is logged); enabling this flag with those policies has
    // no effect on eviction ordering.
    bool recoverEvictionPolicy{false};

    // Whether to write region buffer directly to device on flush path without
    // allocating an intermediate IO buffer and copying. Default false preserves
    // old behavior for safe rollout. When true, avoids redundant copy.
    bool directFlush{false};

    // When not directFlush: copy the region buffer into the flush write
    // buffer on Intel DSA (DTO batch descriptor) instead of memcpy. Requires
    // a DTO build; verified at runtime, falls back to memcpy.
    bool flushCopyOffload{false};

    // name of this BC instance
    std::string name{};

    // Index related config
    BlockCacheIndexConfig indexConfig{};

    // Calculates the total region number.
    uint32_t getNumRegions() const {
      XDCHECK_EQ(0ul, cacheSize % regionSize);
      return cacheSize / regionSize;
    }

    // Checks invariants. Throws exception if failed.
    Config& validate();
  };

  // Random but unique value to be used as the key hash value for Combined Entry
  // Block
  static constexpr uint64_t kCombinedEntrySignatureValue = 0xCEBB0FFE80000001;

  // Contructor can throw std::exception if config is invalid.
  //
  // @param config  config that was validated with Config::validate
  //
  // @throw std::invalid_argument on bad config
  explicit BlockCache(Config&& config);
  BlockCache(const BlockCache&) = delete;
  BlockCache& operator=(const BlockCache&) = delete;
  ~BlockCache() override;

  // return the size of usable space
  uint64_t getSize() const override { return regionManager_.getSize(); }

  // Checks if the key could exist in block cache. This can be used as a
  // pre-check to optimize cache lookups to avoid calling lookup in an async IO
  // environment.
  //
  // @param hk   key to be checked
  //
  // @return  false if the key definitely does not exist and true if it could.
  bool couldExist(HashedKey hk) override;

  // Estimate the device write size if the item is written to BlockCache.
  uint64_t estimateWriteSize(HashedKey hk, BufferView value) const override;

  // Estimated bytes persist() would write for numIndexEntries index entries.
  uint64_t estimatePersistSize(size_t numIndexEntries) const;

  // Inserts a key-value pair into BlockCache.
  //
  // @param hk      key to be inserted
  // @param value   value to be inserted
  //
  // @return  Status::Ok on success,
  //          Status::Rejected on error,
  //          Status::Retry on no space available for now.
  Status insert(HashedKey hk,
                BufferView value,
                uint8_t poolId = 0,
                uint32_t expiryTime = 0,
                uint32_t lastAccessTimeSecs = 0) override;

  // Looks up a key in BlockCache.
  //
  // @param hk      key to be looked up
  // @param value   value to be populated with the data read
  //
  // @return  Status::Ok on success,
  //          Status::NotFound if the key is not found,
  //          Status::Retry read cannot be served and needs to be retried again.
  //          Status::ChecksumError if the internal checksum does not match,
  //          Status::DeviceError otherwise.
  Status lookup(HashedKey hk,
                Buffer& value,
                uint32_t& lastAccessTimeSecs) override;

  // Removes a key from BlockCache.
  //
  // @param hk           key to be removed
  //
  // @return Status::Ok if the key is found and Status::NotFound otherwise.
  Status remove(HashedKey hk) override;

  // Finish all pending jobs
  void drain() override;

  // Flushes all buffered (in flight) operations in BlockCache.
  void flush() override;

  // Resets BlockCache to its initial state.
  void reset() override;

  // Serializes BlockCache state to a RecordWriter.
  //
  // @param rw   RecordWriter to serialize state
  void persist(RecordWriter& rw) override;

  // Deserialize BlockCache state from a RecordReader.
  //
  // @param rr   RecordReader to deserialize state
  //
  // @return  true if recovery succeeds, false otherwise.
  bool recover(RecordReader& rr) override;

  // Exports BlockCache stats via CounterVisitor.
  //
  // @param visitor   CounterVisitor to export stats
  uint64_t getCounters(const CounterVisitor& visitor) const override;

  // Gets the maximum item size that can be inserted into BlockCache.
  uint64_t getMaxItemSize() const override {
    return regionSize_ - sizeof(EntryDesc);
  }

  // Gets the alloc alignment size (must be an integral power of two).
  uint32_t getAllocAlignSize() const {
    XDCHECK(folly::isPowTwo(allocAlignSize_));
    return allocAlignSize_;
  }

  // Return a Buffer containing NvmItem randomly sampled in the backing store
  std::pair<Status, std::string /* key */> getRandomAlloc(
      Buffer& value) override;

  using IndexEntry = Index::Entry;

  struct ReadResult {
    ReadResult(Buffer buffer,
               uint32_t keySize,
               uint32_t valueSize,
               uint32_t lastAccessTimeSecs);

    folly::StringPiece key() const;
    BufferView value() const;
    uint32_t lastAccessTimeSecs() const { return lastAccessTimeSecs_; }
    Buffer releaseBuffer() { return std::move(buffer_); }

   private:
    Buffer buffer_{};
    uint32_t keySize_{0};
    uint32_t valueSize_{0};
    uint32_t lastAccessTimeSecs_{0};
  };

  // Returns a reference to the BlockCache index.
  const Index& index() const { return *index_; }

  // Same as lookup(HashedKey, ...), but for an index entry from an index
  // iterator. Returns NotFound if the entry cannot be revalidated before
  // reading.
  folly::Expected<ReadResult, Status> lookup(const IndexEntry& entry);

  // Update any stats needed to be updated when eviction is done
  void updateEvictionStats(uint32_t lifetime) override {
    bcLifetimeSecs_.trackValue(lifetime);
  }

  // The minimum alloc alignment size can be as small as 1. Since the
  // test cases have very small device size, they will end up with alloc
  // alignment size of 1 (if determined as device_size >> 32 ) and we may
  // not be able to test the "alloc alignment size" feature correctly.
  // Since the real devices are not going to be that small, this is a
  // reasonable minimum size. This can be lowered to 256 or 128 in future,
  // if needed.
  static constexpr uint32_t kMinAllocAlignSize = 512;

  // This is the maximum size of an item that can be cached in block cache.
  static constexpr uint32_t kMaxItemSize =
      kMinAllocAlignSize *
      static_cast<uint32_t>(std::numeric_limits<uint16_t>::max());

 private:
  // Serialization format version. Never 0. Versions < 10 reserved for testing.
  // Version 14: navy::checksum switched from CRC-32 (IEEE) to CRC-32C
  // (Castagnoli) for DSA offload compatibility. Entries written by prior
  // versions would fail checksum verification.
  static constexpr uint32_t kFormatVersion = 14;
  // This should be at least the nextTwoPow(sizeof(EntryDesc)).
  static constexpr uint32_t kDefReadBufferSize = 4096;
  // Default priority for an item inserted into block cache
  static constexpr uint16_t kDefaultItemPriority = 0;

  // When modify @EntryDesc layout, don't forget to bump @kFormatVersion!
  struct EntryDesc {
    uint32_t keySize{};
    uint32_t valueSize{};
    uint64_t keyHash{};
    uint32_t lastAccessTimeSecs{};
    uint32_t csSelf{};
    uint32_t cs{};

    EntryDesc() = default;
    EntryDesc(uint32_t ks, uint32_t vs, uint64_t kh, uint32_t accessTime = 0)
        : keySize{ks},
          valueSize{vs},
          keyHash{kh},
          lastAccessTimeSecs{accessTime} {
      csSelf = computeChecksum();
    }

    uint32_t computeChecksum() const {
      return checksum(BufferView{offsetof(EntryDesc, csSelf),
                                 reinterpret_cast<const uint8_t*>(this)});
    }
  };

  // Instead of unportable packing, we make sure that struct size is equal to
  // the size of its members.
  static_assert(sizeof(EntryDesc) == 32, "packed struct required");

  struct ValidConfigTag {};
  BlockCache(Config&& config, ValidConfigTag);

  std::unique_ptr<Index> createIndex(const BlockCacheIndexConfig& indexConfig,
                                     const NavyPersistParams& persistParams,
                                     bool useCombinedEntryBlock,
                                     const std::string& name);
  std::unique_ptr<CombinedEntryManager> createCombinedEntryManager(
      const Config& config);

  // Entry disk size (with aux data and aligned)
  uint32_t serializedSize(uint32_t keySize, uint32_t valueSize) const;

  using AllocData = std::tuple<navy::RegionDescriptor,
                               uint32_t,
                               navy::RelAddress,
                               navy::MutableBufferView>;

  // Allocate a slot from the allocator
  // @param hk          key to be inserted
  // @param valueSize   size of the value
  // @param priority    priority of async operation
  // @param canWait     whether to wait if space isn't currently available
  AllocData allocateImpl(const HashedKey& hk,
                         const uint32_t valueSize,
                         const uint16_t priority,
                         const bool canWait);

  // Allocate a slot for inserting into cache
  // @param hk          key to be inserted
  // @param valueSize   size of the value
  // @param canWait     whether to wait if space isn't currently available
  folly::Expected<AllocData, Status> allocateForInsert(const HashedKey& hk,
                                                       const uint32_t valueSize,
                                                       bool canWait = true);

  // Write the entry descriptor and cache item key into the item buffer
  // @param buffer      buffer to write into
  // @param hk          key to be inserted
  // @param valueSize   size of the value to be inserted
  static EntryDesc* writeEntryDescAndKey(MutableBufferView buffer,
                                         const HashedKey& hk,
                                         uint32_t valueSize,
                                         uint32_t lastAccessTimeSecs = 0);

  struct LookupData {
    Buffer buffer_;
    uint32_t keySize_{0};
    uint32_t valueSize_{0};
    uint32_t lastAccessTimeSecs_{0};
    Status status_{Status::Ok};
  };

  // Lookup the item, returning everything unmodified to the caller.
  // @param hk          key to be looked up
  LookupData lookupInternal(HashedKey hk);
  template <typename KeyT, typename IndexLookupT>
  LookupData lookupInternal(const KeyT& key, IndexLookupT&& indexLookup);

  // Read and write are time consuming. It isn't worth inlining them from a
  // performance point of view, but makes sense to track them for performance,
  // especially the CPU time spent in std::memcpy.
  // @param addr        Address to write this entry into
  // @param size        Number of bytes this entry will take up on the device
  // @param hk          Key of the entry. Only when combinedEntry == false
  // @param value       Payload of the entry
  // @param combinedEntry whether it's combined entry block or not
  //
  void writeEntry(MutableBufferView buffer,
                  RelAddress addr,
                  std::optional<HashedKey> hk,
                  BufferView value,
                  bool combinedEntry,
                  uint32_t lastAccessTimeSecs = 0);
  // @param regionDesc  Region descriptor for reading
  // @param ld          LookupData containing the entry metadata for reading
  // @param addrEnd     End of the entry since the item layout is backward
  // @param approxSize  Approximate size since we got this size from index
  // @param expected    We expect the entry's key to match with our key when
  //                    provided
  void readEntry(RegionDescriptor& regionDesc,
                 LookupData& ld,
                 RelAddress addrEnd,
                 uint32_t approxSize,
                 std::optional<HashedKey> expected);

  // Update the index with the new entry
  // @param keyHash      Hash of the key
  // @param size         Full size of cache item on disk (allocated slot)
  // @param addr         Address of the entry
  // @param allowReplace Whether to allow replacing an existing entry or not
  // @return true if the index was updated, false if the key already exists and
  // allowReplace is false
  bool updateIndex(uint64_t keyHash,
                   uint32_t size,
                   const RelAddress& addr,
                   bool allowReplace) const;

  // Remove an item previously found via lookup
  // @param hk          key to be removed
  // @param value       value populated with data read from lookup used for
  //                    precise remove and item destructor callback
  Status removeImpl(const HashedKey& hk, BufferView value);

  // Allocator reclaim callback
  // Returns number of slots that were successfully evicted
  uint32_t onRegionReclaim(RegionId rid, BufferView buffer);

  // Allocator cleanup callback
  void onRegionCleanup(RegionId rid, BufferView buffer);

  // Retrieve the key hash by reading the entry from the region.
  // Will be used as a callback for Index so that Index can retrieve the key
  // info using the location info. (Index may maintain only the partial key
  // hash). If it fails to retrieve key hash for whatever reason (mostly because
  // it's blocked for read when region's being released after reclaim), it will
  // fail to retrieve and return nullopt.
  //
  // IMPORTANT: It's caller's responsibility to sync up between this retrieval
  // and the Index change (location change from the other operations : reclaim,
  // insert, remove, etc.)
  // Since location is given by the caller, there's no way to resolve race
  // condition within this function, and here it will just assume the proper
  // location was given by the caller.
  std::optional<uint64_t> onKeyHashRetrievalFromLocation(uint32_t address);

  // To write the combined entry block to currently open region
  Status onWriteCombinedEntryBlock(uint64_t stream,
                                   const CombinedEntryBlock& ceb);

  // To read the combined entry block from the given address.
  // @param address  address to read from
  // @param readSize  size to read, which will be the size of CEB
  // @param cebBuffer  buffer to be filled with combined entry block content
  //
  Status onReadCombinedEntryBlock(uint32_t address,
                                  uint32_t readSize,
                                  Buffer& cebBuffer);

  // Returns true if @config matches this cache's config_
  bool isValidRecoveryData(const serialization::BlockCacheConfig& config) const;

  static serialization::BlockCacheConfig serializeConfig(const Config& config);

  // Tries to recover cache. Throws std::exception on failure.
  void tryRecover(RecordReader& rr);

  // The alloc alignment indicates the granularity of read/write. This
  // granuality is less than the device io alignment size because we buffer
  // writes in memory until we fill up a region.
  uint32_t calcAllocAlignSize() const;

  // Size hint is computed by aligning size up to kMinAllocAlignSize,
  // and then divide by it. It is loosely compressing the size as
  // we may decode into a bigger size later.
  uint16_t encodeSizeHint(uint32_t size) const {
    auto alignedSize = powTwoAlign(size, kMinAllocAlignSize);
    return static_cast<uint16_t>(alignedSize / kMinAllocAlignSize);
  }

  uint32_t decodeSizeHint(uint16_t sizeHint) const {
    return sizeHint * kMinAllocAlignSize;
  }

  uint32_t encodeRelAddress(RelAddress addr) const {
    XDCHECK_NE(addr.offset(), 0u); // See @decodeRelAddress
    return regionManager_.toAbsolute(addr).offset() / allocAlignSize_;
  }

  AbsAddress decodeAbsAddress(uint32_t code) const {
    return AbsAddress{static_cast<uint64_t>(code) * allocAlignSize_};
  }

  RelAddress decodeRelAddress(uint32_t code) const {
    // Remember, we store slot end address, which can have offset equal to
    // region size.
    return regionManager_.toRelative(decodeAbsAddress(code).sub(1)).add(1);
  }

  void recordEvent(folly::StringPiece key,
                   AllocatorApiEvent event,
                   AllocatorApiResult result,
                   uint32_t size,
                   const NvmItem* nvmItem = nullptr);

  // @valueCs  the value's checksum when the caller already computed it (the
  //           batched reclaim path); computed here otherwise.
  AllocatorApiResult reinsertOrRemoveItem(
      HashedKey hk,
      BufferView value,
      uint32_t entrySize,
      RelAddress currAddr,
      const EntryDesc& entryDesc,
      std::optional<uint32_t> valueCs = std::nullopt);

  // Checksums of @values, in order. With checksum offload and
  // checksumOffloadBatchReclaim_ the values at or above the read gate go to
  // DSA as Batch descriptors of up to BatchChecksumOp::kMax members; the rest
  // (and everything without DSA) use navy::checksum() on the CPU.
  std::vector<uint32_t> checksumValues(const std::vector<BufferView>& values) const;

  // Removes an entry key from the index.
  // @return true if the item is successfully removed; false if the item
  // cannot
  //         be found or was removed earlier.
  bool removeItem(HashedKey hk, RelAddress currAddr);

  void validate(Config& config) const;

  void initializeBlockCache();

  // Create the reinsertion policy from config.
  // This function may need a reference to index and should be called the last
  // in the initialization order.
  std::shared_ptr<BlockCacheReinsertionPolicy> makeReinsertionPolicy(
      const BlockCacheReinsertionConfig& reinsertionConfig);

  // Helpers for hole accounting
  void addHole(uint32_t size) const;
  void removeHole(uint32_t size) const;

  const serialization::BlockCacheConfig config_;
  const uint16_t numPriorities_{};
  const ExpiredCheck checkExpired_;
  const DestructorCallback destructorCb_;
  const bool checksumData_{};

  // Whether to offload value checksum+copy to DSA on the write path, and the
  // minimum value size for which to do so. See Config::checksumOffload.
  const bool checksumOffload_{};
  const uint32_t checksumOffloadMinSize_{};
  const uint32_t checksumOffloadReadMinSize_{};
  const bool checksumOffloadCacheControl_{};
  // See Config::checksumOffloadBatchReclaim / checksumOffloadBatch /
  // checksumDeferSmall / skipInMemValueVerify. All resolved against
  // checksum/checksumOffload in the constructor.
  const bool checksumOffloadBatchReclaim_{};
  const bool checksumOffloadBatch_{};
  const bool checksumDeferSmall_{};
  const bool skipInMemValueVerify_{};

  // Computes the checksum of @value for verification (lookup, reclaim and
  // cleanup paths), offloading to DSA when checksum offload is enabled and
  // the value meets the size gate.
  uint32_t valueChecksum(BufferView value) const;
  // reference to the under-lying device.
  const Device& device_;
  // alloc alignment size indicates the granularity of entry sizes on device.
  // this is at least kMinAllocAlignSize and is determined by the size of the
  // device and size of the address (which is 32-bits).
  const uint32_t allocAlignSize_{};
  const uint32_t readBufferSize_{};
  // number of bytes in a region
  const uint64_t regionSize_{};
  // whether ItemDestructor is enabled
  const bool itemDestructorEnabled_{false};
  // whether preciseRemove is enabled
  const bool preciseRemove_{false};

  // combinedEntryMgr_ should be initialized before index_
  std::unique_ptr<CombinedEntryManager> combinedEntryMgr_;

  // Index stores offset of the slot *end*. This enables efficient paradigm
  // "buffer pointer is value pointer", which means value has to be at offset
  // 0 of the slot and header (EntryDescriptor) at the end.
  //
  // ----------------------------------------------------
  // |     Value                    |  EntryDescriptor  |
  // ----------------------------------------------------
  // ^                                                  ^
  // |                                                  |
  // Buffer*                                    Index points here
  std::unique_ptr<Index> index_;
  RegionManager regionManager_;
  Allocator allocator_;

  // It is vital that the reinsertion policy is initialized after index_.
  // Make sure that this class member is defined after index_.
  std::shared_ptr<BlockCacheReinsertionPolicy> reinsertionPolicy_;
  std::optional<std::reference_wrapper<LegacyEventTracker>> legacyEventTracker_;

  // thread local counters in synchronized/critical path
  mutable TLCounter lookupCount_;
  mutable TLCounter succLookupCount_;
  mutable TLCounter retryReadCount_;

  // atomic counters in asynchronized path
  mutable AtomicCounter insertCount_;
  mutable AtomicCounter insertHashCollisionCount_;
  mutable AtomicCounter succInsertCount_;
  mutable AtomicCounter lookupFalsePositiveCount_;
  mutable AtomicCounter lookupEntryHeaderChecksumErrorCount_;
  mutable AtomicCounter lookupValueChecksumErrorCount_;
  mutable AtomicCounter removeCount_;
  mutable AtomicCounter succRemoveCount_;
  mutable AtomicCounter evictionLookupMissCounter_;
  mutable AtomicCounter evictionExpiredCount_;
  mutable AtomicCounter allocErrorCount_;
  mutable AtomicCounter allocRetryCount_;
  mutable AtomicCounter logicalWrittenCount_;
  // Bytes in serialized, allocation-aligned BlockCache slots, including the
  // value, key, EntryDesc, and alignment padding.
  mutable AtomicCounter serializedWrittenCount_;
  // Subset of serializedWrittenCount_ consumed by padding added to satisfy
  // allocAlignSize_.
  mutable AtomicCounter alignmentPaddingWrittenCount_;
  // TODO: deprecate hole count and hole size when we have
  //       confirmed usedSizeBytes is working correctly in prod
  mutable AtomicCounter holeCount_;
  mutable AtomicCounter holeSizeTotal_;
  mutable AtomicCounter usedSizeBytes_;
  mutable AtomicCounter reinsertionErrorCount_;
  mutable AtomicCounter reinsertionCount_;
  mutable AtomicCounter reinsertionBytes_;
  mutable AtomicCounter reclaimEntryHeaderChecksumErrorCount_;
  mutable AtomicCounter reclaimValueChecksumErrorCount_;
  mutable AtomicCounter removeAttemptCollisions_;
  mutable AtomicCounter cleanupEntryHeaderChecksumErrorCount_;
  mutable AtomicCounter cleanupValueChecksumErrorCount_;
  mutable AtomicCounter lookupForItemDestructorErrorCount_;
  mutable AtomicCounter excessiveReadBytes_;

  mutable util::PercentileStats bcLifetimeSecs_;
  mutable util::PercentileStats insertLatency_;
  mutable util::PercentileStats lookupLatency_;
  mutable util::PercentileStats removeLatency_;

  friend class interface::FlashCacheComponent;
  friend class interface::FlashCacheItem;
};
} // namespace navy
} // namespace cachelib
} // namespace facebook
