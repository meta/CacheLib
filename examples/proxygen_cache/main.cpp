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

#include <cachelib/allocator/CacheAllocator.h>
#include <cachelib/allocator/CacheAllocatorConfig.h>
#include <cachelib/allocator/PoolOptimizeStrategy.h>
#include <folly/Conv.h>
#include <folly/Optional.h>
#include <folly/String.h>
#include <folly/init/Init.h>
#include <folly/system/HardwareConcurrency.h>
#include <glog/logging.h>
#include <gflags/gflags.h>
#include <proxygen/httpserver/Filters.h>
#include <proxygen/httpserver/HTTPServer.h>
#include <proxygen/httpserver/HTTPServerOptions.h>
#include <proxygen/httpserver/RequestHandler.h>
#include <proxygen/httpserver/RequestHandlerFactory.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <wangle/ssl/SSLContextConfig.h>

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <stdexcept>

using proxygen::HTTPMessage;
using proxygen::HTTPServer;
using proxygen::RequestHandler;
using proxygen::RequestHandlerFactory;
using proxygen::ResponseBuilder;

namespace {

using Cache = facebook::cachelib::LruAllocator;
using CacheConfig = facebook::cachelib::LruAllocator::Config;
using WriteHandle = Cache::WriteHandle;
using ReadHandle = Cache::ReadHandle;

struct SampleDataView {
  const uint8_t* data;
  size_t len;
  size_t offset;
};

class SampleData {
 public:
  static constexpr size_t kDefaultSize = 64ull * 1024 * 1024; // 64 MiB
  static constexpr size_t kAlign = 64;

  explicit SampleData(size_t bytes = kDefaultSize)
      : size_(bytes) {
    if (bytes == 0) throw std::invalid_argument("data size must be > 0");
#if defined(_ISOC11_SOURCE) || (__STDC_VERSION__ >= 201112L) || defined(_GNU_SOURCE)
    data_ = static_cast<uint8_t*>(aligned_alloc(kAlign, roundUp(bytes, kAlign)));
    if (!data_) throw std::bad_alloc();
#else
    if (posix_memalign(reinterpret_cast<void**>(&data_), kAlign, roundUp(bytes, kAlign)))
      throw std::bad_alloc();
#endif
    fillWithPattern();
  }

  SampleData(const SampleData&) = delete;
  SampleData& operator=(const SampleData&) = delete;
  SampleData(SampleData&& o) noexcept : data_(o.data_), size_(o.size_) {
    o.data_ = nullptr; o.size_ = 0;
  }
  SampleData& operator=(SampleData&& o) noexcept {
    if (this != &o) {
      free(data_);
      data_ = o.data_;
      size_ = o.size_;
      o.data_ = nullptr;
      o.size_ = 0;
    }
    return *this;
  }
  ~SampleData() { free(data_); }

  size_t size() const noexcept { return size_; }
  const uint8_t* raw() const noexcept { return data_; }

  // Get a random aligned slice [ptr, ptr+len)
  SampleDataView getData(size_t len) const {
    if (len == 0) return {data_, 0, 0};
    if (len > size_) throw std::invalid_argument("len exceeds data size");

    thread_local uint64_t seed = initSeed();
    uint64_t r = rngNext(seed);

    size_t maxOff = size_ - len;
    maxOff &= ~(kAlign - 1);
    size_t off = static_cast<size_t>(r % (maxOff + 1));
    off &= ~(kAlign - 1);

    return {data_ + off, len, off};
  }

 private:
  static size_t roundUp(size_t n, size_t a) { return (n + (a - 1)) & ~(a - 1); }

  static inline uint64_t initSeed() {
    uint64_t t = static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    uint64_t x = t ^ (0x9E3779B97F4A7C15ull * reinterpret_cast<uintptr_t>(&t));
    return x ? x : 0xA5A5A5A5A5A5A5A5ull;
  }

  static inline uint64_t rngNext(uint64_t& s) {
    s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
    return s * 0x2545F4914F6CDD1Dull;
  }

  void fillWithPattern() {
    static constexpr char kPattern[] = "the quick brown fox jumps over the lazy dog";
    const size_t plen = sizeof(kPattern) - 1;
    for (size_t off = 0; off < size_;) {
      const size_t n = std::min(plen, size_ - off);
      std::memcpy(data_ + off, kPattern, n);
      off += n;
    }
  }

