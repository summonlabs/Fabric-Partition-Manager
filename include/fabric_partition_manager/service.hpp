// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// Coordinator service and client.
//
// The server owns one PartitionRuntime and exposes it over the framed protocol
// on a loopback (or explicitly configured) address. Every session establishes
// its authority model with a handshake: the server issues an opaque session
// handle and records the epoch and coordinator boot identity that were current
// when the handle was issued. A frame that names a different handle, a
// different epoch, or a sequence that does not advance is refused and the
// session is closed. One session therefore cannot act under another session's
// identity, boot or epoch.
#ifndef FABRIC_PARTITION_MANAGER_SERVICE_HPP
#define FABRIC_PARTITION_MANAGER_SERVICE_HPP

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "fabric_partition_manager/limits.hpp"
#include "fabric_partition_manager/protocol.hpp"
#include "fabric_partition_manager/runtime.hpp"

namespace fabric_partition_manager {

struct ServerOptions {
  std::string bind_address = "127.0.0.1";
  // Zero selects an ephemeral port; the bound port is reported afterwards.
  std::uint16_t port = 0;
  Limits limits = default_limits();
  Provenance provenance;
  // How long the acceptor waits for a connection before re-checking the stop
  // flag. Shutdown is therefore prompt without closing a socket another thread
  // is blocked on.
  std::uint64_t accept_poll_millis = 25;
};

struct ServerStats {
  std::uint64_t accepted_sessions = 0;
  std::uint64_t rejected_sessions = 0;
  std::uint64_t completed_sessions = 0;
  std::uint64_t frames_received = 0;
  std::uint64_t frames_sent = 0;
  std::uint64_t protocol_errors = 0;
  std::uint64_t replay_rejections = 0;
  std::uint64_t binding_rejections = 0;
  std::uint64_t oversized_rejections = 0;
  std::uint64_t active_sessions = 0;
  std::uint64_t peak_sessions = 0;
};

class PartitionServer {
 public:
  PartitionServer(PartitionRuntime& runtime, const ServerOptions& options);
  ~PartitionServer();

  PartitionServer(const PartitionServer&) = delete;
  PartitionServer& operator=(const PartitionServer&) = delete;

  [[nodiscard]] bool start(std::string& error);
  // Releases every blocked accept and read, joins every session thread and
  // closes every descriptor. Idempotent and safe to call from any thread that
  // does not itself hold a session.
  void stop();
  [[nodiscard]] std::uint16_t port() const;
  [[nodiscard]] std::string endpoint() const;
  [[nodiscard]] ServerStats stats() const;
  [[nodiscard]] bool running() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct ClientOptions {
  std::string address = "127.0.0.1";
  std::uint16_t port = 0;
  Limits limits = default_limits();
};

struct ClientResponse {
  bool ok = false;
  MessageType type = MessageType::ProtocolError;
  std::vector<std::byte> payload;
  std::string error;
};

struct MergeAckView {
  OperationResult result;
  LineageId merged_lineage;
  PartitionId merged_partition;
};

class PartitionClient {
 public:
  explicit PartitionClient(const ClientOptions& options);
  ~PartitionClient();

  PartitionClient(const PartitionClient&) = delete;
  PartitionClient& operator=(const PartitionClient&) = delete;

  [[nodiscard]] bool connect(const PublisherId& publisher, const PublisherBootId& boot,
                             const CoordinatorEpoch& expected_epoch, std::string& error);
  void close();
  [[nodiscard]] bool connected() const;
  [[nodiscard]] CoordinatorEpoch epoch() const;
  [[nodiscard]] CoordinatorBootId boot() const;
  [[nodiscard]] SessionId session() const;

  // Raw exchange. Every typed method below is a thin wrapper over this.
  [[nodiscard]] ClientResponse exchange(MessageType request_type,
                                        const std::vector<std::byte>& payload);

  [[nodiscard]] std::optional<OperationResult> publish_evidence(
      const ReachabilityEvidence& evidence, std::string& error);
  [[nodiscard]] std::optional<OperationResult> adopt_topology(
      const TopologyDefinition& definition, std::string& error);
  [[nodiscard]] std::optional<OperationResult> set_policy(const PartitionPolicy& policy,
                                                          std::string& error);
  [[nodiscard]] std::optional<AssessmentView> assess(std::string& error);
  [[nodiscard]] std::optional<StatusView> status(std::string& error);
  [[nodiscard]] std::optional<std::vector<PartitionView>> partitions(std::string& error);
  [[nodiscard]] std::optional<std::vector<FenceRecord>> fences(std::string& error);
  [[nodiscard]] std::optional<std::vector<LineageRecord>> lineage(std::string& error);
  [[nodiscard]] std::optional<MergeAckView> request_merge(const MergeRequest& request,
                                                          std::string& error);
  [[nodiscard]] std::optional<OperationResult> isolate(const IsolationRequest& request,
                                                       std::string& error);
  [[nodiscard]] std::optional<OperationResult> clear_isolation(const IsolationRequest& request,
                                                               std::string& error);
  [[nodiscard]] std::optional<OperationResult> revalidate(const RevalidationRequest& request,
                                                          std::string& error);
  [[nodiscard]] std::optional<OperationResult> retire(const RetireRequest& request,
                                                      std::string& error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Payload codecs used by both ends.
[[nodiscard]] std::vector<std::byte> encode_merge_request(const MergeRequest& request,
                                                          const Limits& limits);
[[nodiscard]] std::optional<MergeRequest> decode_merge_request(const std::vector<std::byte>& bytes,
                                                               const Limits& limits);
[[nodiscard]] std::vector<std::byte> encode_merge_ack(const MergeAckView& view, const Limits& limits);
[[nodiscard]] std::optional<MergeAckView> decode_merge_ack(const std::vector<std::byte>& bytes,
                                                           const Limits& limits);
[[nodiscard]] std::vector<std::byte> encode_isolation_request(const IsolationRequest& request,
                                                              bool clear, const Limits& limits);
[[nodiscard]] std::optional<IsolationRequest> decode_isolation_request(
    const std::vector<std::byte>& bytes, bool& clear, const Limits& limits);
[[nodiscard]] std::vector<std::byte> encode_revalidation_request(
    const RevalidationRequest& request) noexcept;
[[nodiscard]] std::optional<RevalidationRequest> decode_revalidation_request(
    const std::vector<std::byte>& bytes, const Limits& limits);
[[nodiscard]] std::vector<std::byte> encode_retire_request(const RetireRequest& request) noexcept;
[[nodiscard]] std::optional<RetireRequest> decode_retire_request(const std::vector<std::byte>& bytes,
                                                                 const Limits& limits);
[[nodiscard]] std::vector<std::byte> encode_fence_list(const std::vector<FenceRecord>& fences,
                                                       const Limits& limits);
[[nodiscard]] std::optional<std::vector<FenceRecord>> decode_fence_list(
    const std::vector<std::byte>& bytes, const Limits& limits);
[[nodiscard]] std::vector<std::byte> encode_lineage_list(const std::vector<LineageRecord>& records,
                                                         const Limits& limits);
[[nodiscard]] std::optional<std::vector<LineageRecord>> decode_lineage_list(
    const std::vector<std::byte>& bytes, const Limits& limits);

}  // namespace fabric_partition_manager

#endif  // FABRIC_PARTITION_MANAGER_SERVICE_HPP
