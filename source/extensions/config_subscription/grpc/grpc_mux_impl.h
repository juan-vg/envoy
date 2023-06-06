#pragma once

#include <cstdint>
#include <memory>
#include <queue>

#include "envoy/common/random_generator.h"
#include "envoy/common/time.h"
#include "envoy/config/custom_config_validators.h"
#include "envoy/config/grpc_mux.h"
#include "envoy/config/subscription.h"
#include "envoy/config/xds_config_tracker.h"
#include "envoy/config/xds_resources_delegate.h"
#include "envoy/event/dispatcher.h"
#include "envoy/grpc/status.h"
#include "envoy/service/discovery/v3/discovery.pb.h"
#include "envoy/upstream/cluster_manager.h"

#include "source/common/common/cleanup.h"
#include "source/common/common/logger.h"
#include "source/common/common/utility.h"
#include "source/common/config/api_version.h"
#include "source/common/config/ttl.h"
#include "source/common/config/utility.h"
#include "source/common/config/xds_context_params.h"
#include "source/common/config/xds_resource.h"
#include "source/extensions/config_subscription/grpc/grpc_stream.h"

#include "absl/container/node_hash_map.h"
#include "xds/core/v3/resource_name.pb.h"

namespace Envoy {
namespace Config {
/**
 * ADS API implementation that fetches via gRPC.
 */
class GrpcMuxImpl : public GrpcMux,
                    public GrpcStreamCallbacks<envoy::service::discovery::v3::DiscoveryResponse>,
                    public Logger::Loggable<Logger::Id::config> {
public:
  GrpcMuxImpl(const LocalInfo::LocalInfo& local_info, Grpc::RawAsyncClientPtr async_client,
              Event::Dispatcher& dispatcher, const Protobuf::MethodDescriptor& service_method,
              Stats::Scope& scope, const RateLimitSettings& rate_limit_settings,
              bool skip_subsequent_node, CustomConfigValidatorsPtr&& config_validators,
              BackOffStrategyPtr backoff_strategy, XdsConfigTrackerOptRef xds_config_tracker,
              XdsResourcesDelegateOptRef xds_resources_delegate,
              const std::string& target_xds_authority);

  ~GrpcMuxImpl() override;

  // Causes all GrpcMuxImpl objects to stop sending any messages on `grpc_stream_` to fix a crash
  // on Envoy shutdown due to dangling pointers. This may not be the ideal fix; it is probably
  // preferable for the `ServerImpl` to cause all configuration subscriptions to be shutdown, which
  // would then cause all `GrpcMuxImpl` to be destructed.
  // TODO: figure out the correct fix: https://github.com/envoyproxy/envoy/issues/15072.
  static void shutdownAll();

  void shutdown() { shutdown_ = true; }

  void start() override;

  // GrpcMux
  ScopedResume pause(const std::string& type_url) override;
  ScopedResume pause(const std::vector<std::string> type_urls) override;

  GrpcMuxWatchPtr addWatch(const std::string& type_url,
                           const absl::flat_hash_set<std::string>& resources,
                           SubscriptionCallbacks& callbacks,
                           OpaqueResourceDecoderSharedPtr resource_decoder,
                           const SubscriptionOptions& options) override;

  void requestOnDemandUpdate(const std::string&, const absl::flat_hash_set<std::string>&) override {
  }

  void handleDiscoveryResponse(
      std::unique_ptr<envoy::service::discovery::v3::DiscoveryResponse>&& message);

  // Config::GrpcStreamCallbacks
  void onStreamEstablished() override;
  void onEstablishmentFailure() override;
  void
  onDiscoveryResponse(std::unique_ptr<envoy::service::discovery::v3::DiscoveryResponse>&& message,
                      ControlPlaneStats& control_plane_stats) override;
  void onWriteable() override;

  GrpcStream<envoy::service::discovery::v3::DiscoveryRequest,
             envoy::service::discovery::v3::DiscoveryResponse>&
  grpcStreamForTest() {
    return grpc_stream_;
  }

private:
  void drainRequests();
  void setRetryTimer();
  void sendDiscoveryRequest(absl::string_view type_url);

  struct GrpcMuxWatchImpl : public GrpcMuxWatch {
    GrpcMuxWatchImpl(const absl::flat_hash_set<std::string>& resources,
                     SubscriptionCallbacks& callbacks,
                     OpaqueResourceDecoderSharedPtr resource_decoder, const std::string& type_url,
                     GrpcMuxImpl& parent, const SubscriptionOptions& options,
                     const LocalInfo::LocalInfo& local_info)
        : callbacks_(callbacks), resource_decoder_(resource_decoder), type_url_(type_url),
          parent_(parent), subscription_options_(options), local_info_(local_info),
          watches_(parent.apiStateFor(type_url).watches_) {
      updateResources(resources);
    }

    ~GrpcMuxWatchImpl() override {
      watches_.erase(iter_);
      if (!resources_.empty()) {
        parent_.queueDiscoveryRequest(type_url_);
      }
    }

    void update(const absl::flat_hash_set<std::string>& resources) override {
      watches_.erase(iter_);
      if (!resources_.empty()) {
        parent_.queueDiscoveryRequest(type_url_);
      }
      updateResources(resources);
      parent_.queueDiscoveryRequest(type_url_);
    }

    // Maintain deterministic wire ordering via ordered std::set.
    std::set<std::string> resources_;
    SubscriptionCallbacks& callbacks_;
    OpaqueResourceDecoderSharedPtr resource_decoder_;
    const std::string type_url_;
    GrpcMuxImpl& parent_;

  private:
    void updateResources(const absl::flat_hash_set<std::string>& resources) {
      resources_.clear();
      std::transform(
          resources.begin(), resources.end(), std::inserter(resources_, resources_.begin()),
          [this](const std::string& resource_name) -> std::string {
            if (XdsResourceIdentifier::hasXdsTpScheme(resource_name)) {
              auto xdstp_resource = XdsResourceIdentifier::decodeUrn(resource_name);
              if (subscription_options_.add_xdstp_node_context_params_) {
                const auto context = XdsContextParams::encodeResource(
                    local_info_.contextProvider().nodeContext(), xdstp_resource.context(), {}, {});
                xdstp_resource.mutable_context()->CopyFrom(context);
              }
              XdsResourceIdentifier::EncodeOptions encode_options;
              encode_options.sort_context_params_ = true;
              return XdsResourceIdentifier::encodeUrn(xdstp_resource, encode_options);
            }
            return resource_name;
          });
      // move this watch to the beginning of the list
      iter_ = watches_.emplace(watches_.begin(), this);
    }

    using WatchList = std::list<GrpcMuxWatchImpl*>;
    const SubscriptionOptions& subscription_options_;
    const LocalInfo::LocalInfo& local_info_;
    WatchList& watches_;
    WatchList::iterator iter_;
  };

  // Per muxed API state.
  struct ApiState {
    ApiState(Event::Dispatcher& dispatcher,
             std::function<void(const std::vector<std::string>&)> callback)
        : ttl_(callback, dispatcher, dispatcher.timeSource()) {}

    bool paused() const { return pauses_ > 0; }

    // Watches on the returned resources for the API;
    std::list<GrpcMuxWatchImpl*> watches_;
    // Current DiscoveryRequest for API.
    envoy::service::discovery::v3::DiscoveryRequest request_;
    // Count of unresumed pause() invocations.
    uint32_t pauses_{};
    // Was a DiscoveryRequest elided during a pause?
    bool pending_{};
    // Has this API been tracked in subscriptions_?
    bool subscribed_{};
    // This resource type must have a Node sent at next request.
    bool must_send_node_{};
    TtlManager ttl_;
    // The identifier for the server that sent the most recent response, or
    // empty if there is none.
    std::string control_plane_identifier_{};
    // If true, xDS resources were previously fetched from an xDS source or an xDS delegate.
    bool previously_fetched_data_{false};
  };

  bool isHeartbeatResource(const std::string& type_url, const DecodedResource& resource) {
    return !resource.hasResource() &&
           resource.version() == apiStateFor(type_url).request_.version_info();
  }
  void expiryCallback(absl::string_view type_url, const std::vector<std::string>& expired);
  // Request queue management logic.
  void queueDiscoveryRequest(absl::string_view queue_item);
  // Invoked when dynamic context parameters change for a resource type.
  void onDynamicContextUpdate(absl::string_view resource_type_url);
  // Must be invoked from the main or test thread.
  void loadConfigFromDelegate(const std::string& type_url,
                              const absl::flat_hash_set<std::string>& resource_names);
  // Must be invoked from the main or test thread.
  void processDiscoveryResources(const std::vector<DecodedResourcePtr>& resources,
                                 ApiState& api_state, const std::string& type_url,
                                 const std::string& version_info, bool call_delegate);

  GrpcStream<envoy::service::discovery::v3::DiscoveryRequest,
             envoy::service::discovery::v3::DiscoveryResponse>
      grpc_stream_;
  const LocalInfo::LocalInfo& local_info_;
  const bool skip_subsequent_node_;
  CustomConfigValidatorsPtr config_validators_;
  XdsConfigTrackerOptRef xds_config_tracker_;
  XdsResourcesDelegateOptRef xds_resources_delegate_;
  const std::string target_xds_authority_;
  bool first_stream_request_;

  // Helper function for looking up and potentially allocating a new ApiState.
  ApiState& apiStateFor(absl::string_view type_url);

  absl::node_hash_map<std::string, std::unique_ptr<ApiState>> api_state_;

  // Envoy's dependency ordering.
  std::list<std::string> subscriptions_;

  // A queue to store requests while rate limited. Note that when requests
  // cannot be sent due to the gRPC stream being down, this queue does not
  // store them; rather, they are simply dropped. This string is a type
  // URL.
  std::unique_ptr<std::queue<std::string>> request_queue_;

  Event::Dispatcher& dispatcher_;
  Common::CallbackHandlePtr dynamic_update_callback_handle_;

  // True iff Envoy is shutting down; no messages should be sent on the `grpc_stream_` when this is
  // true because it may contain dangling pointers.
  std::atomic<bool> shutdown_{false};
};

using GrpcMuxImplPtr = std::unique_ptr<GrpcMuxImpl>;
using GrpcMuxImplSharedPtr = std::shared_ptr<GrpcMuxImpl>;

class GrpcMuxFactory;
DECLARE_FACTORY(GrpcMuxFactory);

} // namespace Config
} // namespace Envoy
