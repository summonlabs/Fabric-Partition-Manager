// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// ex_persistence_recovery: write a durable store, close it, reopen it, and show
// that lineage and fences survive while reachability evidence does not.
//
// Durable state is policy, the adopted topology, the roster, committed lineage,
// completed decisions, fences and the epoch floor. It is never reachability,
// freshness or live authority: those are re-established by evidence after every
// restart, or explicitly reported as interrupted.
#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "example_support.hpp"

namespace {

namespace fpm = fabric_partition_manager;
using fpm_example::require;

void body() {
  fpm_example::TempWorkspace workspace("ex-persistence-recovery");
  const fpm::PartitionPolicy policy = fpm_example::count_quorum_policy(1, 3);
  std::size_t lineage_before = 0;
  std::size_t fences_before = 0;

  {
    fpm::PartitionRuntime runtime(fpm_example::durable_options(policy, workspace.path()));
    require(runtime.status().durable, "the runtime must be durable");
    (void)fpm_example::setup_synthetic(runtime, {3});
    const fpm::PartitionAssessment assessment = runtime.assess();
    require(assessment.decision.verdict == fpm::DecisionVerdict::Granted,
            "the durable runtime must grant authority");
    lineage_before = runtime.lineage_records().size();
    require(lineage_before >= 1, "an assessment commits durable lineage");

    // A policy change is an authority-bearing dependency change: every live
    // grant is fenced before the new policy takes effect.
    const fpm::PartitionPolicy next = fpm_example::count_quorum_policy(2, 3);
    const fpm::Decision policy_decision = runtime.set_policy(next);
    require(policy_decision.verdict == fpm::DecisionVerdict::Granted,
            "the policy change must be accepted: " + policy_decision.render());
    fences_before = runtime.fences().size();
    require(fences_before >= 1, "changing the policy fences the live authority");
    require(!runtime.interrupted_authorities().empty(),
            "the fenced authority is reported interrupted");
    runtime.close();
    require(runtime.closed(), "close must release the durable store");
  }

  fpm::PartitionRuntime reopened(fpm_example::durable_options(policy, workspace.path()));
  require(reopened.lineage_records().size() == lineage_before,
          "committed lineage must survive a restart");
  require(reopened.fences().size() >= fences_before, "fence records must survive a restart");
  require(reopened.policy().generation.value() == 2,
          "the durable policy definition must survive a restart");
  require(reopened.adopted_topology().has_value(),
          "the adopted topology definition must survive a restart");
  require(reopened.roster().is_set(), "the authoritative roster must survive a restart");
  require(reopened.status().durable, "the reopened runtime must be durable");

  require(reopened.status().evidence_bundles == 0,
          "reachability evidence must not survive a restart");
  require(reopened.reachability_summary().fresh_bundle_count == 0,
          "no evidence is fresh after a restart");
  require(reopened.current_partitions().empty(),
          "no partition holds authority immediately after a restart");
  require(!reopened.interrupted_authorities().empty(),
          "pre-restart authority is reported interrupted, never restored");

  std::cout << "durable state: lineage=" << reopened.lineage_records().size()
            << " fences=" << reopened.fences().size()
            << " policy-generation=" << reopened.policy().generation.value()
            << " topology=" << (reopened.adopted_topology().has_value() ? "restored" : "absent")
            << " epoch=" << reopened.epoch().value() << "\n";
  std::cout << "not durable: evidence=" << reopened.status().evidence_bundles
            << " fresh-bundles=" << reopened.reachability_summary().fresh_bundle_count
            << " live-partitions=" << reopened.current_partitions().size()
            << " interrupted=" << reopened.interrupted_authorities().size() << "\n";
}

}  // namespace

int main() { return fpm_example::run_example("ex_persistence_recovery", &body); }
