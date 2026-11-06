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

#include "cachelib/interface/components/FlashCacheComponent.h"

#include "cachelib/common/Time.h"

using namespace facebook::cachelib::navy;

namespace facebook::cachelib::interface {

/**
 * In-memory buffer for data cached in flash. Reference counting via
 * RegionDescriptors is used to ensure the backing Region is still resident &
 * valid while working on this cache item. The buffer's layout is:
 *
 *   ---------------------------------------------------------------------------
 *   | creation & expiry time ||  value  || (empty) |  key  | entry descriptor |
 *   ---------------------------------------------------------------------------
 *
 * Internally uses a tagged union with two representations:
 *   - Alloc path (allocate / findToWrite): MutableBufferView into a Region
 *     buffer, plus RegionDescriptor and RelAddress for lifecycle management.
 *   - Find path (find / iterator): owned Buffer from a device read.
 *
 * The two representations are overlapped in storage to minimize size. The tag_
 * field discriminates the two paths (1 = alloc, 0 = find).  We don't use a
 * std::variant because it requires an extra 8 bytes of metadata; 63 size bits
 * is more than enough to represent the sizes of cache items.
 */
class FlashCacheItem : public CacheItem {
 public:
  using EntryDesc = BlockCache::EntryDesc;

  static constexpr size_t kMetadataSize = 2 * sizeof(uint32_t);

  // Allocation constructor - uses MutableBufferView into region buffer
  // NOTE: valueSize *must* include space for creation & expiration time
  FlashCacheItem(const HashedKey& hashedKey,
                 uint32_t valueSize,
                 FlashCacheComponent::AllocData&& allocation,
                 const uint32_t creationTime,
                 const uint32_t ttlSecs,
                 ChecksumType checksumType)
      : FlashCacheItem(std::move(allocation)) {
    XDCHECK_GE(valueSize, 2 * sizeof(uint32_t))
        << "valueSize doesn't include space for metadata";
    auto view = getMutableBufferView();
    BlockCache::writeEntryDescAndKey(view, hashedKey, valueSize,
                                     0 /* lastAccessTimeSecs */, checksumType);
    uint32_t* metadata = reinterpret_cast<uint32_t*>(view.data());
    metadata[0] = creationTime;
    metadata[1] = ttlSecs > 0 ? creationTime + ttlSecs : 0;
  }

  // Constructor for find() and iterator() - owns the buffer from device read
  explicit FlashCacheItem(Buffer&& buffer)
      : tag_(0), size_(0), buffer_(std::move(buffer)) {}

  // FindToWrite constructor - uses MutableBufferView into region buffer for new
  // allocation, copies existing data into it
  FlashCacheItem(FlashCacheComponent::AllocData&& allocation,
                 const Buffer& existingData)
      : FlashCacheItem(std::move(allocation)) {
    XDCHECK_EQ(existingData.size(), getTotalSize())
        << "New allocation has a different size than existing data";
    auto view = getMutableBufferView();
    std::memcpy(view.data(), existingData.data(), existingData.size());
  }

  FlashCacheItem(FlashCacheItem&& other) noexcept
      : tag_(other.tag_), size_(other.size_) {
    // We *have* to move-construct/placement-new the union since it's UB to just
    // move-assign into alloc_ or buffer_ (they're not constructed yet)
    if (other.isAllocPath()) {
      new (&alloc_) AllocFields(std::move(other.alloc_));
    } else {
      new (&buffer_) Buffer(std::move(other.buffer_));
    }
  }

  FlashCacheItem(const FlashCacheItem&) = delete;
  FlashCacheItem& operator=(const FlashCacheItem&) = delete;
  FlashCacheItem& operator=(FlashCacheItem&&) = delete;

  ~FlashCacheItem() override {
    if (isAllocPath()) {
      alloc_.~AllocFields();
    } else {
      buffer_.~Buffer();
    }
  }

  bool isAllocPath() const noexcept { return tag_ != 0; }
  RegionDescriptor& getRegionDescriptor() noexcept {
    XDCHECK(isAllocPath());
    return alloc_.desc_;
  }
  RelAddress& getRelAddress() noexcept {
    XDCHECK(isAllocPath());
    return alloc_.relAddr_;
  }
  BufferView getBufferView() const noexcept {
    return isAllocPath() ? BufferView(size_, alloc_.data_) : buffer_.view();
  }
  // NOTE: can only be called for items constructed via the
  // allocate()/findToWrite() constructor
  MutableBufferView getMutableBufferView() noexcept {
    XDCHECK(isAllocPath());
    return MutableBufferView(size_, alloc_.data_);
  }
  // NOTE: can only be called for items constructed via the find() constructor
  Buffer& getBuffer() noexcept {
    XDCHECK(!isAllocPath());
    return buffer_;
  }
  EntryDesc* getEntryDescriptor() const noexcept {
    auto view = getBufferView();
    auto* entryEnd = const_cast<uint8_t*>(view.data()) + view.size();
    return reinterpret_cast<EntryDesc*>(entryEnd - sizeof(EntryDesc));
  }

