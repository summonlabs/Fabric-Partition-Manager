// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// The partition authority runtime.
//
// One runtime instance is one coordinator incarnation. It consumes an
// authoritative component roster, reachability evidence and an authority policy,
// and it produces governed decisions about which components exist as
// partitions, what authority each may retain, what must be isolated or degraded,
// and when partitions may merge or be revalidated.
//
// Restart semantics
//   A runtime constructed over an existing durable store advances the
//   coordinator epoch and takes a fresh coordinator boot identity. It restores
//   policy, the adopted topology definition, committed lineage, completed
//   decisions and fence records. It does NOT restore reachability evidence,
//   evidence freshness, live authority, attempts in flight, acknowledgements or
//   verified effects. Every authority that existed before the restart is
//   reported as INTERRUPTED until fresh evidence re-derives it.
#ifndef FABRIC_PARTITION_MANAGER_RUNTIME_HPP
#define FABRIC_PARTITION_MANAGER_RUNTIME_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "fabric_partition_manager/authority.hpp"
#include "fabric_partition_manager/decision.hpp"
#include "fabric_partition_manager/evidence.hpp"
#include "fabric_partition_manager/limits.hpp"
#include "fabric_partition_manager/lineage.hpp"
#include "fabric_partition_manager/partition.hpp"
#include "fabric_partition_manager/persistence.hpp"
#include "fabric_partition_manager/topology.hpp"

namespace fabric_partition_manager {

struct RuntimeOptions {
  // Directory holding the durable journal and snapshot. An empty path makes the
  // runtime memory-only: nothing is persisted and nothing is restored.
  std::filesystem::path store_directory;
  Provenance provenance;
  Limits limits = default_limits();
  PartitionPolicy policy = default_policy(PolicyGeneration::from_value(1));
  // When false, decisions and lineage are computed but never written durably.
  bool durable = true;
  bool compact_on_close = true;
};

struct ReachabilitySummary {
  bool valid = false;
  bool confirmed = false;
  bool complete_coverage_conflict = false;
  TopologyGeneration topology_generation;
  ReachabilityGeneration reachability_generation;
  EvidenceSetDigest evidence_digest;
  PairCounters counters;
  std::size_t component_count = 0;
  std::size_t fresh_bundle_count = 0;
  std::size_t stale_bundle_count = 0;

  [[nodiscard]] std::uint64_t undirected_reachable() const noexcept {
    return counters.undirected_reachable;
  }
};

struct PartitionAssessment {
  Decision decision;
  PartitionGeneration generation;
  std::vector<Partition> partitions;
  ReachabilitySummary reachability;
  bool confirmed = false;
  bool split_brain = false;
  PartitionId authoritative_partition;
  PartitionAuthorityClass fabric_authority = PartitionAuthorityClass::Unassigned;
};

// A monotonic, runtime-fenced attempt identity. The identifier names the
// attempt; the sequence orders it. Both are required, so a replay is detected
// even when a caller reuses an identifier with a new position in its own order.
struct AttemptToken {
  AttemptId id;
  AttemptSequence sequence;

