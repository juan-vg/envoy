#pragma once

#include <chrono>
#include <thread>

#include "envoy/event/dispatcher.h"
#include "envoy/event/timer.h"
#include "envoy/extensions/filters/network/connection_limit_per_client/v3/connection_limit_per_client.pb.h"
#include "envoy/network/filter.h"
#include "envoy/runtime/runtime.h"
#include "envoy/stats/stats_macros.h"

#include "source/common/common/thread_synchronizer.h"
#include "source/common/runtime/runtime_protos.h"

#include <unordered_map>
#include <mutex>

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace ConnectionLimitPerClientFilter {

/**
 * All connection limit stats. @see stats_macros.h
 */
#define ALL_CONNECTION_LIMIT_STATS(COUNTER, GAUGE)                                                 \
  COUNTER(limited_connections)                                                                     \
  GAUGE(active_connections, Accumulate)

/**
 * Struct definition for connection limit stats. @see stats_macros.h
 */
struct ConnectionLimitPerClientStats {
  ALL_CONNECTION_LIMIT_STATS(GENERATE_COUNTER_STRUCT, GENERATE_GAUGE_STRUCT)
};

/**
 * Configuration shared across all connections on a filter chain basis.
 */
class Config : Logger::Loggable<Logger::Id::filter> {
public:
  Config(const envoy::extensions::filters::network::connection_limit_per_client::v3::ConnectionLimitPerClient&
             proto_config,
         Stats::Scope& scope, Runtime::Loader& runtime);

  bool incrementConnectionWithinLimit(const std::string& client_address);
  void incrementConnection(const std::string& client_address);
  void decrementConnection(const std::string& client_address);
  bool enabled() { return enabled_.enabled(); }
  absl::optional<std::chrono::milliseconds> delay() { return delay_; }
  ConnectionLimitPerClientStats& stats() { return stats_; }

private:
  static ConnectionLimitPerClientStats generateStats(const std::string& prefix, Stats::Scope& scope);
  Runtime::FeatureFlag enabled_;
  ConnectionLimitPerClientStats stats_;
  const uint64_t max_connections_;
  absl::optional<std::chrono::milliseconds> delay_;
  mutable Thread::ThreadSynchronizer synchronizer_; // Used for testing only.

  std::mutex mutex_;
  std::unordered_map<std::string, uint64_t> connections_;

  friend class ConnectionLimitPerClientTestBase;
};

using ConfigSharedPtr = std::shared_ptr<Config>;

/**
 * Per-client connection limit filter
 */
class Filter : public Network::ReadFilter,
               public Network::ConnectionCallbacks,
               Logger::Loggable<Logger::Id::filter> {
public:
  Filter(const ConfigSharedPtr& config) : config_(config) {}

  // Network::ReadFilter
  Network::FilterStatus onData(Buffer::Instance&, bool) override;
  Network::FilterStatus onNewConnection() override;

  void initializeReadFilterCallbacks(Network::ReadFilterCallbacks& read_callbacks) override {
    read_callbacks_ = &read_callbacks;
    read_callbacks_->connection().addConnectionCallbacks(*this);
  }

  // Network::ConnectionCallbacks
  void onEvent(Network::ConnectionEvent event) override;
  void onAboveWriteBufferHighWatermark() override {}
  void onBelowWriteBufferLowWatermark() override {}

private:
  void resetTimerState();
  const ConfigSharedPtr config_;
  Network::ReadFilterCallbacks* read_callbacks_{};
  Event::TimerPtr delay_timer_ = nullptr;
  bool is_rejected_{false};
};

} // namespace ConnectionLimitPerClientFilter
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
