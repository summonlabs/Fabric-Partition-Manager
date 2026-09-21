// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// fabric-partition-publisher: adopts one SYNTHETIC roster and publishes one
// deterministic SYNTHETIC reachability bundle to a running coordinator.
//
// Everything this program emits is SYNTHETIC: a fabricated roster of
// components named node-########## and fabricated reachability observations. It
// is not a measurement of any physical switch, NIC, RDMA transport or
// multi-node deployment.
//
// The topology is adopted before anything is published, because reachability
// evidence is only meaningful against an authoritative roster. Re-adopting the
// identical roster at the same generation is reported OBSERVED, which is an
// acceptance, not a refusal.
//
// Exit codes
//   0  the topology was accepted and every publish was accepted (Granted)
//   1  the topology or at least one publish was refused
//   2  the connection failed, the protocol failed, or the arguments are unusable
//
// Standard output, in order:
//   adopted topology generation=<n> verdict=<VERDICT>
//   published <evidence-id> verdict=<VERDICT> decision=<decision-id>
//   reason=<CODE>                                        (refusals only)
//   reason <CODE> subject=<subject> detail=<detail>      (refusals only)
#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "arguments.hpp"
#include "fabric_partition_manager/fabric_partition_manager.hpp"

namespace {

namespace fpm = fabric_partition_manager;

constexpr const char* kUsage =
    "usage: fabric-partition-publisher [--address=ADDR] [--port=N] [--publisher=ID] "
    "[--groups=a,b,c] [--seed=N] [--interval-ms=N] [--count=N] [--evidence-id=ID] "
    "[--validity-ticks=N] [--complete=true|false] [--expected-epoch=N]";

[[nodiscard]] std::vector<std::string> known_options() {
  return {"address",   "port",      "publisher", "groups",  "seed",   "interval-ms",
          "count",     "evidence-id", "validity-ticks", "complete", "expected-epoch", "help"};
}

// Parses "a,b,c" into the requested group sizes. A group of zero components is
// refused: it names a partition that cannot exist.
[[nodiscard]] bool parse_group_sizes(const std::string& text,
                                     std::vector<std::uint32_t>& groups) {
  groups.clear();
  if (text.empty()) {
    return false;
  }
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t comma = text.find(',', start);
    const std::size_t end = comma == std::string::npos ? text.size() : comma;
    if (end == start) {
      return false;
    }
    const std::string field = text.substr(start, end - start);
    std::uint64_t value = 0;
    for (const char digit : field) {
      if (digit < '0' || digit > '9') {
        return false;
      }
      value = value * 10u + static_cast<std::uint64_t>(digit - '0');
      if (value > 0xFFFF'FFFFull) {
        return false;
      }
    }
    if (value == 0) {
      return false;
    }
    groups.push_back(static_cast<std::uint32_t>(value));
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  return !groups.empty();
}

[[nodiscard]] bool parse_bool(const std::string& text, bool& value) {
  if (text == "true") {
    value = true;
    return true;
  }
  if (text == "false") {
    value = false;
    return true;
  }
  return false;
}

void print_reasons(const std::vector<fpm::Reason>& reasons) {
  for (const fpm::Reason& reason : reasons) {
    std::cout << "reason=" << fpm::reason_code_name(reason.code) << "\n";
  }
  for (const fpm::Reason& reason : reasons) {
    std::cout << "reason " << fpm::reason_code_name(reason.code) << " subject=" << reason.subject
              << " detail=" << reason.detail << "\n";
  }
}

}  // namespace

