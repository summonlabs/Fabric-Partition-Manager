// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// Scale evidence. Everything here is SYNTHETIC: a generated roster with
// generated reachability observations. It measures component maintenance and
// shows that retained state stays bounded; it is not a measurement of any
// physical fabric.
#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "fixture.hpp"

using namespace fpm_test;
namespace fpm = fabric_partition_manager;

namespace {

struct ScaleMeasurement {
  std::size_t components = 0;
  std::size_t edges = 0;
  double milliseconds = 0.0;
  std::size_t partitions = 0;
  std::uint64_t reachable = 0;
  std::uint64_t cross_indeterminate = 0;
  std::size_t evidence_bundles = 0;
  std::size_t lineage_records = 0;
  std::size_t decision_history = 0;
};

ScaleMeasurement measure_sparse(std::size_t chain, std::size_t isolated, std::uint64_t seed) {
  fpm::SyntheticFabricOptions options;
  options.name = "scale-sparse";
  options.seed = seed;
  options.topology_generation = topology_generation(1);
  options.reachability_generation = reachability_generation(1);
  options.validity_ticks = 1'000'000'000;
  const auto built = fpm::build_synthetic_sparse_chain(options, chain, isolated, test_limits());
  if (!built.has_value()) {
    fail(__FILE__, __LINE__, "build_synthetic_sparse_chain", "the fixture was refused");
  }
  fpm::RuntimeOptions runtime_options = memory_runtime_options(count_quorum_policy(1, 1'000'000));
  fpm::PartitionRuntime runtime(runtime_options);
  FPM_CHECK(runtime.adopt_topology(built->topology).verdict == fpm::DecisionVerdict::Granted);
  FPM_CHECK(runtime.ingest_evidence(built->evidence).verdict == fpm::DecisionVerdict::Granted);

  ScaleMeasurement measurement;
  measurement.components = built->component_count();
  measurement.edges = built->evidence.observations.size() / 2;
  const auto started = std::chrono::steady_clock::now();
  const fpm::PartitionAssessment assessment = runtime.assess();
  const auto finished = std::chrono::steady_clock::now();
  measurement.milliseconds =
      std::chrono::duration<double, std::milli>(finished - started).count();
  measurement.partitions = assessment.partitions.size();
  measurement.reachable = assessment.reachability.counters.undirected_reachable;
  measurement.cross_indeterminate =
      assessment.reachability.counters.cross_component_indeterminate;
  const fpm::RuntimeStatus status = runtime.status();
  measurement.evidence_bundles = status.evidence_bundles;
  measurement.lineage_records = status.lineage_records;
  measurement.decision_history = status.decision_history;
  runtime.close();
  return measurement;
}

ScaleMeasurement measure_dense(std::size_t components, std::uint32_t extra_edges,
                               std::uint64_t seed) {
  fpm::SyntheticFabricOptions options;
  options.name = "scale-dense";
  options.group_sizes = {static_cast<std::uint32_t>(components)};
  options.extra_intra_edges = extra_edges;
  options.seed = seed;
  options.topology_generation = topology_generation(1);
  options.reachability_generation = reachability_generation(1);
  options.validity_ticks = 1'000'000'000;
  const auto built = fpm::build_synthetic_fabric(options, test_limits());
  if (!built.has_value()) {
    fail(__FILE__, __LINE__, "build_synthetic_fabric", "the fixture was refused");
  }
  fpm::RuntimeOptions runtime_options = memory_runtime_options(count_quorum_policy(1, 1'000'000));
  fpm::PartitionRuntime runtime(runtime_options);
  FPM_CHECK(runtime.adopt_topology(built->topology).verdict == fpm::DecisionVerdict::Granted);
  FPM_CHECK(runtime.ingest_evidence(built->evidence).verdict == fpm::DecisionVerdict::Granted);

  ScaleMeasurement measurement;
  measurement.components = built->component_count();
  measurement.edges = built->evidence.observations.size() / 2;
  const auto started = std::chrono::steady_clock::now();
  const fpm::PartitionAssessment assessment = runtime.assess();
  const auto finished = std::chrono::steady_clock::now();
  measurement.milliseconds =
      std::chrono::duration<double, std::milli>(finished - started).count();
  measurement.partitions = assessment.partitions.size();
  measurement.reachable = assessment.reachability.counters.undirected_reachable;
  measurement.cross_indeterminate =
      assessment.reachability.counters.cross_component_indeterminate;
  const fpm::RuntimeStatus status = runtime.status();
  measurement.evidence_bundles = status.evidence_bundles;
  measurement.lineage_records = status.lineage_records;
  measurement.decision_history = status.decision_history;
  runtime.close();
  return measurement;
}

}  // namespace

