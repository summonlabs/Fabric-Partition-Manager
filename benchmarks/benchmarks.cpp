// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// fpm_benchmarks: completed-work measurements for a full
// PartitionRuntime::assess over SYNTHETIC fabrics.
//
// Every number below is the wall time of an assess() call that has already
// returned with a verified result: a submission timestamp would measure nothing
// here, because assess() is synchronous and its result is checked before the
// elapsed time is reported. A measurement that does not produce the expected
// decomposition fails the run rather than being reported as a number.
//
// Shapes
//   sparse  a chain of components-1 plus one isolated component: the sparsest
//           connected shape, about one edge per component
//   dense   a single component set with a spanning tree plus seven extra
//           intra-component edges per component, about eight edges per component
//
// The fixture seed is fixed, so two runs of this program measure the same
// graphs. The budget is a benchmark budget, not a test timeout: a size that
// exceeds it is reported and makes the program exit non-zero, so an accidental
// quadratic shows up as a failure instead of a hang.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "fabric_partition_manager/fabric_partition_manager.hpp"

namespace {

namespace fpm = fabric_partition_manager;

constexpr std::uint64_t kSeed = 20'240'517;
constexpr double kBudgetMillis = 120'000.0;

struct Measurement {
  bool ok = false;
  std::string failure;
  std::uint64_t components = 0;
  std::uint64_t edges = 0;
  std::uint64_t partitions = 0;
  double millis = 0.0;
  fpm::PairCounters counters;
  fpm::RuntimeStatus status;
};

[[nodiscard]] fpm::RuntimeOptions benchmark_options() {
  fpm::RuntimeOptions options;
  options.store_directory.clear();
  options.provenance = fpm::Provenance::from_validated("benchmark");
  options.limits = fpm::default_limits();
  options.policy = fpm::default_policy(fpm::PolicyGeneration::from_value(1));
  options.durable = false;
  return options;
}

// Builds the fabric, adopts it, ingests its evidence and measures one complete
// assess(). The fabric is released before the measurement is returned.
[[nodiscard]] Measurement measure_sparse(std::uint64_t components) {
  Measurement measurement;
  measurement.components = components;
  const fpm::Limits limits = fpm::default_limits();

  fpm::SyntheticFabricOptions options;
  options.name = "benchmark-sparse";
  options.seed = kSeed;
  options.complete_coverage = true;
  options.topology_generation = fpm::TopologyGeneration::from_value(1);
  options.reachability_generation = fpm::ReachabilityGeneration::from_value(1);
  options.observed_at_tick = 0;
  options.validity_ticks = 1'000'000;
  options.publisher = fpm::PublisherId::from_validated("benchmark.publisher");
  options.evidence_id = fpm::EvidenceId::from_validated("benchmark.sparse.evidence");
  options.provenance = fpm::Provenance::from_validated("synthetic");

  const std::optional<fpm::SyntheticFabric> fabric =
      fpm::build_synthetic_sparse_chain(options, static_cast<std::size_t>(components - 1), 1, limits);
  if (!fabric.has_value()) {
    measurement.failure = "the sparse fixture could not be built";
    return measurement;
  }

  fpm::PartitionRuntime runtime(benchmark_options());
  const fpm::Decision adopted = runtime.adopt_topology(fabric->topology);
  if (adopted.verdict != fpm::DecisionVerdict::Granted) {
    measurement.failure = "the sparse roster was not adopted";
    return measurement;
  }
  const fpm::Decision ingested = runtime.ingest_evidence(fabric->evidence);
  if (ingested.verdict != fpm::DecisionVerdict::Granted) {
    measurement.failure = "the sparse evidence was not accepted";
    return measurement;
  }

  const auto started = std::chrono::steady_clock::now();
  const fpm::PartitionAssessment assessment = runtime.assess();
  const auto finished = std::chrono::steady_clock::now();
  measurement.millis =
      std::chrono::duration<double, std::milli>(finished - started).count();

  measurement.counters = assessment.reachability.counters;
  measurement.edges = assessment.reachability.undirected_reachable();
  measurement.partitions = assessment.partitions.size();
  measurement.status = runtime.status();

  // Completed work is verified, not assumed: the chain plus the isolated
  // component must be exactly two component sets carrying every member.
  if (assessment.reachability.component_count != 2 || assessment.partitions.size() != 2) {
    measurement.failure = "the sparse decomposition is not the chain plus one isolated component";
    return measurement;
  }
  if (measurement.edges != components - 2) {
    measurement.failure = "the sparse chain does not carry the expected proven edges";
    return measurement;
  }
  std::uint64_t members = 0;
  for (const fpm::Partition& partition : assessment.partitions) {
    members += partition.member_count();
  }
  if (members != components) {
    measurement.failure = "the sparse decomposition does not account for every component";
    return measurement;
  }
  measurement.ok = true;
  return measurement;
}

[[nodiscard]] Measurement measure_dense(std::uint64_t components) {
  Measurement measurement;
  measurement.components = components;
  const fpm::Limits limits = fpm::default_limits();

  fpm::SyntheticFabricOptions options;
  options.name = "benchmark-dense";
  options.seed = kSeed;
  options.complete_coverage = true;
  options.topology_generation = fpm::TopologyGeneration::from_value(1);
  options.reachability_generation = fpm::ReachabilityGeneration::from_value(1);
  options.observed_at_tick = 0;
  options.validity_ticks = 1'000'000;
  options.publisher = fpm::PublisherId::from_validated("benchmark.publisher");
  options.evidence_id = fpm::EvidenceId::from_validated("benchmark.dense.evidence");
  options.provenance = fpm::Provenance::from_validated("synthetic");
  options.group_sizes = {static_cast<std::uint32_t>(components)};
  options.extra_intra_edges = static_cast<std::uint32_t>(components * 7);

  const std::optional<fpm::SyntheticFabric> fabric = fpm::build_synthetic_fabric(options, limits);
  if (!fabric.has_value()) {
    measurement.failure = "the dense fixture could not be built";
    return measurement;
  }

  fpm::PartitionRuntime runtime(benchmark_options());
  const fpm::Decision adopted = runtime.adopt_topology(fabric->topology);
  if (adopted.verdict != fpm::DecisionVerdict::Granted) {
    measurement.failure = "the dense roster was not adopted";
    return measurement;
  }
  const fpm::Decision ingested = runtime.ingest_evidence(fabric->evidence);
  if (ingested.verdict != fpm::DecisionVerdict::Granted) {
    measurement.failure = "the dense evidence was not accepted";
    return measurement;
  }

  const auto started = std::chrono::steady_clock::now();
  const fpm::PartitionAssessment assessment = runtime.assess();
  const auto finished = std::chrono::steady_clock::now();
  measurement.millis =
      std::chrono::duration<double, std::milli>(finished - started).count();

  measurement.counters = assessment.reachability.counters;
  measurement.edges = assessment.reachability.undirected_reachable();
  measurement.partitions = assessment.partitions.size();
  measurement.status = runtime.status();

  if (assessment.reachability.component_count != 1 || assessment.partitions.size() != 1) {
    measurement.failure = "the dense fabric is not one component set";
    return measurement;
  }
  if (assessment.partitions.front().member_count() != components) {
    measurement.failure = "the dense component set does not carry every component";
    return measurement;
  }
  const std::uint64_t planned = components - 1 + components * 7;
  if (measurement.edges < components - 1 || measurement.edges > planned) {
    measurement.failure = "the dense edge count is outside the planned range";
    return measurement;
  }
  measurement.ok = true;
  return measurement;
}

void print_header() {
  std::cout << std::left << std::setw(8) << "kind" << std::right << std::setw(12) << "components"
            << std::setw(12) << "edges" << std::setw(12) << "assess-ms" << std::setw(12)
            << "time-ratio" << std::setw(16) << "ordered-pairs" << std::setw(18)
            << "ordered-reachable" << std::setw(20) << "ordered-unreachable" << std::setw(16)
            << "ordered-unknown" << std::setw(20) << "ordered-conflicting" << "\n";
}

void print_row(const char* kind, const Measurement& measurement, double ratio) {
  std::cout << std::left << std::setw(8) << kind << std::right << std::setw(12)
            << measurement.components << std::setw(12) << measurement.edges << std::setw(12)
            << std::fixed << std::setprecision(3) << measurement.millis << std::setw(12)
            << std::fixed << std::setprecision(3) << ratio << std::setw(16)
            << measurement.counters.ordered_pairs << std::setw(18)
            << measurement.counters.ordered_reachable << std::setw(20)
            << measurement.counters.ordered_unreachable << std::setw(16)
            << measurement.counters.ordered_unknown << std::setw(20)
            << measurement.counters.ordered_conflicting << "\n";
}

void print_retained_header() {
  std::cout << std::left << std::setw(8) << "kind" << std::right << std::setw(12) << "components"
            << std::setw(18) << "evidence-bundles" << std::setw(18) << "lineage-records"
            << std::setw(16) << "fence-records" << std::setw(18) << "decision-history"
            << std::setw(20) << "decisions-dropped" << std::setw(12) << "partitions"
            << std::setw(10) << "bounded" << "\n";
}

[[nodiscard]] bool report_retained(const char* kind, const Measurement& measurement) {
  const fpm::Limits limits = fpm::default_limits();
  const bool bounded = measurement.status.evidence_bundles <= limits.max_retained_evidence &&
                       measurement.status.lineage_records <= limits.max_lineage_records &&
                       measurement.status.fence_records <= limits.max_fence_records &&
                       measurement.status.decision_history <= limits.max_decision_history &&
                       measurement.status.decisions_dropped == 0;
  std::cout << std::left << std::setw(8) << kind << std::right << std::setw(12)
            << measurement.components << std::setw(18) << measurement.status.evidence_bundles
            << std::setw(18) << measurement.status.lineage_records << std::setw(16)
            << measurement.status.fence_records << std::setw(18)
            << measurement.status.decision_history << std::setw(20)
            << measurement.status.decisions_dropped << std::setw(12) << measurement.partitions
            << std::setw(10) << (bounded ? "true" : "false") << "\n";
  return bounded;
}

struct CaseResult {
  std::string kind;
  Measurement measurement;
};

// Runs one family of sizes, printing a row per size and the ratio to the
// previous size in the same family. Each size is measured exactly once; the
// retained-state table reuses the same measurements.
[[nodiscard]] std::vector<CaseResult> run_family(const char* kind,
                                                 const std::vector<std::uint64_t>& sizes,
                                                 bool sparse, bool& ok) {
  std::vector<CaseResult> results;
  double previous_millis = 0.0;
  for (const std::uint64_t size : sizes) {
    CaseResult result;
    result.kind = kind;
    result.measurement = sparse ? measure_sparse(size) : measure_dense(size);
    const double ratio =
        previous_millis > 0.0 ? result.measurement.millis / previous_millis : 0.0;
    print_row(kind, result.measurement, ratio);
    if (!result.measurement.ok) {
      std::cerr << "FAIL " << kind << " components=" << size << ": "
                << result.measurement.failure << std::endl;
      ok = false;
    }
    if (result.measurement.millis > kBudgetMillis) {
      std::cerr << "FAIL " << kind << " components=" << size << ": assess took "
                << result.measurement.millis << " ms which exceeds the " << kBudgetMillis
                << " ms budget" << std::endl;
      ok = false;
    }
    previous_millis = result.measurement.millis;
    results.push_back(std::move(result));
  }
  return results;
}

}  // namespace

