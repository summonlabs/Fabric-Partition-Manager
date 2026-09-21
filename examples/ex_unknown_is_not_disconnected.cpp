// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// ex_unknown_is_not_disconnected: partial evidence leaves the decomposition
// PARTIAL, and UNKNOWN is never reported as disconnected.
//
// The fixture is the same fabric with the same spanning trees, but the bundle
// declares PARTIAL coverage. Nothing is asserted unreachable, so:
//   * no component-crossing pair is proven unreachable,
//   * the decomposition is PARTIAL rather than CONFIRMED,
//   * write authority is withheld, and the write capabilities are reported as
//     denied rather than silently granted.
#include <cstddef>
#include <iostream>
#include <vector>

#include "example_support.hpp"

namespace {

namespace fpm = fabric_partition_manager;
using fpm_example::require;

void body() {
  const fpm::RuntimeOptions options = fpm_example::memory_options(
      fpm::default_policy(fpm::PolicyGeneration::from_value(1)));
  fpm::PartitionRuntime runtime(options);

  const fpm::SyntheticFabric fabric = fpm_example::setup_synthetic(runtime, {4, 3}, 7, false);
  require(fabric.evidence.completeness == fpm::EvidenceCompleteness::Partial,
          "the fixture must carry a partial bundle");
  const fpm::PartitionAssessment assessment = runtime.assess();

  require(assessment.partitions.size() == 2,
          "the best supported decomposition still separates the two groups");
  require(!assessment.confirmed, "partial evidence must never confirm a decomposition");
  require(assessment.reachability.counters.ordered_unknown == 32,
          "every unobserved ordered pair is UNKNOWN");
  require(assessment.reachability.counters.ordered_unreachable == 0,
          "UNKNOWN must never be counted as unreachable");
  require(assessment.reachability.counters.cross_component_unreachable == 0,
          "no component-crossing pair is proven unreachable");
  require(assessment.reachability.counters.cross_component_indeterminate == 24,
          "all twenty-four component-crossing ordered pairs remain indeterminate");
  require(assessment.reachability.counters.cross_component_ordered == 24,
          "two groups of four and three have twenty-four crossing ordered pairs");

  for (const fpm::Partition& partition : assessment.partitions) {
    require(partition.certainty == fpm::PartitionCertainty::Partial,
            "an unconfirmed decomposition is PARTIAL");
    require(partition.authority.authority_class == fpm::PartitionAuthorityClass::ReadOnly ||
                partition.authority.authority_class == fpm::PartitionAuthorityClass::ObserveOnly,
            "partial evidence must degrade authority to read-only or observe-only");
    require(!partition.authority.authorized.contains(fpm::AuthorityCapability::ServeWrites),
            "no write capability may be authorized on an unconfirmed decomposition");
    require(!partition.authority.authorized.contains(fpm::AuthorityCapability::AdmitMutations),
            "no mutation capability may be authorized on an unconfirmed decomposition");
    require(partition.authority.denied.contains(fpm::AuthorityCapability::ServeWrites),
            "the withheld write capability must be reported as denied");
  }

  std::cout << "partial coverage: confirmed=false ordered-unknown="
            << assessment.reachability.counters.ordered_unknown
            << " ordered-unreachable=" << assessment.reachability.counters.ordered_unreachable
            << " cross-component-indeterminate="
            << assessment.reachability.counters.cross_component_indeterminate << "\n";
  for (const fpm::Partition& partition : assessment.partitions) {
    std::cout << "partition members=" << partition.member_count()
              << " certainty=" << fpm::partition_certainty_name(partition.certainty)
              << " authority=" << fpm::partition_authority_class_name(
                                       partition.authority.authority_class)
              << " authorized=[" << partition.authority.authorized.render() << "]"
              << " denied=[" << partition.authority.denied.render() << "]" << "\n";
  }
}

}  // namespace

int main() { return fpm_example::run_example("ex_unknown_is_not_disconnected", &body); }
