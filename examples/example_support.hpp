// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// Shared helpers for the examples.
//
// Everything the examples build is SYNTHETIC: a fabricated component roster and
// fabricated reachability observations produced by
// fabric_partition_manager::build_synthetic_fabric. Nothing here measures or
// represents a physical switch, NIC, RDMA transport or multi-node deployment.
//
// An example is a standalone program that prints what it demonstrates, asserts
// what it claims, and exits non-zero the moment an assertion fails.
#ifndef FABRIC_PARTITION_MANAGER_EXAMPLES_EXAMPLE_SUPPORT_HPP
#define FABRIC_PARTITION_MANAGER_EXAMPLES_EXAMPLE_SUPPORT_HPP

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "fabric_partition_manager/fabric_partition_manager.hpp"

namespace fpm_example {

namespace fpm = fabric_partition_manager;

// A failed assertion. Thrown rather than exiting so that every destructor,
// including the temporary workspace, still runs.
class Failure : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

[[noreturn]] inline void fail(const std::string& message) { throw Failure(message); }

inline void require(bool condition, const std::string& message) {
  if (!condition) {
    fail(message);
  }
}

// A private directory that is removed when the object is destroyed.
class TempWorkspace {
 public:
  explicit TempWorkspace(std::string_view label) {
    std::error_code code;
    const std::filesystem::path base = std::filesystem::temp_directory_path(code);
    require(!code, "the temporary directory could not be located");
    path_ = base / ("fabric-partition-manager-" + std::string(label));
    std::filesystem::remove_all(path_, code);
    code.clear();
    const bool created = std::filesystem::create_directories(path_, code);
    require(created && !code, "the temporary workspace could not be created");
  }

  ~TempWorkspace() {
    std::error_code code;
    std::filesystem::remove_all(path_, code);
  }