int main() {
  std::cout << "fabric partition manager benchmark: SYNTHETIC fabrics seed=" << kSeed
            << " budget-ms=" << static_cast<std::uint64_t>(kBudgetMillis) << "\n";
  std::cout << "sparse: a chain of components-1 plus one isolated component; dense: one component "
               "set with about eight edges per component\n";
  print_header();

  bool ok = true;
  const std::vector<CaseResult> sparse = run_family("sparse", {20'000, 60'000, 180'000}, true, ok);
  const std::vector<CaseResult> dense = run_family("dense", {500, 1'000, 2'000}, false, ok);

  std::cout << "retained state after each assessment (every value must stay inside its "
               "configured bound)\n";
  print_retained_header();
  bool retained_ok = true;
  for (const CaseResult& result : sparse) {
    retained_ok = report_retained(result.kind.c_str(), result.measurement) && retained_ok;
  }
  for (const CaseResult& result : dense) {
    retained_ok = report_retained(result.kind.c_str(), result.measurement) && retained_ok;
  }
  if (!retained_ok) {
    std::cerr << "FAIL retained state exceeded a configured bound" << std::endl;
    ok = false;
  }

  std::cout << "benchmark result=" << (ok ? "PASS" : "FAIL")
            << " budget-ms=" << static_cast<std::uint64_t>(kBudgetMillis) << "\n";
  return ok ? 0 : 1;
}
