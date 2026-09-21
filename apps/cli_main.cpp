// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// fabric-partition-cli: a bounded operator client for a running coordinator.
//
// Every subcommand connects one PartitionClient, performs exactly one governed
// operation and prints a machine-readable report: a block of lowercase
// key=value lines first, then free-form detail. Nothing is inferred: a verdict
// the runtime did not grant is reported as a refusal with its reason codes, and
// the exit code distinguishes a refusal from a transport failure.
//
// Subcommands
//   status  partitions  assess  fences  lineage
//   publish  topology  isolate  clear-isolation  revalidate  retire  merge
//
// Exit codes
//   0  the operation succeeded
//   1  the operation was refused; the verdict and every reason are printed
//   2  the connection, the protocol, or the arguments failed
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "arguments.hpp"
#include "fabric_partition_manager/fabric_partition_manager.hpp"

namespace {

namespace fpm = fabric_partition_manager;

constexpr const char* kUsage =
    "usage: fabric-partition-cli <status|partitions|assess|fences|lineage|publish|topology|"
    "isolate|clear-isolation|revalidate|retire|merge> [--address=ADDR] [--port=N] "
    "[--publisher=ID] [--expected-epoch=N] [--lineage=ID] [--subject=ID] [--left=ID] [--right=ID] "
    "[--groups=a,b] [--seed=N] [--evidence-id=ID] [--topology-generation=N] "
    "[--reachability-generation=N] [--duration-ticks=N]";

[[nodiscard]] std::vector<std::string> known_options() {
  return {"address",    "port",       "publisher",         "expected-epoch", "lineage",
          "subject",    "left",       "right",             "groups",         "seed",
          "evidence-id", "topology-generation", "reachability-generation", "duration-ticks",
          "help"};
}

[[nodiscard]] std::vector<std::string> known_commands() {
  return {"status",          "partitions", "assess", "fences",     "lineage", "publish",
          "topology",        "isolate",    "clear-isolation", "revalidate", "retire", "merge"};
}

struct CliOptions {
  std::string address = "127.0.0.1";
  std::uint16_t port = 0;
  std::string publisher_text = "cli";
  fpm::PublisherId publisher;
  std::uint64_t expected_epoch = 0;
  std::string lineage_text;
  std::string subject_text;
  std::string left_text;
  std::string right_text;
  std::vector<std::uint32_t> groups;
  std::uint64_t seed = 1;
  std::string evidence_text = "cli.evidence";
  std::uint64_t topology_generation = 1;
  std::uint64_t reachability_generation = 1;
  std::uint64_t duration_ticks = 0;
  fpm::Limits limits = fpm::default_limits();
};

// The report envelope. The key block is printed from this structure, so every
// subcommand reports the same keys in the same order.
struct Report {
  fpm::DecisionVerdict verdict = fpm::DecisionVerdict::Invalid;
  std::string decision = "nil";
  std::uint64_t epoch = 0;
  bool confirmed = false;
  std::uint64_t partitions = 0;
  std::uint64_t interrupted = 0;
  std::uint64_t fences = 0;
  std::uint64_t lineage = 0;
  std::uint64_t evidence = 0;
  std::uint64_t partition_generation = 0;
  std::vector<fpm::PartitionView> partition_views;
  std::vector<fpm::Reason> reasons;
  std::vector<std::string> details;
};

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

[[nodiscard]] bool read_u64(const fpm_apps::Arguments& arguments, const std::string& name,
                            std::uint64_t& value, std::string& error) {
  if (!arguments.has(name)) {
    return true;
  }
  const std::optional<std::uint64_t> parsed = arguments.get_u64(name);
  if (!parsed.has_value()) {
    error = "--" + name + " must be an unsigned integer";
    return false;
  }
  value = *parsed;
  return true;
}

[[nodiscard]] bool parse_options(const fpm_apps::Arguments& arguments, CliOptions& options,
                                 std::string& error) {
  options.address = arguments.get("address", "127.0.0.1");

  std::uint64_t port = 0;
  if (!read_u64(arguments, "port", port, error)) {
    return false;
  }
  if (port > 65535u) {
    error = "--port must be an unsigned 16-bit port number";
    return false;
  }
  options.port = static_cast<std::uint16_t>(port);

  options.publisher_text = arguments.get("publisher", "cli");
  const auto publisher = fpm::PublisherId::parse(options.publisher_text);
  if (!publisher.has_value()) {
    error = "--publisher is not a canonical identifier";
    return false;
  }
  options.publisher = *publisher;

  if (!read_u64(arguments, "expected-epoch", options.expected_epoch, error) ||
      !read_u64(arguments, "seed", options.seed, error) ||
      !read_u64(arguments, "topology-generation", options.topology_generation, error) ||
      !read_u64(arguments, "reachability-generation", options.reachability_generation, error) ||
      !read_u64(arguments, "duration-ticks", options.duration_ticks, error)) {
    return false;
  }
  if (options.topology_generation == 0 || options.reachability_generation == 0) {
    error = "--topology-generation and --reachability-generation must be positive";
    return false;
  }

  options.lineage_text = arguments.get("lineage", "");
  options.subject_text = arguments.get("subject", "");
  options.left_text = arguments.get("left", "");
  options.right_text = arguments.get("right", "");
  options.evidence_text = arguments.get("evidence-id", "cli.evidence");

  if (!parse_group_sizes(arguments.get("groups", "2,2"), options.groups)) {
    error = "--groups must be a comma separated list of positive sizes";
    return false;
  }
  return true;
}

template <typename Tag>
[[nodiscard]] std::string render_id(const fpm::TextId<Tag, 96>& value) {
  return value.is_nil() ? std::string("nil") : value.str();
}

[[nodiscard]] std::string render_flag(bool value) { return value ? "true" : "false"; }

[[nodiscard]] std::string render_capabilities(std::uint32_t bits) {
  return fpm::CapabilitySet(bits).render();
}

// Reads the coordinator's current status and the most recent assessment. Both
// are queries: neither advances the partition generation, so a report never has
// the side effect of an assessment.
[[nodiscard]] bool fill_report(const CliOptions& options, fpm::PartitionClient& client,
                               Report& report, std::string& error) {
  const std::optional<fpm::StatusView> status = client.status(error);
  if (!status.has_value()) {
    return false;
  }
  report.epoch = status->epoch.value();
  report.interrupted = status->interrupted_authorities;
  report.fences = status->fence_records;
  report.lineage = status->lineage_records;
  report.evidence = status->evidence_bundles;
  report.partitions = status->partition_count;

  report.details.push_back("status_detail boot=" + status->boot.hex() +
                           " incarnation=" + status->incarnation.hex() +
                           " tick=" + std::to_string(status->tick) +
                           " partition_generation=" +
                           std::to_string(status->partition_generation) +
                           " authority_sequence=" + std::to_string(status->authority_sequence) +
                           " decision_sequence=" + std::to_string(status->decision_sequence) +
                           " attempt_sequence=" + std::to_string(status->attempt_sequence) +
                           " lineage_sequence=" + std::to_string(status->lineage_sequence) +
                           " fence_sequence=" + std::to_string(status->fence_sequence) +
                           " decision_history=" + std::to_string(status->decision_history) +
                           " decisions_dropped=" + std::to_string(status->decisions_dropped) +
                           " durable=" + render_flag(status->durable));

  const fpm::ClientResponse response =
      client.exchange(fpm::MessageType::QueryPartitions, std::vector<std::byte>{});
  if (!response.ok) {
    error = response.error;
    return false;
  }
  if (response.type != fpm::MessageType::QueryPartitionsAck) {
    error = "the partition query was answered with an unexpected message type";
    return false;
  }
  const std::optional<fpm::AssessmentView> assessment =
      fpm::decode_assessment_view(response.payload, options.limits);
  if (!assessment.has_value()) {
    error = "the assessment payload could not be decoded";
    return false;
  }
  report.verdict = assessment->verdict;
  report.decision = assessment->decision.is_nil() ? std::string("nil")
                                                  : assessment->decision.str();
  report.confirmed = assessment->confirmed;
  report.partition_generation = assessment->generation.value();
  report.partitions = assessment->partitions.size();
  report.partition_views = assessment->partitions;
  return true;
}

void print_report(const Report& report) {
  std::cout << "verdict=" << fpm::decision_verdict_name(report.verdict) << "\n";
  std::cout << "decision=" << report.decision << "\n";
  std::cout << "epoch=" << report.epoch << "\n";
  std::cout << "confirmed=" << render_flag(report.confirmed) << "\n";
  std::cout << "partitions=" << report.partitions << "\n";
  std::cout << "interrupted=" << report.interrupted << "\n";
  std::cout << "fences=" << report.fences << "\n";
  std::cout << "lineage=" << report.lineage << "\n";
  std::cout << "evidence=" << report.evidence << "\n";
  std::cout << "partition_generation=" << report.partition_generation << "\n";
  for (const fpm::PartitionView& view : report.partition_views) {
    std::cout << "partition id=" << view.id.view() << " lineage=" << view.lineage.view()
              << " class=" << fpm::partition_authority_class_name(view.authority_class)
              << " members=" << view.member_count
              << " authoritative=" << render_flag(view.authoritative) << "\n";
  }
  for (const fpm::Reason& reason : report.reasons) {
    std::cout << "reason=" << fpm::reason_code_name(reason.code) << "\n";
  }
  for (const std::string& line : report.details) {
    std::cout << line << "\n";
  }
}

void append_partition_details(Report& report) {
  for (const fpm::PartitionView& view : report.partition_views) {
    report.details.push_back(
        "partition_detail id=" + view.id.str() + " lineage=" + view.lineage.str() +
        " generation=" + std::to_string(view.generation.value()) +
        " certainty=" + std::string(fpm::partition_certainty_name(view.certainty)) +
        " lifecycle=" + std::string(fpm::partition_lifecycle_name(view.lifecycle)) +
        " basis=" + std::string(fpm::authority_basis_name(view.basis)) +
        " application=" +
        std::string(fpm::authority_application_state_name(view.application_state)) +
        " authorized=" + render_capabilities(view.authorized_bits) +
        " eligible=" + render_capabilities(view.eligible_bits) +
        " observed=" + render_capabilities(view.observed_bits) +
        " authority_sequence=" + std::to_string(view.authority_sequence) +
        " pending_merge=" + render_flag(view.pending_merge));
  }
}

void append_reason_details(Report& report) {
  for (const fpm::Reason& reason : report.reasons) {
    report.details.push_back("reason_detail code=" + std::string(fpm::reason_code_name(reason.code)) +
                             " subject=" + reason.subject + " detail=" + reason.detail);
  }
}

// A monotonic, process-unique attempt identity. The sequence is derived from
// the steady clock, so a later invocation always advances the runtime floor.
[[nodiscard]] fpm::AttemptToken make_attempt() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  const std::uint64_t sequence = nanos <= 0 ? 1u : static_cast<std::uint64_t>(nanos);
  fpm::AttemptToken token;
  token.sequence = fpm::AttemptSequence::from_value(sequence);
  token.id = fpm::AttemptId::from_validated("a-" + std::to_string(sequence));
  return token;
}

