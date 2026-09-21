// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// Reachability evidence and its resolution.
//
// Reachability evidence is an observation, never an authority. This module
// turns a bounded set of independent observations into a canonical, order
// independent resolution over ordered component pairs, and reports exactly what
// is proven, what contradicts, what is stale, what is asymmetric and what was
// never observed at all.
//
// Three rules define the semantics and are enforced everywhere:
//
//  1. Absence of evidence is not evidence of absence. A pair with no fresh
//     observation resolves UNKNOWN, and UNKNOWN is never turned into
//     "disconnected" or into "connected".
//  2. A publisher that declares COMPLETE coverage of a source component asserts
//     that every roster member it did not list from that source is
//     unreachable. That is a positive, falsifiable claim, and only such a claim
//     can ever make a component decomposition CONFIRMED.
//  3. Two fresh COMPLETE coverage claims about the same source are
//     contradictory evidence about the same subject and are surfaced as a
//     CONFLICT for every pair sourced at that component. They are never merged
//     into one convenient answer.
#ifndef FABRIC_PARTITION_MANAGER_EVIDENCE_HPP
#define FABRIC_PARTITION_MANAGER_EVIDENCE_HPP

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "fabric_partition_manager/digest.hpp"
#include "fabric_partition_manager/limits.hpp"
#include "fabric_partition_manager/policy.hpp"
#include "fabric_partition_manager/reason.hpp"
#include "fabric_partition_manager/strong_id.hpp"
#include "fabric_partition_manager/topology.hpp"

namespace fabric_partition_manager {

// A raw, single-direction observation. Unknown means "the observer did not
// observe this direction"; it never means "not reachable".
enum class Reachability : std::uint8_t {
  Unknown = 0,
  Reachable = 1,
  Unreachable = 2,
};
inline constexpr std::uint8_t reachability_domain_max = 2;

enum class EvidenceCompleteness : std::uint8_t {
  // The publisher tested every roster member from each source it covers and
  // lists every reachable one. Anything unlisted is asserted unreachable.
  Complete = 0,
  // The publisher only reports what it happened to observe. Unlisted pairs stay
  // unknown and can never be turned into unreachable.
  Partial = 1,
};
inline constexpr std::uint8_t evidence_completeness_domain_max = 1;

// The resolved state of an ordered or unordered component pair.
enum class PairResolution : std::uint8_t {
  Unknown = 0,
  Reachable = 1,
  Unreachable = 2,
  Conflicting = 3,
  Stale = 4,
  Asymmetric = 5,
};
inline constexpr std::uint8_t pair_resolution_domain_max = 5;

[[nodiscard]] std::string_view reachability_name(Reachability value) noexcept;
[[nodiscard]] std::string_view evidence_completeness_name(EvidenceCompleteness value) noexcept;
[[nodiscard]] std::string_view pair_resolution_name(PairResolution value) noexcept;

// True only for the two resolutions that are a proven determination.
[[nodiscard]] bool pair_resolution_is_proven(PairResolution value) noexcept;

// True when the resolution is neither proven nor a plain absence of evidence;
// these are the states a caller must surface rather than treat as success.
[[nodiscard]] bool pair_resolution_is_disturbing(PairResolution value) noexcept;

// Joins two directions of the same unordered pair into the undirected
// resolution. The join is commutative, associative and idempotent, which is
// what makes the result independent of observation order.
[[nodiscard]] PairResolution join_directions(PairResolution forward,
                                             PairResolution reverse) noexcept;

struct LinkObservation {
  ComponentId source;
  ComponentId target;
  Reachability value = Reachability::Unknown;