  friend bool operator==(const AttemptToken&, const AttemptToken&) noexcept = default;
};

struct MergeRequest {
  AttemptToken attempt;
  // The epoch the caller believes is current. A mismatch is refused: a request
  // may never act under another incarnation's epoch.
  CoordinatorEpoch expected_epoch;
  PartitionId subject;
  LineageId left;
  LineageId right;
  Provenance requester;
};

struct MergeOutcome {
  DecisionVerdict verdict = DecisionVerdict::Invalid;
  Decision decision;
  LineageId merged_lineage;
  PartitionId merged_partition;
  std::vector<PartitionId> fenced_partitions;
};

struct IsolationRequest {
  AttemptToken attempt;
  CoordinatorEpoch expected_epoch;
  ReasonCode cause = ReasonCode::IsolationApplied;
  Provenance requester;
  // The lineage to isolate. Isolation is bound to lineage, not to a partition
  // identity, so it survives a re-derivation of the same membership.
  LineageId lineage;
  std::uint64_t duration_ticks = 0;
};

struct IsolationOutcome {
  DecisionVerdict verdict = DecisionVerdict::Invalid;
  Decision decision;
};

struct RevalidationRequest {
  AttemptToken attempt;
  CoordinatorEpoch expected_epoch;
  Provenance requester;
};

struct RevalidationOutcome {
  DecisionVerdict verdict = DecisionVerdict::Invalid;
  Decision decision;
  std::vector<Partition> partitions;
  std::vector<PartitionId> restored;
  std::vector<PartitionId> still_interrupted;
};

struct RetireRequest {
  AttemptToken attempt;
  CoordinatorEpoch expected_epoch;
  LineageId lineage;
  Provenance requester;
};

struct InterruptedAuthority {
  PartitionId partition;
  LineageId lineage;
  AuthoritySequence authority_sequence;
  CoordinatorEpoch epoch;
  CoordinatorBootId boot;
  std::uint64_t tick = 0;
};

struct RuntimeStatus {
  CoordinatorEpoch epoch;
  CoordinatorBootId boot;
  ProcessIncarnationId incarnation;
  PartitionGeneration partition_generation;
  AuthoritySequence authority_sequence;
  DecisionSequence decision_sequence;
  AttemptSequence attempt_sequence;
  LineageSequence lineage_sequence;
  FenceSequence fence_sequence;
  std::uint64_t tick = 0;
  std::size_t partition_count = 0;
  std::size_t decision_history = 0;
  std::size_t decisions_dropped = 0;
  std::size_t lineage_records = 0;
  std::size_t fence_records = 0;
  std::size_t evidence_bundles = 0;
  std::size_t interrupted_authorities = 0;
  std::size_t active_isolations = 0;
  bool durable = false;
  bool closed = false;
  StoreStats store;
};

class PartitionRuntime {
 public:
  explicit PartitionRuntime(const RuntimeOptions& options);
  ~PartitionRuntime();

  PartitionRuntime(const PartitionRuntime&) = delete;
  PartitionRuntime& operator=(const PartitionRuntime&) = delete;
  PartitionRuntime(PartitionRuntime&&) = delete;
  PartitionRuntime& operator=(PartitionRuntime&&) = delete;

  // -- Identity of this incarnation ----------------------------------------
  [[nodiscard]] CoordinatorEpoch epoch() const;
  [[nodiscard]] CoordinatorBootId boot() const;
  [[nodiscard]] ProcessIncarnationId incarnation() const;
  [[nodiscard]] std::uint64_t tick() const;
  [[nodiscard]] RuntimeStatus status() const;
  [[nodiscard]] const Limits& limits() const noexcept { return options_.limits; }
  [[nodiscard]] PartitionPolicy policy() const;

  // Advances the logical clock. The clock is durable and monotonic; a
  // regression is refused.
  [[nodiscard]] bool advance_ticks(std::uint64_t delta);

  // -- Inputs ---------------------------------------------------------------
  [[nodiscard]] Decision adopt_topology(const TopologyDefinition& definition);
  [[nodiscard]] Decision set_policy(const PartitionPolicy& policy);
  [[nodiscard]] Decision ingest_evidence(const ReachabilityEvidence& evidence);

  // -- Assessment -----------------------------------------------------------
  [[nodiscard]] PartitionAssessment assess();
  [[nodiscard]] PartitionAssessment assess_locked(bool allow_restore);

  // -- Governed lifecycle ---------------------------------------------------
  [[nodiscard]] MergeOutcome request_merge(const MergeRequest& request);
  [[nodiscard]] IsolationOutcome isolate(const IsolationRequest& request);
  [[nodiscard]] IsolationOutcome clear_isolation(const IsolationRequest& request);
  [[nodiscard]] RevalidationOutcome revalidate(const RevalidationRequest& request);
  [[nodiscard]] Decision retire(const RetireRequest& request);

  // -- Application state of an authorization --------------------------------
  // An acknowledgement is a subject's report. A verified effect is this
  // runtime's own observation. The second is refused unless the first was
  // recorded for the same attempt and partition.
  [[nodiscard]] Decision record_acknowledgement(const AuthorityAcknowledgement& acknowledgement);
  [[nodiscard]] Decision record_verified_effect(const VerifiedEffect& effect);
  [[nodiscard]] AuthorityApplicationState application_state(const PartitionId& partition) const;