[[nodiscard]] fpm::Provenance requester_provenance(const std::string& publisher_text) {
  const auto parsed = fpm::Provenance::parse(publisher_text);
  if (parsed.has_value()) {
    return *parsed;
  }
  return fpm::Provenance::from_validated("cli");
}

[[nodiscard]] bool parse_lineage(const std::string& text, fpm::LineageId& value) {
  const auto parsed = fpm::LineageId::parse(text);
  if (!parsed.has_value()) {
    return false;
  }
  value = *parsed;
  return true;
}

[[nodiscard]] bool parse_partition(const std::string& text, fpm::PartitionId& value) {
  const auto parsed = fpm::PartitionId::parse(text);
  if (!parsed.has_value()) {
    return false;
  }
  value = *parsed;
  return true;
}

[[nodiscard]] std::optional<fpm::SyntheticFabric> build_cli_fabric(const CliOptions& options,
                                                                  bool reachability) {
  const auto evidence = fpm::EvidenceId::parse(options.evidence_text);
  if (!evidence.has_value()) {
    return std::nullopt;
  }
  fpm::SyntheticFabricOptions fabric_options;
  fabric_options.name = "cli";
  fabric_options.group_sizes = options.groups;
  fabric_options.complete_coverage = true;
  fabric_options.seed = options.seed;
  fabric_options.topology_generation =
      fpm::TopologyGeneration::from_value(options.topology_generation);
  fabric_options.reachability_generation =
      fpm::ReachabilityGeneration::from_value(reachability ? options.reachability_generation : 1);
  fabric_options.observed_at_tick = 0;
  fabric_options.validity_ticks = 1'000'000;
  fabric_options.publisher = options.publisher;
  fabric_options.evidence_id = *evidence;
  fabric_options.provenance = fpm::Provenance::from_validated("synthetic");
  return fpm::build_synthetic_fabric(fabric_options, options.limits);
}