  friend bool operator==(const LinkObservation&, const LinkObservation&) noexcept = default;
};

struct ReachabilityEvidence {
  EvidenceId id;
  EvidenceSequence sequence;
  PublisherId publisher;
  PublisherBootId publisher_boot;
  TopologyGeneration topology_generation;
  ReachabilityGeneration generation;
  std::uint64_t observed_at_tick = 0;
  std::uint64_t validity_ticks = 0;
  EvidenceCompleteness completeness = EvidenceCompleteness::Partial;
  Provenance provenance;
  // Sources for which completeness applies. Ignored when completeness is
  // Partial.
  std::vector<ComponentId> covered_sources;
  // Canonicalised (sorted by source then target) on acceptance.
  std::vector<LinkObservation> observations;

  [[nodiscard]] std::uint64_t observed_span_ticks() const noexcept {
    return observed_at_tick > validity_ticks ? observed_at_tick : validity_ticks;
  }
};

// ---------------------------------------------------------------------------
// Acceptance
// ---------------------------------------------------------------------------

enum class EvidenceAcceptanceStatus : std::uint8_t {
  Accepted = 0,
  RejectedEmpty = 1,
  RejectedMalformed = 2,
  RejectedUnknownComponent = 3,
  RejectedFutureDated = 4,
  RejectedStale = 5,
  RejectedDuplicate = 6,
  RejectedRegression = 7,
  RejectedRetiredBoot = 8,
  RejectedLimit = 9,
  RejectedGeneration = 10,
  RejectedUnsupported = 11,
};

[[nodiscard]] std::string_view evidence_acceptance_status_name(
    EvidenceAcceptanceStatus status) noexcept;

struct EvidenceAcceptance {
  EvidenceAcceptanceStatus status = EvidenceAcceptanceStatus::RejectedMalformed;
  EvidenceId id;
  PublisherId publisher;
  EvidenceSequence sequence;
  bool duplicate_observation_dropped = false;
  std::vector<Reason> reasons;

  [[nodiscard]] bool accepted() const noexcept {
    return status == EvidenceAcceptanceStatus::Accepted;
  }
};

// A publisher cursor is durable only in the sense that it fences replays within
// one runtime incarnation. It is never restored as authority across a restart;
// see restart semantics in the README.
struct PublisherCursor {
  PublisherId publisher;
  PublisherBootId boot;
  EvidenceSequence last_sequence;
  std::uint64_t accepted_bundles = 0;
};

// ---------------------------------------------------------------------------
// Resolution counters
// ---------------------------------------------------------------------------

// Exact ordered-pair accounting. The five ordered categories sum to
// ordered_pairs; asymmetric and contradictory are subsets reported separately.
struct PairCounters {
  std::uint64_t ordered_pairs = 0;
  std::uint64_t ordered_reachable = 0;
  std::uint64_t ordered_unreachable = 0;
  std::uint64_t ordered_conflicting = 0;
  std::uint64_t ordered_stale = 0;
  std::uint64_t ordered_unknown = 0;
  // One direction proven, the reverse direction never observed.
  std::uint64_t ordered_asymmetric = 0;
  // One direction proven reachable and the reverse proven unreachable.
  std::uint64_t ordered_contradictory = 0;

  // Unordered pairs whose two directions are both proven reachable. Exactly the
  // usable edge set.
  std::uint64_t undirected_reachable = 0;

  // Ordered pairs that cross component boundaries, how many of those are
  // proven unreachable, and how many are not.
  std::uint64_t cross_component_ordered = 0;
  std::uint64_t cross_component_unreachable = 0;
  std::uint64_t cross_component_indeterminate = 0;

  [[nodiscard]] std::uint64_t ordered_total_classified() const noexcept {
    return ordered_reachable + ordered_unreachable + ordered_conflicting + ordered_stale +
           ordered_unknown;
  }
};

// ---------------------------------------------------------------------------
// Snapshot
// ---------------------------------------------------------------------------

struct ReachabilitySnapshot {
  bool valid = false;
  TopologyGeneration topology_generation;
  ReachabilityGeneration reachability_generation;
  EvidenceSetDigest evidence_digest;

  std::vector<ComponentId> components;
  std::vector<std::uint64_t> weights;
  std::vector<bool> voters;

