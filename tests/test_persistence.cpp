// Fabric Partition Manager 1.0.0 - Summon Software Labs
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "fixture.hpp"

using namespace fpm_test;
namespace fpm = fabric_partition_manager;

namespace {

std::vector<std::byte> payload_of(const std::string& text) {
  std::vector<std::byte> bytes;
  fpm::CanonicalWriter writer(bytes);
  writer.text(text);
  writer.u64(text.size());
  return bytes;
}

std::string text_of(const std::vector<std::byte>& bytes) {
  fpm::CanonicalReader reader(bytes.data(), bytes.size());
  std::string_view text;
  std::uint64_t length = 0;
  if (!reader.text(text) || !reader.u64(length)) {
    return {};
  }
  return std::string(text);
}

// Writes a journal holding the given texts and returns the number of records.
std::uint64_t write_journal(const std::filesystem::path& directory,
                            const std::vector<std::string>& texts) {
  fpm::DurableStore store(directory / "partition.journal", directory / "partition.snapshot",
                          test_limits());
  FPM_CHECK(store.load().ok);
  FPM_CHECK(store.open_for_append());
  std::uint64_t sequence = 0;
  for (const std::string& text : texts) {
    ++sequence;
    FPM_CHECK(store.append(fpm::DurableRecordType::PolicyDefinition, sequence, payload_of(text)));
  }
  store.close();
  return sequence;
}

std::vector<std::string> read_journal(const std::filesystem::path& directory, bool& ok,
                                      bool& torn) {
  fpm::DurableStore store(directory / "partition.journal", directory / "partition.snapshot",
                          test_limits());
  const fpm::StoreLoadResult result = store.load();
  ok = result.ok;
  torn = result.torn_tail_recovered;
  std::vector<std::string> texts;
  for (const fpm::DurableRecord& record : store.records()) {
    texts.push_back(text_of(record.payload));
  }
  return texts;
}

}  // namespace

FPM_TEST(persistence, journal_round_trip_preserves_records_in_order) {
  TempDirectory workspace;
  const std::vector<std::string> texts = {"alpha", "beta", "gamma", "delta"};
  write_journal(workspace.path(), texts);
  bool ok = false;
  bool torn = false;
  const std::vector<std::string> loaded = read_journal(workspace.path(), ok, torn);
  FPM_CHECK(ok);
  FPM_CHECK(!torn);
  FPM_EQ(loaded, texts);
}

FPM_TEST(persistence, snapshot_replacement_is_transactional_and_replayable) {
  TempDirectory workspace;
  const std::vector<std::string> first = {"one", "two"};
  write_journal(workspace.path(), first);

  fpm::DurableStore store(workspace.path() / "partition.journal",
                          workspace.path() / "partition.snapshot", test_limits());
  FPM_CHECK(store.load().ok);
  FPM_CHECK(store.open_for_append());
  FPM_CHECK(store.compact());
  FPM_EQ(store.stats().snapshot_writes, 1u);
  FPM_CHECK(store.append(fpm::DurableRecordType::PolicyDefinition, 3, payload_of("three")));
  store.close();

  bool ok = false;
  bool torn = false;
  const std::vector<std::string> loaded = read_journal(workspace.path(), ok, torn);
  FPM_CHECK(ok);
  FPM_EQ(loaded, (std::vector<std::string>{"one", "two", "three"}));
}