// Applies an operation result to the report and turns it into an exit code.
// The caller's success predicate decides what "the operation succeeded" means
// for its own subcommand.
[[nodiscard]] int finish_operation(const CliOptions& options, fpm::PartitionClient& client,
                                   Report& report, const fpm::OperationResult& result,
                                   bool success) {
  report.verdict = result.verdict;
  report.decision = result.decision.is_nil() ? std::string("nil") : result.decision.str();
  report.reasons = result.reasons;
  std::string error;
  if (!fill_report(options, client, report, error)) {
    std::cerr << "cli failed: " << error << "\n";
    return 2;
  }
  if (!success) {
    append_reason_details(report);
  }
  append_partition_details(report);
  print_report(report);
  return success ? 0 : 1;
}

// -- Subcommands -----------------------------------------------------------

[[nodiscard]] int command_status(const CliOptions& options, fpm::PartitionClient& client) {
  Report report;
  std::string error;
  if (!fill_report(options, client, report, error)) {
    std::cerr << "cli failed: " << error << "\n";
    return 2;
  }
  append_partition_details(report);
  print_report(report);
  return 0;
}

[[nodiscard]] int command_partitions(const CliOptions& options, fpm::PartitionClient& client) {
  Report report;
  std::string error;
  if (!fill_report(options, client, report, error)) {
    std::cerr << "cli failed: " << error << "\n";
    return 2;
  }
  append_partition_details(report);
  print_report(report);
  return 0;
}

