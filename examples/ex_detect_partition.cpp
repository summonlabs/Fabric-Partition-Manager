// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// ex_detect_partition: a synthetic fabric with two groups yields two
// partitions.
//
// The fixture builds a spanning tree inside each group and declares complete
// coverage, so every component-crossing ordered pair is proven unreachable and
// the decomposition is CONFIRMED.
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

  const fpm::SyntheticFabric fabric = fpm_example::setup_synthetic(runtime, {4, 3});
  const fpm::PartitionAssessment assessment = runtime.assess();

  require(fabric.group_count() == 2, "the fixture must describe two groups");
  require(assessment.partitions.size() == 2, "two groups must yield exactly two partitions");
  require(assessment.confirmed, "complete coverage must confirm the decomposition");
  require(assessment.reachability.component_count == 2,
          "the reachability snapshot must contain two components");
  require(assessment.reachability.undirected_reachable() == 5,
          "the two spanning trees carry three plus two proven edges");
  require(assessment.reachability.counters.ordered_reachable == 10,
          "five proven edges are ten proven ordered directions");
  require(assessment.reachability.counters.ordered_unreachable == 32,
          "every remaining ordered pair is proven unreachable under complete coverage");

  const fpm::Partition* const four = fpm_example::find_by_member_count(assessment.partitions, 4);
  const fpm::Partition* const three = fpm_example::find_by_member_count(assessment.partitions, 3);
  if (four == nullptr || three == nullptr) {
    fpm_example::fail("the expected partitions must be present in the assessment");
  }
  require(four->certainty == fpm::PartitionCertainty::Confirmed &&
              three->certainty == fpm::PartitionCertainty::Confirmed,
          "every partition of a confirmed decomposition is CONFIRMED");

  std::cout << "detected partitions=" << assessment.partitions.size()
            << " components=" << assessment.reachability.component_count
            << " confirmed=true reachable-edges="
            << assessment.reachability.undirected_reachable()
            << " ordered-pairs=" << assessment.reachability.counters.ordered_pairs << "\n";
  for (const fpm::Partition& partition : assessment.partitions) {
    std::cout << "partition members=" << partition.member_count()
              << " lineage=" << partition.lineage.view()
              << " certainty=" << fpm::partition_certainty_name(partition.certainty)
              << " lifecycle=" << fpm::partition_lifecycle_name(partition.lifecycle) << "\n";
  }
  std::cout << "authority verdict=" << fpm::decision_verdict_name(assessment.decision.verdict)
            << " class=" << fpm::partition_authority_class_name(assessment.fabric_authority)
            << " (detection is not authority; see ex_authority_is_not_reachability)\n";
}

}  // namespace

int main() { return fpm_example::run_example("ex_detect_partition", &body); }