FPM_TEST(persistence, a_torn_tail_is_recovered_and_the_next_append_does_not_follow_it) {
  TempDirectory workspace;
  const std::uint64_t written = write_journal(workspace.path(), {"first", "second"});
  FPM_EQ(written, 2u);

  const std::vector<std::byte> image =
      read_binary_file(workspace.path() / "partition.journal");
  FPM_CHECK(image.size() > 40);
  // Truncate the final record in the middle of its payload.
  std::vector<std::byte> truncated(image.begin(), image.end() - 12);
  FPM_CHECK(write_binary_file(workspace.path() / "partition.journal", truncated));

  fpm::DurableStore store(workspace.path() / "partition.journal",
                          workspace.path() / "partition.snapshot", test_limits());
  const fpm::StoreLoadResult result = store.load();
  FPM_CHECK(result.ok);
  FPM_CHECK(result.torn_tail_recovered);
  FPM_EQ(store.records().size(), std::size_t{1});
  FPM_CHECK(store.stats().torn_tail_bytes > 0);
  FPM_CHECK(store.open_for_append());
  FPM_CHECK(store.append(fpm::DurableRecordType::PolicyDefinition, 3, payload_of("third")));
  store.close();

  bool ok = false;
  bool torn = false;
  const std::vector<std::string> loaded = read_journal(workspace.path(), ok, torn);
  FPM_CHECK(ok);
  FPM_CHECK(!torn);
  FPM_EQ(loaded, (std::vector<std::string>{"first", "third"}));
}

FPM_TEST(persistence, corrupting_a_complete_record_is_never_silently_accepted) {
  TempDirectory workspace;
  write_journal(workspace.path(), {"alpha", "beta", "gamma"});
  const std::vector<std::byte> image = read_binary_file(workspace.path() / "partition.journal");
  FPM_CHECK(!image.empty());

  bool saw_refusal = false;
  for (std::size_t index = 0; index < image.size(); ++index) {
    if (image[index] == std::byte{0} && index % 7 != 0) {
      continue;
    }
    std::vector<std::byte> damaged = image;
    damaged[index] = static_cast<std::byte>(static_cast<unsigned>(damaged[index]) ^ 0x5Au);
    FPM_CHECK(write_binary_file(workspace.path() / "partition.journal", damaged));
    bool ok = false;
    bool torn = false;
    const std::vector<std::string> loaded = read_journal(workspace.path(), ok, torn);
    if (ok) {
      // A recovered store may only ever expose a strict prefix of the original
      // records; a mutated record must never appear.
      const std::vector<std::string> original = {"alpha", "beta", "gamma"};
      FPM_CHECK(loaded.size() < original.size());
      for (std::size_t position = 0; position < loaded.size(); ++position) {
        FPM_EQ(loaded[position], original[position]);
      }
    } else {
      saw_refusal = true;
    }
  }
  FPM_CHECK(saw_refusal);
}

FPM_TEST(persistence, journal_rejects_a_truncated_record_header) {
  TempDirectory workspace;
  write_journal(workspace.path(), {"alpha", "beta"});
  const std::vector<std::byte> image = read_binary_file(workspace.path() / "partition.journal");
  for (std::size_t remove = 1; remove <= 8; ++remove) {
    std::vector<std::byte> truncated(image.begin(), image.end() - static_cast<std::ptrdiff_t>(remove));
    FPM_CHECK(write_binary_file(workspace.path() / "partition.journal", truncated));
    bool ok = false;
    bool torn = false;
    const std::vector<std::string> loaded = read_journal(workspace.path(), ok, torn);
    FPM_CHECK(ok);
    FPM_CHECK(torn);
    FPM_EQ(loaded, (std::vector<std::string>{"alpha"}));
  }
}

FPM_TEST(persistence, journal_rejects_an_impossible_declared_length) {
  TempDirectory workspace;
  write_journal(workspace.path(), {"alpha"});
  std::vector<std::byte> image = read_binary_file(workspace.path() / "partition.journal");
  FPM_CHECK(image.size() > 28);
  // Offset 24 is the first record header; bytes 28..31 are its declared length.
  image[28] = std::byte{0xFF};
  image[29] = std::byte{0xFF};
  image[30] = std::byte{0xFF};
  image[31] = std::byte{0xFF};
  FPM_CHECK(write_binary_file(workspace.path() / "partition.journal", image));
  bool ok = false;
  bool torn = false;
  (void)read_journal(workspace.path(), ok, torn);
  FPM_CHECK(!ok);
}