  // ------------------------------ Interface ------------------------------ //

  uint32_t getCreationTime() const noexcept override {
    return getCreationTime(getBufferView());
  }
  static uint32_t getCreationTime(navy::BufferView buffer) noexcept {
    return reinterpret_cast<const uint32_t*>(buffer.data())[0];
  }
  uint32_t getExpiryTime() const noexcept override {
    return getExpiryTime(getBufferView());
  }
  static uint32_t getExpiryTime(navy::BufferView buffer) noexcept {
    return reinterpret_cast<const uint32_t*>(buffer.data())[1];
  }
  // Ref-counting for FlashCache is different than RAMCache - multiple
  // RAMCacheItems all refer to a single cache item in memory whereas each
  // FlashCacheItem is separate (even if it references the same cache item in
  // Region memory).
  //
  // The only thing we care about is Region refcounting via RegionDescriptor. We
  // don't need to increment on create (creating the RegionDescriptor does that)
  // and we should always release the cache item to release the descriptor.
  UnitResult incrementRefCount(CacheComponent&) noexcept override {
    return folly::unit;
  }
  bool decrementRefCount(CacheComponent&) noexcept override {
    // get cache to release the descriptor (need access to RegionManager)
    return true;
  }
  Key getKey() const noexcept override {
    auto* desc = getEntryDescriptor();
    return Key{reinterpret_cast<const char*>(desc) - desc->keySize,
               desc->keySize};
  }
  void* getMemory() const noexcept override {
    return const_cast<uint8_t*>(getBufferView().data() + kMetadataSize);
  }
  uint32_t getMemorySize() const noexcept override {
    return getEntryDescriptor()->valueSize - kMetadataSize;
  }
  uint32_t getTotalSize() const noexcept override {
    return static_cast<uint32_t>(getBufferView().size());
  }

 private:
  struct AllocFields {
    uint8_t* data_;
    RegionDescriptor desc_;
    RelAddress relAddr_;

    AllocFields(MutableBufferView view,
                RegionDescriptor&& desc,
                RelAddress&& relAddr) noexcept
        : data_(view.data()),
          desc_(std::move(desc)),
          relAddr_(std::move(relAddr)) {}
    ~AllocFields() = default;

    // Move-constructible but not move-assignable or
    // copy-constructible/assignable
    AllocFields(AllocFields&& rhs) noexcept
        : data_(rhs.data_),
          desc_(std::move(rhs.desc_)),
          relAddr_(std::move(rhs.relAddr_)) {}
    AllocFields& operator=(AllocFields&& rhs) = delete;
    AllocFields(const AllocFields&) = delete;
    AllocFields& operator=(const AllocFields&) = delete;
  };

  // Contains tag bit for both union types but only size for AllocFields
  uint64_t tag_ : 1;
  uint64_t size_ : 63;
  union {
    AllocFields alloc_;
    Buffer buffer_;
  };

  explicit FlashCacheItem(FlashCacheComponent::AllocData&& allocation)
      : tag_(1),
        size_(std::get<3>(allocation).size()),
        alloc_(std::get<3>(allocation),
               std::move(std::get<0>(allocation)),
               std::move(std::get<2>(allocation))) {}

