// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// Adversarial durable images. Every truncated prefix of a valid store, every
// single-byte corruption of a valid store, and a set of hand-built impossible
// structures are loaded and the outcome is checked. The invariant under test is
// that a load either succeeds with a strict prefix of the committed records, or
// refuses; a mutated record must never be observable.
#include <algorithm>
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
  return bytes;
}

std::string text_of(const std::vector<std::byte>& bytes) {
  fpm::CanonicalReader reader(bytes.data(), bytes.size());
  std::string_view text;
  if (!reader.text(text)) {
    return {};
  }
  return std::string(text);
}

struct LoadOutcome {
  bool ok = false;
  bool torn = false;
  std::vector<std::string> texts;
  std::vector<fpm::ReasonCode> codes;
};

LoadOutcome load_journal(const std::filesystem::path& directory, const std::vector<std::byte>& image) {
  FPM_CHECK(write_binary_file(directory / "partition.journal", image));
  std::error_code error;
  std::filesystem::remove(directory / "partition.snapshot", error);
  fpm::DurableStore store(directory / "partition.journal", directory / "partition.snapshot",
                          test_limits());
  const fpm::StoreLoadResult result = store.load();
  LoadOutcome outcome;
  outcome.ok = result.ok;
  outcome.torn = result.torn_tail_recovered;
  for (const fpm::Reason& reason : result.reasons) {
    outcome.codes.push_back(reason.code);
  }
  if (result.ok) {
    for (const fpm::DurableRecord& record : store.records()) {
      outcome.texts.push_back(text_of(record.payload));
    }
  }
  return outcome;
}

std::vector<std::byte> build_journal(const std::filesystem::path& directory,
                                     const std::vector<std::string>& texts) {
  std::error_code error;
  std::filesystem::remove(directory / "partition.journal", error);
  std::filesystem::remove(directory / "partition.snapshot", error);
  fpm::DurableStore store(directory / "partition.journal", directory / "partition.snapshot",
                          test_limits());
  FPM_CHECK(store.load().ok);
  FPM_CHECK(store.open_for_append());
  std::uint64_t sequence = 0;
  for (const std::string& text : texts) {
    ++sequence;
    FPM_CHECK(store.append(fpm::DurableRecordType::LineageRecord, sequence, payload_of(text)));
  }
  store.close();
  return read_binary_file(directory / "partition.journal");
}

bool is_prefix_of(const std::vector<std::string>& candidate,
                  const std::vector<std::string>& original) {
  if (candidate.size() > original.size()) {
    return false;
  }
  for (std::size_t index = 0; index < candidate.size(); ++index) {
    if (!(candidate[index] == original[index])) {
      return false;
    }
  }
  return true;
}

}  // namespace

FPM_TEST(corruption, every_truncated_prefix_is_safe) {
  TempDirectory workspace;
  const std::vector<std::string> original = {"alpha", "beta", "gamma", "delta"};
  const std::vector<std::byte> image = build_journal(workspace.path(), original);
  FPM_CHECK(image.size() > 60);

  std::size_t recoveries = 0;
  std::size_t refusals = 0;
  for (std::size_t length = 0; length <= image.size(); ++length) {
    const std::vector<std::byte> truncated(image.begin(),
                                           image.begin() + static_cast<std::ptrdiff_t>(length));
    const LoadOutcome outcome = load_journal(workspace.path(), truncated);
    if (!outcome.ok) {
      // A store truncated inside its own header was never committed, so nothing
      // was ever acknowledged from it and it is refused rather than repaired.
      FPM_CHECK(length < 24);
      ++refusals;
      continue;
    }
    // A recovered store may only ever contain a strict prefix of the records
    // that were committed, in the order they were committed.
    FPM_CHECK(is_prefix_of(outcome.texts, original));
    if (length < image.size()) {
      ++recoveries;
    }
  }
  FPM_CHECK(recoveries > 0);
  FPM_CHECK(refusals <= 24);
}

FPM_TEST(corruption, every_single_byte_corruption_is_safe) {
  TempDirectory workspace;
  const std::vector<std::string> original = {"one", "two", "three"};
  const std::vector<std::byte> image = build_journal(workspace.path(), original);

  std::size_t accepted_prefixes = 0;
  std::size_t refusals = 0;
  for (std::size_t index = 0; index < image.size(); ++index) {
    for (const unsigned mask : {0x01u, 0x80u, 0xFFu}) {
      std::vector<std::byte> damaged = image;
      damaged[index] = static_cast<std::byte>(static_cast<unsigned>(damaged[index]) ^ mask);
      const LoadOutcome outcome = load_journal(workspace.path(), damaged);
      if (!outcome.ok) {
        ++refusals;
        continue;
      }
      FPM_CHECK(is_prefix_of(outcome.texts, original));
      ++accepted_prefixes;
    }
  }
  FPM_CHECK(refusals > 0);
  (void)accepted_prefixes;
}