  // Canonical undirected edge list. Each edge satisfies source < target.
  std::vector<std::uint32_t> edge_source;
  std::vector<std::uint32_t> edge_target;

  // Component labelling. component_of is the smallest member index of the
  // component that contains the indexed component, so the labelling is
  // canonical and independent of traversal order.
  std::vector<std::uint32_t> component_of;
  std::vector<std::uint32_t> component_sizes;
  std::vector<std::uint32_t> component_representatives;

  PairCounters counters;

  // A decomposition is CONFIRMED only when every ordered pair that crosses a
  // component boundary is proven unreachable. Anything else is PARTIAL.
  bool confirmed = false;
  bool complete_coverage_conflict = false;
  bool any_evidence_fresh = false;

  std::size_t fresh_bundle_count = 0;
  std::size_t stale_bundle_count = 0;
  std::size_t foreign_bundle_count = 0;

  std::vector<EvidenceId> contributing_evidence;
  std::vector<Reason> reasons;

  [[nodiscard]] std::size_t component_count() const noexcept {
    return component_representatives.size();
  }
};

struct ReachabilityBuildInput {
  const ComponentRoster* roster = nullptr;
  const std::vector<ReachabilityEvidence>* evidence = nullptr;
  const PartitionPolicy* policy = nullptr;
  std::uint64_t now_tick = 0;
};

[[nodiscard]] ReachabilitySnapshot build_reachability_snapshot(const ReachabilityBuildInput& input,
                                                               const Limits& limits);

// Deterministic digest over the resolved edge set only. Two evidence sets that
// resolve to the same proven connectivity produce the same digest even when
// they arrived in different orders or with different provenance.
[[nodiscard]] EvidenceSetDigest compute_evidence_digest(const ReachabilitySnapshot& snapshot) noexcept;

// ---------------------------------------------------------------------------
// Ledger
// ---------------------------------------------------------------------------

class EvidenceLedger {
 public:
  explicit EvidenceLedger(const Limits& limits);

  // Ingestion is fail-closed: a bundle that names a component outside the
  // authoritative roster, that is future dated, that replays or regresses a
  // publisher sequence, that arrives from a retired publisher incarnation, or
  // that exceeds a bound is rejected with an explicit status and reasons.
  [[nodiscard]] EvidenceAcceptance accept(const ReachabilityEvidence& evidence,
                                          const ComponentRoster& roster,
                                          const PartitionPolicy& policy,
                                          std::uint64_t now_tick);

  [[nodiscard]] const std::vector<ReachabilityEvidence>& retained() const noexcept {
    return retained_;
  }
  [[nodiscard]] std::size_t retained_count() const noexcept { return retained_.size(); }
  [[nodiscard]] std::size_t retained_bytes() const noexcept { return retained_bytes_; }
  [[nodiscard]] ReachabilityGeneration highest_generation() const noexcept {
    return highest_generation_;
  }
  [[nodiscard]] const std::vector<PublisherCursor>& cursors() const noexcept { return cursors_; }

  // Drops every dynamic observation. Called on restart: persisted lineage and
  // fences survive, reachability currentness does not.
  void drop_dynamic_state() noexcept;

 private:
  [[nodiscard]] std::optional<PublisherCursor> find_cursor(const PublisherId& publisher) const;

  Limits limits_;
  std::vector<ReachabilityEvidence> retained_;
  std::vector<PublisherCursor> cursors_;
  std::vector<PublisherBootId> retired_boots_;
  std::size_t retained_bytes_ = 0;
  ReachabilityGeneration highest_generation_;
};

// Bounded, exact size of an evidence bundle in canonical bytes. Used to refuse
// an oversized bundle before it is materialised.
[[nodiscard]] std::optional<std::size_t> evidence_canonical_size(
    const ReachabilityEvidence& evidence) noexcept;

}  // namespace fabric_partition_manager

#endif  // FABRIC_PARTITION_MANAGER_EVIDENCE_HPP
