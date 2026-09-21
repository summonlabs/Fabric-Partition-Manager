// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// ex_governed_merge: split, then reconnect. The reconnected component set is
// refused authority until request_merge commits the reconciliation, after which
// it is granted.
//
// The fabric is split into two halves and each half becomes durable lineage.
// When the halves reconnect, the new component set spans two live lineages.
// That is a merge candidate, not a partition: it holds no authority above
// observe-only until a governed merge decides the reconciliation. Afterwards
// the merged lineage owns the membership and the authority is granted.
//
// Reachability alone never unions two lineage histories.
#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "example_support.hpp"

namespace {

namespace fpm = fabric_partition_manager;
using fpm_example::require;

void body() {
  const fpm::PartitionPolicy policy = fpm_example::count_quorum_policy(1, 6);
  fpm::PartitionRuntime runtime(fpm_example::memory_options(policy));

  // Phase 1: two halves. Neither holds the six component quorum.
  const fpm::SyntheticFabric split = fpm_example::setup_synthetic(runtime, {3, 3});
  const fpm::PartitionAssessment separated = runtime.assess();
  require(separated.partitions.size() == 2, "the split fabric must yield two partitions");
  require(separated.confirmed, "the split must be confirmed by complete coverage");
  for (const fpm::Partition& partition : separated.partitions) {
    require(partition.authority.authority_class != fpm::PartitionAuthorityClass::Primary,
            "a half of the fabric must not hold primary authority");
  }
  std::cout << "split partitions=2 verdict="
            << fpm::decision_verdict_name(separated.decision.verdict) << "\n";
  for (const fpm::Partition& partition : separated.partitions) {
    std::cout << "  lineage=" << partition.lineage.view()
              << " members=" << partition.member_count()
              << " class=" << fpm::partition_authority_class_name(
                                     partition.authority.authority_class)
              << "\n";
  }

  // Phase 2: the fabric reconnects into one component set that spans both live
  // lineages. The earlier complete claim is allowed to age out first, so the
  // reconnecting observation is the only fresh evidence in the ledger. Without
  // that, two contradictory complete claims would both be fresh and the runtime
  // would refuse to choose between them, which is the fail-closed behaviour the
  // reachability suite pins down.
  // The survey that established the split ages out first, so the reconnecting
// observation is the only fresh evidence in the ledger. Two contradictory
// complete claims would otherwise both be fresh, and the runtime would refuse
// to pick one of them for the operator.
fpm_example::expire_published_evidence(runtime);
const fpm::SyntheticFabric joined = fpm_example::build_fabric(
      {6}, runtime.limits(), 11, false, "merge.publisher", "merge.evidence");
  (void)fpm_example::ingest(runtime, joined);
  const fpm::PartitionAssessment reconnected = runtime.assess();
  require(reconnected.partitions.size() == 1, "the reconnected fabric is one component set");
  const fpm::Partition& subject = reconnected.partitions.front();
  require(subject.member_count() == 6, "the reconnected component set carries six members");
  require(subject.pending_merge_parents.size() == 2,
          "the reconnected component set spans two live lineages");
  require(subject.authority.authority_class == fpm::PartitionAuthorityClass::ObserveOnly,
          "the reconnected component set is refused authority until the merge is decided");
  require(!subject.authoritative, "an undecided merge candidate is not authoritative");
  require(reconnected.decision.verdict != fpm::DecisionVerdict::Granted,
          "an undecided merge candidate is not a grant");
  std::cout << "reconnected partitions=1 members=6 pending-merge-parents=2 class="
            << fpm::partition_authority_class_name(subject.authority.authority_class)
            << " verdict=" << fpm::decision_verdict_name(reconnected.decision.verdict) << "\n";

  // Phase 3: the governed merge.
  fpm::MergeRequest request;
  request.attempt = fpm_example::attempt_token(1);
  request.expected_epoch = runtime.epoch();
  request.subject = subject.id;
  request.left = subject.pending_merge_parents[0];
  request.right = subject.pending_merge_parents[1];
  request.requester = fpm::Provenance::from_validated("example");
  const fpm::MergeOutcome outcome = runtime.request_merge(request);
  require(outcome.verdict == fpm::DecisionVerdict::Granted,
          "the governed merge must be accepted: " + outcome.decision.render());
  require(!outcome.merged_lineage.is_nil(), "a committed merge establishes a merged lineage");
  require(!outcome.merged_partition.is_nil(), "a committed merge names the merged partition");

  const fpm::PartitionAssessment reconciled = runtime.last_assessment();
  require(reconciled.partitions.size() == 1, "the merged fabric is one partition");
  const fpm::Partition& merged_partition = reconciled.partitions.front();
  require(merged_partition.lineage == outcome.merged_lineage,
          "the merged lineage owns the reconciled membership");
  require(merged_partition.pending_merge_parents.empty(),
          "a committed merge leaves no pending parent");
  require(merged_partition.authority.authority_class == fpm::PartitionAuthorityClass::Primary,
          "the reconciled component set must be granted authority");
  require(!merged_partition.authority.authorized.empty(),
          "the reconciled component set holds capabilities");
  require(merged_partition.authoritative, "the reconciled component set is authoritative");
  require(reconciled.decision.verdict == fpm::DecisionVerdict::Granted,
          "the assessment after the merge is a grant");

  std::cout << "merged subject=" << subject.id.view()
            << " merged-lineage=" << outcome.merged_lineage.view()
            << " merged-partition=" << outcome.merged_partition.view()
            << " class=" << fpm::partition_authority_class_name(
                                   merged_partition.authority.authority_class)
            << " authorized=[" << merged_partition.authority.authorized.render() << "]"
            << " verdict=" << fpm::decision_verdict_name(reconciled.decision.verdict) << "\n";
}

}  // namespace

int main() { return fpm_example::run_example("ex_governed_merge", &body); }
