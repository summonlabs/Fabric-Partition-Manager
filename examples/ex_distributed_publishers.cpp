// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// ex_distributed_publishers: a real PartitionServer on an ephemeral port, two
// independent publisher sessions, and one asserted assessment.
//
// The roster is adopted once. Two observers then report independently: one
// observation-only bundle that proves the connectivity it tested, and one
// complete-coverage bundle that additionally asserts that everything it did not
// reach is unreachable. One complete claim per source is what makes the
// decomposition CONFIRMED; two would be a contradiction and are reported as
// such rather than merged into one convenient answer.
//
// Everything here is SYNTHETIC: a fabricated roster with fabricated
// reachability observations, transported over the real framed protocol on
// loopback. It is not physical hardware validation.
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "example_support.hpp"

namespace {

namespace fpm = fabric_partition_manager;
using fpm_example::require;

void body() {
  const fpm::PartitionPolicy policy = fpm_example::count_quorum_policy(1, 3);
  fpm::PartitionRuntime runtime(fpm_example::memory_options(policy));
  require(!runtime.status().durable, "this example runs a memory-only runtime");
  const fpm::Limits limits = runtime.limits();

  const fpm::SyntheticFabric roster =
      fpm_example::build_fabric({4}, limits, 5, true, "observer.a", "observer.a.evidence");
  const fpm::SyntheticFabric observation =
      fpm_example::build_fabric({4}, limits, 5, false, "observer.b", "observer.b.evidence");
  require(roster.evidence.completeness == fpm::EvidenceCompleteness::Complete,
          "observer a publishes a complete coverage claim");
  require(observation.evidence.completeness == fpm::EvidenceCompleteness::Partial,
          "observer b publishes an observation-only bundle");
  require(roster.topology.provenance.view() == std::string_view("synthetic") &&
              roster.evidence.provenance.view() == std::string_view("synthetic") &&
              observation.evidence.provenance.view() == std::string_view("synthetic"),
          "every fixture must be labelled SYNTHETIC");

  fpm::ServerOptions server_options;
  server_options.bind_address = "127.0.0.1";
  server_options.port = 0;
  server_options.limits = limits;
  server_options.provenance = fpm::Provenance::from_validated("synthetic");

  fpm::PartitionServer server(runtime, server_options);
  std::string error;
  require(server.start(error), "the in-process coordinator must start: " + error);
  require(server.running(), "the coordinator must be running");
  const std::uint16_t port = server.port();
  require(port != 0, "an ephemeral port must be bound");

  fpm::ClientOptions client_options;
  client_options.address = "127.0.0.1";
  client_options.port = port;
  client_options.limits = limits;

  {
    fpm::PartitionClient first(client_options);
    require(first.connect(fpm::PublisherId::from_validated("observer.a"),
                          fpm::PublisherBootId::generate(), fpm::CoordinatorEpoch{}, error),
            "the first observer must connect: " + error);
    require(!first.session().is_nil(), "the handshake must issue a session handle");
    const std::optional<fpm::OperationResult> adopted =
        first.adopt_topology(roster.topology, error);
    require(adopted.has_value(), "the topology must be adopted: " + error);
    require(adopted->verdict == fpm::DecisionVerdict::Granted,
            "adopting a first roster must be granted");
    const std::optional<fpm::OperationResult> published =
        first.publish_evidence(observation.evidence, error);
    require(published.has_value(), "the observation-only bundle must be published: " + error);
    require(published->verdict == fpm::DecisionVerdict::Granted,
            "the observation-only bundle must be accepted");
    std::cout << "label=SYNTHETIC publisher=observer.b evidence="
              << observation.evidence.id.view()
              << " verdict=" << fpm::decision_verdict_name(published->verdict) << "\n";
    first.close();
  }

  {
    fpm::PartitionClient second(client_options);
    require(second.connect(fpm::PublisherId::from_validated("observer.b"),
                           fpm::PublisherBootId::generate(), fpm::CoordinatorEpoch{}, error),
            "the second observer must connect: " + error);
    const std::optional<fpm::OperationResult> published =
        second.publish_evidence(roster.evidence, error);
    require(published.has_value(), "the complete bundle must be published: " + error);
    require(published->verdict == fpm::DecisionVerdict::Granted,
            "the complete bundle must be accepted");
    std::cout << "label=SYNTHETIC publisher=observer.a evidence=" << roster.evidence.id.view()
              << " verdict=" << fpm::decision_verdict_name(published->verdict) << "\n";

    const std::optional<fpm::AssessmentView> assessment = second.assess(error);
    require(assessment.has_value(), "the assessment must be available: " + error);
    require(assessment->verdict == fpm::DecisionVerdict::Granted,
            "one connected component set of four must be granted authority");
    require(assessment->confirmed, "one complete coverage claim per source confirms the fabric");
    require(assessment->partitions.size() == 1, "the fabric must decompose into one partition");
    const fpm::PartitionView& partition = assessment->partitions.front();
    require(partition.member_count == 4, "the partition must carry four members");
    require(partition.certainty == fpm::PartitionCertainty::Confirmed,
            "the partition must be CONFIRMED");
    require(partition.authority_class == fpm::PartitionAuthorityClass::Primary,
            "the partition must hold primary authority");
    require(partition.authoritative, "the partition must be authoritative");
    require(partition.authorized_bits == fpm::CapabilitySet::all().bits(),
            "a primary holds every capability");
    require(assessment->undirected_reachable == 3,
            "the spanning tree carries three proven edges");
    require(assessment->ordered_pairs == 12, "four components have twelve ordered pairs");
    require(assessment->ordered_unknown == 0,
            "one complete coverage claim leaves no ordered pair unknown");
    require(assessment->ordered_conflicting == 0,
            "disjoint observation bundles must not contradict each other");
    require(assessment->epoch.value() == runtime.epoch().value(),
            "the assessment must be bound to the current epoch");

    const std::optional<fpm::StatusView> status = second.status(error);
    require(status.has_value(), "the status must be available: " + error);
    require(status->evidence_bundles == 2, "both independent bundles must be retained");
    std::cout << "label=SYNTHETIC port=" << port << " sessions=2 evidence=2 partitions=1"
              << " confirmed=true ordered-reachable=" << assessment->ordered_pairs -
                     assessment->ordered_unknown
              << " verdict=" << fpm::decision_verdict_name(assessment->verdict) << "\n";
    second.close();
  }

  const fpm::ServerStats stats = server.stats();
  require(stats.accepted_sessions == 2, "both publisher sessions must be accepted");
  require(stats.rejected_sessions == 0, "no session may be rejected");
  require(stats.protocol_errors == 0, "the framed protocol must report no error");
  require(stats.frames_received >= 8, "both sessions must exchange several frames");
  server.stop();
  require(!server.running(), "the coordinator must stop cleanly");
  std::cout << "server accepted=" << stats.accepted_sessions
            << " completed=" << stats.completed_sessions
            << " frames=" << stats.frames_received << " protocol-errors="
            << stats.protocol_errors << "\n";
}

}  // namespace

int main() { return fpm_example::run_example("ex_distributed_publishers", &body); }