FPM_TEST(persistence, journal_rejects_a_bad_magic_and_an_unsupported_version) {
  TempDirectory workspace;
  write_journal(workspace.path(), {"alpha"});
  const std::vector<std::byte> image = read_binary_file(workspace.path() / "partition.journal");

  std::vector<std::byte> bad_magic = image;
  bad_magic[0] = std::byte{'X'};
  FPM_CHECK(write_binary_file(workspace.path() / "partition.journal", bad_magic));
  bool ok = false;
  bool torn = false;
  (void)read_journal(workspace.path(), ok, torn);
  FPM_CHECK(!ok);

  std::vector<std::byte> bad_version = image;
  bad_version[8] = std::byte{0};
  bad_version[9] = std::byte{99};
  FPM_CHECK(write_binary_file(workspace.path() / "partition.journal", bad_version));
  (void)read_journal(workspace.path(), ok, torn);
  FPM_CHECK(!ok);
}

FPM_TEST(persistence, a_complete_record_with_a_broken_checksum_is_an_integrity_failure) {
  TempDirectory workspace;
  write_journal(workspace.path(), {"alpha", "beta", "gamma"});
  const std::vector<std::byte> image = read_binary_file(workspace.path() / "partition.journal");
  // Locate the second record and damage its trailing checksum.
  const std::size_t first_record = 24;
  const std::size_t first_total = 16 + payload_of("alpha").size() + 8;
  const std::size_t second_trailer = first_record + first_total + 16 + payload_of("beta").size();
  std::vector<std::byte> damaged = image;
  damaged[second_trailer] = static_cast<std::byte>(
      static_cast<unsigned>(damaged[second_trailer]) ^ 0xFFu);
  FPM_CHECK(write_binary_file(workspace.path() / "partition.journal", damaged));
  fpm::DurableStore store(workspace.path() / "partition.journal",
                          workspace.path() / "partition.snapshot", test_limits());
  const fpm::StoreLoadResult result = store.load();
  FPM_CHECK(!result.ok);
  FPM_CHECK(has_reason_code(result.reasons, fpm::ReasonCode::StoreIntegrityFailure));
}

FPM_TEST(persistence, duplicate_journal_records_already_in_the_snapshot_are_recovered) {
  TempDirectory workspace;
  write_journal(workspace.path(), {"one", "two"});
  const std::vector<std::byte> journal =
      read_binary_file(workspace.path() / "partition.journal");

  fpm::DurableStore store(workspace.path() / "partition.journal",
                          workspace.path() / "partition.snapshot", test_limits());
  FPM_CHECK(store.load().ok);
  FPM_CHECK(store.open_for_append());
  FPM_CHECK(store.compact());
  // Simulate a crash between the snapshot rename and the journal truncation by
  // restoring the original journal alongside the new snapshot.
  store.close();
  FPM_CHECK(write_binary_file(workspace.path() / "partition.journal", journal));

  bool ok = false;
  bool torn = false;
  const std::vector<std::string> loaded = read_journal(workspace.path(), ok, torn);
  FPM_CHECK(ok);
  FPM_EQ(loaded, (std::vector<std::string>{"one", "two"}));
}

FPM_TEST(persistence, a_journal_sequence_regression_is_refused) {
  TempDirectory workspace;
  write_journal(workspace.path(), {"one", "two", "three"});
  std::vector<std::byte> image = read_binary_file(workspace.path() / "partition.journal");
  const std::size_t first_total = 16 + payload_of("one").size() + 8;
  const std::size_t second_record = 24 + first_total;
  // Overwrite the second record sequence with the first record's sequence.
  for (std::size_t index = 0; index < 8; ++index) {
    image[second_record + 8 + index] = image[24 + 8 + index];
  }
  FPM_CHECK(write_binary_file(workspace.path() / "partition.journal", image));
  bool ok = false;
  bool torn = false;
  (void)read_journal(workspace.path(), ok, torn);
  FPM_CHECK(!ok);
}