  TempWorkspace(const TempWorkspace&) = delete;
  TempWorkspace& operator=(const TempWorkspace&) = delete;
  TempWorkspace(TempWorkspace&&) = delete;
  TempWorkspace& operator=(TempWorkspace&&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

[[nodiscard]] inline fpm::SyntheticFabric build_fabric(
    const std::vector<std::uint32_t>& groups, const fpm::Limits& limits, std::uint64_t seed = 7,
    bool complete = true, std::string_view publisher = "synthetic.publisher",
    std::string_view evidence_id = "synthetic.evidence", std::uint64_t topology_generation = 1,
    std::uint64_t reachability_generation = 1, std::uint64_t validity_ticks = 1000) {
  fpm::SyntheticFabricOptions options;
  options.name = "example";
  options.group_sizes = groups;
  options.complete_coverage = complete;
  options.seed = seed;
  options.topology_generation = fpm::TopologyGeneration::from_value(topology_generation);
  options.reachability_generation = fpm::ReachabilityGeneration::from_value(reachability_generation);
  options.observed_at_tick = 0;
  options.validity_ticks = validity_ticks;
  options.publisher = fpm::PublisherId::from_validated(publisher);
  options.evidence_id = fpm::EvidenceId::from_validated(evidence_id);
  options.provenance = fpm::Provenance::from_validated("synthetic");
  const std::optional<fpm::SyntheticFabric> built = fpm::build_synthetic_fabric(options, limits);
  require(built.has_value(), "the synthetic fabric could not be built within the configured limits");
  return *built;
}

[[nodiscard]] inline fpm::Decision adopt(fpm::PartitionRuntime& runtime,
                                        const fpm::SyntheticFabric& fabric) {
  const fpm::Decision decision = runtime.adopt_topology(fabric.topology);
  require(decision.verdict == fpm::DecisionVerdict::Granted ||
              decision.verdict == fpm::DecisionVerdict::Observed,
          "the synthetic topology was not adopted: " + decision.render());
  return decision;
}

[[nodiscard]] inline fpm::Decision ingest(fpm::PartitionRuntime& runtime,
                                          const fpm::SyntheticFabric& fabric) {
  // The observation is stamped with the runtime's current logical tick, so it
  // is fresh at the moment it is published and ages out on its own afterwards.
  fpm::ReachabilityEvidence evidence = fabric.evidence;
  evidence.observed_at_tick = runtime.tick();
  const fpm::Decision decision = runtime.ingest_evidence(evidence);
  require(decision.verdict == fpm::DecisionVerdict::Granted,
          "the synthetic evidence was not accepted: " + decision.render());
  return decision;
}

// Advances the logical clock past the validity horizon of everything published
// so far. After this call the only usable evidence is what is published next,
// which is what a real operator observes when an old survey ages out.
inline void expire_published_evidence(fpm::PartitionRuntime& runtime,
                                      std::uint64_t validity_ticks = 1000) {
  require(runtime.advance_ticks(validity_ticks + 1),
          "the runtime clock could not be advanced past the evidence horizon");
}

// Builds a synthetic fabric, adopts its topology and ingests its evidence. This
// is the shape every example starts from.
[[nodiscard]] inline fpm::SyntheticFabric setup_synthetic(
    fpm::PartitionRuntime& runtime, const std::vector<std::uint32_t>& groups,
    std::uint64_t seed = 7, bool complete = true,
    std::string_view publisher = "synthetic.publisher",
    std::string_view evidence_id = "synthetic.evidence") {
  const fpm::SyntheticFabric fabric =
      build_fabric(groups, runtime.limits(), seed, complete, publisher, evidence_id);
  (void)adopt(runtime, fabric);
  (void)ingest(runtime, fabric);
  return fabric;
}

// A component-count quorum. No weight table is required: a component that is
// not listed carries zero weight and is not a voter, and this requirement
// constrains only the number of members.
[[nodiscard]] inline fpm::PartitionPolicy count_quorum_policy(std::uint64_t generation,
                                                              std::uint32_t min_components,
                                                              std::uint32_t min_voters = 0) {
  fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(generation));
  policy.id = fpm::PolicyId::from_validated("example.count-quorum");
  policy.full_authority = fpm::QuorumRequirement{};
  policy.full_authority.min_components = min_components;
  policy.full_authority.min_voters = min_voters;
  policy.full_authority.min_weight = 0;
  policy.degraded_authority = fpm::QuorumRequirement{};
  policy.degraded_authority.min_components = 1;
  policy.max_evidence_age_ticks = 1'000'000;
  return policy;
}

// A weighted quorum that requires a strict majority of every listed component,
// with every listed component a voter of weight one.
[[nodiscard]] inline fpm::PartitionPolicy strict_majority_policy(
    std::uint64_t generation, const std::vector<fpm::ComponentId>& members,
    std::uint32_t min_components, std::uint32_t min_voters) {
  fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(generation));
  policy.id = fpm::PolicyId::from_validated("example.strict-majority");
  policy.weights.clear();
  for (const fpm::ComponentId& member : members) {
    fpm::ComponentWeight entry;
    entry.component = member;
    entry.weight = 1;
    entry.voter = true;
    policy.weights.push_back(entry);
  }
  policy.full_authority = fpm::QuorumRequirement{};
  policy.full_authority.min_components = min_components;
  policy.full_authority.min_voters = min_voters;
  policy.full_authority.require_strict_majority = true;
  policy.degraded_authority = fpm::QuorumRequirement{};
  policy.degraded_authority.min_components = min_components;
  policy.degraded_authority.min_voters = min_voters;
  policy.degraded_authority.require_strict_majority = true;
  policy.max_evidence_age_ticks = 1'000'000;
  return policy;
}

[[nodiscard]] inline std::vector<fpm::ComponentId> components_of(
    const std::vector<std::vector<fpm::ComponentId>>& groups) {
  std::vector<fpm::ComponentId> all;
  for (const std::vector<fpm::ComponentId>& group : groups) {
    for (const fpm::ComponentId& member : group) {
      all.push_back(member);
    }
  }
  return all;
}

[[nodiscard]] inline fpm::RuntimeOptions memory_options(const fpm::PartitionPolicy& policy) {
  fpm::RuntimeOptions options;
  options.store_directory.clear();
  options.provenance = fpm::Provenance::from_validated("example");
  options.limits = fpm::default_limits();
  options.policy = policy;
  options.durable = false;
  return options;
}

[[nodiscard]] inline fpm::RuntimeOptions durable_options(const fpm::PartitionPolicy& policy,
                                                         const std::filesystem::path& store) {
  fpm::RuntimeOptions options = memory_options(policy);
  options.store_directory = store;
  options.durable = true;
  return options;
}

[[nodiscard]] inline const fpm::Partition* find_by_lineage(
    const std::vector<fpm::Partition>& partitions, const fpm::LineageId& lineage) {
  for (const fpm::Partition& partition : partitions) {
    if (partition.lineage == lineage) {
      return &partition;
    }
  }
  return nullptr;
}

[[nodiscard]] inline const fpm::Partition* find_by_member_count(
    const std::vector<fpm::Partition>& partitions, std::size_t member_count) {
  for (const fpm::Partition& partition : partitions) {
    if (partition.members.size() == member_count) {
      return &partition;
    }
  }
  return nullptr;
}

[[nodiscard]] inline fpm::AttemptToken attempt_token(std::uint64_t sequence) {
  fpm::AttemptToken token;
  token.id = fpm::AttemptId::from_validated("example-attempt-" + std::to_string(sequence));
  token.sequence = fpm::AttemptSequence::from_value(sequence);
  return token;
}

[[nodiscard]] inline std::string render_verdict(fpm::DecisionVerdict verdict) {
  return std::string(fpm::decision_verdict_name(verdict));
}

[[nodiscard]] inline std::string render_class(fpm::PartitionAuthorityClass value) {
  return std::string(fpm::partition_authority_class_name(value));
}

inline void print_reasons(const std::vector<fpm::Reason>& reasons) {
  for (const fpm::Reason& reason : reasons) {
    std::cout << "  reason " << fpm::reason_code_name(reason.code) << " subject=" << reason.subject
              << " detail=" << reason.detail << "\n";
  }
}

[[nodiscard]] inline std::string describe_rendering(const fpm::AuthorityVector& authority) {
  return authority.render();
}

// Runs an example body with uniform failure reporting. The name is printed on
// entry and on success, so a partial run is always distinguishable.
inline int run_example(const char* name, void (*body)()) {
  std::cout << "example " << name << ": begin" << std::endl;
  try {
    body();
  } catch (const Failure& failure) {
    std::cerr << "FAIL " << name << ": " << failure.what() << std::endl;
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << name << ": unexpected exception: " << error.what() << std::endl;
    return 1;
  }
  std::cout << "example " << name << ": OK" << std::endl;
  return 0;
}

}  // namespace fpm_example

#endif  // FABRIC_PARTITION_MANAGER_EXAMPLES_EXAMPLE_SUPPORT_HPP