  uint8_t* data_{nullptr};
  size_t size_{0};
};


struct CacheCtx {
  std::unique_ptr<Cache> cache;
  SampleData sampleData_;
  bool enableLookaside{false};
  std::function<size_t()> getSampleDataSize;
  facebook::cachelib::PoolId poolId{};
};

class CacheHandler : public RequestHandler {
 public:
  explicit CacheHandler(CacheCtx* ctx) : ctx_(ctx) {}

  void onRequest(std::unique_ptr<HTTPMessage> headers) noexcept override {
    req_ = std::move(headers);
  }

  void onBody(std::unique_ptr<folly::IOBuf> body) noexcept override {
    if (body) {
      bodyQueue_.append(std::move(body));
    }
  }

  void onEOM() noexcept override {
    try {
      if (!req_) {
        sendError(400, "Bad Request");
        return;
      }
      const auto method = req_->getMethod();
      const auto path = req_->getPath(); // e.g. /cache/foo
      std::string key;
      if (!extractKey(path, key)) {
        sendError(404, "Not Found");
        return;
      }

      if (method == proxygen::HTTPMethod::GET) {
        handleGet(key);
      } else if (method == proxygen::HTTPMethod::PUT ||
                  method == proxygen::HTTPMethod::POST) {
        handlePut(key);
      } else if (method == proxygen::HTTPMethod::DELETE) {
        handleDelete(key);
      } else {
        ResponseBuilder(downstream_)
            .status(405, "Method Not Allowed")
            .header("Allow", "GET, PUT, DELETE")
            .body("Method Not Allowed\n")
            .sendWithEOM();
      }
    } catch (const std::exception& ex) {
      LOG(ERROR) << "Request handling exception: " << ex.what();
      sendError(500, "Internal Server Error");
    }
  }

  void onUpgrade(proxygen::UpgradeProtocol /*protocol*/) noexcept override {}
  void requestComplete() noexcept override { delete this; }
  void onError(proxygen::ProxygenError /*err*/) noexcept override {
    delete this;
  }

 private:
  bool extractKey(const std::string& path, std::string& keyOut) {
    // Expect paths of form /cache/<key>
    // Allow any characters after /cache/; you may want to URL-decode in real
    // apps.
    static constexpr char kPrefix[] = "/cache/";
    if (path.rfind(kPrefix, 0) != 0) {
      return false;
    }
    keyOut = path.substr(sizeof(kPrefix) - 1);
    return !keyOut.empty();
  }

  void handleGet(const std::string& key) {
    ReadHandle h = ctx_->cache->find(key);
    if (!h) {
      if (ctx_->enableLookaside) {
        WriteHandle wh = ctx_->cache->allocate(ctx_->poolId, key, ctx_->getSampleDataSize());
        if (wh) {
          auto sample = ctx_->sampleData_.getData(ctx_->getSampleDataSize());
          std::memcpy(wh->getMemory(), sample.data, sample.len);
          ctx_->cache->insertOrReplace(std::move(wh));
          VLOG(1) << "Lookaside: populated key " << key << " with "
                  << sample.len << " bytes from offset " << sample.offset;
        } else {
          VLOG(1) << "Lookaside: allocation failed for key " << key;
        }
      }
      ResponseBuilder(downstream_)
          .status(404, "Not Found")
          .body("Key not found\n")
          .sendWithEOM();
      return;
    }
    auto data = reinterpret_cast<const char*>(h->getMemory());
    auto size = h->getSize();
    ResponseBuilder(downstream_)
        .status(200, "OK")
        .header("Content-Type", "application/octet-stream")
        .body(folly::StringPiece(data, size))
        .sendWithEOM();
  }

  void handlePut(const std::string& key) {
    // Flatten body
    std::unique_ptr<folly::IOBuf> buf = bodyQueue_.move();
    auto body = buf->coalesce();
    if (body.size() == 0) {
      ResponseBuilder(downstream_)
          .status(400, "Bad Request")
          .body("Empty body\n")
          .sendWithEOM();
      return;
    }

    WriteHandle wh = ctx_->cache->allocate(ctx_->poolId, key, body.size());
    if (!wh) {
      // Allocation failed (e.g., item too big); try eviction via remove + retry
      // could be added.
      ResponseBuilder(downstream_)
          .status(507, "Insufficient Storage")
          .body("Allocation failed\n")
          .sendWithEOM();
      return;
    }

    std::memcpy(wh->getMemory(), body.data(), body.size());
    ctx_->cache->insertOrReplace(std::move(wh));

    ResponseBuilder(downstream_)
        .status(201, "Created")
        .body("OK\n")
        .sendWithEOM();
  }