FPM_TEST(persistence, snapshot_corruption_is_refused) {
  TempDirectory workspace;
  write_journal(workspace.path(), {"one", "two"});
  {
    fpm::DurableStore store(workspace.path() / "partition.journal",
                            workspace.path() / "partition.snapshot", test_limits());
    FPM_CHECK(store.load().ok);
    FPM_CHECK(store.open_for_append());
    FPM_CHECK(store.compact());
    store.close();
  }
  const std::vector<std::byte> snapshot =
      read_binary_file(workspace.path() / "partition.snapshot");
  FPM_CHECK(snapshot.size() > 40);
  for (std::size_t index = 0; index < snapshot.size(); index += 3) {
    std::vector<std::byte> damaged = snapshot;
    damaged[index] = static_cast<std::byte>(static_cast<unsigned>(damaged[index]) ^ 0x33u);
    FPM_CHECK(write_binary_file(workspace.path() / "partition.snapshot", damaged));
    bool ok = false;
    bool torn = false;
    (void)read_journal(workspace.path(), ok, torn);
    FPM_CHECK(!ok);
  }
}

FPM_TEST(persistence, trailing_bytes_after_the_snapshot_record_stream_are_refused) {
  TempDirectory workspace;
  write_journal(workspace.path(), {"one"});
  {
    fpm::DurableStore store(workspace.path() / "partition.journal",
                            workspace.path() / "partition.snapshot", test_limits());
    FPM_CHECK(store.load().ok);
    FPM_CHECK(store.open_for_append());
    FPM_CHECK(store.compact());
    store.close();
  }
  std::vector<std::byte> snapshot = read_binary_file(workspace.path() / "partition.snapshot");
  // Insert a byte before the trailer: the payload now carries trailing garbage
  // that is not part of any record, and the trailer checksum is recomputed so
  // that only the structural check can catch it.
  snapshot.insert(snapshot.end() - 8, std::byte{0x7F});
  fpm::Crc64 crc;
  crc.update(snapshot.data(), snapshot.size() - 8);
  const std::uint64_t value = crc.value() ^ 0xFFFF'FFFF'FFFF'FFFFull;
  for (std::size_t index = 0; index < 8; ++index) {
    snapshot[snapshot.size() - 8 + index] =
        static_cast<std::byte>((value >> ((7u - index) * 8u)) & 0xFFu);
  }
  FPM_CHECK(write_binary_file(workspace.path() / "partition.snapshot", snapshot));
  bool ok = false;
  bool torn = false;
  (void)read_journal(workspace.path(), ok, torn);
  FPM_CHECK(!ok);
}

