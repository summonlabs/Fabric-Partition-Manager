// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// Resolution of a bounded evidence set into a canonical reachability snapshot.
//
// Every reported count is exact, and every step is linear or n log n in the
// observation count. Nothing here iterates over the N^2 component pairs of the
// fabric: the per-source classification is derived analytically from observed
// out-degrees and coverage claims, and the cross-component proof is accumulated
// from the small exception sets of each source.
#include "fabric_partition_manager/evidence.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "fabric_partition_manager/encoding.hpp"

namespace fabric_partition_manager {
namespace {

constexpr std::uint8_t kExplicitReachable = 1u;
constexpr std::uint8_t kExplicitUnreachable = 2u;
constexpr std::uint32_t kUnassigned = 0xFFFF'FFFFu;

class DisjointSet {
 public:
  explicit DisjointSet(std::size_t count) : parent_(count), rank_(count, 0) {
    for (std::size_t index = 0; index < count; ++index) {
      parent_[index] = static_cast<std::uint32_t>(index);
    }
  }

  [[nodiscard]] std::uint32_t find(std::uint32_t value) noexcept {
    while (parent_[value] != value) {
      parent_[value] = parent_[parent_[value]];
      value = parent_[value];
    }
    return value;
  }

  void unite(std::uint32_t lhs, std::uint32_t rhs) noexcept {
    lhs = find(lhs);
    rhs = find(rhs);
    if (lhs == rhs) {
      return;
    }
    if (rank_[lhs] < rank_[rhs]) {
      std::swap(lhs, rhs);
    }
    parent_[rhs] = lhs;
    if (rank_[lhs] == rank_[rhs]) {
      ++rank_[lhs];
    }
  }

 private:
  std::vector<std::uint32_t> parent_;
  std::vector<std::uint8_t> rank_;
};

struct PackedObservation {
  std::uint32_t source = 0;
  std::uint32_t target = 0;
  std::uint8_t value = 0;
};

[[nodiscard]] bool observation_less(const PackedObservation& lhs,
                                    const PackedObservation& rhs) noexcept {
  if (lhs.source != rhs.source) {
    return lhs.source < rhs.source;
  }
  if (lhs.target != rhs.target) {
    return lhs.target < rhs.target;
  }
  return lhs.value < rhs.value;
}

// Folded ordered-pair index in compressed sparse row form.
class OrderedIndex {
 public:
  void build(std::size_t source_count, std::vector<PackedObservation>& observations) {
    std::sort(observations.begin(), observations.end(), observation_less);
    std::vector<std::uint32_t> row_source;
    std::vector<std::uint32_t> row_target;
    std::vector<std::uint8_t> row_flags;
    std::size_t index = 0;
    while (index < observations.size()) {
      const std::uint32_t source = observations[index].source;
      const std::uint32_t target = observations[index].target;
      std::uint8_t flags = 0;
      while (index < observations.size() && observations[index].source == source &&
             observations[index].target == target) {
        flags |= observations[index].value;
        ++index;
      }
      row_source.push_back(source);
      row_target.push_back(target);
      row_flags.push_back(flags);
    }

    offset_.assign(source_count + 1, 0);
    for (const std::uint32_t source : row_source) {
      ++offset_[static_cast<std::size_t>(source) + 1];
    }
    for (std::size_t row = 0; row < source_count; ++row) {
      offset_[row + 1] += offset_[row];
    }
    targets_ = std::move(row_target);
    flags_ = std::move(row_flags);
    source_count_ = source_count;
  }

  [[nodiscard]] std::size_t source_count() const noexcept { return source_count_; }
  [[nodiscard]] std::size_t begin(std::uint32_t source) const noexcept {
    return offset_[source];
  }
  [[nodiscard]] std::size_t end(std::uint32_t source) const noexcept { return offset_[source + 1]; }
  [[nodiscard]] std::uint32_t target(std::size_t row) const noexcept { return targets_[row]; }
  [[nodiscard]] std::uint8_t flags(std::size_t row) const noexcept { return flags_[row]; }

  [[nodiscard]] bool lookup(std::uint32_t source, std::uint32_t target,
                            std::uint8_t& flags) const noexcept {
    const auto first = targets_.begin() + static_cast<std::ptrdiff_t>(offset_[source]);
    const auto last = targets_.begin() + static_cast<std::ptrdiff_t>(offset_[source + 1]);
    const auto found = std::lower_bound(first, last, target);
    if (found == last || *found != target) {
      flags = 0;
      return false;
    }
    flags = flags_[static_cast<std::size_t>(found - targets_.begin())];
    return true;
  }

 private:
  std::size_t source_count_ = 0;
  std::vector<std::uint32_t> offset_;
  std::vector<std::uint32_t> targets_;
  std::vector<std::uint8_t> flags_;
};

// Per-pair count of how many covering complete claims explicitly observed a
// pair. A covering claim that did not list a pair asserts that pair is
// unreachable, so "silent" means "fewer covering claims observed it than cover
// the source".
class CoverCounter {
 public:
  void build(std::vector<PackedObservation>& observations) {
    std::sort(observations.begin(), observations.end(), observation_less);
    std::size_t index = 0;
    while (index < observations.size()) {
      const std::uint32_t source = observations[index].source;
      const std::uint32_t target = observations[index].target;
      std::uint32_t count = 0;
      while (index < observations.size() && observations[index].source == source &&
             observations[index].target == target) {
        ++count;
        ++index;
      }
      keys_.push_back((static_cast<std::uint64_t>(source) << 32) | target);
      counts_.push_back(count);
    }
  }