  void move(void* dest) noexcept override {
    new (dest) FlashCacheItem(std::move(*this));
    this->~FlashCacheItem();
  }
};

static_assert(sizeof(FlashCacheItem) <= Handle::kInlineBufSize,
              "FlashCacheItem must fit in Handle's inline buffer");
static_assert(
    alignof(FlashCacheItem) <= Handle::kInlineBufAlignment,
    "FlashCacheItem alignment is bigger than inline buffer alignment");

namespace {
UnitResult initConfig(
    navy::BlockCache::Config& config,
    navy::Device* device,
    const FlashCacheComponent::PersistenceConfig& persistenceConfig) {
  constexpr auto expireCheck = [](navy::BufferView v) -> bool {
    return util::isExpired(FlashCacheItem::getExpiryTime(v));
  };

  if (config.checkExpired) {
    return makeError(Error::Code::INVALID_CONFIG,
                     "expiration callback set in BlockCache::Config but "
                     "component is going to set it");
  } else if (!device) {
    return makeError(Error::Code::INVALID_CONFIG,
                     "need to supply a device to create()");
  } else if (config.device && config.device != device) {
    return makeError(Error::Code::INVALID_CONFIG,
                     "device set in BlockCache::Config is not the same as "
                     "the device passed to create()");
  } else if (config.cacheBaseOffset != 0 &&
             config.cacheBaseOffset !=
                 device->getIOAlignedSize(persistenceConfig.metadataSize())) {
    return makeError(Error::Code::INVALID_CONFIG,
                     "cacheBaseOffset set in BlockCache::Config but "
                     "component is going to set it");
  }

  config.device = device;
  config.checkExpired = expireCheck;
  // TODO for now we're assuming we're the only cache on this flash device
  config.cacheBaseOffset =
      device->getIOAlignedSize(persistenceConfig.metadataSize());
  return folly::unit;
}

// findToWriteGeneric() yields the handle the alloc path built; the public API
// hands back a descriptor. Error and miss cases pass through untouched.
Result<std::optional<WriteDescriptor>> toWriteDescriptor(
    Result<std::optional<WriteHandle>>&& result) {
  if (result.hasError()) {
    return folly::makeUnexpected(std::move(result).error());
  }
  if (!result->has_value()) {
    return std::optional<WriteDescriptor>{};
  }
  return std::optional<WriteDescriptor>{std::in_place, std::move(**result)};
}
} // namespace

// ============================================================================
// FlashCacheComponent
// ============================================================================

/* static */ FlashCacheComponent::PersistenceConfig
FlashCacheComponent::PersistenceConfig::noPersistenceOrRecovery() {
  return PersistenceConfig(/* persist */ false, /* recover */ false,
                           /* metadataSize */ 0);
}

/* static */ FlashCacheComponent::PersistenceConfig
FlashCacheComponent::PersistenceConfig::persistenceAndRecovery(
    size_t metadataSize) {
  return PersistenceConfig(/* persist */ true, /* recover */ true,
                           metadataSize);
}

/* static */ FlashCacheComponent::PersistenceConfig
FlashCacheComponent::PersistenceConfig::persistenceButNoRecovery(
    size_t metadataSize) {
  return PersistenceConfig(/* persist */ true, /* recover */ false,
                           metadataSize);
}

/* static */ Result<FlashCacheComponent> FlashCacheComponent::create(
    std::string name,
    navy::BlockCache::Config&& config,
    std::unique_ptr<navy::Device> device,
    const utils::CoroFiberAdapter::Config& executorConfig,
    PersistenceConfig persistenceConfig) noexcept {
  try {
    if (auto result = initConfig(config, device.get(), persistenceConfig);
        result.hasError()) {
      return folly::makeUnexpected(std::move(result).error());
    }
    auto component = FlashCacheComponent(std::move(name), std::move(config),
                                         std::move(device), executorConfig,
                                         std::move(persistenceConfig));
    component.tryRecover();
    return component;
  } catch (const std::invalid_argument& ia) {
    return makeError(Error::Code::INVALID_CONFIG, ia.what());
  }
}

template <typename FuncT, typename ReturnT, typename CleanupFuncT>
folly::coro::Task<ReturnT> FlashCacheComponent::onWorkerThread(
    FuncT&& func, CleanupFuncT&& cleanup) {
  using namespace std::chrono;

  auto start = steady_clock::now();
  auto wrappedFunc = [this, start, f = std::forward<FuncT>(func)]() mutable {
    coroToFiberLatency_->trackValue(static_cast<double>(
        (duration_cast<nanoseconds>(steady_clock::now() - start)).count()));
    return f();
  };
  co_return co_await fiberWorkers_->onWorkerThread(
      std::move(wrappedFunc), std::forward<CleanupFuncT>(cleanup));
}

const std::string& FlashCacheComponent::getName() const noexcept {
  return name_;
}

folly::coro::Task<Result<FlashCacheComponent::AllocData>>
FlashCacheComponent::allocateImpl(const HashedKey& key, uint32_t valueSize) {
  auto ret = co_await onWorkerThread(
      [=, this]() {
        return cache_->allocateForInsert(key, valueSize, /* canWait */ false);
      },
      [this](auto&& result) {
        if (result.hasValue()) {
          // Successfully allocated Region memory but coroutine was cancelled -
          // close the descriptor to release refcount on the Region
          auto& [desc, slotSize, _, _2] = result.value();
          cache_->addHole(slotSize);
          cache_->regionManager_.close(std::move(desc));
        }
      });
  if (ret.hasValue()) {
    co_return std::move(ret).value();
  } else {
    co_return makeError(Error::Code::ALLOCATE_FAILED,
                        "could not allocate space in a region");
  }
}

template <typename CacheItemT>
folly::coro::Task<Result<AllocatedHandle>> FlashCacheComponent::allocateGeneric(
    Key key, uint32_t size, uint32_t creationTime, uint32_t ttlSecs) {
  stats_->allocate_.throughput_.calls_.inc();

  if (key.empty()) {
    stats_->allocate_.throughput_.errors_.inc();
    co_return makeError(Error::Code::INVALID_ARGUMENTS, "empty key");
  }

  auto latencyGuard = stats_->allocate_.latency_.start();
  HashedKey hashedKey(key);
  uint32_t valueSize = size + FlashCacheItem::kMetadataSize;
  auto result = co_await allocateImpl(hashedKey, valueSize);
  if (result.hasError()) {
    stats_->allocate_.throughput_.errors_.inc();
    co_return folly::makeUnexpected(std::move(result).error());
  }
  stats_->allocate_.throughput_.successes_.inc();
  // NOTE: we can't checksum until the user has finished writing their data,
  // defer until they call insert()
  AllocatedHandle handle(*this, InlineItem);
  new (getInlineBuf(handle))
      CacheItemT(hashedKey, valueSize, std::move(result).value(), creationTime,
                 ttlSecs, cache_->checksumType_);
  co_return std::move(handle);
}

folly::coro::Task<Result<AllocatedDescriptor>> FlashCacheComponent::allocate(
    Key key, uint32_t size, uint32_t creationTime, uint32_t ttlSecs) {
  auto result = co_await allocateGeneric<FlashCacheItem>(key, size,
                                                         creationTime, ttlSecs);
  if (result.hasError()) {
    co_return folly::makeUnexpected(std::move(result).error());
  }
  co_return AllocatedDescriptor(std::move(result).value());
}

folly::coro::Task<UnitResult> FlashCacheComponent::insertImpl(
    AllocatedHandle&& handle, bool allowReplace) {
  auto& counter = allowReplace ? stats_->insertOrReplace_ : stats_->insert_;
  counter.throughput_.calls_.inc();

  if (!handle) {
    counter.throughput_.errors_.inc();
    co_return makeError(Error::Code::INVALID_ARGUMENTS,
                        "empty AllocatedHandle");
  }

  auto latencyGuard = counter.latency_.start();
  if (writeBackImpl(*handle, allowReplace)) {
    setInserted(handle, true);
    auto _ = std::move(handle);
    counter.throughput_.successes_.inc();
    co_return folly::unit;
  } else {
    // NOTE: do hole accounting in release()
    counter.throughput_.errors_.inc();
    co_return makeError(Error::Code::ALREADY_INSERTED, "key already inserted");
  }
}

folly::coro::Task<UnitResult> FlashCacheComponent::insert(
    AllocatedHandle&& handle) {
  co_return co_await insertImpl(std::move(handle), /* allowReplace */ false);
}

folly::coro::Task<Result<std::optional<AllocatedHandle>>>
FlashCacheComponent::insertOrReplace(AllocatedHandle&& handle) {
  auto inserted =
      co_await insertImpl(std::move(handle), /* allowReplace */ true);
  XDCHECK(inserted) << "insertOrReplace should never fail";
  // NOTE: returning the old data requires a flash read, don't expose for now
  co_return std::move(inserted).then([](auto&&) {
    return Result<std::optional<AllocatedHandle>>(std::nullopt);
  });
}

folly::coro::Task<Result<std::optional<ReadDescriptor>>>
FlashCacheComponent::find(Key key) {
  // calls_, hits_, and misses_ are not tracked here; BlockCache already tracks
  // them via lookupCount_ and succLookupCount_. See getStats().
  auto latencyGuard = stats_->find_.latency_.start();

  auto res = co_await onWorkerThread(
      [this, hashedKey = HashedKey(key)]()
          -> folly::Expected<BlockCache::LookupData, navy::Status> {
        auto ld = cache_->lookupInternal(hashedKey);
        if (ld.status_ == Status::Retry) {
          return folly::makeUnexpected(ld.status_);
        }
        return ld;
      });
  if (res.hasError()) {
    stats_->find_.throughput_.errors_.inc();
    co_return makeError(Error::Code::FIND_FAILED,
                        "flash lookup retries exhausted");
  }
  auto& ld = res.value();

  switch (ld.status_) {
  case Status::Ok: {
    stats_->find_.throughput_.successes_.inc();
    auto expiryTime = FlashCacheItem::getExpiryTime(ld.buffer_.view());
    if (util::isExpired(expiryTime)) {
      co_return std::nullopt;
    }
    ReadHandle handle(*this, InlineItem);
    // only need the buffer, it has all the fields we need
    new (getInlineBuf(handle)) FlashCacheItem(std::move(ld.buffer_));
    co_return ReadDescriptor(std::move(handle));
  }
  case Status::NotFound:
    stats_->find_.throughput_.successes_.inc();
    co_return std::nullopt;
  default:
    stats_->find_.throughput_.errors_.inc();
    co_return makeError(Error::Code::FIND_FAILED,
                        "flash device or checksum error");
  }
}

template <typename CacheItemT>
folly::coro::Task<Result<std::optional<WriteHandle>>>
FlashCacheComponent::findToWriteGeneric(Key key) {
  stats_->findToWrite_.throughput_.calls_.inc();
  auto latencyGuard = stats_->findToWrite_.latency_.start();

  uint64_t keyHash;
  uint32_t valueSize;
  Buffer buf;
  {
    // NOTE: findToWriteGeneric can be called by child classes, make sure we're
    // calling *our* version of find (not the child's)
    auto findResult = co_await FlashCacheComponent::find(key);
    if (findResult.hasError()) {
      stats_->findToWrite_.throughput_.errors_.inc();
      co_return folly::makeUnexpected(std::move(findResult).error());
    } else if (!findResult->has_value()) {
      stats_->findToWrite_.throughput_.misses_.inc();
      stats_->findToWrite_.throughput_.successes_.inc();
      co_return std::nullopt;
    }

    auto handle = std::move(findResult->value()).release();
    auto* fccItem =
        static_cast<FlashCacheItem*>(const_cast<CacheItem*>(handle.get()));
    const auto* entryDesc = fccItem->getEntryDescriptor();
    keyHash = entryDesc->keyHash;
    valueSize = entryDesc->valueSize;
    buf = std::move(fccItem->getBuffer());
  }

  stats_->findToWrite_.throughput_.hits_.inc();
  auto hashedKey = HashedKey::precomputed(key, keyHash);
  auto allocResult = co_await allocateImpl(hashedKey, valueSize);
  if (allocResult.hasError()) {
    stats_->findToWrite_.throughput_.errors_.inc();
    co_return folly::makeUnexpected(std::move(allocResult).error());
  }
  stats_->findToWrite_.throughput_.successes_.inc();
  WriteHandle handle(*this, InlineItem);
  new (getInlineBuf(handle))
      CacheItemT(std::move(allocResult).value(), std::move(buf));
  co_return std::move(handle);
}

folly::coro::Task<Result<std::optional<WriteDescriptor>>>
FlashCacheComponent::findToWrite(Key key) {
  co_return toWriteDescriptor(co_await findToWriteGeneric<FlashCacheItem>(key));
}

folly::coro::AsyncGenerator<ReadDescriptor> FlashCacheComponent::iterator() {
  // Iterate over the BlockCache index and lookup each entry
  for (const auto& entry : cache_->index()) {
    auto result = co_await onWorkerThread(
        [this, &entry]() { return cache_->lookup(entry); });
    if (result.hasValue()) {
      if (!util::isExpired(FlashCacheItem::getExpiryTime(result->value()))) {
        ReadHandle handle(*this, InlineItem);
        new (getInlineBuf(handle))
            FlashCacheItem(result.value().releaseBuffer());
        co_yield ReadDescriptor(std::move(handle));
      }
    } else {
      XLOG_EVERY_MS(WARN, 250)
          << "Failed to lookup entry in BlockCache: " << result.error();
    }
  }
}

folly::coro::Task<Result<bool>> FlashCacheComponent::remove(Key key) {
  stats_->removeByKey_.throughput_.calls_.inc();
  auto latencyGuard = stats_->removeByKey_.latency_.start();

  auto res = co_await onWorkerThread(
      [this,
       hashedKey = HashedKey(key)]() -> folly::Expected<bool, navy::Status> {
        auto innerResult = cache_->remove(hashedKey);
        switch (innerResult) {
        case navy::Status::Ok:
          return true;
        case navy::Status::NotFound:
          return false;
        default:
          return folly::makeUnexpected(innerResult);
        }
      }
      // No cleanup needed - it doesn't matter if we removed the item or not if
      // the operation was cancelled (operation is "indeterminate")
  );
  if (res.hasValue()) {
    if (res.value()) {
      stats_->removeByKey_.throughput_.hits_.inc();
    } else {
      stats_->removeByKey_.throughput_.misses_.inc();
    }
    stats_->removeByKey_.throughput_.successes_.inc();
    co_return res.value();
  } else {
    stats_->removeByKey_.throughput_.errors_.inc();
    co_return makeError(Error::Code::REMOVE_FAILED,
                        "could not remove item from flash");
  }
}

folly::coro::Task<UnitResult> FlashCacheComponent::remove(ReadHandle&& handle) {
  stats_->removeByHandle_.throughput_.calls_.inc();

  if (!handle) {
    stats_->removeByHandle_.throughput_.errors_.inc();
    co_return makeError(Error::Code::INVALID_ARGUMENTS, "empty ReadHandle");
  }

  auto latencyGuard = stats_->removeByHandle_.latency_.start();
  cache_->removeCount_.inc();
  auto& fccItem = static_cast<FlashCacheItem&>(const_cast<CacheItem&>(*handle));
  auto hashedKey = HashedKey::precomputed(
      fccItem.getKey(), fccItem.getEntryDescriptor()->keyHash);
  auto status = cache_->removeImpl(hashedKey, fccItem.getBufferView());
  XDCHECK(status == navy::Status::Ok || status == navy::Status::NotFound);
  auto _ = std::move(handle);
  stats_->removeByHandle_.throughput_.successes_.inc();
  co_return folly::unit;
}

FlashCacheComponent::FlashCacheComponent(
    std::string&& name,
    navy::BlockCache::Config&& config,
    std::unique_ptr<Device> device,
    const utils::CoroFiberAdapter::Config& executorConfig,
    PersistenceConfig persistenceConfig)
    : CacheComponentWithStats(EvictionCallback{}),
      name_(std::move(name)),
      device_(std::move(device)),
      cache_(std::make_unique<navy::BlockCache>(std::move(config))),
      fiberWorkers_(std::make_unique<utils::CoroFiberAdapter>(executorConfig)),
      persistenceConfig_(std::move(persistenceConfig)),
      coroToFiberLatency_(std::make_unique<util::PercentileStats>()) {}

void FlashCacheComponent::tryRecover() {
  if (persistenceConfig_.recover()) {
    bool recovered = false;
    try {
      auto metadataSize = persistenceConfig_.metadataSize();
      auto rr = navy::createMetadataRecordReader(*device_, metadataSize);
      XDCHECK(rr) << "failed to create metadata reader";
      if (!rr->isEnd() && cache_->recover(*rr)) {
        // If recovery is successful, invalidate the metadata
        auto rw = createMetadataRecordWriter(*device_, metadataSize);
        XDCHECK(rw) << "failed to create metadata writer";
        recovered = rw->invalidate();
      }
    } catch (const std::exception& e) {
      XLOG(WARN) << "Exception while recovering flash cache: " << e.what();
      recovered = false;
    }

    if (!recovered) {
      cache_->reset();
      XLOG(WARN) << "Failed to recover flash cache from device, creating a "
                    "new empty cache";
    }
  }
}

bool FlashCacheComponent::writeBackImpl(CacheItem& item, bool allowReplace) {
  auto& fccItem = static_cast<FlashCacheItem&>(item);
  auto* desc = fccItem.getEntryDescriptor();
  auto logicalSizeWritten = desc->keySize + desc->valueSize;
  auto totalSize = fccItem.getTotalSize();
  auto keyHash = desc->keyHash;

  // NOTE: we don't need to run this on a worker thread because everything here
  // is synchronous - only allocation is async

  if (cache_->checksumData_) {
    desc->cs = checksum(fccItem.getBufferView().slice(0, desc->valueSize),
                        cache_->checksumType_);
  }
  cache_->logicalWrittenCount_.add(logicalSizeWritten);
  return cache_->updateIndex(keyHash, totalSize, fccItem.getRelAddress(),
                             allowReplace);
}

UnitResult FlashCacheComponent::writeBack(CacheItem& item) {
  stats_->writeBack_.throughput_.calls_.inc();
  auto latencyGuard = stats_->writeBack_.latency_.start();
  auto inserted = writeBackImpl(item, /* allowReplace */ true);
  XDCHECK(inserted) << "writeBack should never fail";
  stats_->writeBack_.throughput_.successes_.inc();
  return folly::unit;
}

void FlashCacheComponent::release(CacheItem& item, bool inserted) {
  stats_->release_.throughput_.calls_.inc();
  auto latencyGuard = stats_->release_.latency_.start();
  auto& fccItem = static_cast<FlashCacheItem&>(item);
  if (!inserted) {
    cache_->addHole(fccItem.getTotalSize());
  }
  if (fccItem.isAllocPath() && fccItem.getRegionDescriptor().isReady()) {
    cache_->regionManager_.close(std::move(fccItem.getRegionDescriptor()));
  }
  // Item is stored inline in the Handle's buffer; destroy without deallocating
  item.~CacheItem();
  stats_->release_.throughput_.successes_.inc();
}

UnitResult FlashCacheComponent::shutdown() {
  try {
    cache_->drain();
    cache_->flush();
    if (persistenceConfig_.persist()) {
      auto rw = navy::createMetadataRecordWriter(
          *device_, persistenceConfig_.metadataSize());
      XDCHECK(rw) << "failed to create metadata writer";
      cache_->persist(*rw);
    }
    return folly::unit;
  } catch (const std::exception& e) {
    return makeError(Error::Code::SHUTDOWN_FAILED, e.what());
  }
}

CacheComponentStats FlashCacheComponent::getStats() const noexcept {
  CacheComponentStats stats(*stats_);

  // Populate find throughput from BlockCache's counters
  stats.find_.throughput_.calls_ = cache_->lookupCount_.get();
  stats.find_.throughput_.hits_ = cache_->succLookupCount_.get();
  stats.find_.throughput_.misses_ =
      stats.find_.throughput_.calls_ - stats.find_.throughput_.hits_;

  coroToFiberLatency_->visitQuantileEstimator(
      stats.extraStats_.createCountVisitor(), "coro_to_fiber_hop_latency_ns");
  cache_->getCounters(stats.extraStats_.createCountVisitor());

  // Populate numItems from BlockCache's index
  auto it = stats.extraStats_.getCounts().find("navy_bc_items");
  if (it != stats.extraStats_.getCounts().end()) {
    stats.numItems = static_cast<size_t>(it->second);
  }

  return stats;
}

// ============================================================================
// ConsistentFlashCacheComponent
// ============================================================================

/**
 * Same as FlashCacheItem but holds a lock while the item is still outstanding.
 */
class ConsistentFlashCacheItem : public FlashCacheItem {
 public:
  ConsistentFlashCacheItem(const HashedKey& hashedKey,
                           uint32_t valueSize,
                           FlashCacheComponent::AllocData&& allocation,
                           const uint32_t creationTime,
                           const uint32_t ttlSecs,
                           ChecksumType checksumType)
      : FlashCacheItem(hashedKey,
                       valueSize,
                       std::move(allocation),
                       creationTime,
                       ttlSecs,
                       checksumType) {}
  ConsistentFlashCacheItem(FlashCacheComponent::AllocData&& allocation,
                           const Buffer& existingData)
      : FlashCacheItem(std::move(allocation), existingData) {}
  void setLock(utils::ShardedSerializer::WriteLock&& lock) {
    lock_ = std::move(lock);
  }

