// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// Decisions, bindings and fences.
//
// Every externally visible outcome is a Decision. A decision always identifies
// its exact subject and scope, binds the generations and evidence that made it
// legal, carries a verdict that distinguishes a grant from a denial, a
// degradation, a conflict and an indeterminate result, explains itself with
// stable reason codes, and states whether it is revocable and what would revoke
// it.
#ifndef FABRIC_PARTITION_MANAGER_DECISION_HPP
#define FABRIC_PARTITION_MANAGER_DECISION_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "fabric_partition_manager/authority.hpp"
#include "fabric_partition_manager/digest.hpp"
#include "fabric_partition_manager/evidence.hpp"
#include "fabric_partition_manager/limits.hpp"
#include "fabric_partition_manager/reason.hpp"
#include "fabric_partition_manager/strong_id.hpp"

namespace fabric_partition_manager {

enum class ScopeKind : std::uint8_t {
  Fabric = 0,
  Region = 1,
  Domain = 2,
  Partition = 3,
  Component = 4,
};
inline constexpr std::uint8_t scope_kind_domain_max = 4;

[[nodiscard]] std::string_view scope_kind_name(ScopeKind value) noexcept;

struct Scope {
  ScopeKind kind = ScopeKind::Fabric;
  ScopeId id;

  friend bool operator==(const Scope&, const Scope&) noexcept = default;
};

[[nodiscard]] Scope fabric_scope();
[[nodiscard]] Scope partition_scope(const PartitionId& partition);
[[nodiscard]] Scope component_scope(const ComponentId& component);
[[nodiscard]] Scope named_scope(ScopeKind kind, std::string_view name);

enum class DecisionKind : std::uint8_t {
  AdoptTopology = 0,
  SetPolicy = 1,
  IngestEvidence = 2,
  AssessFabric = 3,
  GrantAuthority = 4,
  DegradeAuthority = 5,
  IsolateComponents = 6,
  MergePartitions = 7,
  Revalidate = 8,
  Fence = 9,
  Retire = 10,
  AcknowledgeAuthority = 11,
  VerifyEffect = 12,
};
inline constexpr std::uint8_t decision_kind_domain_max = 12;

// The verdict vocabulary. UNKNOWN, INDETERMINATE, CONFLICT, INVALID and
// UNSUPPORTED are first-class outcomes; none of them is reported as success and
// none of them is reported as an ordinary absence.
enum class DecisionVerdict : std::uint8_t {
  Observed = 0,
  Granted = 1,
  Denied = 2,
  Degraded = 3,
  Isolated = 4,
  Indeterminate = 5,
  Unsupported = 6,
  Invalid = 7,
  Conflict = 8,
};
inline constexpr std::uint8_t decision_verdict_domain_max = 8;

[[nodiscard]] std::string_view decision_kind_name(DecisionKind value) noexcept;
[[nodiscard]] std::string_view decision_verdict_name(DecisionVerdict value) noexcept;

// Every authority-bearing dependency of a decision.
struct DecisionBindings {
  CoordinatorEpoch epoch;
  CoordinatorBootId boot;
  ProcessIncarnationId incarnation;
  TopologyGeneration topology_generation;
  MembershipDigest topology_digest;
  ReachabilityGeneration reachability_generation;
  EvidenceSetDigest evidence_digest;
  PolicyGeneration policy_generation;
  PartitionGeneration partition_generation;
  AuthoritySequence authority_sequence;
  DecisionSequence decision_sequence;
  AttemptId attempt;
  std::uint64_t evaluated_at_tick = 0;
};

struct FenceRecord {
  FenceId id;
  FenceSequence sequence;
  Scope scope;
  PartitionId partition;
  AuthoritySequence fenced_authority_sequence;
  CoordinatorEpoch fenced_epoch;
  CoordinatorBootId fenced_boot;
  CoordinatorEpoch issuing_epoch;
  CoordinatorBootId issuing_boot;
  std::uint64_t tick = 0;
  ReasonCode cause = ReasonCode::FenceIssued;
};

struct Decision {
  DecisionId id;
  DecisionKind kind = DecisionKind::AssessFabric;
  DecisionVerdict verdict = DecisionVerdict::Observed;
  Scope scope;
  DecisionBindings bindings;
  // The exact subjects the decision applies to, in canonical order.
  std::vector<PartitionId> subjects;
  AuthorityVector authority;
  std::vector<FenceId> fences_issued;
  std::vector<PartitionId> fenced_partitions;
  // Reason codes whose occurrence would invalidate this decision.
  std::vector<ReasonCode> revocation_triggers;
  bool revocable = true;
  std::vector<Reason> reasons;

  [[nodiscard]] std::string render() const;
};

// Derives the verdict from the terminating conditions in a fixed priority
// order, so that the same conditions always produce the same verdict.
[[nodiscard]] DecisionVerdict derive_verdict(bool invalid, bool unsupported, bool indeterminate,
                                             bool conflict, bool granted, bool degraded,
                                             bool isolated) noexcept;

[[nodiscard]] bool decision_verdict_is_success(DecisionVerdict value) noexcept;
[[nodiscard]] bool decision_verdict_needs_attention(DecisionVerdict value) noexcept;

// Bounded audit log of decisions. The most recent entries are retained; the
// bound is explicit and observable.
class DecisionLog {
 public:
  explicit DecisionLog(const Limits& limits);

  void append(Decision decision);
  [[nodiscard]] const std::vector<Decision>& entries() const noexcept { return entries_; }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] std::size_t dropped() const noexcept { return dropped_; }
  [[nodiscard]] const Decision* find(const DecisionId& id) const;
  void clear() noexcept;

 private:
  Limits limits_;
  std::vector<Decision> entries_;
  std::size_t dropped_ = 0;
};

}  // namespace fabric_partition_manager

#endif  // FABRIC_PARTITION_MANAGER_DECISION_HPP
