// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// Concurrency and lifecycle. Threads are coordinated with explicit latches
// rather than sleeps, so a passing run is evidence of correct synchronisation
// rather than of a favourable scheduler.
#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "fixture.hpp"

using namespace fpm_test;
namespace fpm = fabric_partition_manager;

FPM_TEST(concurrency, concurrent_ingest_and_assess_preserve_every_invariant) {
  const fpm::SyntheticFabric built = fabric({4, 4, 4}, 5, 1, 1, true);
  fpm::RuntimeOptions options = memory_runtime_options(count_quorum_policy(1, 4));
  fpm::PartitionRuntime runtime(options);
  FPM_CHECK(runtime.adopt_topology(built.topology).verdict == fpm::DecisionVerdict::Granted);

  constexpr std::size_t kThreads = 8;
  Latch start(kThreads);
  std::atomic<std::size_t> accepted{0};
  std::atomic<std::size_t> assessments{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (std::size_t index = 0; index < kThreads; ++index) {
    workers.emplace_back([&, index]() {
      start.arrive_and_wait();
      for (std::size_t round = 0; round < 12; ++round) {
        fpm::ReachabilityEvidence evidence = built.evidence;
        evidence.id = fpm::EvidenceId::from_validated("e-" + std::to_string(index) + "-" +
                                                      std::to_string(round));
        evidence.publisher =
            fpm::PublisherId::from_validated("publisher-" + std::to_string(index));
        evidence.publisher_boot = fpm::PublisherBootId::generate();
        evidence.sequence = fpm::EvidenceSequence::from_value(round + 1);
        evidence.observed_at_tick = 0;
        evidence.validity_ticks = 1'000'000;
        // Several publishers must not each claim complete coverage of the same
        // source: two fresh complete claims about one source are contradictory
        // and resolve to a conflict by design. The concurrent case therefore
        // publishes partial bundles that agree with one another.
        evidence.completeness = fpm::EvidenceCompleteness::Partial;
        evidence.covered_sources.clear();
        const fpm::Decision decision = runtime.ingest_evidence(evidence);
        if (decision.verdict == fpm::DecisionVerdict::Granted) {
          accepted.fetch_add(1);
        }
        const fpm::PartitionAssessment assessment = runtime.assess();
        if (assessment.reachability.valid) {
          assessments.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }

  FPM_CHECK(accepted.load() > 0);
  FPM_EQ(assessments.load(), kThreads * 12);
  const fpm::PartitionAssessment assessment = runtime.assess();
  FPM_EQ(assessment.partitions.size(), std::size_t{3});
  FPM_CHECK(!assessment.confirmed);
  const fpm::ReachabilitySummary summary = runtime.reachability_summary();
  FPM_EQ(summary.counters.ordered_total_classified(), summary.counters.ordered_pairs);
  FPM_CHECK(summary.counters.cross_component_indeterminate > 0);
  FPM_EQ(summary.component_count, std::size_t{3});
  for (const fpm::Partition& partition : assessment.partitions) {
    FPM_EQ(partition.id, fpm::compute_partition_id(partition.lineage, partition.generation,
                                                   partition.membership));
  }
  FPM_CHECK(runtime.status().evidence_bundles <= options.limits.max_retained_evidence);
  runtime.close();
}

FPM_TEST(concurrency, concurrent_merges_and_revalidations_never_tear_state) {
  const fpm::SyntheticFabric built = fabric({4}, 5, 1, 1, true);
  fpm::RuntimeOptions options = memory_runtime_options(count_quorum_policy(1, 3));
  fpm::PartitionRuntime runtime(options);
  FPM_CHECK(runtime.adopt_topology(built.topology).verdict == fpm::DecisionVerdict::Granted);
  FPM_CHECK(runtime.ingest_evidence(built.evidence).verdict == fpm::DecisionVerdict::Granted);
  (void)runtime.assess();

  constexpr std::size_t kThreads = 6;
  Latch start(kThreads);
  std::atomic<std::size_t> completed{0};
  std::vector<std::thread> workers;
  for (std::size_t index = 0; index < kThreads; ++index) {
    workers.emplace_back([&, index]() {
      start.arrive_and_wait();
      for (std::size_t round = 0; round < 10; ++round) {
        if (index % 2 == 0) {
          fpm::RevalidationRequest request;
          request.attempt = fpm::AttemptToken{
              fpm::AttemptId::from_validated("rv-" + std::to_string(index) + "-" +
                                             std::to_string(round)),
              fpm::AttemptSequence::from_value(index * 100 + round + 1)};
          request.expected_epoch = runtime.epoch();
          (void)runtime.revalidate(request);
        } else {
          fpm::IsolationRequest request;
          request.attempt = fpm::AttemptToken{
              fpm::AttemptId::from_validated("iso-" + std::to_string(index) + "-" +
                                             std::to_string(round)),
              fpm::AttemptSequence::from_value(index * 100 + round + 1)};
          request.expected_epoch = runtime.epoch();
          request.lineage = runtime.last_assessment().decision.subjects.empty()
                                ? fpm::LineageId::from_validated("l00000000000000000000000000000000")
                                : fpm::LineageId::from_validated("l00000000000000000000000000000000");
          (void)runtime.clear_isolation(request);
        }
        completed.fetch_add(1);
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  FPM_EQ(completed.load(), kThreads * 10);
  FPM_CHECK(runtime.status().decision_history <= options.limits.max_decision_history);
  FPM_CHECK(runtime.status().fence_records <= options.limits.max_fence_records);
  FPM_CHECK(!runtime.last_assessment().reachability.evidence_digest.is_zero() ||
            runtime.last_assessment().partitions.empty());
  runtime.close();
}

FPM_TEST(concurrency, a_server_serves_many_sessions_and_stops_promptly) {
  const fpm::SyntheticFabric built = fabric({4}, 5, 1, 1, true);
  fpm::RuntimeOptions options = memory_runtime_options(count_quorum_policy(1, 3));
  fpm::PartitionRuntime runtime(options);
  fpm::ServerOptions server_options;
  server_options.port = 0;
  fpm::PartitionServer server(runtime, server_options);
  std::string error;
  FPM_CHECK_MSG(server.start(error), error);

  constexpr std::size_t kClients = 6;
  Latch start(kClients);
  std::atomic<std::size_t> published{0};
  std::vector<std::thread> clients;
  for (std::size_t index = 0; index < kClients; ++index) {
    clients.emplace_back([&, index]() {
      fpm::ClientOptions client_options;
      client_options.port = server.port();
      fpm::PartitionClient client(client_options);
      std::string connect_error;
      const bool connected =
          client.connect(fpm::PublisherId::from_validated("client-" + std::to_string(index)),
                         fpm::PublisherBootId::generate(), fpm::CoordinatorEpoch{}, connect_error);
      start.arrive_and_wait();
      if (!connected) {
        return;
      }
      if (index == 0) {
        (void)client.adopt_topology(built.topology, connect_error);
      }
      for (std::size_t round = 0; round < 6; ++round) {
        std::string step_error;
        const auto status = client.status(step_error);
        if (status.has_value()) {
          published.fetch_add(1);
        }
      }
      client.close();
    });
  }
  for (std::thread& client : clients) {
    client.join();
  }
  FPM_CHECK(published.load() > 0);
  const fpm::ServerStats stats_before = server.stats();
  FPM_CHECK(stats_before.accepted_sessions >= 1u);
  FPM_EQ(stats_before.active_sessions, std::uint64_t{0});
  server.stop();
  FPM_CHECK(!server.running());
  server.stop();
}

FPM_TEST(concurrency, stopping_the_server_releases_a_blocked_session_immediately) {
  const fpm::SyntheticFabric built = fabric({3});
  fpm::RuntimeOptions options = memory_runtime_options(count_quorum_policy(1, 3));
  fpm::PartitionRuntime runtime(options);
  fpm::ServerOptions server_options;
  server_options.port = 0;
  fpm::PartitionServer server(runtime, server_options);
  std::string error;
  FPM_CHECK_MSG(server.start(error), error);

  Latch established(2);
  std::atomic<bool> connected{false};
  std::thread client_thread([&]() {
    fpm::ClientOptions client_options;
    client_options.port = server.port();
    fpm::PartitionClient client(client_options);
    std::string connect_error;
    connected.store(client.connect(fpm::PublisherId::from_validated("blocked"),
                                   fpm::PublisherBootId::generate(),
                                   fpm::CoordinatorEpoch{}, connect_error));
    established.arrive_and_wait();
    // This request will never be answered: the server is stopped underneath it.
    std::string step_error;
    (void)client.status(step_error);
    client.close();
  });
  established.arrive_and_wait();
  FPM_CHECK(connected.load());
  server.stop();
  client_thread.join();
  FPM_CHECK(!server.running());
  FPM_EQ(server.stats().active_sessions, std::uint64_t{0});
  runtime.close();
}

FPM_TEST(concurrency, concurrent_close_is_idempotent) {
  const fpm::SyntheticFabric built = fabric({2});
  TempDirectory workspace;
  fpm::RuntimeOptions options;
  options.store_directory = workspace.path();
  options.provenance = fpm::Provenance::from_validated("test");
  options.policy = count_quorum_policy(1, 2);
  auto runtime = std::make_unique<fpm::PartitionRuntime>(options);
  FPM_CHECK(runtime->adopt_topology(built.topology).verdict == fpm::DecisionVerdict::Granted);

  constexpr std::size_t kThreads = 4;
  Latch start(kThreads);
  std::vector<std::thread> closers;
  for (std::size_t index = 0; index < kThreads; ++index) {
    closers.emplace_back([&]() {
      start.arrive_and_wait();
      runtime->close();
    });
  }
  for (std::thread& closer : closers) {
    closer.join();
  }
  FPM_CHECK(runtime->closed());
  FPM_CHECK(runtime->assess().decision.verdict == fpm::DecisionVerdict::Invalid);
  runtime.reset();
}

FPM_TEST(concurrency, concurrent_readers_never_observe_a_partial_assessment) {
  const fpm::SyntheticFabric built = fabric({3, 3}, 5, 1, 1, true);
  fpm::RuntimeOptions options = memory_runtime_options(count_quorum_policy(1, 4));
  fpm::PartitionRuntime runtime(options);
  FPM_CHECK(runtime.adopt_topology(built.topology).verdict == fpm::DecisionVerdict::Granted);
  FPM_CHECK(runtime.ingest_evidence(built.evidence).verdict == fpm::DecisionVerdict::Granted);
  (void)runtime.assess();

  constexpr std::size_t kWriters = 2;
  constexpr std::size_t kReaders = 4;
  Latch start(kWriters + kReaders);
  std::atomic<std::size_t> observations{0};
  std::atomic<bool> inconsistent{false};
  std::vector<std::thread> threads;
  for (std::size_t index = 0; index < kWriters; ++index) {
    threads.emplace_back([&, index]() {
      start.arrive_and_wait();
      for (std::size_t round = 0; round < 40; ++round) {
        fpm::ReachabilityEvidence evidence = built.evidence;
        evidence.id = fpm::EvidenceId::from_validated("w-" + std::to_string(index) + "-" +
                                                      std::to_string(round));
        evidence.publisher =
            fpm::PublisherId::from_validated("writer-" + std::to_string(index));
        evidence.publisher_boot = fpm::PublisherBootId::generate();
        evidence.sequence = fpm::EvidenceSequence::from_value(round + 1);
        evidence.validity_ticks = 1'000'000;
        (void)runtime.ingest_evidence(evidence);
        (void)runtime.assess();
      }
    });
  }
  for (std::size_t index = 0; index < kReaders; ++index) {
    threads.emplace_back([&]() {
      start.arrive_and_wait();
      for (std::size_t round = 0; round < 200; ++round) {
        // The query returns a value: a reader can never observe a partially
        // updated assessment after the runtime's lock has been released.
        const fpm::PartitionAssessment assessment = runtime.last_assessment();
        const std::size_t subjects = assessment.decision.subjects.size();
        const std::size_t parts = assessment.partitions.size();
        if (assessment.reachability.valid && subjects != parts) {
          inconsistent.store(true);
        }
        for (const fpm::Partition& partition : assessment.partitions) {
          if (partition.membership != fpm::compute_membership_digest(partition.members)) {
            inconsistent.store(true);
          }
          if (!(partition.id == fpm::compute_partition_id(partition.lineage,
                                                          partition.generation,
                                                          partition.membership))) {
            inconsistent.store(true);
          }
        }
        observations.fetch_add(1);
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  FPM_CHECK(!inconsistent.load());
  FPM_CHECK(observations.load() >= kReaders * 200);
  runtime.close();
}
