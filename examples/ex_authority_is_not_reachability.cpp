// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// ex_authority_is_not_reachability: an internally connected, fully confirmed
// component set still receives no authority when the quorum is not met.
//
// Each half of the fabric is proven internally connected and every crossing
// pair is proven unreachable, so the decomposition is CONFIRMED. The policy,
// however, is a strict majority of the whole authority domain: a half holds
// exactly half of the weight, which is not a majority. Reachability proved a
// component set exists; it did not, and cannot, authorize it.
#include <cstddef>
#include <iostream>
#include <vector>

#include "example_support.hpp"

namespace {

namespace fpm = fabric_partition_manager;
using fpm_example::require;

void body() {
  const fpm::Limits limits = fpm::default_limits();
  const fpm::SyntheticFabric fabric = fpm_example::build_fabric({3, 3}, limits);
  const std::vector<fpm::ComponentId> domain = fpm_example::components_of(fabric.groups);
  require(domain.size() == 6, "the fixture must contain six components");

  const fpm::PartitionPolicy policy =
      fpm_example::strict_majority_policy(1, domain, 3, 2);
  fpm::PartitionRuntime runtime(fpm_example::memory_options(policy));
  (void)fpm_example::adopt(runtime, fabric);
  (void)fpm_example::ingest(runtime, fabric);

  const fpm::PartitionAssessment assessment = runtime.assess();
  require(assessment.confirmed, "the decomposition must be confirmed");
  require(assessment.partitions.size() == 2, "the fabric must decompose into two partitions");
  require(assessment.decision.verdict == fpm::DecisionVerdict::Isolated,
          "no partition may be authorized when the quorum is not met");
  require(assessment.fabric_authority != fpm::PartitionAuthorityClass::Primary,
          "the fabric must not report a primary authority");

  for (const fpm::Partition& partition : assessment.partitions) {
    require(partition.member_count() == 3, "each partition must carry three components");
    require(partition.certainty == fpm::PartitionCertainty::Confirmed,
            "each partition is a confirmed component set");
    require(partition.voter_count == 3, "each listed component is a voter");
    require(partition.total_weight == 3, "each partition holds three of six weight units");
    require(partition.authority.authority_class == fpm::PartitionAuthorityClass::Isolated,
            "a partition below the quorum is ISOLATED");
    require(partition.authority.basis == fpm::AuthorityBasis::QuorumNotSatisfied,
            "the basis must name the unsatisfied quorum");
    require(partition.authority.authorized.empty(),
            "no capability may be authorized without a satisfied quorum");
    require(partition.authority.observed.count() == fpm::authority_capability_count,
            "the observation stage still records what the component set could exercise");
    require(partition.authority.denied.count() == fpm::authority_capability_count,
            "everything physically possible is denied while the quorum is unmet");
  }

  std::cout << "connected=true confirmed=true quorum=strict-majority-of-"
            << policy.full_authority.domain_weight << " retained-weight=3"
            << " verdict=" << fpm::decision_verdict_name(assessment.decision.verdict) << "\n";
  for (const fpm::Partition& partition : assessment.partitions) {
    std::cout << "partition members=" << partition.member_count()
              << " voters=" << partition.voter_count << " weight=" << partition.total_weight
              << " class=" << fpm::partition_authority_class_name(
                                     partition.authority.authority_class)
              << " basis=" << fpm::authority_basis_name(partition.authority.basis)
              << " observed=[" << partition.authority.observed.render() << "]"
              << " authorized=[" << partition.authority.authorized.render() << "]" << "\n";
  }
}

}  // namespace

int main() { return fpm_example::run_example("ex_authority_is_not_reachability", &body); }