  void handleDelete(const std::string& key) {
    auto res = ctx_->cache->remove(key);
    bool removed = true;
    ResponseBuilder(downstream_)
        .status(removed ? 204 : 404, removed ? "No Content" : "Not Found")
        .sendWithEOM();
  }

  void sendError(uint16_t code, folly::StringPiece msg) {
    ResponseBuilder(downstream_)
        .status(code, std::string(msg))
        .body(folly::to<std::string>(msg, "\n"))
        .sendWithEOM();
  }

  CacheCtx* ctx_;
  std::unique_ptr<HTTPMessage> req_;
  folly::IOBufQueue bodyQueue_{folly::IOBufQueue::cacheChainLength()};
};

class CacheHandlerFactory : public RequestHandlerFactory {
 public:
  explicit CacheHandlerFactory(std::shared_ptr<CacheCtx> ctx)
      : ctx_(std::move(ctx)) {}

  void onServerStart(folly::EventBase*) noexcept override {}
  void onServerStop() noexcept override {}

  RequestHandler* onRequest(RequestHandler*, HTTPMessage*) noexcept override {
    return new CacheHandler(ctx_.get());
  }

 private:
  std::shared_ptr<CacheCtx> ctx_;
};

std::unique_ptr<Cache> buildCache(size_t cacheBytes,
                                  facebook::cachelib::PoolId& poolOut) {
  CacheConfig config;
  config.setCacheName("proxygen-cache");
  config.setCacheSize(cacheBytes);

  auto cache = std::make_unique<Cache>(config);
  auto numBytes = cache->getCacheMemoryStats().ramCacheSize;

  LOG(INFO) << "creating cache with size " << numBytes << " bytes";
  // Make a single pool covering all memory
  poolOut = cache->addPool("default", numBytes);
  return cache;
}

} // anonymous namespace

// ---- Flags ----
DEFINE_uint32(port, 8111, "Port to listen on");
DEFINE_uint64(cache_size_mb, 1ULL * 1024, "Cache size in MB");
DEFINE_int32(item_size, 65536, "Default item size (bytes) for lookaside fills");
DEFINE_bool(enable_lookaside, false, "Enable lookaside population on GET miss");

int main(int argc, char* argv[]) {
  folly::Init init(&argc, &argv);
  FLAGS_logtostderr = 1; // show logs on stderr
  FLAGS_minloglevel = 0; // include INFO
  FLAGS_stderrthreshold = 0;
  FLAGS_v = 0; // enable VLOG(1..2)

  // All argument parsing is handled by gflags via folly::Init above.
  // Read the values from FLAGS_*.
  const uint16_t port = static_cast<uint16_t>(FLAGS_port);
  const size_t cacheBytes = static_cast<size_t>(FLAGS_cache_size_mb) * 1024 * 1024;
  const bool enableLookaside = FLAGS_enable_lookaside;
  const int itemSize = FLAGS_item_size;

  if (itemSize <= 0) {
    LOG(ERROR) << "--item_size must be > 0 (got " << itemSize << ")";
    return 2;
  }
  if (cacheBytes < static_cast<size_t>(itemSize)) {
    LOG(WARNING) << "--cache_size_mb (" << cacheBytes
                 << ") is smaller than --item_size (" << itemSize
                 << "); allocations may fail.";
  }

  LOG(INFO) << "Effective config: port=" << port
            << " cache_size=" << cacheBytes
            << " enable_lookaside=" << (enableLookaside ? "true" : "false")
            << " item_size=" << itemSize;

  auto ctx = std::make_shared<CacheCtx>();
  ctx->cache = buildCache(cacheBytes, ctx->poolId);
  ctx->enableLookaside = enableLookaside;
  ctx->getSampleDataSize = [itemSize]() { return static_cast<size_t>(itemSize); };

  proxygen::HTTPServerOptions options;
  options.threads = static_cast<size_t>(folly::available_concurrency());
  options.idleTimeout = std::chrono::milliseconds(60000);
  options.shutdownOn = {SIGINT, SIGTERM};
  options.handlerFactories =
      proxygen::RequestHandlerChain().addThen<CacheHandlerFactory>(ctx).build();

  HTTPServer server(std::move(options));
  std::vector<HTTPServer::IPConfig> ips = {
      {folly::SocketAddress("0.0.0.0", static_cast<uint16_t>(port), true),
       proxygen::HTTPServer::Protocol::HTTP}};
  server.bind(ips);

  LOG(INFO) << "Listening on port " << port;
  server.start();
  return 0;
}
