// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// Deterministic explanation data.
//
// Every externally visible outcome in Fabric Partition Manager carries an
// ordered, bounded list of reasons. Reason codes are stable identifiers, never
// prose: a caller can branch on them. Reason text is bounded so that no
// explanation can grow without limit under adversarial input.
#ifndef FABRIC_PARTITION_MANAGER_REASON_HPP
#define FABRIC_PARTITION_MANAGER_REASON_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "fabric_partition_manager/limits.hpp"

namespace fabric_partition_manager {

enum class ReasonCode : std::uint16_t {
  Ok = 0,

  // Evidence ingestion.
  EvidenceAccepted = 100,
  EvidenceRejectedInvalid = 101,
  EvidenceRejectedStale = 102,
  EvidenceRejectedDuplicate = 103,
  EvidenceRejectedRegression = 104,
  EvidenceRejectedRetiredBoot = 105,
  EvidenceRejectedLimit = 106,
  EvidenceRejectedUnknownComponent = 107,
  EvidenceRejectedFutureDated = 108,
  EvidenceRejectedGeneration = 109,
  EvidenceRejectedUnsupportedCompleteness = 110,
  EvidenceRejectedEmpty = 111,
  EvidenceRejectedMalformed = 112,
  EvidenceUnknownPairs = 120,
  EvidenceConflictingPairs = 121,
  EvidenceStalePairs = 122,
  EvidenceAsymmetricPairs = 123,
  EvidenceCompleteCoverage = 124,
  EvidenceIncompleteCoverage = 125,
  EvidenceDuplicateCoverage = 126,

  // Partition lifecycle.
  PartitionConfirmed = 200,
  PartitionPartial = 201,
  PartitionDetected = 202,
  PartitionRetired = 203,
  PartitionMembershipCanonical = 204,
  PartitionGenerationAdvanced = 205,
  PartitionRosterEmpty = 210,

  // Authority.
  AuthorityQuorumSatisfied = 300,
  AuthorityComponentsNotSatisfied = 301,
  AuthorityWeightNotSatisfied = 302,
  AuthorityVoterNotSatisfied = 303,
  AuthorityDowngradedUnknownEvidence = 304,
  AuthorityDowngradedPartialPartition = 305,
  AuthoritySplitBrainConflict = 306,
  AuthorityIsolated = 307,
  AuthorityObserveOnly = 308,
  AuthorityReadOnly = 309,
  AuthorityDegraded = 310,
  AuthorityDeniedFailClosed = 311,
  AuthorityRetained = 312,
  AuthorityInterruptedByRestart = 313,

  // Merge governance.
  MergeAccepted = 400,
  MergeRejectedGenerationMismatch = 401,
  MergeRejectedLineageConflict = 402,
  MergeRejectedNotReachable = 403,
  MergeRejectedStaleEvidence = 404,
  MergeRejectedAuthorityConflict = 405,
  MergeRejectedIndeterminate = 406,
  MergeRejectedUnknownSubject = 407,
  MergeRejectedAlreadyMerged = 408,
  MergeRejectedPolicy = 409,
  MergeFencedPriorAuthority = 410,
  MergeNewLineageEstablished = 411,
  MergeInheritedLineage = 412,
  MergeRejectedDuplicate = 413,

  // Isolation, degradation and revalidation.
  IsolationApplied = 500,
  IsolationRejectedUnknownSubject = 501,
  DegradationApplied = 502,
  RevalidationGranted = 510,
  RevalidationDenied = 511,
  RevalidationIndeterminate = 512,
  RevalidationRequired = 513,

  // Restart and fencing.
  RestartEpochAdvanced = 600,
  RestartIncarnationAdvanced = 601,
  RestartAuthorityInterrupted = 602,
  RestartEvidenceDropped = 603,
  RestartFencesRetained = 604,
  RestartLineageRetained = 605,
  RestartAttemptSequenceRetained = 606,
  FenceIssued = 610,
  FenceRetained = 611,
  FenceMatched = 612,

  // Algorithmic and request outcomes.
  SearchLimitReached = 700,
  IndeterminateResult = 701,
  UnsupportedRequest = 702,
  InvalidRequest = 703,
  DeterministicOrdering = 704,
  ReferenceAgreement = 705,

  // Persistence.
  DurableCommitAdvanced = 800,
  DurableRecordRejected = 801,
  StoreTornTailRecovered = 802,
  StoreIntegrityFailure = 803,
  SnapshotWritten = 804,
  SnapshotRejected = 805,
  StoreVersionUnsupported = 806,
  StoreSequenceRegression = 807,
  StoreTrailingGarbage = 808,
  StoreIoFailure = 809,

  // Limits and accounting.
  LimitExceededReason = 900,
  ArithmeticRefused = 901,
  CapacityExhausted = 902,

  // Attempt identity.
  AttemptDuplicate = 1005,
  AttemptRegression = 1006,
  AttemptUnknown = 1007,

  // Scope and subject.
  SubjectOutOfScope = 1000,
  SubjectUnknown = 1001,
  EpochMismatchReason = 1002,
  BootMismatchReason = 1003,
};

[[nodiscard]] std::string_view reason_code_name(ReasonCode code) noexcept;

struct Reason {
  ReasonCode code = ReasonCode::Ok;
  std::string subject;
  std::string detail;

  friend bool operator==(const Reason&, const Reason&) noexcept = default;
};

// Truncates subject and detail to the configured text bound. Truncation is
// marked so a clipped explanation is never mistaken for a complete one.
[[nodiscard]] Reason make_reason(ReasonCode code, std::string_view subject, std::string_view detail,
                                 const Limits& limits);

// Appends a reason when the list is below the bound. Returns true when the
// reason was appended, false when the list is full.
bool append_reason(std::vector<Reason>& reasons, ReasonCode code, std::string_view subject,
                   std::string_view detail, const Limits& limits);

[[nodiscard]] bool has_reason_code(const std::vector<Reason>& reasons, ReasonCode code) noexcept;

// Canonical text form: newline separated, stable order preserved.
[[nodiscard]] std::string render_reasons(const std::vector<Reason>& reasons);

}  // namespace fabric_partition_manager

#endif  // FABRIC_PARTITION_MANAGER_REASON_HPP