FPM_TEST(corruption, an_empty_or_header_only_journal_is_an_empty_store) {
  TempDirectory workspace;
  const LoadOutcome empty = load_journal(workspace.path(), {});
  FPM_CHECK(empty.ok);
  FPM_EQ(empty.texts.size(), std::size_t{0});

  const std::vector<std::byte> image = build_journal(workspace.path(), {"one"});
  const std::vector<std::byte> header_only(image.begin(), image.begin() + 24);
  const LoadOutcome header = load_journal(workspace.path(), header_only);
  FPM_CHECK(header.ok);
  FPM_EQ(header.texts.size(), std::size_t{0});
}

FPM_TEST(corruption, a_record_type_outside_its_domain_is_refused) {
  TempDirectory workspace;
  const std::vector<std::byte> image = build_journal(workspace.path(), {"one"});
  for (const std::uint32_t raw : {0u, 7u, 99u, 0xFFFFu}) {
    const auto type = static_cast<std::uint16_t>(raw);
    std::vector<std::byte> damaged = image;
    damaged[24] = static_cast<std::byte>((type >> 8) & 0xFFu);
    damaged[25] = static_cast<std::byte>(type & 0xFFu);
    const LoadOutcome outcome = load_journal(workspace.path(), damaged);
    FPM_CHECK(!outcome.ok);
    FPM_CHECK(std::find(outcome.codes.begin(), outcome.codes.end(),
                        fpm::ReasonCode::StoreIntegrityFailure) != outcome.codes.end());
  }
}

FPM_TEST(corruption, a_reserved_field_that_is_not_zero_is_refused) {
  TempDirectory workspace;
  const std::vector<std::byte> image = build_journal(workspace.path(), {"one"});
  std::vector<std::byte> damaged = image;
  damaged[2] = std::byte{0xAA};
  const LoadOutcome outcome = load_journal(workspace.path(), damaged);
  FPM_CHECK(!outcome.ok);
}