[[nodiscard]] int command_assess(const CliOptions& options, fpm::PartitionClient& client) {
  std::string error;
  const std::optional<fpm::AssessmentView> assessment = client.assess(error);
  if (!assessment.has_value()) {
    std::cerr << "cli failed: " << error << "\n";
    return 2;
  }
  Report report;
  if (!fill_report(options, client, report, error)) {
    std::cerr << "cli failed: " << error << "\n";
    return 2;
  }
  report.verdict = assessment->verdict;
  report.decision = assessment->decision.is_nil() ? std::string("nil") : assessment->decision.str();
  report.confirmed = assessment->confirmed;
  report.partition_generation = assessment->generation.value();
  report.partitions = assessment->partitions.size();
  report.partition_views = assessment->partitions;
  report.reasons = assessment->reasons;
  report.epoch = assessment->epoch.value();
  const bool success = !fpm::decision_verdict_needs_attention(assessment->verdict);
  report.details.push_back(
      "assess_detail split_brain=" + render_flag(assessment->split_brain) +
      " ordered_pairs=" + std::to_string(assessment->ordered_pairs) +
      " ordered_unknown=" + std::to_string(assessment->ordered_unknown) +
      " ordered_conflicting=" + std::to_string(assessment->ordered_conflicting) +
      " ordered_stale=" + std::to_string(assessment->ordered_stale) +
      " ordered_asymmetric=" + std::to_string(assessment->ordered_asymmetric) +
      " undirected_reachable=" + std::to_string(assessment->undirected_reachable) +
      " cross_component_indeterminate=" +
      std::to_string(assessment->cross_component_indeterminate) +
      " topology_generation=" + std::to_string(assessment->topology_generation) +
      " reachability_generation=" + std::to_string(assessment->reachability_generation));
  if (!success) {
    append_reason_details(report);
  }
  append_partition_details(report);
  print_report(report);
  return success ? 0 : 1;
}

