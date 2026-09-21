// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// Versioned, integrity-checked durable state.
//
// What is durable: policy definitions, the adopted topology definition, the
// component roster, committed lineage, completed decisions, fence records and
// the coordinator epoch/sequence floors.
//
// What is NOT durable: reachability evidence, evidence freshness, live
// authority, attempts in flight, acknowledgements and verified effects. A
// restart never converts a durable record into current authority.
//
// Layout
//   journal  : 24-byte header, then a stream of self-describing records
//   snapshot : 24-byte header, a bounded payload, and a trailing checksum
//
// Every record carries its type, its monotonic sequence and a CRC-64/XZ
// integrity field over the header and payload. Declared lengths are validated
// against the configured bound before any allocation, and a declared length
// that runs past the end of the journal is a recoverable torn tail only when it
// is the final record; anything else is an integrity failure and the store
// refuses to load.
#ifndef FABRIC_PARTITION_MANAGER_PERSISTENCE_HPP
#define FABRIC_PARTITION_MANAGER_PERSISTENCE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "fabric_partition_manager/decision.hpp"
#include "fabric_partition_manager/limits.hpp"
#include "fabric_partition_manager/lineage.hpp"
#include "fabric_partition_manager/policy.hpp"
#include "fabric_partition_manager/reason.hpp"
#include "fabric_partition_manager/topology.hpp"

namespace fabric_partition_manager {

inline constexpr std::string_view journal_magic = "FPMJRNL1";
inline constexpr std::string_view snapshot_magic = "FPMSNAP1";
inline constexpr std::size_t store_header_bytes = 24;
inline constexpr std::size_t record_header_bytes = 16;
inline constexpr std::size_t record_trailer_bytes = 8;

enum class DurableRecordType : std::uint16_t {
  EpochState = 1,
  PolicyDefinition = 2,
  TopologyDefinition = 3,
  LineageRecord = 4,
  DecisionRecord = 5,
  FenceRecord = 6,
};
inline constexpr std::uint16_t durable_record_type_domain_max = 6;

[[nodiscard]] std::string_view durable_record_type_name(DurableRecordType value) noexcept;

struct DurableRecord {
  DurableRecordType type = DurableRecordType::EpochState;
  std::uint64_t sequence = 0;
  std::vector<std::byte> payload;
};

// A record that authority was live for a lineage at the moment the durable
// state was written. It exists so that a restart can report that authority as
// INTERRUPTED. It never restores authority: a restored mark grants nothing.
struct AuthorityGrantMark {
  PartitionId partition;
  LineageId lineage;
  AuthoritySequence authority_sequence;

  friend bool operator==(const AuthorityGrantMark&, const AuthorityGrantMark&) noexcept = default;
};

// The restart authority boundary. Advanced on every start from durable state.
struct EpochState {
  CoordinatorEpoch epoch;
  CoordinatorBootId boot;
  ProcessIncarnationId incarnation;
  BootSequence boot_sequence;
  PartitionGeneration partition_generation;
  AuthoritySequence authority_sequence;
  DecisionSequence decision_sequence;
  AttemptSequence attempt_sequence;
  LineageSequence lineage_sequence;
  FenceSequence fence_sequence;
  std::uint64_t tick = 0;
  std::vector<AuthorityGrantMark> live_grants;
};

struct StoreStats {
  bool open = false;
  std::uint64_t appended_records = 0;
  std::uint64_t replayed_records = 0;
  std::uint64_t rejected_records = 0;
  std::uint64_t integrity_failures = 0;
  std::uint64_t torn_tail_bytes = 0;
  std::uint64_t snapshot_writes = 0;
  std::uint64_t bytes_written = 0;
  std::uint64_t highest_sequence = 0;
  bool loaded_from_snapshot = false;
  bool trailing_garbage = false;
};

struct StoreLoadResult {
  bool ok = false;
  bool torn_tail_recovered = false;
  std::uint64_t records_loaded = 0;
  std::uint64_t records_rejected = 0;
  std::vector<Reason> reasons;
};

class DurableStore {
 public:
  DurableStore(std::filesystem::path journal_path, std::filesystem::path snapshot_path,
               const Limits& limits);
  ~DurableStore();