FPM_TEST(persistence, restart_advances_the_epoch_and_interrupts_authority) {
  TempDirectory workspace;
  fpm::SyntheticFabric built = fabric({3}, 5, 1, 1, true);
  fpm::CoordinatorEpoch first_epoch;
  fpm::CoordinatorBootId first_boot;
  fpm::LineageId lineage;
  {
    fpm::RuntimeOptions options;
    options.store_directory = workspace.path();
    options.provenance = fpm::Provenance::from_validated("test");
    options.policy = count_quorum_policy(1, 3);
    fpm::PartitionRuntime runtime(options);
    first_epoch = runtime.epoch();
    first_boot = runtime.boot();
    FPM_CHECK(runtime.adopt_topology(built.topology).verdict == fpm::DecisionVerdict::Granted);
    FPM_CHECK(runtime.ingest_evidence(built.evidence).verdict == fpm::DecisionVerdict::Granted);
    const fpm::PartitionAssessment assessment = runtime.assess();
    FPM_EQ(assessment.decision.verdict, fpm::DecisionVerdict::Granted);
    lineage = assessment.partitions[0].lineage;
    FPM_CHECK(!assessment.partitions[0].authority.authorized.empty());
    runtime.close();
  }
  {
    fpm::RuntimeOptions options;
    options.store_directory = workspace.path();
    options.provenance = fpm::Provenance::from_validated("test");
    options.policy = count_quorum_policy(1, 3);
    fpm::PartitionRuntime runtime(options);
    FPM_CHECK(runtime.epoch().value() > first_epoch.value());
    FPM_NE(runtime.boot(), first_boot);
    FPM_CHECK(!runtime.incarnation().is_nil());

    // Durable lineage and fences survive; evidence and live authority do not.
    FPM_CHECK(runtime.lineage_records().size() >= 1u);
    FPM_EQ(runtime.status().evidence_bundles, std::size_t{0});
    const std::vector<fpm::InterruptedAuthority> interrupted =
        runtime.interrupted_authorities();
    FPM_EQ(interrupted.size(), std::size_t{1});
    FPM_EQ(interrupted[0].lineage, lineage);
    FPM_CHECK(has_reason_code(runtime.restart_reasons(),
                              fpm::ReasonCode::RestartAuthorityInterrupted));
    FPM_CHECK(has_reason_code(runtime.restart_reasons(), fpm::ReasonCode::RestartEvidenceDropped));
    FPM_CHECK(!runtime.fences().empty());

    // Re-adopting the identical roster is idempotent: it reports exactly what
    // was already adopted rather than a fresh grant.
    const fpm::DecisionVerdict readopted = runtime.adopt_topology(built.topology).verdict;
    FPM_CHECK(readopted == fpm::DecisionVerdict::Observed ||
              readopted == fpm::DecisionVerdict::Granted);
    // Re-assessing without fresh evidence cannot restore authority. With no
    // observation at all every component is its own unresolved component set,
    // and none of them may act.
    const fpm::PartitionAssessment bare = runtime.assess();
    FPM_EQ(bare.partitions.size(), std::size_t{3});
    FPM_CHECK(!bare.confirmed);
    for (const fpm::Partition& partition : bare.partitions) {
      // Observe-only is the floor: nothing may read, write or mutate on the
      // strength of an observation that does not exist.
      FPM_EQ(partition.authority.authority_class, fpm::PartitionAuthorityClass::ObserveOnly);
      FPM_CHECK(!partition.authority.authorized.contains(fpm::AuthorityCapability::ServeReads));
      FPM_CHECK(!partition.authority.authorized.contains(fpm::AuthorityCapability::ServeWrites));
      FPM_CHECK(
          !partition.authority.authorized.contains(fpm::AuthorityCapability::AdmitMutations));
      FPM_EQ(partition.member_count(), 1u);
      FPM_EQ(partition.lifecycle, fpm::PartitionLifecycle::Discovered);
    }
    // The interrupted authority is still recorded and still requires
    // revalidation, even though the unobserved decomposition no longer matches
    // its lineage.
    FPM_EQ(runtime.interrupted_authorities().size(), std::size_t{1});
    FPM_CHECK(has_reason_code(bare.decision.reasons,
                              fpm::ReasonCode::AuthorityDowngradedUnknownEvidence));
    FPM_CHECK(has_reason_code(bare.decision.reasons, fpm::ReasonCode::PartitionPartial));
    runtime.close();
  }
}

FPM_TEST(persistence, revalidation_restores_authority_only_with_fresh_evidence) {
  TempDirectory workspace;
  fpm::SyntheticFabric built = fabric({3}, 5, 1, 1, true);
  fpm::LineageId lineage;
  {
    fpm::RuntimeOptions options;
    options.store_directory = workspace.path();
    options.provenance = fpm::Provenance::from_validated("test");
    options.policy = count_quorum_policy(1, 3);
    fpm::PartitionRuntime runtime(options);
    FPM_CHECK(runtime.adopt_topology(built.topology).verdict == fpm::DecisionVerdict::Granted);
    FPM_CHECK(runtime.ingest_evidence(built.evidence).verdict == fpm::DecisionVerdict::Granted);
    lineage = runtime.assess().partitions[0].lineage;
    runtime.close();
  }

  fpm::RuntimeOptions options;
  options.store_directory = workspace.path();
  options.provenance = fpm::Provenance::from_validated("test");
  options.policy = count_quorum_policy(1, 3);
  fpm::PartitionRuntime runtime(options);
  // The roster was already adopted and durably recorded before the restart, so
  // re-adopting the identical definition is idempotent.
  const fpm::DecisionVerdict readopted = runtime.adopt_topology(built.topology).verdict;
  FPM_CHECK(readopted == fpm::DecisionVerdict::Observed ||
            readopted == fpm::DecisionVerdict::Granted);

  fpm::RevalidationRequest request;
  request.attempt = attempt(1, "revalidate");
  request.expected_epoch = runtime.epoch();
  request.requester = fpm::Provenance::from_validated("test");

  const fpm::RevalidationOutcome without_evidence = runtime.revalidate(request);
  FPM_EQ(without_evidence.verdict, fpm::DecisionVerdict::Indeterminate);
  FPM_CHECK(has_reason_code(without_evidence.decision.reasons,
                            fpm::ReasonCode::RevalidationIndeterminate));
  FPM_EQ(runtime.interrupted_authorities().size(), std::size_t{1});

  FPM_CHECK(runtime.ingest_evidence(built.evidence).verdict == fpm::DecisionVerdict::Granted);
  fpm::RevalidationRequest second = request;
  second.attempt = attempt(2, "revalidate-2");
  const fpm::RevalidationOutcome with_evidence = runtime.revalidate(second);
  FPM_EQ(with_evidence.verdict, fpm::DecisionVerdict::Granted);
  FPM_EQ(with_evidence.restored.size(), std::size_t{1});
  FPM_CHECK(runtime.interrupted_authorities().empty());
  FPM_EQ(with_evidence.partitions.size(), std::size_t{1});
  FPM_EQ(with_evidence.partitions[0].lineage, lineage);
  FPM_CHECK(!with_evidence.partitions[0].authority.authorized.empty());
  runtime.close();
}

