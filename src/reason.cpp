// Fabric Partition Manager 1.0.0 - Summon Software Labs
#include "fabric_partition_manager/reason.hpp"

#include <algorithm>

namespace fabric_partition_manager {

std::string_view reason_code_name(ReasonCode code) noexcept {
  switch (code) {
    case ReasonCode::Ok: return "OK";
    case ReasonCode::EvidenceAccepted: return "EVIDENCE_ACCEPTED";
    case ReasonCode::EvidenceRejectedInvalid: return "EVIDENCE_REJECTED_INVALID";
    case ReasonCode::EvidenceRejectedStale: return "EVIDENCE_REJECTED_STALE";
    case ReasonCode::EvidenceRejectedDuplicate: return "EVIDENCE_REJECTED_DUPLICATE";
    case ReasonCode::EvidenceRejectedRegression: return "EVIDENCE_REJECTED_REGRESSION";
    case ReasonCode::EvidenceRejectedRetiredBoot: return "EVIDENCE_REJECTED_RETIRED_BOOT";
    case ReasonCode::EvidenceRejectedLimit: return "EVIDENCE_REJECTED_LIMIT";
    case ReasonCode::EvidenceRejectedUnknownComponent: return "EVIDENCE_REJECTED_UNKNOWN_COMPONENT";
    case ReasonCode::EvidenceRejectedFutureDated: return "EVIDENCE_REJECTED_FUTURE_DATED";
    case ReasonCode::EvidenceRejectedGeneration: return "EVIDENCE_REJECTED_GENERATION";
    case ReasonCode::EvidenceRejectedUnsupportedCompleteness:
      return "EVIDENCE_REJECTED_UNSUPPORTED_COMPLETENESS";
    case ReasonCode::EvidenceRejectedEmpty: return "EVIDENCE_REJECTED_EMPTY";
    case ReasonCode::EvidenceRejectedMalformed: return "EVIDENCE_REJECTED_MALFORMED";
    case ReasonCode::EvidenceUnknownPairs: return "EVIDENCE_UNKNOWN_PAIRS";
    case ReasonCode::EvidenceConflictingPairs: return "EVIDENCE_CONFLICTING_PAIRS";
    case ReasonCode::EvidenceStalePairs: return "EVIDENCE_STALE_PAIRS";
    case ReasonCode::EvidenceAsymmetricPairs: return "EVIDENCE_ASYMMETRIC_PAIRS";
    case ReasonCode::EvidenceCompleteCoverage: return "EVIDENCE_COMPLETE_COVERAGE";
    case ReasonCode::EvidenceIncompleteCoverage: return "EVIDENCE_INCOMPLETE_COVERAGE";
    case ReasonCode::EvidenceDuplicateCoverage: return "EVIDENCE_DUPLICATE_COVERAGE";
    case ReasonCode::PartitionConfirmed: return "PARTITION_CONFIRMED";
    case ReasonCode::PartitionPartial: return "PARTITION_PARTIAL";
    case ReasonCode::PartitionDetected: return "PARTITION_DETECTED";
    case ReasonCode::PartitionRetired: return "PARTITION_RETIRED";
    case ReasonCode::PartitionMembershipCanonical: return "PARTITION_MEMBERSHIP_CANONICAL";
    case ReasonCode::PartitionGenerationAdvanced: return "PARTITION_GENERATION_ADVANCED";
    case ReasonCode::PartitionRosterEmpty: return "PARTITION_ROSTER_EMPTY";
    case ReasonCode::AuthorityQuorumSatisfied: return "AUTHORITY_QUORUM_SATISFIED";
    case ReasonCode::AuthorityComponentsNotSatisfied: return "AUTHORITY_COMPONENTS_NOT_SATISFIED";
    case ReasonCode::AuthorityWeightNotSatisfied: return "AUTHORITY_WEIGHT_NOT_SATISFIED";
    case ReasonCode::AuthorityVoterNotSatisfied: return "AUTHORITY_VOTER_NOT_SATISFIED";
    case ReasonCode::AuthorityDowngradedUnknownEvidence: return "AUTHORITY_DOWNGRADED_UNKNOWN_EVIDENCE";
    case ReasonCode::AuthorityDowngradedPartialPartition:
      return "AUTHORITY_DOWNGRADED_PARTIAL_PARTITION";
    case ReasonCode::AuthoritySplitBrainConflict: return "AUTHORITY_SPLIT_BRAIN_CONFLICT";
    case ReasonCode::AuthorityIsolated: return "AUTHORITY_ISOLATED";
    case ReasonCode::AuthorityObserveOnly: return "AUTHORITY_OBSERVE_ONLY";
    case ReasonCode::AuthorityReadOnly: return "AUTHORITY_READ_ONLY";
    case ReasonCode::AuthorityDegraded: return "AUTHORITY_DEGRADED";
    case ReasonCode::AuthorityDeniedFailClosed: return "AUTHORITY_DENIED_FAIL_CLOSED";
    case ReasonCode::AuthorityRetained: return "AUTHORITY_RETAINED";
    case ReasonCode::AuthorityInterruptedByRestart: return "AUTHORITY_INTERRUPTED_BY_RESTART";
    case ReasonCode::MergeAccepted: return "MERGE_ACCEPTED";
    case ReasonCode::MergeRejectedGenerationMismatch: return "MERGE_REJECTED_GENERATION_MISMATCH";
    case ReasonCode::MergeRejectedLineageConflict: return "MERGE_REJECTED_LINEAGE_CONFLICT";
    case ReasonCode::MergeRejectedNotReachable: return "MERGE_REJECTED_NOT_REACHABLE";
    case ReasonCode::MergeRejectedStaleEvidence: return "MERGE_REJECTED_STALE_EVIDENCE";
    case ReasonCode::MergeRejectedAuthorityConflict: return "MERGE_REJECTED_AUTHORITY_CONFLICT";
    case ReasonCode::MergeRejectedIndeterminate: return "MERGE_REJECTED_INDETERMINATE";
    case ReasonCode::MergeRejectedUnknownSubject: return "MERGE_REJECTED_UNKNOWN_SUBJECT";
    case ReasonCode::MergeRejectedAlreadyMerged: return "MERGE_REJECTED_ALREADY_MERGED";
    case ReasonCode::MergeRejectedPolicy: return "MERGE_REJECTED_POLICY";
    case ReasonCode::MergeFencedPriorAuthority: return "MERGE_FENCED_PRIOR_AUTHORITY";
    case ReasonCode::MergeNewLineageEstablished: return "MERGE_NEW_LINEAGE_ESTABLISHED";
    case ReasonCode::MergeInheritedLineage: return "MERGE_INHERITED_LINEAGE";
    case ReasonCode::MergeRejectedDuplicate: return "MERGE_REJECTED_DUPLICATE";
    case ReasonCode::IsolationApplied: return "ISOLATION_APPLIED";
    case ReasonCode::IsolationRejectedUnknownSubject: return "ISOLATION_REJECTED_UNKNOWN_SUBJECT";
    case ReasonCode::DegradationApplied: return "DEGRADATION_APPLIED";
    case ReasonCode::RevalidationGranted: return "REVALIDATION_GRANTED";
    case ReasonCode::RevalidationDenied: return "REVALIDATION_DENIED";
    case ReasonCode::RevalidationIndeterminate: return "REVALIDATION_INDETERMINATE";
    case ReasonCode::RevalidationRequired: return "REVALIDATION_REQUIRED";
    case ReasonCode::RestartEpochAdvanced: return "RESTART_EPOCH_ADVANCED";
    case ReasonCode::RestartIncarnationAdvanced: return "RESTART_INCARNATION_ADVANCED";
    case ReasonCode::RestartAuthorityInterrupted: return "RESTART_AUTHORITY_INTERRUPTED";
    case ReasonCode::RestartEvidenceDropped: return "RESTART_EVIDENCE_DROPPED";
    case ReasonCode::RestartFencesRetained: return "RESTART_FENCES_RETAINED";
    case ReasonCode::RestartLineageRetained: return "RESTART_LINEAGE_RETAINED";
    case ReasonCode::RestartAttemptSequenceRetained: return "RESTART_ATTEMPT_SEQUENCE_RETAINED";
    case ReasonCode::FenceIssued: return "FENCE_ISSUED";
    case ReasonCode::FenceRetained: return "FENCE_RETAINED";
    case ReasonCode::FenceMatched: return "FENCE_MATCHED";
    case ReasonCode::SearchLimitReached: return "SEARCH_LIMIT_REACHED";
    case ReasonCode::IndeterminateResult: return "INDETERMINATE_RESULT";
    case ReasonCode::UnsupportedRequest: return "UNSUPPORTED_REQUEST";
    case ReasonCode::InvalidRequest: return "INVALID_REQUEST";
    case ReasonCode::DeterministicOrdering: return "DETERMINISTIC_ORDERING";
    case ReasonCode::ReferenceAgreement: return "REFERENCE_AGREEMENT";
    case ReasonCode::DurableCommitAdvanced: return "DURABLE_COMMIT_ADVANCED";
    case ReasonCode::DurableRecordRejected: return "DURABLE_RECORD_REJECTED";
    case ReasonCode::StoreTornTailRecovered: return "STORE_TORN_TAIL_RECOVERED";
    case ReasonCode::StoreIntegrityFailure: return "STORE_INTEGRITY_FAILURE";
    case ReasonCode::SnapshotWritten: return "SNAPSHOT_WRITTEN";
    case ReasonCode::SnapshotRejected: return "SNAPSHOT_REJECTED";
    case ReasonCode::StoreVersionUnsupported: return "STORE_VERSION_UNSUPPORTED";
    case ReasonCode::StoreSequenceRegression: return "STORE_SEQUENCE_REGRESSION";
    case ReasonCode::StoreTrailingGarbage: return "STORE_TRAILING_GARBAGE";
    case ReasonCode::StoreIoFailure: return "STORE_IO_FAILURE";
    case ReasonCode::LimitExceededReason: return "LIMIT_EXCEEDED";
    case ReasonCode::ArithmeticRefused: return "ARITHMETIC_REFUSED";
    case ReasonCode::CapacityExhausted: return "CAPACITY_EXHAUSTED";
    case ReasonCode::AttemptDuplicate: return "ATTEMPT_DUPLICATE";
    case ReasonCode::AttemptRegression: return "ATTEMPT_REGRESSION";
    case ReasonCode::AttemptUnknown: return "ATTEMPT_UNKNOWN";
    case ReasonCode::SubjectOutOfScope: return "SUBJECT_OUT_OF_SCOPE";
    case ReasonCode::SubjectUnknown: return "SUBJECT_UNKNOWN";
    case ReasonCode::EpochMismatchReason: return "EPOCH_MISMATCH";
    case ReasonCode::BootMismatchReason: return "BOOT_MISMATCH";
  }
  return "UNRECOGNIZED";
}

namespace {

constexpr std::string_view kTruncationMarker = "...";

void assign_bounded(std::string& target, std::string_view text, std::size_t bound) {
  if (bound == 0) {
    target.clear();
    return;
  }
  if (text.size() <= bound) {
    target.assign(text);
    return;
  }
  const std::size_t keep = bound > kTruncationMarker.size() + 1
                               ? bound - kTruncationMarker.size()
                               : bound;
  target.assign(text.substr(0, std::min(keep, text.size())));
  if (bound > kTruncationMarker.size() && target.size() + kTruncationMarker.size() <= bound) {
    target.append(kTruncationMarker);
  }
}

}  // namespace

Reason make_reason(ReasonCode code, std::string_view subject, std::string_view detail,
                   const Limits& limits) {
  Reason reason;
  reason.code = code;
  assign_bounded(reason.subject, subject, limits.max_text_bytes);
  assign_bounded(reason.detail, detail, limits.max_text_bytes);
  return reason;
}

bool append_reason(std::vector<Reason>& reasons, ReasonCode code, std::string_view subject,
                   std::string_view detail, const Limits& limits) {
  if (reasons.size() >= limits.max_reasons_per_decision) {
    return false;
  }
  reasons.push_back(make_reason(code, subject, detail, limits));
  return true;
}

bool has_reason_code(const std::vector<Reason>& reasons, ReasonCode code) noexcept {
  for (const Reason& reason : reasons) {
    if (reason.code == code) {
      return true;
    }
  }
  return false;
}

std::string render_reasons(const std::vector<Reason>& reasons) {
  std::string result;
  for (const Reason& reason : reasons) {
    if (!result.empty()) {
      result.push_back('\n');
    }
    result.append(reason_code_name(reason.code));
    if (!reason.subject.empty()) {
      result.append(" subject=");
      result.append(reason.subject);
    }
    if (!reason.detail.empty()) {
      result.append(" detail=");
      result.append(reason.detail);
    }
  }
  return result;
}

}  // namespace fabric_partition_manager
