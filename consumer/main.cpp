// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// An independent consumer of the installed package.
//
// This program includes only the public umbrella header, links only
// SummonSoftwareLabs::FabricPartitionManager, builds a SYNTHETIC fabric,
// adopts its topology in a memory-only runtime, ingests its evidence and
// asserts the resulting assessment: exactly one partition, authoritative, with
// at least one authorized capability and a Granted verdict.
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "fabric_partition_manager/fabric_partition_manager.hpp"

namespace {

namespace fpm = fabric_partition_manager;

[[nodiscard]] int fail(const std::string& message) {
  std::cerr << "consumer failed: " << message << std::endl;
  return 1;
}

}  // namespace

int main() {
  const fpm::Limits limits = fpm::default_limits();

  fpm::SyntheticFabricOptions fixture;
  fixture.name = "consumer";
  fixture.group_sizes = {3};
  fixture.complete_coverage = true;
  fixture.seed = 11;
  fixture.topology_generation = fpm::TopologyGeneration::from_value(1);
  fixture.reachability_generation = fpm::ReachabilityGeneration::from_value(1);
  fixture.observed_at_tick = 0;
  fixture.validity_ticks = 1000;
  fixture.publisher = fpm::PublisherId::from_validated("consumer.publisher");
  fixture.evidence_id = fpm::EvidenceId::from_validated("consumer.evidence");
  fixture.provenance = fpm::Provenance::from_validated("synthetic");

  const std::optional<fpm::SyntheticFabric> fabric =
      fpm::build_synthetic_fabric(fixture, limits);
  if (!fabric.has_value()) {
    return fail("the synthetic fixture could not be built");
  }

  fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(1));
  policy.id = fpm::PolicyId::from_validated("consumer.policy");
  policy.full_authority = fpm::QuorumRequirement{};
  policy.full_authority.min_components = 3;
  policy.full_authority.min_voters = 0;
  policy.full_authority.min_weight = 0;
  policy.degraded_authority = fpm::QuorumRequirement{};
  policy.degraded_authority.min_components = 1;

  fpm::RuntimeOptions runtime_options;
  runtime_options.provenance = fpm::Provenance::from_validated("consumer");
  runtime_options.limits = limits;
  runtime_options.policy = policy;
  runtime_options.durable = false;
  runtime_options.store_directory.clear();

  try {
    fpm::PartitionRuntime runtime(runtime_options);

    const fpm::Decision adopted = runtime.adopt_topology(fabric->topology);
    if (adopted.verdict != fpm::DecisionVerdict::Granted) {
      return fail("the synthetic topology was not adopted: " + adopted.render());
    }
    const fpm::Decision ingested = runtime.ingest_evidence(fabric->evidence);
    if (ingested.verdict != fpm::DecisionVerdict::Granted) {
      return fail("the synthetic evidence was not accepted: " + ingested.render());
    }

    const fpm::PartitionAssessment assessment = runtime.assess();
    if (assessment.decision.verdict != fpm::DecisionVerdict::Granted) {
      return fail("the assessment is not a grant: " + assessment.decision.render());
    }
    if (!assessment.confirmed) {
      return fail("the assessment is not confirmed");
    }
    if (assessment.partitions.size() != 1) {
      return fail("the assessment does not contain exactly one partition");
    }
    const fpm::Partition& partition = assessment.partitions.front();
    if (!partition.authoritative) {
      return fail("the single partition is not the authoritative partition");
    }
    if (partition.authority.authorized.empty()) {
      return fail("the authoritative partition holds no authorized capability");
    }
    if (partition.authority.authority_class != fpm::PartitionAuthorityClass::Primary) {
      return fail("the authoritative partition is not primary");
    }

    std::cout << "consumer ok product=" << fpm::product_name
              << " version=" << fpm::version_string
              << " linked=" << fpm::linked_version() << " partitions=" << assessment.partitions.size()
              << " members=" << partition.member_count()
              << " class=" << fpm::partition_authority_class_name(partition.authority.authority_class)
              << " authorized=[" << partition.authority.authorized.render() << "]"
              << " authoritative=" << partition.id.view()
              << " verdict=" << fpm::decision_verdict_name(assessment.decision.verdict)
              << std::endl;
  } catch (const fpm::PartitionError& error) {
    return fail(error.render());
  } catch (const std::exception& error) {
    return fail(error.what());
  }
  return 0;
}
