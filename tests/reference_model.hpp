// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// A deliberately slow, independent reference implementation of the documented
// reachability resolution rules. It enumerates every ordered pair directly
// instead of using the analytic per-source accounting the library uses, so the
// two implementations share no code and disagree only if one of them is wrong.
//
// It is O(N^2) and is only ever used on small instances.
#ifndef FABRIC_PARTITION_MANAGER_TESTS_REFERENCE_MODEL_HPP
#define FABRIC_PARTITION_MANAGER_TESTS_REFERENCE_MODEL_HPP

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "fabric_partition_manager/fabric_partition_manager.hpp"

namespace fpm_test {

namespace fpm = fabric_partition_manager;

struct ReferenceOutcome {
  bool valid = false;
  std::vector<std::vector<fpm::PairResolution>> ordered;
  std::vector<std::vector<fpm::PairResolution>> undirected;
  fpm::PairCounters counters;
  std::vector<std::vector<std::uint32_t>> component_members;
  std::vector<std::uint32_t> component_of;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
  bool confirmed = false;
};

// Resolves one ordered pair exactly as the documented rule describes it.
inline fpm::PairResolution reference_ordered(
    const fpm::ComponentRoster& roster,
    const std::vector<const fpm::ReachabilityEvidence*>& fresh,
    const std::vector<std::pair<fpm::ComponentId, fpm::ComponentId>>& stale_pairs,
    const std::vector<std::uint32_t>& cover_count, const fpm::ComponentId& source,
    const fpm::ComponentId& target) {
  const auto source_index = roster.index_of(source);
  if (!source_index.has_value()) {
    return fpm::PairResolution::Unknown;
  }
  bool explicit_reachable = false;
  bool explicit_unreachable = false;
  std::uint32_t covering = 0;
  std::uint32_t covering_hits = 0;
  for (const fpm::ReachabilityEvidence* bundle : fresh) {
    bool covers_source = false;
    if (bundle->completeness == fpm::EvidenceCompleteness::Complete) {
      for (const fpm::ComponentId& covered : bundle->covered_sources) {
        if (covered == source) {
          covers_source = true;
          break;
        }
      }
    }
    if (covers_source) {
      ++covering;
    }
    bool observed_here = false;
    for (const fpm::LinkObservation& observation : bundle->observations) {
      if (!(observation.source == source) || !(observation.target == target)) {
        continue;
      }
      observed_here = true;
      if (observation.value == fpm::Reachability::Reachable) {
        explicit_reachable = true;
      } else if (observation.value == fpm::Reachability::Unreachable) {
        explicit_unreachable = true;
      }
    }
    if (covers_source && observed_here) {
      ++covering_hits;
    }
  }
  (void)cover_count;
  // A fresh complete claim that did not list the pair asserts the pair is
  // unreachable, so the silence of a covering claim is a positive
  // determination rather than an absence of evidence.
  const bool implied_unreachable = covering != 0u && covering_hits < covering;
  if (explicit_reachable && (explicit_unreachable || implied_unreachable)) {
    return fpm::PairResolution::Conflicting;
  }
  if (explicit_reachable) {
    return fpm::PairResolution::Reachable;
  }
  if (explicit_unreachable || implied_unreachable) {
    return fpm::PairResolution::Unreachable;
  }
  for (const auto& pair : stale_pairs) {
    if (pair.first == source && pair.second == target) {
      return fpm::PairResolution::Stale;
    }
  }
  return fpm::PairResolution::Unknown;
}

inline ReferenceOutcome reference_resolve(const fpm::ComponentRoster& roster,
                                          const std::vector<fpm::ReachabilityEvidence>& evidence,
                                          std::uint64_t now_tick,
                                          const fpm::PartitionPolicy& policy) {
  ReferenceOutcome outcome;
  const std::vector<fpm::ComponentId>& components = roster.components();
  const std::size_t count = components.size();
  outcome.valid = true;

  std::vector<const fpm::ReachabilityEvidence*> fresh;
  std::vector<std::pair<fpm::ComponentId, fpm::ComponentId>> stale_pairs;
  std::vector<std::uint32_t> cover_count(count, 0);
  for (const fpm::ReachabilityEvidence& bundle : evidence) {
    if (!(bundle.topology_generation == roster.generation())) {
      continue;
    }
    const bool future = bundle.observed_at_tick > now_tick;
    const std::uint64_t elapsed = future ? 0 : now_tick - bundle.observed_at_tick;
    std::uint64_t horizon = bundle.validity_ticks;
    if (policy.max_evidence_age_ticks != 0 &&
        (horizon == 0 || policy.max_evidence_age_ticks < horizon)) {
      horizon = policy.max_evidence_age_ticks;
    }
    if (!future && elapsed <= horizon) {
      fresh.push_back(&bundle);
      if (bundle.completeness == fpm::EvidenceCompleteness::Complete) {
        for (const fpm::ComponentId& covered : bundle.covered_sources) {
          const auto index = roster.index_of(covered);
          if (index.has_value()) {
            ++cover_count[*index];
          }
        }
      }
    } else {
      for (const fpm::LinkObservation& observation : bundle.observations) {
        stale_pairs.emplace_back(observation.source, observation.target);
      }
    }
  }

  outcome.ordered.assign(count, std::vector<fpm::PairResolution>(count, fpm::PairResolution::Unknown));
  outcome.undirected.assign(count,
                            std::vector<fpm::PairResolution>(count, fpm::PairResolution::Unknown));
  for (std::size_t lhs = 0; lhs < count; ++lhs) {
    for (std::size_t rhs = 0; rhs < count; ++rhs) {
      if (lhs == rhs) {
        continue;
      }
      outcome.ordered[lhs][rhs] = reference_ordered(roster, fresh, stale_pairs, cover_count,
                                                    components[lhs], components[rhs]);
    }
  }
  for (std::size_t lhs = 0; lhs < count; ++lhs) {
    for (std::size_t rhs = lhs + 1; rhs < count; ++rhs) {
      const fpm::PairResolution joined =
          fpm::join_directions(outcome.ordered[lhs][rhs], outcome.ordered[rhs][lhs]);
      outcome.undirected[lhs][rhs] = joined;
      outcome.undirected[rhs][lhs] = joined;
    }
  }

  fpm::PairCounters counters;
  const std::uint64_t total = static_cast<std::uint64_t>(count);
  counters.ordered_pairs = total * (total - 1);
  for (std::size_t lhs = 0; lhs < count; ++lhs) {
    for (std::size_t rhs = 0; rhs < count; ++rhs) {
      if (lhs == rhs) {
        continue;
      }
      switch (outcome.ordered[lhs][rhs]) {
        case fpm::PairResolution::Reachable: ++counters.ordered_reachable; break;
        case fpm::PairResolution::Unreachable: ++counters.ordered_unreachable; break;
        case fpm::PairResolution::Conflicting: ++counters.ordered_conflicting; break;
        case fpm::PairResolution::Stale: ++counters.ordered_stale; break;
        default: ++counters.ordered_unknown; break;
      }
      const bool forward_proven =
          outcome.ordered[lhs][rhs] == fpm::PairResolution::Reachable ||
          outcome.ordered[lhs][rhs] == fpm::PairResolution::Unreachable;
      if (forward_proven && outcome.ordered[rhs][lhs] == fpm::PairResolution::Unknown) {
        ++counters.ordered_asymmetric;
      }
      if (outcome.ordered[lhs][rhs] == fpm::PairResolution::Reachable &&
          outcome.ordered[rhs][lhs] == fpm::PairResolution::Unreachable) {
        ++counters.ordered_contradictory;
      }
    }
  }

  // Components by breadth-first traversal over the undirected edges.
  std::vector<std::uint32_t> label(count, 0xFFFF'FFFFu);
  std::uint32_t next_label = 0;
  for (std::size_t start = 0; start < count; ++start) {
    if (label[start] != 0xFFFF'FFFFu) {
      continue;
    }
    std::vector<std::uint32_t> frontier{static_cast<std::uint32_t>(start)};
    label[start] = next_label;
    while (!frontier.empty()) {
      const std::uint32_t current = frontier.back();
      frontier.pop_back();
      for (std::size_t other = 0; other < count; ++other) {
        if (other == current || label[other] != 0xFFFF'FFFFu) {
          continue;
        }
        if (outcome.undirected[current][other] == fpm::PairResolution::Reachable) {
          label[other] = next_label;
          frontier.push_back(static_cast<std::uint32_t>(other));
        }
      }
    }
    ++next_label;
  }
  // Canonicalise the traversal labels to the smallest member index so that the
  // reference labelling is comparable with the library's canonical labelling,
  // which is independent of traversal order.
  std::vector<std::uint32_t> representative(next_label, 0xFFFF'FFFFu);
  for (std::size_t index = 0; index < count; ++index) {
    const std::uint32_t slot = label[index];
    if (representative[slot] == 0xFFFF'FFFFu) {
      representative[slot] = static_cast<std::uint32_t>(index);
    }
  }
  outcome.component_of.assign(count, 0);
  for (std::size_t index = 0; index < count; ++index) {
    outcome.component_of[index] = representative[label[index]];
  }
  std::vector<std::vector<std::uint32_t>> grouped(next_label);
  for (std::size_t index = 0; index < count; ++index) {
    grouped[label[index]].push_back(static_cast<std::uint32_t>(index));
  }
  outcome.component_members.clear();
  for (const std::vector<std::uint32_t>& members : grouped) {
    if (!members.empty()) {
      outcome.component_members.push_back(members);
    }
  }
  for (std::size_t lhs = 0; lhs < count; ++lhs) {
    for (std::size_t rhs = lhs + 1; rhs < count; ++rhs) {
      if (outcome.undirected[lhs][rhs] == fpm::PairResolution::Reachable) {
        outcome.edges.emplace_back(static_cast<std::uint32_t>(lhs), static_cast<std::uint32_t>(rhs));
        ++counters.undirected_reachable;
      }
    }
  }

  std::uint64_t within = 0;
  for (const std::vector<std::uint32_t>& members : outcome.component_members) {
    within += static_cast<std::uint64_t>(members.size()) * (members.size() - 1);
  }
  counters.cross_component_ordered = counters.ordered_pairs - within;
  std::uint64_t crossing_unreachable = 0;
  for (std::size_t lhs = 0; lhs < count; ++lhs) {
    for (std::size_t rhs = 0; rhs < count; ++rhs) {
      if (lhs == rhs || outcome.component_of[lhs] == outcome.component_of[rhs]) {
        continue;
      }
      if (outcome.ordered[lhs][rhs] == fpm::PairResolution::Unreachable) {
        ++crossing_unreachable;
      }
    }
  }
  counters.cross_component_unreachable = crossing_unreachable;
  counters.cross_component_indeterminate =
      counters.cross_component_ordered - crossing_unreachable;
  outcome.confirmed = counters.cross_component_indeterminate == 0 && count != 0;
  outcome.counters = counters;
  return outcome;
}

}  // namespace fpm_test

#endif  // FABRIC_PARTITION_MANAGER_TESTS_REFERENCE_MODEL_HPP
