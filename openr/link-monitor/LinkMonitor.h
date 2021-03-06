/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include <fbzmq/zmq/Zmq.h>
#include <folly/CppAttributes.h>
#include <folly/IPAddress.h>
#include <folly/Optional.h>
#include <folly/futures/Future.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/EventBase.h>
#include <glog/logging.h>
#include <re2/re2.h>
#include <re2/set.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/allocators/RangeAllocator.h>
#include <openr/common/AsyncThrottle.h>
#include <openr/common/OpenrEventBase.h>
#include <openr/config-store/PersistentStore.h>
#include <openr/if/gen-cpp2/Fib_types.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/if/gen-cpp2/Lsdb_types.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>
#include <openr/if/gen-cpp2/Platform_types.h>
#include <openr/if/gen-cpp2/PrefixManager_types.h>
#include <openr/kvstore/KvStoreClientInternal.h>
#include <openr/link-monitor/InterfaceEntry.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/nl/NetlinkProtocolSocket.h>
#include <openr/spark/Spark.h>

namespace openr {

// Pair <remoteNodeName, interface>
using AdjacencyKey = std::pair<std::string, std::string>;

struct AdjacencyValue {
  thrift::PeerSpec peerSpec;
  thrift::Adjacency adjacency;
  bool isRestarting{false};
  std::string area{};
  AdjacencyValue() {}
  AdjacencyValue(
      thrift::PeerSpec spec,
      thrift::Adjacency adj,
      bool restarting = false,
      std::string areaId = openr::thrift::KvStore_constants::kDefaultArea())
      : peerSpec(spec),
        adjacency(adj),
        isRestarting(restarting),
        area(areaId) {}
};

//
// This class is responsible for reacting to neighbor
// up and down events. The reaction constitutes of starting
// a peering session on the new link and reporting the link
// as an adjacency.
//

class LinkMonitor final : public OpenrEventBase {
 public:
  LinkMonitor(
      // config
      std::shared_ptr<const Config> config,
      // raw ptr for modules
      fbnl::NetlinkProtocolSocket* nlSock,
      KvStore* kvstore,
      PersistentStore* configStore,
      // enable convergence performance measurement for Adjacencies update
      bool enablePerfMeasurement,
      // producer queue
      messaging::ReplicateQueue<thrift::InterfaceDatabase>& intfUpdatesQueue,
      messaging::ReplicateQueue<thrift::PrefixUpdateRequest>& prefixUpdatesQ,
      messaging::ReplicateQueue<thrift::PeerUpdateRequest>& peerUpdatesQueue,
      messaging::ReplicateQueue<LogSample>& logSampleQueue,
      // consumer queue
      messaging::RQueue<thrift::SparkNeighborEvent> neighborUpdatesQueue,
      messaging::RQueue<fbnl::NetlinkEvent> netlinkEventsQueue,
      // if set, we will assume drained if no drain state is found in the
      // persitentStore
      bool assumeDrained,
      // if set, we will override drain state from persistent store with
      // assumeDrained value
      bool overrideDrainState,
      // how long to wait before initial adjacency advertisement
      std::chrono::seconds adjHoldTime);

  ~LinkMonitor() override = default;

  /**
   * Override stop method of OpenrEventBase
   */
  void stop() override;

  // set in mock mode
  // under mock mode, will report tcp://[::]:port as kvstore communication
  // URL instead of using link local address
  void
  setAsMockMode() {
    mockMode_ = true;
  }

  /*
   * Public APIs to change metric:
   * NOTE: except node overload, all requests will be throttled.
   *
   * - Set/unset node overload (Node Drain)
   * - Set/unset interface overload
   * - Set/unset interface metric
   * - Set/unset node adj metric
   * - Dump interface/link information
   * - Dump adjacency database information
   * - Dump links information from netlinkProtocolSocket
   */
  folly::SemiFuture<folly::Unit> setNodeOverload(bool isOverloaded);
  folly::SemiFuture<folly::Unit> setInterfaceOverload(
      std::string interfaceName, bool isOverloaded);
  folly::SemiFuture<folly::Unit> setLinkMetric(
      std::string interfaceName, std::optional<int32_t> overrideMetric);
  folly::SemiFuture<folly::Unit> setAdjacencyMetric(
      std::string interfaceName,
      std::string adjNodeName,
      std::optional<int32_t> overrideMetric);
  folly::SemiFuture<std::unique_ptr<thrift::DumpLinksReply>> getInterfaces();
  folly::SemiFuture<std::unique_ptr<thrift::AdjacencyDatabase>>
  getAdjacencies();
  folly::SemiFuture<std::vector<LinkEntry>> getAllLinks();

