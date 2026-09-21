// Fabric Partition Manager 1.0.0 - Summon Software Labs
#include <string>
#include <vector>

#include "fixture.hpp"

using namespace fpm_test;
namespace fpm = fabric_partition_manager;

FPM_TEST(identity, sha256_matches_published_vectors) {
  FPM_EQ(fpm::sha256_text("").hex(),
         std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  FPM_EQ(fpm::sha256_text("abc").hex(),
         std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  FPM_EQ(fpm::sha256_text("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq").hex(),
         std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  const std::string million(1000000, 'a');
  FPM_EQ(fpm::sha256_text(million).hex(),
         std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

FPM_TEST(identity, sha256_streaming_equals_one_shot) {
  const std::string text = "fabric partition manager streaming digest equivalence";
  fpm::Sha256 streaming;
  for (const char value : text) {
    streaming.update_u8(static_cast<std::uint8_t>(value));
  }
  FPM_EQ(streaming.finish(), fpm::sha256_text(text));
}

FPM_TEST(identity, crc64_matches_published_check_value) {
  FPM_EQ(fpm::crc64_text("123456789"), 0x995DC9BBDF1939FAull);
  FPM_EQ(fpm::crc64_text(""), fpm::crc64_initial ^ 0xFFFF'FFFF'FFFF'FFFFull);
  fpm::Crc64 streaming;
  streaming.update("1234", 4);
  streaming.update("56789", 5);
  FPM_EQ(streaming.value() ^ 0xFFFF'FFFF'FFFF'FFFFull, 0x995DC9BBDF1939FAull);
}

FPM_TEST(identity, text_identifiers_reject_malformed_encodings) {
  FPM_CHECK(!fpm::ComponentId::parse("").has_value());
  FPM_CHECK(!fpm::ComponentId::parse("Node-1").has_value());
  FPM_CHECK(!fpm::ComponentId::parse("node/1").has_value());
  FPM_CHECK(!fpm::ComponentId::parse("..").has_value());
  FPM_CHECK(!fpm::ComponentId::parse("node\\1").has_value());
  FPM_CHECK(!fpm::ComponentId::parse("node 1").has_value());
  FPM_CHECK(!fpm::ComponentId::parse("-node").has_value());
  FPM_CHECK(!fpm::ComponentId::parse("node-").has_value());
  FPM_CHECK(!fpm::ComponentId::parse("node*").has_value());
  FPM_CHECK(!fpm::ComponentId::parse(std::string(97, 'a')).has_value());
  FPM_CHECK(fpm::ComponentId::parse("node-0000000").has_value());
  FPM_CHECK(fpm::ComponentId::parse("a").has_value());
  FPM_CHECK(fpm::ComponentId::parse("fabric:rack-1.unit_2").has_value());
}

FPM_TEST(identity, identifiers_of_different_domains_are_distinct_types) {
  // A PartitioId and a LineageId with the same text are different values that
  // cannot be assigned across domains. The compile-time half of this property
  // is enforced by the type system; this test pins the runtime rendering.
  const auto partition = fpm::PartitionId::parse("p0123456789abcdef0123456789abcdef");
  const auto lineage = fpm::LineageId::parse("p0123456789abcdef0123456789abcdef");
  FPM_CHECK(partition.has_value());
  FPM_CHECK(lineage.has_value());
  FPM_EQ(partition->view(), lineage->view());
}

FPM_TEST(identity, binary_identifiers_round_trip_and_reject_corruption) {
  const fpm::CoordinatorBootId boot = fpm::CoordinatorBootId::generate();
  FPM_CHECK(!boot.is_nil());
  const auto parsed = fpm::CoordinatorBootId::parse_hex(boot.hex());
  FPM_CHECK(parsed.has_value());
  FPM_EQ(*parsed, boot);
  FPM_CHECK(!fpm::CoordinatorBootId::parse_hex("").has_value());
  FPM_CHECK(!fpm::CoordinatorBootId::parse_hex("00").has_value());
  FPM_CHECK(!fpm::CoordinatorBootId::parse_hex(std::string(31, 'a')).has_value());
  FPM_CHECK(!fpm::CoordinatorBootId::parse_hex(std::string(32, 'A')).has_value());
  std::string with_g = boot.hex();
  with_g[3] = 'g';
  FPM_CHECK(!fpm::CoordinatorBootId::parse_hex(with_g).has_value());
}

FPM_TEST(identity, counters_refuse_to_wrap) {
  const auto near_top = fpm::CoordinatorEpoch::from_value(fpm::CoordinatorEpoch::max_value);
  FPM_CHECK(!near_top.next().has_value());
  const auto small = fpm::CoordinatorEpoch::from_value(5);
  FPM_CHECK(small.next().has_value());
  FPM_EQ(small.next()->value(), 6u);
  FPM_CHECK(!small.advance(fpm::CoordinatorEpoch::max_value).has_value());
  FPM_CHECK(small.advance(10).has_value());
}

FPM_TEST(identity, canonical_encoding_is_total_and_sticky) {
  std::vector<std::byte> bytes;
  fpm::CanonicalWriter writer(bytes);
  writer.u8(0xAB);
  writer.u16(0xBEEF);
  writer.u32(0xDEADBEEF);
  writer.u64(0x0123456789ABCDEFull);
  writer.text("hello");
  writer.boolean(true);

  fpm::CanonicalReader reader(bytes.data(), bytes.size());
  std::uint8_t a = 0;
  std::uint16_t b = 0;
  std::uint32_t c = 0;
  std::uint64_t d = 0;
  std::string_view text;
  bool flag = false;
  FPM_CHECK(reader.u8(a));
  FPM_CHECK(reader.u16(b));
  FPM_CHECK(reader.u32(c));
  FPM_CHECK(reader.u64(d));
  FPM_CHECK(reader.text(text));
  FPM_CHECK(reader.boolean(flag));
  FPM_CHECK(reader.exhausted());
  FPM_EQ(a, 0xABu);
  FPM_EQ(b, 0xBEEFu);
  FPM_EQ(c, 0xDEADBEEFu);
  FPM_EQ(d, 0x0123456789ABCDEFull);
  FPM_EQ(std::string(text), std::string("hello"));
  FPM_CHECK(flag);

  // Every truncated prefix must fail rather than yield a shorter valid value.
  for (std::size_t length = 0; length < bytes.size(); ++length) {
    fpm::CanonicalReader truncated(bytes.data(), length);
    std::uint8_t first = 0;
    if (length == 0) {
      FPM_CHECK(!truncated.u8(first));
      continue;
    }
    FPM_CHECK(truncated.u8(first));
    std::uint16_t second = 0;
    std::uint32_t third = 0;
    std::uint64_t fourth = 0;
    std::string_view value;
    bool boolean = false;
    const bool complete = truncated.u16(second) && truncated.u32(third) &&
                          truncated.u64(fourth) && truncated.text(value) &&
                          truncated.boolean(boolean) && truncated.exhausted();
    FPM_CHECK(!complete);
  }
}

FPM_TEST(identity, canonical_reader_refuses_impossible_lengths) {
  std::vector<std::byte> bytes;
  fpm::CanonicalWriter writer(bytes);
  writer.u32(0xFFFF'FFFFu);
  writer.u8(1);
  fpm::CanonicalReader reader(bytes.data(), bytes.size());
  std::string_view text;
  FPM_CHECK(!reader.text(text));
  FPM_CHECK(reader.failed());
  // Failure is sticky: a later field cannot be read from a poisoned reader.
  std::uint8_t value = 0;
  FPM_CHECK(!reader.u8(value));

  std::vector<std::byte> bounded;
  fpm::CanonicalWriter bounded_writer(bounded);
  bounded_writer.text("0123456789");
  fpm::CanonicalReader bounded_reader(bounded.data(), bounded.size());
  std::string_view limited;
  FPM_CHECK(!bounded_reader.bounded_text(4, limited));
}

FPM_TEST(identity, canonical_reader_rejects_non_boolean_and_bad_identifiers) {
  std::vector<std::byte> bytes;
  fpm::CanonicalWriter writer(bytes);
  writer.u8(7);
  fpm::CanonicalReader reader(bytes.data(), bytes.size());
  bool value = false;
  FPM_CHECK(!reader.boolean(value));

  std::vector<std::byte> bad;
  fpm::CanonicalWriter bad_writer(bad);
  bad_writer.text("UPPERCASE");
  fpm::CanonicalReader bad_reader(bad.data(), bad.size());
  fpm::ComponentId id;
  FPM_CHECK(!bad_reader.text_id(id));
}

FPM_TEST(identity, membership_identity_is_order_independent) {
  std::vector<fpm::ComponentId> forward;
  for (std::size_t index = 0; index < 8; ++index) {
    forward.push_back(component(index));
  }
  std::vector<fpm::ComponentId> reversed(forward.rbegin(), forward.rend());
  std::vector<fpm::ComponentId> shuffled = {forward[3], forward[0], forward[7], forward[1],
                                            forward[6], forward[2], forward[5], forward[4]};
  std::vector<fpm::ComponentId> sorted_forward = forward;
  std::vector<fpm::ComponentId> sorted_reversed = reversed;
  std::vector<fpm::ComponentId> sorted_shuffled = shuffled;
  fpm::canonicalise_members(sorted_forward);
  fpm::canonicalise_members(sorted_reversed);
  fpm::canonicalise_members(sorted_shuffled);
  const fpm::MembershipDigest a = fpm::compute_membership_digest(sorted_forward);
  const fpm::MembershipDigest b = fpm::compute_membership_digest(sorted_reversed);
  const fpm::MembershipDigest c = fpm::compute_membership_digest(sorted_shuffled);
  FPM_EQ(a, b);
  FPM_EQ(b, c);
}

FPM_TEST(identity, partition_identity_is_generation_and_lineage_bound) {
  std::vector<fpm::ComponentId> members;
  for (std::size_t index = 0; index < 4; ++index) {
    members.push_back(component(index));
  }
  const fpm::MembershipDigest membership = fpm::compute_membership_digest(members);
  const fpm::LineageId lineage = fpm::genesis_lineage_id(membership);
  const fpm::PartitionId first =
      fpm::compute_partition_id(lineage, fpm::PartitionGeneration::from_value(1), membership);
  const fpm::PartitionId same =
      fpm::compute_partition_id(lineage, fpm::PartitionGeneration::from_value(1), membership);
  const fpm::PartitionId later =
      fpm::compute_partition_id(lineage, fpm::PartitionGeneration::from_value(2), membership);
  const fpm::LineageId other_lineage = fpm::split_lineage_id(lineage, membership);
  const fpm::PartitionId other =
      fpm::compute_partition_id(other_lineage, fpm::PartitionGeneration::from_value(1), membership);
  FPM_EQ(first, same);
  FPM_NE(first, later);
  FPM_NE(first, other);
  FPM_CHECK(fpm::PartitionId::is_valid(first.view()));
}

FPM_TEST(identity, lineage_identity_is_order_independent) {
  const fpm::LineageId left = fpm::LineageId::from_validated("laaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  const fpm::LineageId right = fpm::LineageId::from_validated("lbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
  FPM_EQ(fpm::merge_lineage_id(left, right), fpm::merge_lineage_id(right, left));
  FPM_NE(fpm::merge_lineage_id(left, right),
         fpm::merge_lineage_id(left, fpm::LineageId::from_validated("lccccccccccccccccccccccccccccccc")));
}

FPM_TEST(identity, digest_ordering_is_deterministic) {
  std::vector<fpm::Digest> digests;
  for (std::size_t index = 0; index < 64; ++index) {
    digests.push_back(fpm::sha256_text("value-" + std::to_string(index)));
  }
  std::vector<fpm::Digest> sorted = digests;
  std::sort(sorted.begin(), sorted.end());
  std::vector<fpm::Digest> reversed(digests.rbegin(), digests.rend());
  std::sort(reversed.begin(), reversed.end());
  FPM_CHECK(sorted == reversed);
  FPM_EQ(sorted.front().hex().size(), std::size_t{64});
}