int main(int argc, char** argv) {
  const fpm_apps::Arguments arguments = fpm_apps::parse_arguments(argc, argv);
  std::string unknown;
  if (!fpm_apps::options_known(arguments, known_options(), unknown)) {
    std::cerr << "publisher failed: unknown option --" << unknown << "\n" << kUsage << "\n";
    return 2;
  }
  if (!arguments.positional.empty()) {
    std::cerr << "publisher failed: unexpected argument '" << arguments.positional.front() << "'\n"
              << kUsage << "\n";
    return 2;
  }
  if (arguments.has("help")) {
    std::cout << kUsage << "\n";
    return 0;
  }

  const fpm::Limits limits = fpm::default_limits();

  const std::string address = arguments.get("address", "127.0.0.1");
  std::uint16_t port = 0;
  if (arguments.has("port")) {
    const std::optional<std::uint64_t> parsed = arguments.get_u64("port");
    if (!parsed.has_value() || *parsed > 65535u) {
      std::cerr << "publisher failed: --port must be an unsigned 16-bit port number\n" << kUsage
                << "\n";
      return 2;
    }
    port = static_cast<std::uint16_t>(*parsed);
  }

  const std::string publisher_text = arguments.get("publisher", "publisher");
  const auto publisher = fpm::PublisherId::parse(publisher_text);
  if (!publisher.has_value()) {
    std::cerr << "publisher failed: --publisher is not a canonical identifier\n";
    return 2;
  }
  const std::string evidence_text = arguments.get("evidence-id", "publisher.evidence");
  const auto evidence_id = fpm::EvidenceId::parse(evidence_text);
  if (!evidence_id.has_value()) {
    std::cerr << "publisher failed: --evidence-id is not a canonical identifier\n";
    return 2;
  }

  std::vector<std::uint32_t> groups;
  if (!parse_group_sizes(arguments.get("groups", "2,2"), groups)) {
    std::cerr << "publisher failed: --groups must be a comma separated list of positive sizes\n";
    return 2;
  }

  std::uint64_t seed = 1;
  if (arguments.has("seed")) {
    const std::optional<std::uint64_t> parsed = arguments.get_u64("seed");
    if (!parsed.has_value()) {
      std::cerr << "publisher failed: --seed must be an unsigned integer\n";
      return 2;
    }
    seed = *parsed;
  }

  std::uint64_t interval_millis = 0;
  if (arguments.has("interval-ms")) {
    const std::optional<std::uint64_t> parsed = arguments.get_u64("interval-ms");
    if (!parsed.has_value()) {
      std::cerr << "publisher failed: --interval-ms must be an unsigned integer\n";
      return 2;
    }
    interval_millis = *parsed;
  }

  std::uint64_t count = 1;
  if (arguments.has("count")) {
    const std::optional<std::uint64_t> parsed = arguments.get_u64("count");
    if (!parsed.has_value() || *parsed == 0) {
      std::cerr << "publisher failed: --count must be a positive integer\n";
      return 2;
    }
    count = *parsed;
  }

  std::uint64_t validity_ticks = 1'000'000;
  if (arguments.has("validity-ticks")) {
    const std::optional<std::uint64_t> parsed = arguments.get_u64("validity-ticks");
    if (!parsed.has_value() || *parsed == 0) {
      std::cerr << "publisher failed: --validity-ticks must be a positive integer\n";
      return 2;
    }
    validity_ticks = *parsed;
  }

  bool complete = true;
  if (arguments.has("complete") && !parse_bool(arguments.get("complete"), complete)) {
    std::cerr << "publisher failed: --complete must be true or false\n";
    return 2;
  }

  std::uint64_t expected_epoch = 0;
  if (arguments.has("expected-epoch")) {
    const std::optional<std::uint64_t> parsed = arguments.get_u64("expected-epoch");
    if (!parsed.has_value()) {
      std::cerr << "publisher failed: --expected-epoch must be an unsigned integer\n";
      return 2;
    }
    expected_epoch = *parsed;
  }

  fpm::SyntheticFabricOptions fabric_options;
  fabric_options.name = "publisher";
  fabric_options.group_sizes = groups;
  fabric_options.complete_coverage = complete;
  fabric_options.seed = seed;
  fabric_options.topology_generation = fpm::TopologyGeneration::from_value(1);
  fabric_options.reachability_generation = fpm::ReachabilityGeneration::from_value(1);
  fabric_options.observed_at_tick = 0;
  fabric_options.validity_ticks = validity_ticks;
  fabric_options.publisher = *publisher;
  fabric_options.evidence_id = *evidence_id;
  fabric_options.provenance = fpm::Provenance::from_validated("synthetic");

  const std::optional<fpm::SyntheticFabric> fabric =
      fpm::build_synthetic_fabric(fabric_options, limits);
  if (!fabric.has_value()) {
    std::cerr << "publisher failed: the synthetic fabric does not fit the configured limits\n";
    return 2;
  }

  fpm::ClientOptions client_options;
  client_options.address = address;
  client_options.port = port;
  client_options.limits = limits;

  fpm::PartitionClient client(client_options);
  std::string error;
  if (!client.connect(*publisher, fpm::PublisherBootId::generate(),
                      fpm::CoordinatorEpoch::from_value(expected_epoch), error)) {
    std::cerr << "publisher failed: " << error << "\n";
    return 2;
  }

  // The epoch a publish is bound to. When the caller did not name one, the
  // coordinator's current epoch is read from a status query before anything is
  // published, so the publisher never acts under a guessed incarnation.
  const std::optional<fpm::StatusView> status = client.status(error);
  if (!status.has_value()) {
    std::cerr << "publisher failed: " << error << "\n";
    client.close();
    return 2;
  }
  if (!(status->epoch == client.epoch())) {
    std::cerr << "publisher failed: the session epoch does not match the coordinator epoch\n";
    client.close();
    return 2;
  }

  const std::optional<fpm::OperationResult> adopted =
      client.adopt_topology(fabric->topology, error);
  if (!adopted.has_value()) {
    std::cerr << "publisher failed: " << error << "\n";
    client.close();
    return 2;
  }
  std::cout << "adopted topology generation=" << fabric->topology.generation.value()
            << " verdict=" << fpm::decision_verdict_name(adopted->verdict) << "\n";
  if (adopted->verdict != fpm::DecisionVerdict::Granted &&
      adopted->verdict != fpm::DecisionVerdict::Observed) {
    print_reasons(adopted->reasons);
    std::cout << std::flush;
    client.close();
    return 1;
  }

  std::uint64_t client_status_tick = fabric->evidence.observed_at_tick;
  {
    const std::optional<fpm::StatusView> current = client.status(error);
    if (current.has_value() && current->tick > client_status_tick) {
      client_status_tick = current->tick;
    }
  }

  bool all_accepted = true;
  for (std::uint64_t index = 0; index < count; ++index) {
    // Each iteration publishes a genuinely new observation: the publisher
    // sequence advances, so the runtime sees a fresh attempt rather than a
    // replay of the one before it.
    fpm::ReachabilityEvidence evidence = fabric->evidence;
    evidence.sequence = fpm::EvidenceSequence::from_value(index + 1);
    evidence.observed_at_tick = client_status_tick;
    const std::optional<fpm::OperationResult> result =
        client.publish_evidence(evidence, error);
    if (!result.has_value()) {
      std::cerr << "publisher failed: " << error << "\n";
      client.close();
      return 2;
    }
    // Each line is flushed as it is produced so a supervising process can
    // observe progress and terminate this publisher at a chosen point.
    std::cout << "published " << evidence.id.view()
              << " sequence=" << (index + 1)
              << " verdict=" << fpm::decision_verdict_name(result->verdict)
              << " decision=" << result->decision.view() << std::endl;
    if (result->verdict == fpm::DecisionVerdict::Granted) {
      if (index + 1 < count && interval_millis != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_millis));
      }
      continue;
    }
    all_accepted = false;
    print_reasons(result->reasons);
  }
  client.close();
  return all_accepted ? 0 : 1;
}