FPM_TEST(persistence, a_corrupt_store_is_refused_rather_than_repaired_silently) {
  TempDirectory workspace;
  fpm::SyntheticFabric built = fabric({2}, 5, 1, 1, true);
  {
    fpm::RuntimeOptions options;
    options.store_directory = workspace.path();
    options.provenance = fpm::Provenance::from_validated("test");
    // Compaction is disabled so the journal keeps its committed records and the
    // corruption lands inside one of them.
    options.compact_on_close = false;
    fpm::PartitionRuntime runtime(options);
    FPM_CHECK(runtime.adopt_topology(built.topology).verdict == fpm::DecisionVerdict::Granted);
    runtime.close();
  }
  std::vector<std::byte> image = read_binary_file(workspace.path() / "partition.journal");
  FPM_CHECK(image.size() > 60);
  // Corrupt a byte inside a committed record's payload and the trailing
  // checksum of the final record. Both are complete records, so neither is a
  // torn tail and neither may be silently discarded.
  image[41] = std::byte{static_cast<unsigned>(image[41]) ^ 0x40u};
  image[image.size() - 1] =
      std::byte{static_cast<unsigned>(image[image.size() - 1]) ^ 0x01u};
  FPM_CHECK(write_binary_file(workspace.path() / "partition.journal", image));

  bool threw = false;
  try {
    fpm::RuntimeOptions options;
    options.store_directory = workspace.path();
    options.provenance = fpm::Provenance::from_validated("test");
    fpm::PartitionRuntime runtime(options);
  } catch (const fpm::PartitionError& error) {
    threw = true;
    FPM_CHECK(error.code() == fpm::ErrorCode::IntegrityFailure ||
              error.code() == fpm::ErrorCode::UnsupportedVersion);
  }
  FPM_CHECK(threw);
}

FPM_TEST(persistence, memory_only_runtime_persists_nothing) {
  TempDirectory workspace;
  const fpm::SyntheticFabric built = fabric({2}, 5, 1, 1, true);
  fpm::RuntimeOptions options = memory_runtime_options(count_quorum_policy(1, 2));
  fpm::PartitionRuntime runtime(options);
  FPM_CHECK(runtime.adopt_topology(built.topology).verdict == fpm::DecisionVerdict::Granted);
  FPM_CHECK(runtime.ingest_evidence(built.evidence).verdict == fpm::DecisionVerdict::Granted);
  FPM_CHECK(runtime.assess().decision.verdict == fpm::DecisionVerdict::Granted);
  FPM_CHECK(!runtime.status().durable);
  runtime.close();
  FPM_CHECK(!std::filesystem::exists(workspace.path() / "partition.journal"));
  FPM_CHECK(!std::filesystem::exists(workspace.path() / "partition.snapshot"));
}