[[nodiscard]] int command_fences(const CliOptions& options, fpm::PartitionClient& client) {
  std::string error;
  const std::optional<std::vector<fpm::FenceRecord>> fences = client.fences(error);
  if (!fences.has_value()) {
    std::cerr << "cli failed: " << error << "\n";
    return 2;
  }
  Report report;
  if (!fill_report(options, client, report, error)) {
    std::cerr << "cli failed: " << error << "\n";
    return 2;
  }
  for (const fpm::FenceRecord& fence : *fences) {
    report.details.push_back(
        "fence id=" + fence.id.str() + " sequence=" + std::to_string(fence.sequence.value()) +
        " partition=" + fence.partition.str() +
        " scope=" + std::string(fpm::scope_kind_name(fence.scope.kind)) + ":" +
        fence.scope.id.str() +
        " fenced_authority_sequence=" +
        std::to_string(fence.fenced_authority_sequence.value()) +
        " fenced_epoch=" + std::to_string(fence.fenced_epoch.value()) +
        " issuing_epoch=" + std::to_string(fence.issuing_epoch.value()) +
        " cause=" + std::string(fpm::reason_code_name(fence.cause)) +
        " tick=" + std::to_string(fence.tick));
  }
  append_partition_details(report);
  print_report(report);
  return 0;
}

[[nodiscard]] int command_lineage(const CliOptions& options, fpm::PartitionClient& client) {
  std::string error;
  const std::optional<std::vector<fpm::LineageRecord>> records = client.lineage(error);
  if (!records.has_value()) {
    std::cerr << "cli failed: " << error << "\n";
    return 2;
  }
  Report report;
  if (!fill_report(options, client, report, error)) {
    std::cerr << "cli failed: " << error << "\n";
    return 2;
  }
  for (const fpm::LineageRecord& record : *records) {
    report.details.push_back(
        "lineage_record id=" + record.lineage.str() + " sequence=" +
        std::to_string(record.sequence.value()) + " generation=" +
        std::to_string(record.generation.value()) +
        " event=" + std::string(fpm::lineage_event_kind_name(record.event)) +
        " members=" + std::to_string(record.member_count()) +
        " parent_left=" + render_id(record.parent_left) +
        " parent_right=" + render_id(record.parent_right) +
        " epoch=" + std::to_string(record.epoch.value()) + " tick=" + std::to_string(record.tick) +
        " membership=" + record.membership.hex());
  }
  append_partition_details(report);
  print_report(report);
  return 0;
}

[[nodiscard]] int command_publish(const CliOptions& options, fpm::PartitionClient& client) {
  const std::optional<fpm::SyntheticFabric> fabric = build_cli_fabric(options, true);
  if (!fabric.has_value()) {
    std::cerr << "cli failed: the synthetic fabric does not fit the configured limits\n";
    return 2;
  }
  std::string error;
  const std::optional<fpm::OperationResult> result =
      client.publish_evidence(fabric->evidence, error);
  if (!result.has_value()) {
    std::cerr << "cli failed: " << error << "\n";
    return 2;
  }
  Report report;
  report.details.push_back("published " + fabric->evidence.id.str());
  return finish_operation(options, client, report, *result,
                          result->verdict == fpm::DecisionVerdict::Granted);
}