  // -- Queries --------------------------------------------------------------
  // Every query returns a value. Nothing hands out a reference or a pointer
  // into mutex-guarded mutable state, so a caller can never observe a torn
  // assessment after the lock has been released.
  [[nodiscard]] PartitionAssessment last_assessment() const;
  [[nodiscard]] std::vector<Partition> current_partitions() const;
  [[nodiscard]] std::optional<Partition> find_partition(const PartitionId& id) const;
  [[nodiscard]] std::vector<Decision> decision_history() const;
  [[nodiscard]] std::optional<Decision> find_decision(const DecisionId& id) const;
  [[nodiscard]] std::vector<FenceRecord> fences() const;
  [[nodiscard]] std::vector<LineageRecord> lineage_records() const;
  [[nodiscard]] std::vector<InterruptedAuthority> interrupted_authorities() const;
  [[nodiscard]] std::vector<Reason> restart_reasons() const;
  [[nodiscard]] ReachabilitySummary reachability_summary() const;
  [[nodiscard]] ComponentRoster roster() const;
  [[nodiscard]] std::optional<TopologyDefinition> adopted_topology() const;

  // -- Shutdown -------------------------------------------------------------
  // Releases the durable store and refuses every further mutation. Idempotent.
  void close();
  [[nodiscard]] bool closed() const;

 private:
  struct AttemptRecord {
    AttemptToken attempt;
    DecisionKind kind = DecisionKind::AssessFabric;
    std::uint64_t tick = 0;
  };

  struct IsolationDirective {
    LineageId lineage;
    std::uint64_t expires_at_tick = 0;
    ReasonCode cause = ReasonCode::IsolationApplied;
  };

  [[nodiscard]] Decision make_decision(DecisionKind kind, DecisionVerdict verdict, const Scope& scope,
                                       ReasonCode primary);
  // Allocates the decision sequence and identity at the moment the decision is
  // about to be published, so the audit log is always monotonic in sequence and
  // a nested operation can never append out of order.
  void assign_decision_identity(Decision& decision);
  void finalize_decision(Decision& decision);
  [[nodiscard]] bool commit_record(DurableRecordType type, const std::vector<std::byte>& payload);
  [[nodiscard]] bool check_attempt(const AttemptToken& attempt, DecisionKind kind,
                                   Decision& decision);
  void record_attempt(const AttemptToken& attempt, DecisionKind kind);
  void restore_from_records(const std::vector<DurableRecord>& records);
  struct AuthorityEvaluation {
    DecisionVerdict verdict = DecisionVerdict::Denied;
    PartitionId authoritative;
    bool split_brain = false;
    std::vector<LineageId> restored_lineages;
  };

  [[nodiscard]] AuthorityEvaluation evaluate_authority(const ReachabilitySnapshot& snapshot,
                                                      std::vector<Partition>& partitions,
                                                      bool allow_restore,
                                                      std::vector<Reason>& reasons);
  [[nodiscard]] bool is_isolated(const LineageId& lineage) const;
  [[nodiscard]] FenceRecord make_fence(const Partition& partition, AuthoritySequence sequence,
                                       ReasonCode cause);
  void interrupt_all_authority(ReasonCode cause);
  void store_fence(const FenceRecord& fence);
  [[nodiscard]] std::optional<Partition> partition_by_id(const PartitionId& id) const;

  RuntimeOptions options_;
  mutable std::mutex mutex_;
  EpochState state_;
  PartitionPolicy policy_;
  ComponentRoster roster_;
  TopologyDefinition topology_;
  bool roster_set_ = false;
  EvidenceLedger ledger_;
  LineageStore lineage_;
  DecisionLog decisions_;
  std::vector<FenceRecord> fences_;
  std::vector<AttemptRecord> attempts_;
  std::vector<IsolationDirective> isolations_;
  std::vector<InterruptedAuthority> interrupted_;
  std::vector<AuthorityAcknowledgement> acknowledgements_;
  std::vector<VerifiedEffect> verified_effects_;
  std::vector<Partition> partitions_;
  PartitionAssessment assessment_;
  ReachabilitySnapshot snapshot_;
  bool last_assessment_confirmed_ = false;
  std::vector<Reason> restart_reasons_;
  std::unique_ptr<DurableStore> store_;
  std::uint64_t durable_sequence_ = 0;
  bool closed_ = false;
};

}  // namespace fabric_partition_manager

#endif  // FABRIC_PARTITION_MANAGER_RUNTIME_HPP
