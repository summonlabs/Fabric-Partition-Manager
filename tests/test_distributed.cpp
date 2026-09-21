// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// Real multiprocess proof. The coordinator is a separate operating system
// process reached over a real loopback socket. Threads are not used as a
// substitute: the processes below are started, hard-killed and restarted, and
// the resulting state is checked for conservatism.
#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "fixture.hpp"
#include "test_executables.hpp"

using namespace fpm_test;
namespace fpm = fabric_partition_manager;

namespace {

// Bounded start-up synchronisation. This is not a test watchdog: the test has
// no timeout and will run to completion; this only waits for a child process to
// publish the port it bound.
bool wait_for_file(const std::filesystem::path& path, std::size_t attempts) {
  for (std::size_t attempt = 0; attempt < attempts; ++attempt) {
    std::error_code error;
    if (std::filesystem::exists(path, error)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return false;
}

std::string trim(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
    text.pop_back();
  }
  std::size_t start = 0;
  while (start < text.size() && (text[start] == ' ' || text[start] == '\n')) {
    ++start;
  }
  return text.substr(start);
}

// Extracts the value of the first line of the form key=value.
std::string field_of(const std::string& text, const std::string& key) {
  const std::string needle = key + "=";
  std::size_t position = text.find(needle);
  while (position != std::string::npos) {
    const bool at_line_start = position == 0 || text[position - 1] == '\n';
    if (at_line_start) {
      const std::size_t begin = position + needle.size();
      const std::size_t end = text.find('\n', begin);
      return trim(text.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
    }
    position = text.find(needle, position + 1);
  }
  return {};
}

struct CoordinatorFixture {
  explicit CoordinatorFixture(TempDirectory& workspace, const std::string& name)
      : store(workspace.file(name + "_store")),
        port_file(workspace.file(name + "_port")),
        ready_file(workspace.file(name + "_ready")) {}

  bool start() {
    std::error_code error;
    std::filesystem::remove(port_file, error);
    std::filesystem::remove(ready_file, error);
    const std::vector<std::string> arguments = {
        FPM_COORDINATOR_EXE,
        "--store=" + store.string(),
        "--bind=127.0.0.1",
        "--port=0",
        "--accept-port-file=" + port_file.string(),
        "--ready-file=" + ready_file.string(),
        "--run-seconds=0"};
    if (!process.start(arguments, store.parent_path())) {
      return false;
    }
    if (!wait_for_file(ready_file, 1200)) {
      return false;
    }
    const std::string text = trim(read_text_file(port_file));
    if (text.empty()) {
      return false;
    }
    port = static_cast<std::uint16_t>(std::stoul(text));
    return port != 0;
  }

  bool stop_hard() { return process.hard_kill(); }

  std::filesystem::path store;
  std::filesystem::path port_file;
  std::filesystem::path ready_file;
  std::uint16_t port = 0;
  BackgroundProcess process;
};

fpm::ClientOptions client_options_for(std::uint16_t port) {
  fpm::ClientOptions options;
  options.address = "127.0.0.1";
  options.port = port;
  return options;
}

}  // namespace

FPM_TEST(distributed, a_real_coordinator_process_answers_over_loopback) {
  TempDirectory workspace;
  const fpm::SyntheticFabric built = fabric({4}, 5, 1, 1, true);
  CoordinatorFixture coordinator(workspace, "roundtrip");
  FPM_CHECK_MSG(coordinator.start(), trim(coordinator.process.output()));

  fpm::PartitionClient client(client_options_for(coordinator.port));
  std::string error;
  FPM_CHECK_MSG(client.connect(fpm::PublisherId::from_validated("harness"),
                               fpm::PublisherBootId::generate(), fpm::CoordinatorEpoch{}, error),
                error);
  const auto adopted = client.adopt_topology(built.topology, error);
  FPM_CHECK_MSG(adopted.has_value(), error);
  FPM_CHECK(adopted->verdict == fpm::DecisionVerdict::Granted ||
            adopted->verdict == fpm::DecisionVerdict::Observed);
  const auto published = client.publish_evidence(built.evidence, error);
  FPM_CHECK_MSG(published.has_value(), error);
  FPM_EQ(published->verdict, fpm::DecisionVerdict::Granted);
  const auto assessment = client.assess(error);
  FPM_CHECK_MSG(assessment.has_value(), error);
  FPM_EQ(assessment->verdict, fpm::DecisionVerdict::Granted);
  FPM_EQ(assessment->partitions.size(), std::size_t{1});
  FPM_CHECK(assessment->confirmed);
  client.close();

  FPM_CHECK(coordinator.stop_hard());
  (void)coordinator.process.wait();
}

FPM_TEST(distributed, the_publisher_and_cli_processes_run_against_a_real_coordinator) {
  TempDirectory workspace;
  CoordinatorFixture coordinator(workspace, "tools");
  FPM_CHECK_MSG(coordinator.start(), trim(coordinator.process.output()));

  const std::vector<std::string> publish_arguments = {
      FPM_PUBLISHER_EXE,
      "--address=127.0.0.1",
      "--port=" + std::to_string(coordinator.port),
      "--publisher=process-publisher",
      "--groups=4",
      "--seed=11",
      "--count=1",
      "--validity-ticks=1000000"};
  const ProcessResult published = run_process(publish_arguments, workspace.path());
  FPM_CHECK_MSG(published.started, published.error);
  FPM_EQ(published.exit_code, 0);
  FPM_CHECK_MSG(published.output.find("verdict=GRANTED") != std::string::npos, published.output);

  const std::vector<std::string> status_arguments = {FPM_CLI_EXE,
                                                     "status",
                                                     "--address=127.0.0.1",
                                                     "--port=" + std::to_string(coordinator.port)};
  const ProcessResult status = run_process(status_arguments, workspace.path());
  FPM_CHECK_MSG(status.started, status.error);
  FPM_EQ(status.exit_code, 0);
  FPM_CHECK_MSG(!field_of(status.output, "epoch").empty(), status.output);
  FPM_EQ(field_of(status.output, "interrupted"), std::string("0"));

  const std::vector<std::string> assess_arguments = {FPM_CLI_EXE,
                                                     "assess",
                                                     "--address=127.0.0.1",
                                                     "--port=" + std::to_string(coordinator.port)};
  const ProcessResult assessed = run_process(assess_arguments, workspace.path());
  FPM_CHECK_MSG(assessed.started, assessed.error);
  FPM_EQ(assessed.exit_code, 0);
  FPM_CHECK_MSG(field_of(assessed.output, "verdict") == std::string("GRANTED"),
                assessed.output);

  FPM_CHECK(coordinator.stop_hard());
  (void)coordinator.process.wait();
}

FPM_TEST(distributed, a_hard_killed_coordinator_restarts_conservatively) {
  TempDirectory workspace;
  const fpm::SyntheticFabric built = fabric({4}, 5, 1, 1, true);
  CoordinatorFixture first(workspace, "restart");
  FPM_CHECK_MSG(first.start(), trim(first.process.output()));

  fpm::CoordinatorEpoch first_epoch;
  fpm::CoordinatorBootId first_boot;
  {
    fpm::PartitionClient client(client_options_for(first.port));
    std::string error;
    FPM_CHECK_MSG(client.connect(fpm::PublisherId::from_validated("harness"),
                                 fpm::PublisherBootId::generate(), fpm::CoordinatorEpoch{},
                                 error),
                  error);
    const auto adopted = client.adopt_topology(built.topology, error);
    FPM_CHECK(adopted.has_value());
    const auto published = client.publish_evidence(built.evidence, error);
    FPM_CHECK(published.has_value());
    FPM_EQ(published->verdict, fpm::DecisionVerdict::Granted);
    const auto assessment = client.assess(error);
    FPM_CHECK(assessment.has_value());
    FPM_EQ(assessment->verdict, fpm::DecisionVerdict::Granted);
    first_epoch = assessment->epoch;
    first_boot = assessment->boot;
    client.close();
  }

  // Hard kill: no orderly shutdown, no flush, no goodbye frame.
  FPM_CHECK(first.stop_hard());
  const int killed = first.process.wait();
  FPM_CHECK(killed != 0);

  CoordinatorFixture second(workspace, "restart");
  FPM_CHECK_MSG(second.start(), trim(second.process.output()));
  {
    fpm::PartitionClient client(client_options_for(second.port));
    std::string error;
    FPM_CHECK_MSG(client.connect(fpm::PublisherId::from_validated("harness"),
                                 fpm::PublisherBootId::generate(), fpm::CoordinatorEpoch{},
                                 error),
                  error);
    const auto status = client.status(error);
    FPM_CHECK_MSG(status.has_value(), error);
    FPM_CHECK(status->epoch.value() > first_epoch.value());
    FPM_NE(status->boot, first_boot);
    // Durable lineage survives the kill; live authority does not.
    FPM_CHECK(status->lineage_records >= 1u);
    FPM_EQ(status->evidence_bundles, std::uint64_t{0});
    FPM_CHECK(status->interrupted_authorities >= 1u);

    const auto fences = client.fences(error);
    FPM_CHECK(fences.has_value());

    // Re-assessing without fresh evidence cannot restore authority.
    fpm::RevalidationRequest request;
    request.attempt = attempt(1, "after-restart");
    request.expected_epoch = status->epoch;
    request.requester = fpm::Provenance::from_validated("harness");
    const auto revalidated = client.revalidate(request, error);
    FPM_CHECK_MSG(revalidated.has_value(), error);
    FPM_EQ(revalidated->verdict, fpm::DecisionVerdict::Indeterminate);

    // With fresh evidence the authority is re-derived and the interruption is
    // cleared. The rebuilt roster must be adopted again because the restarted
    // runtime has no roster until a topology is presented.
    const auto adopted = client.adopt_topology(built.topology, error);
    FPM_CHECK_MSG(adopted.has_value(), error);
    const auto published = client.publish_evidence(built.evidence, error);
    FPM_CHECK_MSG(published.has_value(), error);
    FPM_EQ(published->verdict, fpm::DecisionVerdict::Granted);
    fpm::RevalidationRequest second_request = request;
    second_request.attempt = attempt(2, "after-restart-2");
    second_request.expected_epoch = status->epoch;
    const auto restored = client.revalidate(second_request, error);
    FPM_CHECK_MSG(restored.has_value(), error);
    FPM_EQ(restored->verdict, fpm::DecisionVerdict::Granted);
    const auto cleared = client.status(error);
    FPM_CHECK(cleared.has_value());
    FPM_EQ(cleared->interrupted_authorities, std::uint64_t{0});
    client.close();
  }
  FPM_CHECK(second.stop_hard());
  (void)second.process.wait();
}

FPM_TEST(distributed, hard_killing_a_publisher_leaves_the_coordinator_consistent) {
  TempDirectory workspace;
  const fpm::SyntheticFabric built = fabric({4}, 5, 1, 1, true);
  CoordinatorFixture coordinator(workspace, "publisher-kill");
  FPM_CHECK_MSG(coordinator.start(), trim(coordinator.process.output()));

  // Seed the roster so the long-running publisher only has to publish.
  {
    fpm::PartitionClient client(client_options_for(coordinator.port));
    std::string error;
    FPM_CHECK_MSG(client.connect(fpm::PublisherId::from_validated("harness"),
                                 fpm::PublisherBootId::generate(), fpm::CoordinatorEpoch{},
                                 error),
                  error);
    FPM_CHECK(client.adopt_topology(built.topology, error).has_value());
    client.close();
  }

  BackgroundProcess publisher;
  const std::vector<std::string> arguments = {
      FPM_PUBLISHER_EXE,
      "--address=127.0.0.1",
      "--port=" + std::to_string(coordinator.port),
      "--publisher=killed-publisher",
      "--groups=4",
      "--seed=13",
      "--count=100000",
      "--interval-ms=10",
      "--validity-ticks=1000000"};
  FPM_CHECK(publisher.start(arguments, workspace.path()));
  bool progressed = false;
  for (std::size_t attempt = 0; attempt < 1200; ++attempt) {
    if (publisher.output().find("published") != std::string::npos) {
      progressed = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  FPM_CHECK_MSG(progressed, publisher.output());
  // The publisher streams observations, so it is still running here and can be
  // terminated in the middle of its sequence.
  FPM_CHECK_MSG(publisher.running(), publisher.output());
  FPM_CHECK(publisher.hard_kill());
  (void)publisher.wait();

  // The coordinator must still answer, and its state must remain internally
  // consistent after a publisher died mid-sequence.
  fpm::PartitionClient client(client_options_for(coordinator.port));
  std::string error;
  FPM_CHECK_MSG(client.connect(fpm::PublisherId::from_validated("harness-2"),
                               fpm::PublisherBootId::generate(), fpm::CoordinatorEpoch{},
                               error),
                error);
  const auto assessment = client.assess(error);
  FPM_CHECK_MSG(assessment.has_value(), error);
  FPM_EQ(assessment->partitions.size(), std::size_t{1});
  FPM_EQ(assessment->ordered_pairs,
         static_cast<std::uint64_t>(4 * 3));
  FPM_CHECK(assessment->ordered_unknown == 0u);
  client.close();

  FPM_CHECK(coordinator.stop_hard());
  (void)coordinator.process.wait();
}

FPM_TEST(distributed, a_stale_epoch_handshake_is_refused_over_a_real_socket) {
  TempDirectory workspace;
  CoordinatorFixture coordinator(workspace, "epoch");
  FPM_CHECK_MSG(coordinator.start(), trim(coordinator.process.output()));

  fpm::PartitionClient client(client_options_for(coordinator.port));
  std::string error;
  const bool connected =
      client.connect(fpm::PublisherId::from_validated("harness"),
                     fpm::PublisherBootId::generate(),
                     fpm::CoordinatorEpoch::from_value(9999), error);
  FPM_CHECK(!connected);
  FPM_CHECK(!error.empty());

  FPM_CHECK(coordinator.stop_hard());
  (void)coordinator.process.wait();
}

FPM_TEST(distributed, a_corrupt_store_stops_the_coordinator_with_a_diagnostic) {
  TempDirectory workspace;
  CoordinatorFixture coordinator(workspace, "corrupt");
  FPM_CHECK_MSG(coordinator.start(), trim(coordinator.process.output()));
  FPM_CHECK(coordinator.stop_hard());
  (void)coordinator.process.wait();

  const std::filesystem::path journal = coordinator.store / "partition.journal";
  std::vector<std::byte> image = read_binary_file(journal);
  FPM_CHECK(!image.empty());
  for (std::size_t index = 0; index < image.size(); index += 5) {
    image[index] = static_cast<std::byte>(static_cast<unsigned>(image[index]) ^ 0x5Au);
  }
  FPM_CHECK(write_binary_file(journal, image));

  const std::vector<std::string> arguments = {
      FPM_COORDINATOR_EXE,
      "--store=" + coordinator.store.string(),
      "--bind=127.0.0.1",
      "--port=0",
      "--accept-port-file=" + coordinator.port_file.string(),
      "--ready-file=" + coordinator.ready_file.string(),
      "--run-seconds=1"};
  const ProcessResult result = run_process(arguments, workspace.path());
  FPM_CHECK_MSG(result.started, result.error);
  FPM_EQ(result.exit_code, 2);
  FPM_CHECK_MSG(result.output.find("coordinator failed") != std::string::npos, result.output);
}