  // create required peers <nodeName: PeerSpec> map from current adjacencies_
  static std::unordered_map<std::string, thrift::PeerSpec>
  getPeersFromAdjacencies(
      const std::unordered_map<AdjacencyKey, AdjacencyValue>& adjacencies,
      const std::string& area = thrift::KvStore_constants::kDefaultArea());

 private:
  // make no-copy
  LinkMonitor(const LinkMonitor&) = delete;
  LinkMonitor& operator=(const LinkMonitor&) = delete;

  /*
   * [Spark] neighbor event functions
   */

  // process neighbor event updates from Spark module
  void processNeighborEvent(thrift::SparkNeighborEvent&& event);

  // individual neighbor event function
  void neighborUpEvent(const thrift::SparkNeighborEvent& event);
  void neighborRestartingEvent(const thrift::SparkNeighborEvent& event);
  void neighborDownEvent(const thrift::SparkNeighborEvent& event);
  void neighborRttChangeEvent(const thrift::SparkNeighborEvent& event);

  /*
   * [Netlink Platform] related functions
   */

  // process LINK/ADDR event updates from platform
  void processNetlinkEvent(fbnl::NetlinkEvent&& event);

  // Used for initial interface discovery and periodic sync with system handler
  // return true if sync is successful
  bool syncInterfaces();

  // Get or create InterfaceEntry object.
  // Returns nullptr if ifName doesn't qualify regex match
  // used in syncInterfaces() and LINK/ADDRESS EVENT
  InterfaceEntry* FOLLY_NULLABLE
  getOrCreateInterfaceEntry(const std::string& ifName);

  // call advertiseInterfaces() and advertiseRedistAddrs()
  // throttle updates if there's any unstable interface by
  // getRetryTimeOnUnstableInterfaces() time
  // used in advertiseIfaceAddrThrottled_ and advertiseIfaceAddrTimer_
  // called upon interface change in getOrCreateInterfaceEntry()
  void advertiseIfaceAddr();

  /*
   * [Kvstore] PEER UP/DOWN events sent to Kvstore over peerUpdatesQueue_
   *
   * Called upon spark neighbor events: up/down/restarting
   */

  // derive current peer-spec info from current adjacencies_
  // calculate delta and announce them to KvStore (peer add/remove) if any
  //
  // upPeers: a set of peers we just detected them UP.
  // this covers the case where peer restarted, but we didn't detect restarting
  // spark packet (e.g peer non-graceful-shutdown or all spark messages lost)
  // in this case, the above delta will miss these peers, advertise them
  // if peer-spec matches
  void advertiseKvStorePeers(
      const std::string& area,
      const std::unordered_map<std::string, thrift::PeerSpec>& upPeers = {});

  // advertise to all areas
  void advertiseKvStorePeers(
      const std::unordered_map<std::string, thrift::PeerSpec>& upPeers = {});

  /*
   * [Kvstore] Advertise my adjacencies_ (kvStoreClient_->persistKey)
   *
   * Called upon spark neighbor events: up/down/rtt (restarting does not trigger
   * adj update)
   */
  void advertiseAdjacencies(const std::string& area);
  void advertiseAdjacencies(); // Advertise my adjacencies_ in to all areas

  /*
   * [Spark/Fib] Advertise interfaces_ over interfaceUpdatesQueue_ to Spark/Fib
   *
   * Called in advertiseIfaceAddr() upon interface changes
   */
  void advertiseInterfaces();

  /*
   * [PrefixManager] Advertise redistribute prefixes over prefixUpdatesQueue_ to
   * prefix manager "redistribute prefixes" includes addresses of interfaces
   * that match redistribute_interface_regexes
   *
   * Called in
   * - adjHoldTimer_ during initial start
   * - and advertiseIfaceAddr() upon interface changes
   */
  void advertiseRedistAddrs();

  /*
   * [Util function] general function used for util purpose
   */

  // get next try time, which should be the minimum remaining time among
  // all unstable (getTimeRemainingUntilRetry() > 0) interfaces.
  // return 0 if no more unstable interface
  std::chrono::milliseconds getRetryTimeOnUnstableInterfaces();

  // build AdjacencyDatabase
  thrift::AdjacencyDatabase buildAdjacencyDatabase(
      const std::string& area = thrift::KvStore_constants::kDefaultArea());

  // submit events to monitor
  void logNeighborEvent(thrift::SparkNeighborEvent const& event);

  // link events
  void logLinkEvent(
      const std::string& iface,
      bool wasUp,
      bool isUp,
      std::chrono::milliseconds backoffTime);