FPM_TEST(corruption, an_impossible_payload_length_is_refused_before_any_allocation) {
  TempDirectory workspace;
  const std::vector<std::byte> image = build_journal(workspace.path(), {"one"});
  const std::vector<std::uint32_t> lengths = {0xFFFF'FFFFu, 0x8000'0000u, 0x1000'0000u};
  for (const std::uint32_t length : lengths) {
    std::vector<std::byte> damaged = image;
    damaged[28] = static_cast<std::byte>((length >> 24) & 0xFFu);
    damaged[29] = static_cast<std::byte>((length >> 16) & 0xFFu);
    damaged[30] = static_cast<std::byte>((length >> 8) & 0xFFu);
    damaged[31] = static_cast<std::byte>(length & 0xFFu);
    const LoadOutcome outcome = load_journal(workspace.path(), damaged);
    FPM_CHECK(!outcome.ok);
  }
}

FPM_TEST(corruption, a_duplicated_final_record_is_a_sequence_regression) {
  TempDirectory workspace;
  const std::vector<std::byte> image = build_journal(workspace.path(), {"one", "two"});
  const std::size_t first_total = 16 + payload_of("one").size() + 8;
  std::vector<std::byte> duplicated(image);
  duplicated.insert(duplicated.end(), image.begin() + 24,
                    image.begin() + 24 + static_cast<std::ptrdiff_t>(first_total));
  const LoadOutcome outcome = load_journal(workspace.path(), duplicated);
  FPM_CHECK(!outcome.ok);
  FPM_CHECK(std::find(outcome.codes.begin(), outcome.codes.end(),
                      fpm::ReasonCode::StoreSequenceRegression) != outcome.codes.end());
}

FPM_TEST(corruption, a_snapshot_with_a_stale_watermark_is_refused) {
  TempDirectory workspace;
  build_journal(workspace.path(), {"one", "two"});
  {
    fpm::DurableStore store(workspace.path() / "partition.journal",
                            workspace.path() / "partition.snapshot", test_limits());
    FPM_CHECK(store.load().ok);
    FPM_CHECK(store.open_for_append());
    FPM_CHECK(store.compact());
    store.close();
  }
  std::vector<std::byte> snapshot = read_binary_file(workspace.path() / "partition.snapshot");
  // The payload begins at offset 24: 8 bytes of watermark then 8 bytes of count.
  snapshot[24 + 7] = std::byte{0x01};
  fpm::Crc64 crc;
  crc.update(snapshot.data(), snapshot.size() - 8);
  const std::uint64_t value = crc.value() ^ 0xFFFF'FFFF'FFFF'FFFFull;
  for (std::size_t index = 0; index < 8; ++index) {
    snapshot[snapshot.size() - 8 + index] =
        static_cast<std::byte>((value >> ((7u - index) * 8u)) & 0xFFu);
  }
  FPM_CHECK(write_binary_file(workspace.path() / "partition.snapshot", snapshot));
  std::error_code error;
  std::filesystem::remove(workspace.path() / "partition.journal", error);
  fpm::DurableStore store(workspace.path() / "partition.journal",
                          workspace.path() / "partition.snapshot", test_limits());
  const fpm::StoreLoadResult result = store.load();
  FPM_CHECK(!result.ok);
  FPM_CHECK(has_reason_code(result.reasons, fpm::ReasonCode::StoreSequenceRegression));
}

FPM_TEST(corruption, an_excessive_declared_snapshot_record_count_is_refused) {
  TempDirectory workspace;
  build_journal(workspace.path(), {"one"});
  {
    fpm::DurableStore store(workspace.path() / "partition.journal",
                            workspace.path() / "partition.snapshot", test_limits());
    FPM_CHECK(store.load().ok);
    FPM_CHECK(store.open_for_append());
    FPM_CHECK(store.compact());
    store.close();
  }
  std::vector<std::byte> snapshot = read_binary_file(workspace.path() / "partition.snapshot");
  for (std::size_t index = 0; index < 8; ++index) {
    snapshot[24 + 8 + index] = std::byte{0xFF};
  }
  fpm::Crc64 crc;
  crc.update(snapshot.data(), snapshot.size() - 8);
  const std::uint64_t value = crc.value() ^ 0xFFFF'FFFF'FFFF'FFFFull;
  for (std::size_t index = 0; index < 8; ++index) {
    snapshot[snapshot.size() - 8 + index] =
        static_cast<std::byte>((value >> ((7u - index) * 8u)) & 0xFFu);
  }
  FPM_CHECK(write_binary_file(workspace.path() / "partition.snapshot", snapshot));
  std::error_code error;
  std::filesystem::remove(workspace.path() / "partition.journal", error);
  fpm::DurableStore store(workspace.path() / "partition.journal",
                          workspace.path() / "partition.snapshot", test_limits());
  const fpm::StoreLoadResult result = store.load();
  FPM_CHECK(!result.ok);
  FPM_CHECK(has_reason_code(result.reasons, fpm::ReasonCode::LimitExceededReason));
}

FPM_TEST(corruption, durable_records_with_invalid_enums_are_refused_by_the_runtime) {
  TempDirectory workspace;
  // A well-formed durable envelope holding a domain value that is not a legal
  // verdict must be refused by the codec rather than coerced.
  std::vector<std::byte> payload;
  fpm::CanonicalWriter writer(payload);
  writer.text_id(fpm::DecisionId::from_validated("d1"));
  writer.u8(200);
  fpm::DurableStore store(workspace.path() / "partition.journal",
                          workspace.path() / "partition.snapshot", test_limits());
  FPM_CHECK(store.load().ok);
  FPM_CHECK(store.open_for_append());
  FPM_CHECK(store.append(fpm::DurableRecordType::DecisionRecord, 1, payload));
  store.close();

  bool threw = false;
  try {
    fpm::RuntimeOptions options;
    options.store_directory = workspace.path();
    options.provenance = fpm::Provenance::from_validated("test");
    fpm::PartitionRuntime runtime(options);
  } catch (const fpm::PartitionError& error) {
    threw = true;
    FPM_CHECK(error.code() == fpm::ErrorCode::IntegrityFailure);
  }
  FPM_CHECK(threw);
}

FPM_TEST(corruption, durable_records_with_trailing_bytes_are_refused) {
  TempDirectory workspace;
  std::vector<std::byte> payload;
  fpm::CanonicalWriter writer(payload);
  writer.text_id(fpm::DecisionId::from_validated("d1"));
  writer.u8(0);
  writer.u8(0);
  writer.u8(0);
  writer.text_id(fpm::ScopeId::from_validated("fabric"));
  writer.u8(0xFF);
  fpm::DurableStore store(workspace.path() / "partition.journal",
                          workspace.path() / "partition.snapshot", test_limits());
  FPM_CHECK(store.load().ok);
  FPM_CHECK(store.open_for_append());
  FPM_CHECK(store.append(fpm::DurableRecordType::DecisionRecord, 1, payload));
  store.close();

  bool threw = false;
  try {
    fpm::RuntimeOptions options;
    options.store_directory = workspace.path();
    options.provenance = fpm::Provenance::from_validated("test");
    fpm::PartitionRuntime runtime(options);
  } catch (const fpm::PartitionError&) {
    threw = true;
  }
  FPM_CHECK(threw);
}