FPM_TEST(scale, sparse_fabric_maintenance_grows_subquadratically) {
  const ScaleMeasurement small = measure_sparse(20'000, 5'000, 21);
  const ScaleMeasurement medium = measure_sparse(60'000, 15'000, 22);
  const ScaleMeasurement large = measure_sparse(180'000, 45'000, 23);

  FPM_EQ(small.components, std::size_t{25'000});
  FPM_EQ(medium.components, std::size_t{75'000});
  FPM_EQ(large.components, std::size_t{225'000});
  FPM_EQ(small.edges, std::size_t{19'999});
  FPM_EQ(medium.edges, std::size_t{59'999});
  FPM_EQ(large.edges, std::size_t{179'999});
  FPM_EQ(small.partitions, std::size_t{5'001});
  FPM_EQ(medium.partitions, std::size_t{15'001});
  FPM_EQ(large.partitions, std::size_t{45'001});
  FPM_EQ(small.reachable, 19'999u);
  FPM_EQ(large.reachable, 179'999u);

  // A threefold increase in size must not produce anything close to a ninefold
  // increase in time. The bound is generous because this runs in Debug builds
  // too; an accidental quadratic term would exceed it by orders of magnitude.
  const double first_ratio =
      medium.milliseconds / std::max(small.milliseconds, 0.001);
  const double second_ratio =
      large.milliseconds / std::max(medium.milliseconds, 0.001);
  FPM_CHECK_MSG(first_ratio < 8.0, std::to_string(first_ratio));
  FPM_CHECK_MSG(second_ratio < 8.0, std::to_string(second_ratio));

  // Retained state stays bounded and independent of the fabric size. The
  // structural bound is deterministic and catches a lineage store that loses
  // its lookup indexes long before a wall-clock ratio would.
  FPM_EQ(large.evidence_bundles, std::size_t{1});
  FPM_CHECK_MSG(large.lineage_records <= large.partitions * 2 + 8,
                std::to_string(large.lineage_records));
  FPM_CHECK(large.lineage_records <= test_limits().max_lineage_records);
  FPM_CHECK(large.decision_history <= test_limits().max_decision_history);
}

FPM_TEST(scale, dense_fabric_maintenance_grows_subquadratically) {
  const ScaleMeasurement small = measure_dense(1'000, 4'000, 31);
  const ScaleMeasurement medium = measure_dense(2'000, 8'000, 32);
  const ScaleMeasurement large = measure_dense(4'000, 16'000, 33);

  FPM_EQ(small.components, std::size_t{1'000});
  FPM_EQ(large.components, std::size_t{4'000});
  FPM_EQ(small.partitions, std::size_t{1});
  FPM_EQ(large.partitions, std::size_t{1});
  FPM_CHECK(large.edges >= 4'000u);

  const double first_ratio = medium.milliseconds / std::max(small.milliseconds, 0.001);
  const double second_ratio = large.milliseconds / std::max(medium.milliseconds, 0.001);
  FPM_CHECK_MSG(first_ratio < 8.0, std::to_string(first_ratio));
  FPM_CHECK_MSG(second_ratio < 8.0, std::to_string(second_ratio));
}

FPM_TEST(scale, a_large_fabric_remains_exact_under_repetition) {
  // Re-assessing a large fabric must not accumulate state and must produce the
  // same decomposition every time.
  fpm::SyntheticFabricOptions options;
  options.name = "scale-repeat";
  options.group_sizes = {5'000, 5'000, 5'000};
  options.seed = 41;
  options.topology_generation = topology_generation(1);
  options.reachability_generation = reachability_generation(1);
  options.validity_ticks = 1'000'000'000;
  const auto built = fpm::build_synthetic_fabric(options, test_limits());
  FPM_CHECK(built.has_value());
  fpm::RuntimeOptions runtime_options = memory_runtime_options(count_quorum_policy(1, 1'000'000));
  fpm::PartitionRuntime runtime(runtime_options);
  FPM_CHECK(runtime.adopt_topology(built->topology).verdict == fpm::DecisionVerdict::Granted);
  FPM_CHECK(runtime.ingest_evidence(built->evidence).verdict == fpm::DecisionVerdict::Granted);

  fpm::EvidenceSetDigest previous{};
  for (std::size_t round = 0; round < 5; ++round) {
    FPM_CHECK(runtime.advance_ticks(1));
    const fpm::PartitionAssessment assessment = runtime.assess();
    FPM_EQ(assessment.partitions.size(), std::size_t{3});
    FPM_EQ(assessment.reachability.counters.ordered_total_classified(),
           assessment.reachability.counters.ordered_pairs);
    if (round > 0) {
      FPM_EQ(assessment.reachability.evidence_digest, previous);
    }
    previous = assessment.reachability.evidence_digest;
    FPM_EQ(runtime.status().evidence_bundles, std::size_t{1});
  }
  FPM_CHECK(runtime.status().lineage_records <= test_limits().max_lineage_records);
  runtime.close();
}

FPM_TEST(scale, a_fabric_beyond_the_configured_component_bound_is_refused) {
  fpm::Limits limits = test_limits();
  limits.max_components = 1'000;
  fpm::SyntheticFabricOptions options;
  options.name = "over-bound";
  options.group_sizes = {2'000};
  options.seed = 51;
  options.topology_generation = topology_generation(1);
  options.reachability_generation = reachability_generation(1);
  options.validity_ticks = 1'000'000'000;
  FPM_CHECK(!fpm::build_synthetic_fabric(options, limits).has_value());

  options.group_sizes = {500, 500};
  const auto built = fpm::build_synthetic_fabric(options, limits);
  FPM_CHECK(built.has_value());
  fpm::RuntimeOptions runtime_options = memory_runtime_options(count_quorum_policy(1, 1'000'000));
  runtime_options.limits = limits;
  fpm::PartitionRuntime runtime(runtime_options);
  FPM_CHECK(runtime.adopt_topology(built->topology).verdict == fpm::DecisionVerdict::Granted);
  FPM_CHECK(runtime.ingest_evidence(built->evidence).verdict == fpm::DecisionVerdict::Granted);
  FPM_EQ(runtime.assess().partitions.size(), std::size_t{2});
  runtime.close();
}