[[nodiscard]] int command_topology(const CliOptions& options, fpm::PartitionClient& client) {
  const std::optional<fpm::SyntheticFabric> fabric = build_cli_fabric(options, false);
  if (!fabric.has_value()) {
    std::cerr << "cli failed: the synthetic fabric does not fit the configured limits\n";
    return 2;
  }
  std::string error;
  const std::optional<fpm::OperationResult> result = client.adopt_topology(fabric->topology, error);
  if (!result.has_value()) {
    std::cerr << "cli failed: " << error << "\n";
    return 2;
  }
  Report report;
  report.details.push_back("topology adopted generation=" +
                           std::to_string(fabric->topology.generation.value()) +
                           " components=" + std::to_string(fabric->topology.components.size()));
  const bool success = result->verdict == fpm::DecisionVerdict::Granted ||
                       result->verdict == fpm::DecisionVerdict::Observed;
  return finish_operation(options, client, report, *result, success);
}

[[nodiscard]] int command_isolate(const CliOptions& options, fpm::PartitionClient& client,
                                  fpm::CoordinatorEpoch epoch, bool clear) {
  fpm::LineageId lineage;
  if (!parse_lineage(options.lineage_text, lineage)) {
    std::cerr << "cli failed: --lineage must be a canonical lineage identifier\n";
    return 2;
  }
  fpm::IsolationRequest request;
  request.attempt = make_attempt();
  request.expected_epoch = epoch;
  request.cause = fpm::ReasonCode::IsolationApplied;
  request.requester = requester_provenance(options.publisher_text);
  request.lineage = lineage;
  request.duration_ticks = options.duration_ticks;

  std::string error;
  const std::optional<fpm::OperationResult> result =
      clear ? client.clear_isolation(request, error) : client.isolate(request, error);
  if (!result.has_value()) {
    std::cerr << "cli failed: " << error << "\n";
    return 2;
  }
  Report report;
  report.details.push_back(std::string(clear ? "clear_isolation" : "isolate") +
                           " lineage=" + lineage.str() +
                           " duration_ticks=" + std::to_string(options.duration_ticks));
  const bool success = clear ? result->verdict == fpm::DecisionVerdict::Granted
                             : result->verdict == fpm::DecisionVerdict::Isolated;
  return finish_operation(options, client, report, *result, success);
}

[[nodiscard]] int command_revalidate(const CliOptions& options, fpm::PartitionClient& client,
                                     fpm::CoordinatorEpoch epoch) {
  fpm::RevalidationRequest request;
  request.attempt = make_attempt();
  request.expected_epoch = epoch;
  request.requester = requester_provenance(options.publisher_text);

  std::string error;
  const std::optional<fpm::OperationResult> result = client.revalidate(request, error);
  if (!result.has_value()) {
    std::cerr << "cli failed: " << error << "\n";
    return 2;
  }
  Report report;
  const bool success = result->verdict == fpm::DecisionVerdict::Granted ||
                       result->verdict == fpm::DecisionVerdict::Observed;
  return finish_operation(options, client, report, *result, success);
}

[[nodiscard]] int command_retire(const CliOptions& options, fpm::PartitionClient& client,
                                 fpm::CoordinatorEpoch epoch) {
  fpm::LineageId lineage;
  if (!parse_lineage(options.lineage_text, lineage)) {
    std::cerr << "cli failed: --lineage must be a canonical lineage identifier\n";
    return 2;
  }
  fpm::RetireRequest request;
  request.attempt = make_attempt();
  request.expected_epoch = epoch;
  request.lineage = lineage;
  request.requester = requester_provenance(options.publisher_text);

  std::string error;
  const std::optional<fpm::OperationResult> result = client.retire(request, error);
  if (!result.has_value()) {
    std::cerr << "cli failed: " << error << "\n";
    return 2;
  }
  Report report;
  report.details.push_back("retire lineage=" + lineage.str());
  return finish_operation(options, client, report, *result,
                          result->verdict == fpm::DecisionVerdict::Granted);
}