  DurableStore(const DurableStore&) = delete;
  DurableStore& operator=(const DurableStore&) = delete;

  // Reads the snapshot, then the journal, validating every header, length,
  // checksum, type and sequence. On success the merged record stream is
  // available through records().
  [[nodiscard]] StoreLoadResult load();

  // Opens the journal for append. Must be called after load().
  [[nodiscard]] bool open_for_append();

  // Appends one record and flushes it durably. Returns false when the store is
  // closed, when the sequence is not strictly increasing, or when the payload
  // exceeds the record bound.
  [[nodiscard]] bool append(DurableRecordType type, std::uint64_t sequence,
                            const std::vector<std::byte>& payload);

  // Writes a snapshot of every currently loaded record using transactional
  // replacement, then truncates the journal.
  [[nodiscard]] bool compact();

  void close();

  [[nodiscard]] const std::vector<DurableRecord>& records() const noexcept { return records_; }
  [[nodiscard]] const StoreStats& stats() const noexcept { return stats_; }
  [[nodiscard]] std::uint64_t highest_sequence() const noexcept { return highest_sequence_; }
  [[nodiscard]] bool is_open() const noexcept { return journal_ != nullptr; }
  [[nodiscard]] const std::filesystem::path& journal_path() const noexcept { return journal_path_; }
  [[nodiscard]] const std::filesystem::path& snapshot_path() const noexcept {
    return snapshot_path_;
  }

 private:
  [[nodiscard]] bool write_snapshot();
  [[nodiscard]] bool truncate_journal();

  std::filesystem::path journal_path_;
  std::filesystem::path snapshot_path_;
  Limits limits_;
  std::vector<DurableRecord> records_;
  std::uint64_t highest_sequence_ = 0;
  std::uint64_t journal_good_bytes_ = 0;
  StoreStats stats_;
  void* journal_ = nullptr;
};

// ---------------------------------------------------------------------------
// Canonical codecs. Every decoder is total: it validates the type tag, every
// enum domain, every range and the absence of trailing bytes, and returns
// nullopt rather than a partially populated value.
// ---------------------------------------------------------------------------

[[nodiscard]] std::vector<std::byte> encode_epoch_state(const EpochState& state) noexcept;
[[nodiscard]] std::optional<EpochState> decode_epoch_state(const std::vector<std::byte>& bytes,
                                                           const Limits& limits) noexcept;

[[nodiscard]] std::vector<std::byte> encode_policy(const PartitionPolicy& policy) noexcept;
[[nodiscard]] std::optional<PartitionPolicy> decode_policy(const std::vector<std::byte>& bytes,
                                                           const Limits& limits) noexcept;

[[nodiscard]] std::vector<std::byte> encode_topology(const TopologyDefinition& definition) noexcept;
[[nodiscard]] std::optional<TopologyDefinition> decode_topology(
    const std::vector<std::byte>& bytes, const Limits& limits) noexcept;

[[nodiscard]] std::vector<std::byte> encode_lineage(const LineageRecord& record) noexcept;
[[nodiscard]] std::optional<LineageRecord> decode_lineage(const std::vector<std::byte>& bytes,
                                                          const Limits& limits) noexcept;

[[nodiscard]] std::vector<std::byte> encode_decision(const Decision& decision) noexcept;
[[nodiscard]] std::optional<Decision> decode_decision(const std::vector<std::byte>& bytes,
                                                      const Limits& limits) noexcept;

[[nodiscard]] std::vector<std::byte> encode_fence(const FenceRecord& fence) noexcept;
[[nodiscard]] std::optional<FenceRecord> decode_fence(const std::vector<std::byte>& bytes,
                                                      const Limits& limits) noexcept;

// Canonical byte image of an authority vector, used inside decision records.
[[nodiscard]] std::vector<std::byte> encode_authority(const AuthorityVector& authority) noexcept;
[[nodiscard]] std::optional<AuthorityVector> decode_authority(const std::vector<std::byte>& bytes,
                                                              std::size_t& offset,
                                                              const Limits& limits) noexcept;

}  // namespace fabric_partition_manager

#endif  // FABRIC_PARTITION_MANAGER_PERSISTENCE_HPP