  [[nodiscard]] std::uint32_t hits(std::uint32_t source, std::uint32_t target) const noexcept {
    const std::uint64_t key = (static_cast<std::uint64_t>(source) << 32) | target;
    const auto found = std::lower_bound(keys_.begin(), keys_.end(), key);
    if (found == keys_.end() || *found != key) {
      return 0;
    }
    return counts_[static_cast<std::size_t>(found - keys_.begin())];
  }

 private:
  std::vector<std::uint64_t> keys_;
  std::vector<std::uint32_t> counts_;
};

// Sorted set of packed (source, target) keys.
class PackedKeySet {
 public:
  void assign(std::vector<std::uint64_t> keys) {
    keys_ = std::move(keys);
    std::sort(keys_.begin(), keys_.end());
    keys_.erase(std::unique(keys_.begin(), keys_.end()), keys_.end());
  }

  [[nodiscard]] bool contains(std::uint32_t source, std::uint32_t target) const noexcept {
    const std::uint64_t key = (static_cast<std::uint64_t>(source) << 32) | target;
    return std::binary_search(keys_.begin(), keys_.end(), key);
  }

  [[nodiscard]] const std::vector<std::uint64_t>& keys() const noexcept { return keys_; }
  [[nodiscard]] std::size_t size() const noexcept { return keys_.size(); }