[[nodiscard]] int command_merge(const CliOptions& options, fpm::PartitionClient& client,
                                fpm::CoordinatorEpoch epoch) {
  fpm::PartitionId subject;
  fpm::LineageId left;
  fpm::LineageId right;
  if (!parse_partition(options.subject_text, subject)) {
    std::cerr << "cli failed: --subject must be a canonical partition identifier\n";
    return 2;
  }
  if (!parse_lineage(options.left_text, left) || !parse_lineage(options.right_text, right)) {
    std::cerr << "cli failed: --left and --right must be canonical lineage identifiers\n";
    return 2;
  }
  fpm::MergeRequest request;
  request.attempt = make_attempt();
  request.expected_epoch = epoch;
  request.subject = subject;
  request.left = left;
  request.right = right;
  request.requester = requester_provenance(options.publisher_text);

  std::string error;
  const std::optional<fpm::MergeAckView> result = client.request_merge(request, error);
  if (!result.has_value()) {
    std::cerr << "cli failed: " << error << "\n";
    return 2;
  }
  Report report;
  report.details.push_back("merge subject=" + subject.str() + " left=" + left.str() +
                           " right=" + right.str() + " merged_lineage=" +
                           render_id(result->merged_lineage) + " merged_partition=" +
                           render_id(result->merged_partition));
  return finish_operation(options, client, report, result->result,
                          result->result.verdict == fpm::DecisionVerdict::Granted);
}

}  // namespace

int main(int argc, char** argv) {
  const fpm_apps::Arguments arguments = fpm_apps::parse_arguments(argc, argv);
  std::string unknown;
  if (!fpm_apps::options_known(arguments, known_options(), unknown)) {
    std::cerr << "cli failed: unknown option --" << unknown << "\n" << kUsage << "\n";
    return 2;
  }
  if (arguments.positional.empty() || arguments.positional.size() > 1) {
    std::cerr << "cli failed: exactly one subcommand is required\n" << kUsage << "\n";
    return 2;
  }
  if (arguments.has("help")) {
    std::cout << kUsage << "\n";
    return 0;
  }
  const std::string command = arguments.positional.front();
  bool recognised = false;
  for (const std::string& candidate : known_commands()) {
    if (candidate == command) {
      recognised = true;
      break;
    }
  }
  if (!recognised) {
    std::cerr << "cli failed: unknown subcommand '" << command << "'\n" << kUsage << "\n";
    return 2;
  }

  CliOptions options;
  std::string error;
  if (!parse_options(arguments, options, error)) {
    std::cerr << "cli failed: " << error << "\n" << kUsage << "\n";
    return 2;
  }

  fpm::ClientOptions client_options;
  client_options.address = options.address;
  client_options.port = options.port;
  client_options.limits = options.limits;

  auto client = std::make_unique<fpm::PartitionClient>(client_options);
  if (!client->connect(options.publisher, fpm::PublisherBootId::generate(),
                       fpm::CoordinatorEpoch::from_value(options.expected_epoch), error)) {
    std::cerr << "cli failed: " << error << "\n";
    return 2;
  }

  // The epoch every governed request is bound to: the caller's when one was
  // named, otherwise the epoch the coordinator reports for this incarnation.
  fpm::CoordinatorEpoch epoch = fpm::CoordinatorEpoch::from_value(options.expected_epoch);
  if (options.expected_epoch == 0) {
    const std::optional<fpm::StatusView> status = client->status(error);
    if (!status.has_value()) {
      std::cerr << "cli failed: " << error << "\n";
      return 2;
    }
    epoch = status->epoch;
  }

  int result = 2;
  if (command == "status") {
    result = command_status(options, *client);
  } else if (command == "partitions") {
    result = command_partitions(options, *client);
  } else if (command == "assess") {
    result = command_assess(options, *client);
  } else if (command == "fences") {
    result = command_fences(options, *client);
  } else if (command == "lineage") {
    result = command_lineage(options, *client);
  } else if (command == "publish") {
    result = command_publish(options, *client);
  } else if (command == "topology") {
    result = command_topology(options, *client);
  } else if (command == "isolate") {
    result = command_isolate(options, *client, epoch, false);
  } else if (command == "clear-isolation") {
    result = command_isolate(options, *client, epoch, true);
  } else if (command == "revalidate") {
    result = command_revalidate(options, *client, epoch);
  } else if (command == "retire") {
    result = command_retire(options, *client, epoch);
  } else if (command == "merge") {
    result = command_merge(options, *client, epoch);
  }
  std::cout << std::flush;
  client->close();
  return result;
}
