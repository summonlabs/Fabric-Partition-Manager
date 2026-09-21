// Fabric Partition Manager 1.0.0 - Summon Software Labs
#include "fabric_partition_manager/synthetic.hpp"

#include <algorithm>
#include <array>
#include <string>

namespace fabric_partition_manager {
namespace {

constexpr std::string_view kComponentPrefix = "node-";

[[nodiscard]] std::string pad(std::size_t index) {
  std::array<char, 10> digits{};
  std::size_t value = index;
  for (std::size_t position = digits.size(); position > 0; --position) {
    digits[position - 1] = static_cast<char>('0' + (value % 10));
    value /= 10;
  }
  return std::string(digits.data(), digits.size());
}

}  // namespace

std::uint64_t DeterministicRandom::next_u64() noexcept {
  state_ += 0x9E3779B97F4A7C15ull;
  std::uint64_t value = state_;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
  return value ^ (value >> 31);
}

std::uint32_t DeterministicRandom::next_below(std::uint32_t bound) noexcept {
  if (bound < 2) {
    return 0;
  }
  const std::uint64_t limit = (0x1'0000'0000ull / bound) * bound;
  for (;;) {
    const std::uint64_t value = next_u64() & 0xFFFF'FFFFull;
    if (value < limit) {
      return static_cast<std::uint32_t>(value % bound);
    }
  }
}

ComponentId synthetic_component_id(std::size_t index) {
  std::string text(kComponentPrefix);
  text.append(pad(index));
  return ComponentId::from_validated(text);
}

namespace {

[[nodiscard]] bool build_common(const SyntheticFabricOptions& options, const Limits& limits,
                                SyntheticFabric& fabric, std::vector<std::uint32_t>& offsets) {
  std::uint64_t total = 0;
  for (const std::uint32_t size : options.group_sizes) {
    const auto sum = checked_add(total, size);
    if (!sum.has_value()) {
      return false;
    }
    total = *sum;
  }
  if (total == 0 || total > limits.max_components) {
    return false;
  }
  fabric.components.reserve(static_cast<std::size_t>(total));
  for (std::size_t index = 0; index < static_cast<std::size_t>(total); ++index) {
    fabric.components.push_back(synthetic_component_id(index));
  }
  offsets.clear();
  offsets.reserve(options.group_sizes.size() + 1);
  std::uint32_t running = 0;
  for (const std::uint32_t size : options.group_sizes) {
    offsets.push_back(running);
    running += size;
  }
  offsets.push_back(running);

  fabric.topology.id = TopologyId::from_validated("synthetic.topology");
  fabric.topology.generation = options.topology_generation;
  fabric.topology.provenance = Provenance::from_validated("synthetic");
  fabric.topology.components = fabric.components;

  fabric.evidence.id = options.evidence_id.is_nil()
                           ? EvidenceId::from_validated("synthetic.evidence")
                           : options.evidence_id;
  fabric.evidence.sequence = EvidenceSequence::from_value(1);
  fabric.evidence.publisher = options.publisher.is_nil()
                                  ? PublisherId::from_validated("synthetic.publisher")
                                  : options.publisher;
  fabric.evidence.publisher_boot = PublisherBootId::generate();
  fabric.evidence.topology_generation = options.topology_generation;
  fabric.evidence.generation = options.reachability_generation;
  fabric.evidence.observed_at_tick = options.observed_at_tick;
  fabric.evidence.validity_ticks = options.validity_ticks;
  fabric.evidence.completeness = options.complete_coverage ? EvidenceCompleteness::Complete
                                                           : EvidenceCompleteness::Partial;
  fabric.evidence.provenance = options.provenance.is_nil()
                                   ? Provenance::from_validated("synthetic")
                                   : options.provenance;
  return true;
}

void finalize(SyntheticFabric& fabric, const SyntheticFabricOptions& options) {
  std::sort(fabric.evidence.observations.begin(), fabric.evidence.observations.end(),
            [](const LinkObservation& lhs, const LinkObservation& rhs) {
              if (lhs.source != rhs.source) {
                return lhs.source < rhs.source;
              }
              if (lhs.target != rhs.target) {
                return lhs.target < rhs.target;
              }
              return static_cast<unsigned>(lhs.value) < static_cast<unsigned>(rhs.value);
            });
  fabric.evidence.observations.erase(
      std::unique(fabric.evidence.observations.begin(), fabric.evidence.observations.end(),
                  [](const LinkObservation& lhs, const LinkObservation& rhs) {
                    return lhs.source == rhs.source && lhs.target == rhs.target &&
                           lhs.value == rhs.value;
                  }),
      fabric.evidence.observations.end());
  if (options.complete_coverage) {
    fabric.evidence.covered_sources = fabric.components;
  }
  std::sort(fabric.evidence.covered_sources.begin(), fabric.evidence.covered_sources.end());
}

}  // namespace

std::optional<SyntheticFabric> build_synthetic_fabric(const SyntheticFabricOptions& options,
                                                      const Limits& limits) {
  SyntheticFabric fabric;
  std::vector<std::uint32_t> offsets;
  if (!build_common(options, limits, fabric, offsets)) {
    return std::nullopt;
  }
  DeterministicRandom random(options.seed);
  fabric.groups.reserve(options.group_sizes.size());

  for (std::size_t group = 0; group < options.group_sizes.size(); ++group) {
    const std::uint32_t begin = offsets[group];
    const std::uint32_t end = offsets[group + 1];
    std::vector<ComponentId> members;
    for (std::uint32_t index = begin; index < end; ++index) {
      members.push_back(fabric.components[index]);
    }
    fabric.groups.push_back(members);

    // A spanning tree guarantees exactly one component per group.
    for (std::uint32_t index = begin + 1; index < end; ++index) {
      const std::uint32_t parent = begin + random.next_below(index - begin);
      LinkObservation forward;
      forward.source = fabric.components[index];
      forward.target = fabric.components[parent];
      forward.value = Reachability::Reachable;
      LinkObservation reverse;
      reverse.source = fabric.components[parent];
      reverse.target = fabric.components[index];
      reverse.value = Reachability::Reachable;
      fabric.evidence.observations.push_back(forward);
      fabric.evidence.observations.push_back(reverse);
    }
    for (std::uint32_t extra = 0; extra < options.extra_intra_edges; ++extra) {
      const std::uint32_t span = end - begin;
      if (span < 2) {
        break;
      }
      const std::uint32_t lhs = begin + random.next_below(span);
      const std::uint32_t rhs = begin + random.next_below(span);
      if (lhs == rhs) {
        continue;
      }
      LinkObservation forward;
      forward.source = fabric.components[lhs];
      forward.target = fabric.components[rhs];
      forward.value = Reachability::Reachable;
      LinkObservation reverse;
      reverse.source = fabric.components[rhs];
      reverse.target = fabric.components[lhs];
      reverse.value = Reachability::Reachable;
      fabric.evidence.observations.push_back(forward);
      fabric.evidence.observations.push_back(reverse);
    }
  }

  if (fabric.evidence.observations.size() > limits.max_observations_per_evidence) {
    return std::nullopt;
  }
  finalize(fabric, options);
  return fabric;
}

std::optional<SyntheticFabric> build_synthetic_sparse_chain(
    const SyntheticFabricOptions& options, std::size_t chain_length, std::size_t isolated_count,
    const Limits& limits) {
  const auto chain = checked_narrow_u32(chain_length);
  const auto isolated = checked_narrow_u32(isolated_count);
  if (!chain.has_value() || !isolated.has_value()) {
    return std::nullopt;
  }
  SyntheticFabricOptions shaped = options;
  shaped.group_sizes.clear();
  if (chain_length != 0) {
    shaped.group_sizes.push_back(*chain);
  }
  for (std::size_t index = 0; index < isolated_count; ++index) {
    shaped.group_sizes.push_back(1);
  }
  // The builder gives every group a spanning tree, so a chain group of N
  // components carries exactly N-1 edges: the sparsest connected shape.
  shaped.extra_intra_edges = 0;
  return build_synthetic_fabric(shaped, limits);
}

}  // namespace fabric_partition_manager