 private:
  std::vector<std::uint64_t> keys_;
};

[[nodiscard]] std::uint32_t key_source(std::uint64_t key) noexcept {
  return static_cast<std::uint32_t>(key >> 32);
}

[[nodiscard]] std::uint32_t key_target(std::uint64_t key) noexcept {
  return static_cast<std::uint32_t>(key & 0xFFFF'FFFFu);
}

struct ResolutionContext {
  const OrderedIndex* ordered = nullptr;
  const PackedKeySet* stale = nullptr;
  const std::vector<std::uint32_t>* cover_count = nullptr;
  const CoverCounter* cover_hits = nullptr;

  // True when at least one fresh complete claim covering this source did not
  // list the pair, which is a positive assertion that the pair is unreachable.
  [[nodiscard]] bool silent(std::uint32_t source, std::uint32_t target) const noexcept {
    const std::uint32_t covering = (*cover_count)[source];
    if (covering == 0u) {
      return false;
    }
    return cover_hits->hits(source, target) < covering;
  }

  [[nodiscard]] PairResolution resolve_ordered(std::uint32_t source,
                                               std::uint32_t target) const noexcept {
    std::uint8_t flags = 0;
    const bool observed = ordered->lookup(source, target, flags);
    (void)observed;
    const bool explicit_reachable = (flags & kExplicitReachable) != 0u;
    const bool explicit_unreachable = (flags & kExplicitUnreachable) != 0u;
    const bool implied_unreachable = silent(source, target);
    if (explicit_reachable && (explicit_unreachable || implied_unreachable)) {
      // A reachable observation contradicts either an explicit unreachable
      // observation or the silence of a complete claim.
      return PairResolution::Conflicting;
    }
    if (explicit_reachable) {
      return PairResolution::Reachable;
    }
    if (explicit_unreachable || implied_unreachable) {
      return PairResolution::Unreachable;
    }
    if (stale->contains(source, target)) {
      return PairResolution::Stale;
    }
    return PairResolution::Unknown;
  }
};

}  // namespace

std::string_view reachability_name(Reachability value) noexcept {
  switch (value) {
    case Reachability::Unknown: return "UNKNOWN";
    case Reachability::Reachable: return "REACHABLE";
    case Reachability::Unreachable: return "UNREACHABLE";
  }
  return "UNRECOGNIZED";
}

std::string_view evidence_completeness_name(EvidenceCompleteness value) noexcept {
  switch (value) {
    case EvidenceCompleteness::Complete: return "COMPLETE";
    case EvidenceCompleteness::Partial: return "PARTIAL";
  }
  return "UNRECOGNIZED";
}

std::string_view pair_resolution_name(PairResolution value) noexcept {
  switch (value) {
    case PairResolution::Unknown: return "UNKNOWN";
    case PairResolution::Reachable: return "REACHABLE";
    case PairResolution::Unreachable: return "UNREACHABLE";
    case PairResolution::Conflicting: return "CONFLICTING";
    case PairResolution::Stale: return "STALE";
    case PairResolution::Asymmetric: return "ASYMMETRIC";
  }
  return "UNRECOGNIZED";
}

bool pair_resolution_is_proven(PairResolution value) noexcept {
  return value == PairResolution::Reachable || value == PairResolution::Unreachable;
}

bool pair_resolution_is_disturbing(PairResolution value) noexcept {
  return value == PairResolution::Conflicting || value == PairResolution::Stale ||
         value == PairResolution::Asymmetric;
}

PairResolution join_directions(PairResolution forward, PairResolution reverse) noexcept {
  using R = PairResolution;
  if (forward == R::Conflicting || reverse == R::Conflicting) {
    return R::Conflicting;
  }
  const bool forward_proven = pair_resolution_is_proven(forward);
  const bool reverse_proven = pair_resolution_is_proven(reverse);
  if (forward_proven && reverse_proven) {
    if (forward == reverse) {
      return forward;
    }
    // One direction is proven reachable and the reverse is proven unreachable.
    return R::Conflicting;
  }
  if (forward == R::Stale && reverse == R::Stale) {
    return R::Stale;
  }
  if (forward == R::Unknown && reverse == R::Unknown) {
    return R::Unknown;
  }
  if (forward_proven || reverse_proven) {
    // One direction carries a determination and the other does not. That is
    // incomplete evidence about the pair and is reported as ASYMMETRIC rather
    // than folded into UNKNOWN or, worse, into a connectivity claim.
    return R::Asymmetric;
  }
  return R::Unknown;
}

std::string_view evidence_acceptance_status_name(EvidenceAcceptanceStatus status) noexcept {
  switch (status) {
    case EvidenceAcceptanceStatus::Accepted: return "ACCEPTED";
    case EvidenceAcceptanceStatus::RejectedEmpty: return "REJECTED_EMPTY";
    case EvidenceAcceptanceStatus::RejectedMalformed: return "REJECTED_MALFORMED";
    case EvidenceAcceptanceStatus::RejectedUnknownComponent: return "REJECTED_UNKNOWN_COMPONENT";
    case EvidenceAcceptanceStatus::RejectedFutureDated: return "REJECTED_FUTURE_DATED";
    case EvidenceAcceptanceStatus::RejectedStale: return "REJECTED_STALE";
    case EvidenceAcceptanceStatus::RejectedDuplicate: return "REJECTED_DUPLICATE";
    case EvidenceAcceptanceStatus::RejectedRegression: return "REJECTED_REGRESSION";
    case EvidenceAcceptanceStatus::RejectedRetiredBoot: return "REJECTED_RETIRED_BOOT";
    case EvidenceAcceptanceStatus::RejectedLimit: return "REJECTED_LIMIT";
    case EvidenceAcceptanceStatus::RejectedGeneration: return "REJECTED_GENERATION";
    case EvidenceAcceptanceStatus::RejectedUnsupported: return "REJECTED_UNSUPPORTED";
  }
  return "UNRECOGNIZED";
}

std::optional<std::size_t> evidence_canonical_size(const ReachabilityEvidence& evidence) noexcept {
  // Conservative upper bound of the canonical byte image: 100 bytes per covered
  // source (4 length + 96 maximum identifier) and 201 bytes per observation
  // (two bounded identifiers plus the value byte). Used to refuse an oversized
  // bundle before anything is materialised for it.
  std::size_t total = 0;
  const auto add = [&total](std::size_t amount) -> bool {
    const auto sum = checked_size_add(total, amount);
    if (!sum.has_value()) {
      return false;
    }
    total = *sum;
    return true;
  };
  const auto add_text = [&add](std::string_view text) -> bool {
    return add(4) && add(text.size());
  };
  // Fixed-width part: six 64-bit fields (sequence, topology generation,
  // reachability generation, observation tick, validity horizon and the two
  // list counts are accounted separately below), the publisher boot identity,
  // the completeness byte, and the covered-source list count.
  constexpr std::size_t kFixedBytes = (8 * 5) + PublisherBootId::byte_count + 1 + 4;
  if (!add_text(evidence.id.view()) || !add_text(evidence.publisher.view()) ||
      !add(kFixedBytes) || !add_text(evidence.provenance.view()) || !add(4)) {
    return std::nullopt;
  }
  const auto covered_bytes = checked_size_mul(evidence.covered_sources.size(), 100);
  if (!covered_bytes.has_value() || !add(*covered_bytes)) {
    return std::nullopt;
  }
  const auto observation_bytes = checked_size_mul(evidence.observations.size(), 201);
  if (!observation_bytes.has_value() || !add(*observation_bytes)) {
    return std::nullopt;
  }
  return total;
}

ReachabilitySnapshot build_reachability_snapshot(const ReachabilityBuildInput& input,
                                                 const Limits& limits) {
  ReachabilitySnapshot snapshot;
  if (input.roster == nullptr || !input.roster->is_set()) {
    append_reason(snapshot.reasons, ReasonCode::SubjectUnknown, "roster",
                  "no authoritative roster has been accepted", limits);
    return snapshot;
  }
  const ComponentRoster& roster = *input.roster;
  const std::vector<ComponentId>& components = roster.components();
  const std::size_t count = components.size();

  snapshot.valid = true;
  snapshot.topology_generation = roster.generation();
  snapshot.components = components;
  snapshot.weights.assign(count, 0);
  snapshot.voters.assign(count, false);
  snapshot.component_of.assign(count, 0);

  if (input.policy != nullptr) {
    for (std::size_t index = 0; index < count; ++index) {
      snapshot.weights[index] = input.policy->weight_of(components[index]);
      snapshot.voters[index] = input.policy->is_voter(components[index]);
    }
  } else {
    snapshot.weights.assign(count, 1);
  }

  if (count == 0) {
    append_reason(snapshot.reasons, ReasonCode::PartitionRosterEmpty, "roster",
                  "the authoritative roster contains no components", limits);
    return snapshot;
  }

  const std::uint64_t tick_now = input.now_tick;
  const std::uint64_t policy_age =
      input.policy != nullptr ? input.policy->max_evidence_age_ticks : 0;

  std::vector<PackedObservation> fresh;
  std::vector<PackedObservation> covering;
  std::vector<std::uint64_t> stale_keys;
  std::vector<std::uint32_t> cover_count(count, 0);
  bool coverage_conflict = false;

  if (input.evidence != nullptr) {
    for (const ReachabilityEvidence& bundle : *input.evidence) {
      if (!(bundle.topology_generation == roster.generation())) {
        ++snapshot.foreign_bundle_count;
        continue;
      }
      const bool future_dated = bundle.observed_at_tick > tick_now;
      const std::uint64_t elapsed = future_dated ? 0 : tick_now - bundle.observed_at_tick;
      std::uint64_t horizon = bundle.validity_ticks;
      if (policy_age != 0 && (horizon == 0 || policy_age < horizon)) {
        horizon = policy_age;
      }
      const bool fresh_bundle = !future_dated && elapsed <= horizon;

      if (fresh_bundle) {
        ++snapshot.fresh_bundle_count;
        snapshot.any_evidence_fresh = true;
        snapshot.contributing_evidence.push_back(bundle.id);
        if (bundle.generation.value() > snapshot.reachability_generation.value()) {
          snapshot.reachability_generation = bundle.generation;
        }
      } else {
        ++snapshot.stale_bundle_count;
        append_reason(snapshot.reasons, ReasonCode::EvidenceRejectedStale, bundle.id.view(),
                      "evidence is outside its freshness horizon at the evaluation tick", limits);
      }

      for (const LinkObservation& observation : bundle.observations) {
        const auto source = roster.index_of(observation.source);
        const auto target = roster.index_of(observation.target);
        if (!source.has_value() || !target.has_value()) {
          continue;
        }
        if (fresh_bundle) {
          PackedObservation packed;
          packed.source = *source;
          packed.target = *target;
          packed.value = observation.value == Reachability::Reachable ? kExplicitReachable
                                                                     : kExplicitUnreachable;
          fresh.push_back(packed);
        } else {
          stale_keys.push_back((static_cast<std::uint64_t>(*source) << 32) | *target);
        }
      }

      if (fresh_bundle && bundle.completeness == EvidenceCompleteness::Complete) {
        // Observations sourced at a component this claim covers are part of the
        // claim, so they are also folded into the covering index.
        std::vector<bool> covered_here(count, false);
        for (const ComponentId& covered : bundle.covered_sources) {
          const auto index = roster.index_of(covered);
          if (!index.has_value()) {
            continue;
          }
          covered_here[*index] = true;
          ++cover_count[*index];
          if (cover_count[*index] >= 2u) {
            coverage_conflict = true;
          }
        }
        for (const LinkObservation& observation : bundle.observations) {
          const auto source = roster.index_of(observation.source);
          const auto target = roster.index_of(observation.target);
          if (!source.has_value() || !target.has_value() || !covered_here[*source]) {
            continue;
          }
          PackedObservation packed;
          packed.source = *source;
          packed.target = *target;
          packed.value = observation.value == Reachability::Reachable ? kExplicitReachable
                                                                     : kExplicitUnreachable;
          covering.push_back(packed);
        }
      }
    }
  }

  OrderedIndex ordered;
  ordered.build(count, fresh);
  CoverCounter cover_hits;
  cover_hits.build(covering);
  PackedKeySet stale;
  stale.assign(std::move(stale_keys));

  ResolutionContext context;
  context.ordered = &ordered;
  context.stale = &stale;
  context.cover_count = &cover_count;
  context.cover_hits = &cover_hits;

  snapshot.complete_coverage_conflict = coverage_conflict;

  if (coverage_conflict) {
    append_reason(snapshot.reasons, ReasonCode::EvidenceDuplicateCoverage, "coverage",
                  "two fresh bundles claim complete coverage of the same source component", limits);
  }

  // ---------------------------------------------------------------
  // Per-source statistics.
  // ---------------------------------------------------------------
  std::vector<std::uint64_t> observed_targets(count, 0);
  std::vector<std::uint64_t> stale_only_targets(count, 0);

  for (std::size_t source = 0; source < count; ++source) {
    const auto index = static_cast<std::uint32_t>(source);
    observed_targets[source] = ordered.end(index) - ordered.begin(index);
  }
  for (const std::uint64_t key : stale.keys()) {
    const std::uint32_t source = key_source(key);
    const std::uint32_t target = key_target(key);
    std::uint8_t flags = 0;
    if (!ordered.lookup(source, target, flags)) {
      ++stale_only_targets[source];
    }
  }

  const std::uint64_t total = static_cast<std::uint64_t>(count);
  const std::uint64_t all_pairs = total * (total - 1);
  const std::uint64_t pairs_from_source = total - 1;

  PairCounters counters;
  counters.ordered_pairs = all_pairs;

  // The ordered classification is tallied directly from the resolved state of
  // every pair the source observed, plus the closed form for the pairs it did
  // not. This is linear in the observation count and shares one code path with
  // the resolution used for the edges, so the counters cannot drift from it.
  for (std::size_t index = 0; index < count; ++index) {
    const auto source_index = static_cast<std::uint32_t>(index);
    const std::uint64_t observed = observed_targets[index];
    std::uint64_t reachable_count = 0;
    std::uint64_t unreachable_count = 0;
    std::uint64_t conflicting_count = 0;
    std::uint64_t stale_count = 0;
    for (std::size_t row = ordered.begin(source_index); row < ordered.end(source_index); ++row) {
      const std::uint32_t target = ordered.target(row);
      switch (context.resolve_ordered(source_index, target)) {
        case PairResolution::Reachable: ++reachable_count; break;
        case PairResolution::Unreachable: ++unreachable_count; break;
        case PairResolution::Conflicting: ++conflicting_count; break;
        case PairResolution::Stale: ++stale_count; break;
        case PairResolution::Unknown:
        case PairResolution::Asymmetric: break;
      }
    }
    // Every unobserved pair of a component that some fresh complete claim
    // covers is asserted unreachable by that claim. Without such a claim an
    // unobserved pair is stale at best and unknown otherwise.
    const std::uint64_t unobserved = pairs_from_source - observed;
    std::uint64_t unknown_count = 0;
    if (cover_count[index] != 0u) {
      unreachable_count += unobserved;
    } else {
      stale_count += stale_only_targets[index];
      unknown_count = unobserved - stale_only_targets[index];
    }
    counters.ordered_reachable += reachable_count;
    counters.ordered_unreachable += unreachable_count;
    counters.ordered_conflicting += conflicting_count;
    counters.ordered_stale += stale_count;
    counters.ordered_unknown += unknown_count;
  }

  // ---------------------------------------------------------------
  // Proven edges and canonical components.
  // ---------------------------------------------------------------
  std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
  for (std::size_t source = 0; source < count; ++source) {
    const auto source_index = static_cast<std::uint32_t>(source);
    for (std::size_t row = ordered.begin(source_index); row < ordered.end(source_index); ++row) {
      if ((ordered.flags(row) & kExplicitReachable) == 0u) {
        continue;
      }
      const std::uint32_t target = ordered.target(row);
      if (context.resolve_ordered(source_index, target) != PairResolution::Reachable) {
        continue;
      }
      if (context.resolve_ordered(target, source_index) != PairResolution::Reachable) {
        continue;
      }
      edges.emplace_back(std::min(source_index, target), std::max(source_index, target));
    }
  }
  std::sort(edges.begin(), edges.end());
  edges.erase(std::unique(edges.begin(), edges.end()), edges.end());

  snapshot.edge_source.reserve(edges.size());
  snapshot.edge_target.reserve(edges.size());
  for (const auto& edge : edges) {
    snapshot.edge_source.push_back(edge.first);
    snapshot.edge_target.push_back(edge.second);
  }
  counters.undirected_reachable = static_cast<std::uint64_t>(edges.size());

  DisjointSet sets(count);
  for (const auto& edge : edges) {
    sets.unite(edge.first, edge.second);
  }

  std::vector<std::uint32_t> representative(count, kUnassigned);
  for (std::size_t index = 0; index < count; ++index) {
    const auto root = sets.find(static_cast<std::uint32_t>(index));
    if (representative[root] == kUnassigned) {
      representative[root] = static_cast<std::uint32_t>(index);
    }
  }
  std::vector<std::uint32_t> canonical(count, 0);
  for (std::size_t index = 0; index < count; ++index) {
    canonical[index] = representative[sets.find(static_cast<std::uint32_t>(index))];
  }
  std::vector<std::uint32_t> sizes(count, 0);
  for (std::size_t index = 0; index < count; ++index) {
    ++sizes[canonical[index]];
  }
  for (std::size_t index = 0; index < count; ++index) {
    if (sizes[index] != 0) {
      snapshot.component_representatives.push_back(static_cast<std::uint32_t>(index));
    }
  }
  snapshot.component_of = std::move(canonical);
  snapshot.component_sizes = std::move(sizes);

  std::uint64_t within_component_pairs = 0;
  for (const std::uint32_t value : snapshot.component_sizes) {
    within_component_pairs += static_cast<std::uint64_t>(value) * (value - 1);
  }
  counters.cross_component_ordered = all_pairs - within_component_pairs;

  // ---------------------------------------------------------------
  // Cross-component proof accounting.
  // ---------------------------------------------------------------
  std::uint64_t cross_unreachable = 0;
  for (std::size_t source = 0; source < count; ++source) {
    const auto source_index = static_cast<std::uint32_t>(source);
    const std::uint32_t home = snapshot.component_of[source_index];
    const std::uint64_t outside =
        total - static_cast<std::uint64_t>(snapshot.component_sizes[home]);
    if (cover_count[source] != 0u) {
      // When a fresh complete claim covers this source, every crossing pair is
      // asserted unreachable unless the source observed it. The exception set E
      // is therefore bounded by the observed out-degree.
      std::uint64_t exceptions_outside = 0;
      for (std::size_t row = ordered.begin(source_index); row < ordered.end(source_index); ++row) {
        if (context.resolve_ordered(source_index, ordered.target(row)) ==
            PairResolution::Unreachable) {
          continue;
        }
        if (snapshot.component_of[ordered.target(row)] != home) {
          ++exceptions_outside;
        }
      }
      cross_unreachable += outside - exceptions_outside;
    } else {
      for (std::size_t row = ordered.begin(source_index); row < ordered.end(source_index); ++row) {
        if (context.resolve_ordered(source_index, ordered.target(row)) !=
            PairResolution::Unreachable) {
          continue;
        }
        if (snapshot.component_of[ordered.target(row)] != home) {
          ++cross_unreachable;
        }
      }
    }
  }
  counters.cross_component_unreachable = cross_unreachable;
  counters.cross_component_indeterminate = counters.cross_component_ordered - cross_unreachable;
  snapshot.confirmed = counters.cross_component_indeterminate == 0;

  // ---------------------------------------------------------------
  // Asymmetry and contradiction accounting.
  // ---------------------------------------------------------------
  std::vector<std::uint64_t> reverse_keys;
  for (std::size_t source = 0; source < count; ++source) {
    if (cover_count[source] != 0u) {
      continue;
    }
    const auto index = static_cast<std::uint32_t>(source);
    for (std::size_t row = ordered.begin(index); row < ordered.end(index); ++row) {
      reverse_keys.push_back((static_cast<std::uint64_t>(ordered.target(row)) << 32) | index);
    }
  }
  for (const std::uint64_t key : stale.keys()) {
    const std::uint32_t source = key_source(key);
    if (cover_count[source] != 0u) {
      continue;
    }
    reverse_keys.push_back((static_cast<std::uint64_t>(key_target(key)) << 32) | source);
  }
  std::sort(reverse_keys.begin(), reverse_keys.end());
  reverse_keys.erase(std::unique(reverse_keys.begin(), reverse_keys.end()), reverse_keys.end());

  std::uint64_t covered_source_total = 0;
  for (std::size_t index = 0; index < count; ++index) {
    if (cover_count[index] != 0u) {
      ++covered_source_total;
    }
  }

  std::vector<std::uint64_t> reverse_touch(count, 0);
  for (std::size_t index = 0; index < reverse_keys.size();) {
    const std::uint32_t target = key_source(reverse_keys[index]);
    while (index < reverse_keys.size() && key_source(reverse_keys[index]) == target) {
      ++index;
      ++reverse_touch[target];
    }
  }

  std::uint64_t asymmetric = 0;
  for (std::size_t index = 0; index < count; ++index) {
    const auto source_index = static_cast<std::uint32_t>(index);
    // Number of reverse directions that resolve UNKNOWN. Only an uncovered
    // source can be unknown towards anything, so the count is the uncovered
    // population minus the uncovered sources that supplied a fresh or stale
    // observation of this source.
    const std::uint64_t non_unknown_reverse =
        covered_source_total - (cover_count[index] != 0u ? 1u : 0u) + reverse_touch[index];
    const std::uint64_t reverse_unknown = pairs_from_source - non_unknown_reverse;
    std::uint64_t value = 0;
    if (cover_count[index] != 0u) {
      // Every pair sourced at a covered component is determined, so the
      // determination set is the whole row minus the conflicting entries. The
      // count therefore starts from the number of reverse directions that are
      // UNKNOWN and removes the conflicting forward entries among them.
      value = reverse_unknown;
      for (std::size_t row = ordered.begin(source_index); row < ordered.end(source_index); ++row) {
        const std::uint32_t target = ordered.target(row);
        if (context.resolve_ordered(target, source_index) != PairResolution::Unknown) {
          continue;
        }
        if (context.resolve_ordered(source_index, target) == PairResolution::Conflicting &&
            value > 0) {
          --value;
        }
      }
    } else {
      // An uncovered source has an observed out-degree bounded by the evidence
      // it supplied, so its determination set is enumerated directly.
      for (std::size_t row = ordered.begin(source_index); row < ordered.end(source_index); ++row) {
        const std::uint32_t target = ordered.target(row);
        const PairResolution forward = context.resolve_ordered(source_index, target);
        if (forward != PairResolution::Reachable && forward != PairResolution::Unreachable) {
          continue;
        }
        if (context.resolve_ordered(target, source_index) == PairResolution::Unknown) {
          ++value;
        }
      }
    }
    asymmetric += value;
  }
  counters.ordered_asymmetric = asymmetric;

  std::uint64_t contradictory = 0;
  for (std::size_t source = 0; source < count; ++source) {
    const auto source_index = static_cast<std::uint32_t>(source);
    for (std::size_t row = ordered.begin(source_index); row < ordered.end(source_index); ++row) {
      if ((ordered.flags(row) & kExplicitReachable) == 0u) {
        continue;
      }
      const std::uint32_t target = ordered.target(row);
      if (context.resolve_ordered(source_index, target) != PairResolution::Reachable) {
        continue;
      }
      if (context.resolve_ordered(target, source_index) == PairResolution::Unreachable) {
        ++contradictory;
      }
    }
  }
  counters.ordered_contradictory = contradictory;

  snapshot.counters = counters;
  snapshot.evidence_digest = compute_evidence_digest(snapshot);

  if (snapshot.confirmed) {
    append_reason(snapshot.reasons, ReasonCode::PartitionConfirmed, "fabric",
                  "every component-crossing ordered pair is proven unreachable", limits);
  } else {
    append_reason(snapshot.reasons, ReasonCode::PartitionPartial, "fabric",
                  std::to_string(snapshot.counters.cross_component_indeterminate) +
                      " component-crossing ordered pairs are not proven unreachable",
                  limits);
  }
  if (counters.ordered_conflicting != 0) {
    append_reason(snapshot.reasons, ReasonCode::EvidenceConflictingPairs, "fabric",
                  std::to_string(counters.ordered_conflicting) +
                      " ordered pairs carry contradictory observations",
                  limits);
  }
  if (counters.ordered_stale != 0) {
    append_reason(snapshot.reasons, ReasonCode::EvidenceStalePairs, "fabric",
                  std::to_string(counters.ordered_stale) +
                      " ordered pairs are covered only by expired observations",
                  limits);
  }
  if (counters.ordered_asymmetric != 0) {
    append_reason(snapshot.reasons, ReasonCode::EvidenceAsymmetricPairs, "fabric",
                  std::to_string(counters.ordered_asymmetric) +
                      " ordered pairs have a proven direction and an unobserved reverse",
                  limits);
  }
  return snapshot;
}

EvidenceSetDigest compute_evidence_digest(const ReachabilitySnapshot& snapshot) noexcept {
  Sha256 hasher;
  hasher.update_text("fabric-partition-manager/resolved-connectivity/v1");
  hasher.update_be64(snapshot.topology_generation.value());
  hasher.update_be64(static_cast<std::uint64_t>(snapshot.components.size()));
  hasher.update_be64(static_cast<std::uint64_t>(snapshot.edge_source.size()));
  for (std::size_t index = 0; index < snapshot.edge_source.size(); ++index) {
    const std::uint32_t source = snapshot.edge_source[index];
    const std::uint32_t target = snapshot.edge_target[index];
    hasher.update_text(snapshot.components[source].view());
    hasher.update_text(snapshot.components[target].view());
  }
  return EvidenceSetDigest::from_digest(hasher.finish());
}

EvidenceLedger::EvidenceLedger(const Limits& limits) : limits_(limits) {}

std::optional<PublisherCursor> EvidenceLedger::find_cursor(const PublisherId& publisher) const {
  for (const PublisherCursor& cursor : cursors_) {
    if (cursor.publisher == publisher) {
      return cursor;
    }
  }
  return std::nullopt;
}

EvidenceAcceptance EvidenceLedger::accept(const ReachabilityEvidence& evidence,
                                          const ComponentRoster& roster,
                                          const PartitionPolicy& policy,
                                          std::uint64_t now_tick) {
  EvidenceAcceptance result;
  result.id = evidence.id;
  result.publisher = evidence.publisher;
  result.sequence = evidence.sequence;

  const Limits& limits = limits_;
  const auto reject = [&result, &limits](EvidenceAcceptanceStatus status, ReasonCode code,
                                         std::string_view subject, std::string_view detail) {
    result.status = status;
    append_reason(result.reasons, code, subject, detail, limits);
  };

  if (evidence.id.is_nil() || evidence.publisher.is_nil()) {
    reject(EvidenceAcceptanceStatus::RejectedMalformed, ReasonCode::EvidenceRejectedMalformed,
           evidence.id.view(), "evidence id and publisher id are both required");
    return result;
  }
  if (evidence.validity_ticks == 0) {
    reject(EvidenceAcceptanceStatus::RejectedMalformed, ReasonCode::EvidenceRejectedMalformed,
           evidence.id.view(), "a validity horizon of zero ticks is not a usable observation");
    return result;
  }
  if (evidence.observations.size() > limits.max_observations_per_evidence) {
    reject(EvidenceAcceptanceStatus::RejectedLimit, ReasonCode::EvidenceRejectedLimit,
           evidence.id.view(), "observation count exceeds the configured bound");
    return result;
  }
  if (evidence.covered_sources.size() > limits.max_covered_components_per_evidence) {
    reject(EvidenceAcceptanceStatus::RejectedLimit, ReasonCode::EvidenceRejectedLimit,
           evidence.id.view(), "covered source count exceeds the configured bound");
    return result;
  }
  const auto canonical_size = evidence_canonical_size(evidence);
  if (!canonical_size.has_value()) {
    reject(EvidenceAcceptanceStatus::RejectedLimit, ReasonCode::ArithmeticRefused,
           evidence.id.view(), "canonical size computation overflowed");
    return result;
  }
  if (*canonical_size > limits.max_evidence_bytes) {
    reject(EvidenceAcceptanceStatus::RejectedLimit, ReasonCode::EvidenceRejectedLimit,
           evidence.id.view(), "canonical evidence size exceeds the configured bound");
    return result;
  }
  if (evidence.observations.empty() && evidence.covered_sources.empty()) {
    reject(EvidenceAcceptanceStatus::RejectedEmpty, ReasonCode::EvidenceRejectedEmpty,
           evidence.id.view(), "the bundle carries no observation and no coverage claim");
    return result;
  }
  if (evidence.completeness != EvidenceCompleteness::Complete &&
      evidence.completeness != EvidenceCompleteness::Partial) {
    reject(EvidenceAcceptanceStatus::RejectedUnsupported,
           ReasonCode::EvidenceRejectedUnsupportedCompleteness, evidence.id.view(),
           "unrecognised completeness value");
    return result;
  }
  if (!(evidence.topology_generation == roster.generation())) {
    reject(EvidenceAcceptanceStatus::RejectedGeneration, ReasonCode::EvidenceRejectedGeneration,
           evidence.id.view(), "evidence was observed against a different topology generation");
    return result;
  }
  if (evidence.observed_at_tick > now_tick) {
    reject(EvidenceAcceptanceStatus::RejectedFutureDated, ReasonCode::EvidenceRejectedFutureDated,
           evidence.id.view(), "observation tick is in the future relative to the runtime clock");
    return result;
  }

  std::vector<ComponentId> covered = evidence.covered_sources;
  std::sort(covered.begin(), covered.end());
  for (std::size_t index = 0; index < covered.size(); ++index) {
    if (!roster.contains(covered[index])) {
      reject(EvidenceAcceptanceStatus::RejectedUnknownComponent,
             ReasonCode::EvidenceRejectedUnknownComponent, covered[index].view(),
             "covered source is not a member of the authoritative roster");
      return result;
    }
    if (index > 0 && covered[index] == covered[index - 1]) {
      reject(EvidenceAcceptanceStatus::RejectedMalformed, ReasonCode::EvidenceRejectedMalformed,
             covered[index].view(), "the coverage claim repeats the same source");
      return result;
    }
  }
  if (evidence.completeness == EvidenceCompleteness::Partial && !covered.empty()) {
    reject(EvidenceAcceptanceStatus::RejectedMalformed, ReasonCode::EvidenceRejectedMalformed,
           evidence.id.view(), "a partial bundle must not carry a coverage claim");
    return result;
  }
  if (evidence.completeness == EvidenceCompleteness::Complete && covered.empty()) {
    reject(EvidenceAcceptanceStatus::RejectedMalformed, ReasonCode::EvidenceRejectedMalformed,
           evidence.id.view(), "a complete bundle must name the sources it covered");
    return result;
  }

  for (const LinkObservation& observation : evidence.observations) {
    if (observation.source == observation.target) {
      reject(EvidenceAcceptanceStatus::RejectedMalformed, ReasonCode::EvidenceRejectedMalformed,
             observation.source.view(), "an observation of a component against itself is invalid");
      return result;
    }
    if (observation.value != Reachability::Reachable &&
        observation.value != Reachability::Unreachable) {
      reject(EvidenceAcceptanceStatus::RejectedMalformed, ReasonCode::EvidenceRejectedMalformed,
             observation.source.view(), "an explicit observation must be reachable or unreachable");
      return result;
    }
    if (!roster.contains(observation.source)) {
      reject(EvidenceAcceptanceStatus::RejectedUnknownComponent,
             ReasonCode::EvidenceRejectedUnknownComponent, observation.source.view(),
             "observation source is not a member of the authoritative roster");
      return result;
    }
    if (!roster.contains(observation.target)) {
      reject(EvidenceAcceptanceStatus::RejectedUnknownComponent,
             ReasonCode::EvidenceRejectedUnknownComponent, observation.target.view(),
             "observation target is not a member of the authoritative roster");
      return result;
    }
  }

  for (const PublisherBootId& retired : retired_boots_) {
    if (retired == evidence.publisher_boot) {
      reject(EvidenceAcceptanceStatus::RejectedRetiredBoot,
             ReasonCode::EvidenceRejectedRetiredBoot, evidence.publisher.view(),
             "the publisher incarnation that produced this evidence has been retired");
      return result;
    }
  }

  const auto cursor = find_cursor(evidence.publisher);
  if (cursor.has_value() && cursor->boot == evidence.publisher_boot) {
    if (evidence.sequence == cursor->last_sequence) {
      reject(EvidenceAcceptanceStatus::RejectedDuplicate, ReasonCode::EvidenceRejectedDuplicate,
             evidence.publisher.view(), "the publisher sequence has already been accepted");
      return result;
    }
    if (evidence.sequence.value() < cursor->last_sequence.value()) {
      reject(EvidenceAcceptanceStatus::RejectedRegression, ReasonCode::EvidenceRejectedRegression,
             evidence.publisher.view(), "the publisher sequence regressed");
      return result;
    }
  }

  if (retained_.size() >= limits.max_retained_evidence) {
    reject(EvidenceAcceptanceStatus::RejectedLimit, ReasonCode::EvidenceRejectedLimit,
           evidence.id.view(), "the retained evidence table is full");
    return result;
  }
  const auto projected = checked_size_add(retained_bytes_, *canonical_size);
  if (!projected.has_value() || *projected > limits.max_evidence_bytes) {
    reject(EvidenceAcceptanceStatus::RejectedLimit, ReasonCode::EvidenceRejectedLimit,
           evidence.id.view(), "the retained evidence budget is exhausted");
    return result;
  }

  const std::uint64_t horizon = policy.max_evidence_age_ticks;
  const std::uint64_t elapsed = now_tick - evidence.observed_at_tick;
  const std::uint64_t effective =
      horizon == 0 || horizon > evidence.validity_ticks ? evidence.validity_ticks : horizon;
  if (elapsed > effective) {
    reject(EvidenceAcceptanceStatus::RejectedStale, ReasonCode::EvidenceRejectedStale,
           evidence.id.view(), "the bundle is already outside its freshness horizon");
    return result;
  }

  ReachabilityEvidence stored = evidence;
  stored.covered_sources = std::move(covered);
  std::sort(stored.observations.begin(), stored.observations.end(),
            [](const LinkObservation& lhs, const LinkObservation& rhs) {
              if (lhs.source != rhs.source) {
                return lhs.source < rhs.source;
              }
              if (lhs.target != rhs.target) {
                return lhs.target < rhs.target;
              }
              return static_cast<unsigned>(lhs.value) < static_cast<unsigned>(rhs.value);
            });
  const std::size_t before = stored.observations.size();
  stored.observations.erase(
      std::unique(stored.observations.begin(), stored.observations.end(),
                  [](const LinkObservation& lhs, const LinkObservation& rhs) {
                    return lhs.source == rhs.source && lhs.target == rhs.target &&
                           lhs.value == rhs.value;
                  }),
      stored.observations.end());
  result.duplicate_observation_dropped = stored.observations.size() != before;

  if (cursor.has_value() && !(cursor->boot == evidence.publisher_boot)) {
    if (retired_boots_.size() >= limits.max_retained_evidence) {
      reject(EvidenceAcceptanceStatus::RejectedLimit, ReasonCode::CapacityExhausted,
             evidence.publisher.view(), "the retired publisher incarnation table is full");
      return result;
    }
    retired_boots_.push_back(cursor->boot);
  }

  bool cursor_updated = false;
  for (PublisherCursor& entry : cursors_) {
    if (entry.publisher == evidence.publisher) {
      entry.boot = evidence.publisher_boot;
      entry.last_sequence = evidence.sequence;
      ++entry.accepted_bundles;
      cursor_updated = true;
      break;
    }
  }
  if (!cursor_updated) {
    if (cursors_.size() >= limits.max_retained_evidence) {
      reject(EvidenceAcceptanceStatus::RejectedLimit, ReasonCode::CapacityExhausted,
             evidence.publisher.view(), "the publisher cursor table is full");
      return result;
    }
    PublisherCursor entry;
    entry.publisher = evidence.publisher;
    entry.boot = evidence.publisher_boot;
    entry.last_sequence = evidence.sequence;
    entry.accepted_bundles = 1;
    cursors_.push_back(entry);
  }

  if (evidence.generation.value() > highest_generation_.value()) {
    highest_generation_ = evidence.generation;
  }
  retained_bytes_ = *projected;
  retained_.push_back(std::move(stored));

  result.status = EvidenceAcceptanceStatus::Accepted;
  append_reason(result.reasons, ReasonCode::EvidenceAccepted, evidence.id.view(),
                "observation accepted into the live evidence ledger", limits);
  return result;
}

void EvidenceLedger::drop_dynamic_state() noexcept {
  retained_.clear();
  cursors_.clear();
  retired_boots_.clear();
  retained_bytes_ = 0;
  highest_generation_ = ReachabilityGeneration{};
}

}  // namespace fabric_partition_manager
