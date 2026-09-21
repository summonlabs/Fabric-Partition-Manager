// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// Deterministic synthetic fabric fixtures.
//
// Everything produced here is labelled SYNTHETIC. It exercises the runtime
// against a fabricated roster and fabricated reachability observations. It is
// not a measurement of any physical switch, NIC, RDMA transport or multi-node
// deployment, and no behaviour observed through it is claimed as physical
// hardware validation.
#ifndef FABRIC_PARTITION_MANAGER_SYNTHETIC_HPP
#define FABRIC_PARTITION_MANAGER_SYNTHETIC_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "fabric_partition_manager/evidence.hpp"
#include "fabric_partition_manager/limits.hpp"
#include "fabric_partition_manager/topology.hpp"

namespace fabric_partition_manager {

// splitmix64. Seeded, reproducible and independent of any standard library
// implementation detail, so a failing case can always be replayed exactly.
class DeterministicRandom {
 public:
  explicit DeterministicRandom(std::uint64_t seed) noexcept : state_(seed) {}

  [[nodiscard]] std::uint64_t next_u64() noexcept;
  // Uniform value in [0, bound). Returns 0 for a bound of 0 or 1.
  [[nodiscard]] std::uint32_t next_below(std::uint32_t bound) noexcept;
  [[nodiscard]] bool next_bool() noexcept { return (next_u64() & 1u) != 0u; }
  [[nodiscard]] std::uint64_t state() const noexcept { return state_; }

 private:
  std::uint64_t state_;
};

[[nodiscard]] ComponentId synthetic_component_id(std::size_t index);

struct SyntheticFabricOptions {
  std::string name = "synthetic";
  // The intended ground truth: the size of each partition the fixture should
  // generate, and the edges that connect it internally. The list is a spanning
  // tree per group, so a fixture with group sizes {4, 3} yields exactly two
  // components of those sizes.
  std::vector<std::uint32_t> group_sizes;
  // Additional intra-group edges beyond the spanning tree.
  std::uint32_t extra_intra_edges = 0;
  // When true the bundle declares complete coverage of every component it
  // observed from, which is what makes a decomposition CONFIRMED.
  bool complete_coverage = true;
  std::uint64_t seed = 1;
  TopologyGeneration topology_generation;
  ReachabilityGeneration reachability_generation;
  std::uint64_t observed_at_tick = 0;
  std::uint64_t validity_ticks = 1'000'000;
  PublisherId publisher;
  EvidenceId evidence_id;
  Provenance provenance;
};

struct SyntheticFabric {
  std::vector<ComponentId> components;
  TopologyDefinition topology;
  ReachabilityEvidence evidence;
  // Ground truth: the members of each intended group, in canonical order.
  std::vector<std::vector<ComponentId>> groups;

  [[nodiscard]] std::size_t component_count() const noexcept { return components.size(); }
  [[nodiscard]] std::size_t group_count() const noexcept { return groups.size(); }
};

// Builds a deterministic synthetic fabric. Returns nullopt when the requested
// shape exceeds the configured limits.
[[nodiscard]] std::optional<SyntheticFabric> build_synthetic_fabric(
    const SyntheticFabricOptions& options, const Limits& limits);

// Builds a connected chain of the first count components plus a fully isolated
// remainder, which is the shape used by the scale proofs.
[[nodiscard]] std::optional<SyntheticFabric> build_synthetic_sparse_chain(
    const SyntheticFabricOptions& options, std::size_t chain_length, std::size_t isolated_count,
    const Limits& limits);

}  // namespace fabric_partition_manager

#endif  // FABRIC_PARTITION_MANAGER_SYNTHETIC_HPP