 private:
  utils::ShardedSerializer::WriteLock lock_;

  void move(void* dest) noexcept override {
    new (dest) ConsistentFlashCacheItem(std::move(*this));
    this->~ConsistentFlashCacheItem();
  }
};

static_assert(sizeof(ConsistentFlashCacheItem) <= Handle::kInlineBufSize,
              "ConsistentFlashCacheItem must fit in Handle's inline buffer");
static_assert(alignof(ConsistentFlashCacheItem) <= Handle::kInlineBufAlignment,
              "ConsistentFlashCacheItem alignment is bigger than inline buffer "
              "alignment");

/* static */ Result<ConsistentFlashCacheComponent>
ConsistentFlashCacheComponent::create(
    std::string name,
    navy::BlockCache::Config&& config,
    std::unique_ptr<Device> device,
    std::unique_ptr<Hash> hasher,
    uint8_t shardsPower,
    const utils::CoroFiberAdapter::Config& executorConfig,
    FlashCacheComponent::PersistenceConfig persistenceConfig) noexcept {
  try {
    if (auto result = initConfig(config, device.get(), persistenceConfig);
        result.hasError()) {
      return folly::makeUnexpected(std::move(result).error());
    }
    auto component = ConsistentFlashCacheComponent(
        std::move(name), std::move(config), std::move(device), executorConfig,
        std::move(persistenceConfig), std::move(hasher), shardsPower);
    component.tryRecover();
    return component;
  } catch (const std::invalid_argument& ia) {
    return makeError(Error::Code::INVALID_CONFIG, ia.what());
  }
}

folly::coro::Task<Result<AllocatedDescriptor>>
ConsistentFlashCacheComponent::allocate(Key key,
                                        uint32_t size,
                                        uint32_t creationTime,
                                        uint32_t ttlSecs) {
  auto lock = co_await timedWlock(key, lockLatency_->allocate_);
  auto res = co_await Base::allocateGeneric<ConsistentFlashCacheItem>(
      key, size, creationTime, ttlSecs);
  if (res.hasError()) {
    co_return folly::makeUnexpected(std::move(res).error());
  }
  // need to hold the lock through insert()/insertOrReplace() because the cache
  // item stores a region descriptor
  static_cast<ConsistentFlashCacheItem*>(res->get())->setLock(std::move(lock));
  co_return AllocatedDescriptor(std::move(res).value());
}

folly::coro::Task<Result<std::optional<ReadDescriptor>>>
ConsistentFlashCacheComponent::find(Key key) {
  // NOTE: we don't need to hold onto the lock because the returned item isn't
  // holding on to a region descriptor
  [[maybe_unused]] auto lock = co_await timedRlock(key, lockLatency_->find_);
  co_return co_await Base::find(key);
}

folly::coro::Task<Result<std::optional<WriteDescriptor>>>
ConsistentFlashCacheComponent::findToWrite(Key key) {
  auto lock = co_await timedWlock(key, lockLatency_->findToWrite_);
  auto res = co_await Base::findToWriteGeneric<ConsistentFlashCacheItem>(key);
  if (res.hasValue() && res->has_value()) {
    // need to hold the lock through writeBack() since the cache item stores a
    // region descriptor
    static_cast<ConsistentFlashCacheItem*>(res->value().get())
        ->setLock(std::move(lock));
  }
  co_return toWriteDescriptor(std::move(res));
}

folly::coro::Task<Result<bool>> ConsistentFlashCacheComponent::remove(Key key) {
  auto lock = co_await timedWlock(key, lockLatency_->removeByKey_);
  co_return co_await Base::remove(key);
}

folly::coro::Task<UnitResult> ConsistentFlashCacheComponent::remove(
    ReadHandle&& handle) {
  auto lock =
      co_await timedWlock(handle->getKey(), lockLatency_->removeByHandle_);
  co_return co_await Base::remove(std::move(handle));
}

CacheComponentStats ConsistentFlashCacheComponent::getStats() const noexcept {
  auto stats = FlashCacheComponent::getStats();
  auto visitor = stats.extraStats_.createCountVisitor();
  util::PercentileStats::visitQuantileEstimates(
      visitor, lockLatency_->allocate_.toEstimates(),
      "allocate.lock_latency_ns");
  util::PercentileStats::visitQuantileEstimates(
      visitor, lockLatency_->find_.toEstimates(), "find.lock_latency_ns");
  util::PercentileStats::visitQuantileEstimates(
      visitor,
      lockLatency_->findToWrite_.toEstimates(),
      "findToWrite.lock_latency_ns");
  util::PercentileStats::visitQuantileEstimates(
      visitor,
      lockLatency_->removeByKey_.toEstimates(),
      "removeByKey.lock_latency_ns");
  util::PercentileStats::visitQuantileEstimates(
      visitor,
      lockLatency_->removeByHandle_.toEstimates(),
      "removeByHandle.lock_latency_ns");
  return stats;
}

ConsistentFlashCacheComponent::ConsistentFlashCacheComponent(
    std::string&& name,
    navy::BlockCache::Config&& config,
    std::unique_ptr<Device> device,
    const utils::CoroFiberAdapter::Config& executorConfig,
    FlashCacheComponent::PersistenceConfig persistenceConfig,
    std::unique_ptr<Hash> hasher,
    uint8_t shardsPower)
    : FlashCacheComponent(std::move(name),
                          std::move(config),
                          std::move(device),
                          executorConfig,
                          std::move(persistenceConfig)),
      serializer_(std::move(hasher), shardsPower) {}

folly::coro::Task<utils::ShardedSerializer::WriteLock>
ConsistentFlashCacheComponent::timedWlock(
    Key key, detail::LatencyMeasurementCounter& counter) {
  auto guard = counter.start();
  co_return co_await serializer_.wlock(key);
}

folly::coro::Task<utils::ShardedSerializer::ReadLock>
ConsistentFlashCacheComponent::timedRlock(
    Key key, detail::LatencyMeasurementCounter& counter) {
  auto guard = counter.start();
  co_return co_await serializer_.rlock(key);
}

} // namespace facebook::cachelib::interface