  // peer events
  void logPeerEvent(
      const std::string& event,
      const std::string& peerName,
      const thrift::PeerSpec& peerSpec);

  // used to build the key names for this node
  const std::string nodeId_;
  // enable performance measurement
  const bool enablePerfMeasurement_{false};
  // enable v4
  bool enableV4_{false};
  // enable segment routing
  bool enableSegmentRouting_{false};
  // prefix forwarding type and algorithm
  thrift::PrefixForwardingType prefixForwardingType_;
  thrift::PrefixForwardingAlgorithm prefixForwardingAlgorithm_;
  // Use spark measured RTT to neighbor as link metric
  bool useRttMetric_{false};
  // link flap back offs
  std::chrono::milliseconds linkflapInitBackoff_;
  std::chrono::milliseconds linkflapMaxBackoff_;
  // TTL for a key in the key value store
  std::chrono::milliseconds ttlKeyInKvStore_;
  // interface regexes
  std::shared_ptr<const re2::RE2::Set> includeItfRegexes_;
  std::shared_ptr<const re2::RE2::Set> excludeItfRegexes_;
  std::shared_ptr<const re2::RE2::Set> redistributeItfRegexes_;
  // area ids
  std::unordered_set<std::string> areas_{};

  //
  // Mutable state
  //

  // flag to indicate whether it's running in mock mode or not
  // TODO: Get rid of mockMode_
  bool mockMode_{false};

  // LinkMonitor config attributes (defined in LinkMonitor.thrift)
  thrift::LinkMonitorState state_;

  // Queue to publish interface updates to fib/spark
  messaging::ReplicateQueue<thrift::InterfaceDatabase>& interfaceUpdatesQueue_;

  // Queue to publish prefix updates to PrefixManager
  messaging::ReplicateQueue<thrift::PrefixUpdateRequest>& prefixUpdatesQueue_;

  // Queue to publish peer updates to KvStore
  messaging::ReplicateQueue<thrift::PeerUpdateRequest>& peerUpdatesQueue_;

  // Queue to publish the event log
  messaging::ReplicateQueue<LogSample>& logSampleQueue_;

  // used for communicating over thrift/zmq sockets
  apache::thrift::CompactSerializer serializer_;

  // currently active adjacencies
  // an adjacency is uniquely identified by interface and remote node
  // there can be multiple interfaces to a remote node, but at most 1 interface
  // (we use the "min" interface) for tcp connection
  std::unordered_map<AdjacencyKey, AdjacencyValue> adjacencies_;

  // Previously announced KvStore peers
  std::unordered_map<
      std::string /* area */,
      std::unordered_map<std::string /* node name */, thrift::PeerSpec>>
      peers_;

  // all interfaces states, including DOWN one
  // Keyed by interface Name
  std::unordered_map<std::string, InterfaceEntry> interfaces_;

  // Cache of interface index to name. Used for resolving ifIndex
  // on address events
  std::unordered_map<int64_t, std::string> ifIndexToName_;

  // Throttled versions of "advertise<>" functions. It batches
  // up multiple calls and send them in one go!
  std::unique_ptr<AsyncThrottle> advertiseAdjacenciesThrottled_;
  std::unique_ptr<AsyncThrottle> advertiseIfaceAddrThrottled_;

  // Timer for processing interfaces which are in backoff states
  std::unique_ptr<folly::AsyncTimeout> advertiseIfaceAddrTimer_;

  // Timer for resyncing InterfaceDb from netlink
  std::unique_ptr<folly::AsyncTimeout> interfaceDbSyncTimer_;
  ExponentialBackoff<std::chrono::milliseconds> expBackoff_;

  // client to interact with KvStore
  std::unique_ptr<KvStoreClientInternal> kvStoreClient_;

  // RangAlloctor to get unique nodeLabel for this node
  std::unordered_map<std::string /* area */, RangeAllocator<int32_t>>
      rangeAllocator_;

  // Raw ptr to interact with ConfigStore
  PersistentStore* configStore_{nullptr};

  // Raw ptr to interact with NetlinkProtocolSocket
  fbnl::NetlinkProtocolSocket* nlSock_{nullptr};

  // Timer for starting range allocator
  // this is equal to adjHoldTimer_
  // because we'll delay range allocation until we have formed all of our
  // adjcencies
  std::vector<std::unique_ptr<folly::AsyncTimeout>> startAllocationTimers_;

  // Timer for initial hold time expiry
  std::unique_ptr<folly::AsyncTimeout> adjHoldTimer_;
}; // LinkMonitor

} // namespace openr
