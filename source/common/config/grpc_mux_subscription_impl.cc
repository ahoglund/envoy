#include "common/config/grpc_mux_subscription_impl.h"

#include "common/common/assert.h"
#include "common/common/logger.h"
#include "common/grpc/common.h"
#include "common/protobuf/protobuf.h"
#include "common/protobuf/utility.h"

namespace Envoy {
namespace Config {

GrpcMuxSubscriptionImpl::GrpcMuxSubscriptionImpl(GrpcMux& grpc_mux, SubscriptionStats stats,
                                                 absl::string_view type_url,
                                                 Event::Dispatcher& dispatcher,
                                                 std::chrono::milliseconds init_fetch_timeout)
    : grpc_mux_(grpc_mux), stats_(stats), type_url_(type_url), dispatcher_(dispatcher),
      init_fetch_timeout_(init_fetch_timeout) {}

// Config::Subscription
void GrpcMuxSubscriptionImpl::start(const std::set<std::string>& resources,
                                    SubscriptionCallbacks& callbacks) {
  callbacks_ = &callbacks;

  if (init_fetch_timeout_.count() > 0) {
    init_fetch_timeout_timer_ = dispatcher_.createTimer([this]() -> void {
      ENVOY_LOG(warn, "gRPC config: initial fetch timed out for {}", type_url_);
      callbacks_->onConfigUpdateFailed(nullptr);
    });
    init_fetch_timeout_timer_->enableTimer(init_fetch_timeout_);
  }

  watch_ = grpc_mux_.subscribe(type_url_, resources, *this);
  // The attempt stat here is maintained for the purposes of having consistency between ADS and
  // gRPC/filesystem/REST Subscriptions. Since ADS is push based and muxed, the notion of an
  // "attempt" for a given xDS API combined by ADS is not really that meaningful.
  stats_.update_attempt_.inc();
}

void GrpcMuxSubscriptionImpl::updateResources(const std::set<std::string>& update_to_these_names) {
  // First destroy the watch, so that this subscribe doesn't send a request for both the
  // previously watched resources and the new ones (we may have lost interest in some of the
  // previously watched ones).
  watch_.reset();
  watch_ = grpc_mux_.subscribe(type_url_, update_to_these_names, *this);
  stats_.update_attempt_.inc();
}

// Config::GrpcMuxCallbacks
void GrpcMuxSubscriptionImpl::onConfigUpdate(
    const Protobuf::RepeatedPtrField<ProtobufWkt::Any>& resources,
    const std::string& version_info) {
  disableInitFetchTimeoutTimer();
  // TODO(mattklein123): In the future if we start tracking per-resource versions, we need to
  // supply those versions to onConfigUpdate() along with the xDS response ("system")
  // version_info. This way, both types of versions can be tracked and exposed for debugging by
  // the configuration update targets.
  callbacks_->onConfigUpdate(resources, version_info);
  stats_.update_success_.inc();
  stats_.update_attempt_.inc();
  stats_.version_.set(HashUtil::xxHash64(version_info));
  ENVOY_LOG(debug, "gRPC config for {} accepted with {} resources with version {}", type_url_,
            resources.size(), version_info);
}

void GrpcMuxSubscriptionImpl::onConfigUpdateFailed(const EnvoyException* e) {
  disableInitFetchTimeoutTimer();
  // TODO(htuch): Less fragile signal that this is failure vs. reject.
  if (e == nullptr) {
    stats_.update_failure_.inc();
    ENVOY_LOG(debug, "gRPC update for {} failed", type_url_);
  } else {
    stats_.update_rejected_.inc();
    ENVOY_LOG(warn, "gRPC config for {} rejected: {}", type_url_, e->what());
  }
  stats_.update_attempt_.inc();
  callbacks_->onConfigUpdateFailed(e);
}

std::string GrpcMuxSubscriptionImpl::resourceName(const ProtobufWkt::Any& resource) {
  return callbacks_->resourceName(resource);
}

void GrpcMuxSubscriptionImpl::disableInitFetchTimeoutTimer() {
  if (init_fetch_timeout_timer_) {
    init_fetch_timeout_timer_->disableTimer();
    init_fetch_timeout_timer_.reset();
  }
}

} // namespace Config
} // namespace Envoy
