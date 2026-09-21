// Fabric Partition Manager 1.0.0 - Summon Software Labs
#include "fabric_partition_manager/runtime.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "fabric_partition_manager/encoding.hpp"

namespace fabric_partition_manager {
namespace {

// Class ordering runs from strongest to weakest, so "weaker" is the larger
// enum value. Downgrades therefore compose without a lookup table.
[[nodiscard]] PartitionAuthorityClass weaker(PartitionAuthorityClass lhs,
                                             PartitionAuthorityClass rhs) noexcept {
  return static_cast<std::uint8_t>(lhs) >= static_cast<std::uint8_t>(rhs) ? lhs : rhs;
}

[[nodiscard]] CapabilitySet effective_capabilities(const PartitionPolicy& policy,
                                                   PartitionAuthorityClass klass) noexcept {
  return capabilities_for_class(policy, klass);
}

[[nodiscard]] std::string requirement_detail(const QuorumRequirement& requirement,
                                             std::uint32_t components, std::uint32_t voters,
                                             std::uint64_t weight) {
  std::string detail;
  detail.append("components=");
  detail.append(std::to_string(components));
  detail.push_back('/');
  detail.append(std::to_string(requirement.min_components));
  detail.append(" voters=");
  detail.append(std::to_string(voters));
  detail.push_back('/');
  detail.append(std::to_string(requirement.min_voters));
  detail.append(" weight=");
  detail.append(std::to_string(weight));
  detail.push_back('/');
  detail.append(std::to_string(requirement.min_weight));
  if (requirement.require_strict_majority) {
    detail.append(" strict-majority-of=");
    detail.append(std::to_string(requirement.domain_weight));
  }
  return detail;
}

}  // namespace

PartitionRuntime::PartitionRuntime(const RuntimeOptions& options)
    : options_(options),
      policy_(options.policy),
      ledger_(options.limits),
      lineage_(options.limits),
      decisions_(options.limits) {
  const auto canonical = policy_.canonicalised(options_.limits);
  if (!canonical.has_value()) {
    throw PartitionError(ErrorCode::InvalidArgument, "runtime.policy",
                         "the supplied policy is not a valid canonical policy definition");
  }
  policy_ = *canonical;

  state_.epoch = CoordinatorEpoch::from_value(1);
  state_.boot = CoordinatorBootId::generate();
  state_.incarnation = ProcessIncarnationId::generate();
  state_.boot_sequence = BootSequence::from_value(1);

  if (options_.durable && !options_.store_directory.empty()) {
    store_ = std::make_unique<DurableStore>(options_.store_directory / "partition.journal",
                                            options_.store_directory / "partition.snapshot",
                                            options_.limits);
    const StoreLoadResult loaded = store_->load();
    if (!loaded.ok) {
      throw PartitionError(ErrorCode::IntegrityFailure, "runtime.store",
                           render_reasons(loaded.reasons));
    }
    durable_sequence_ = store_->highest_sequence();
    if (!store_->open_for_append()) {
      throw PartitionError(ErrorCode::IoFailure, "runtime.store",
                           "the durable journal could not be opened for append");
    }
    restore_from_records(store_->records());
    if (loaded.torn_tail_recovered) {
      append_reason(restart_reasons_, ReasonCode::StoreTornTailRecovered, "journal",
                    "a partially written final record was discarded during recovery",
                    options_.limits);
    }

    const auto next_epoch = state_.epoch.next();
    const auto next_boot = state_.boot_sequence.next();
    if (!next_epoch.has_value() || !next_boot.has_value()) {
      throw PartitionError(ErrorCode::ArithmeticOverflow, "runtime.epoch",
                           "the coordinator epoch or boot sequence is exhausted");
    }
    state_.epoch = *next_epoch;
    state_.boot_sequence = *next_boot;
    state_.boot = CoordinatorBootId::generate();
    state_.incarnation = ProcessIncarnationId::generate();
    append_reason(restart_reasons_, ReasonCode::RestartEpochAdvanced, "coordinator",
                  "epoch advanced to " + state_.epoch.render() +
                      " and a fresh coordinator boot identity was established",
                  options_.limits);
    append_reason(restart_reasons_, ReasonCode::RestartIncarnationAdvanced, "process",
                  "process incarnation " + state_.incarnation.hex().substr(0, 16),
                  options_.limits);
    if (!lineage_.records().empty()) {
      append_reason(restart_reasons_, ReasonCode::RestartLineageRetained, "lineage",
                    std::to_string(lineage_.size()) + " committed lineage records restored",
                    options_.limits);
    }
    if (!fences_.empty()) {
      append_reason(restart_reasons_, ReasonCode::RestartFencesRetained, "fences",
                    std::to_string(fences_.size()) + " fence records restored",
                    options_.limits);
    }
    append_reason(restart_reasons_, ReasonCode::RestartEvidenceDropped, "evidence",
                  "no reachability evidence survives a restart; all evidence must be re-published",
                  options_.limits);
    if (!interrupted_.empty()) {
      append_reason(restart_reasons_, ReasonCode::RestartAuthorityInterrupted, "authority",
                    std::to_string(interrupted_.size()) +
                        " pre-restart authority grants are INTERRUPTED and require revalidation",
                    options_.limits);
    }
    append_reason(restart_reasons_, ReasonCode::RestartAttemptSequenceRetained, "attempts",
                  "attempt sequence floor restored at " + state_.attempt_sequence.render(),
                  options_.limits);

    // The epoch advance is committed before any authority can be issued under
    // it, so a crash immediately after startup can never reuse an epoch.
    std::vector<std::byte> payload;
    CanonicalWriter writer(payload);
    (void)writer;
    const std::vector<std::byte> encoded = encode_epoch_state(state_);
    if (!commit_record(DurableRecordType::EpochState, encoded)) {
      throw PartitionError(ErrorCode::IoFailure, "runtime.store",
                           "the new epoch state could not be committed durably");
    }
  }

  assessment_.decision.id = DecisionId::from_validated("d0");
  assessment_.decision.kind = DecisionKind::AssessFabric;
  assessment_.decision.verdict = DecisionVerdict::Observed;
  assessment_.decision.scope = fabric_scope();
  assessment_.generation = state_.partition_generation;
}

PartitionRuntime::~PartitionRuntime() { close(); }

// ---------------------------------------------------------------------------
// Identity and status
// ---------------------------------------------------------------------------

CoordinatorEpoch PartitionRuntime::epoch() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return state_.epoch;
}

CoordinatorBootId PartitionRuntime::boot() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return state_.boot;
}

ProcessIncarnationId PartitionRuntime::incarnation() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return state_.incarnation;
}

std::uint64_t PartitionRuntime::tick() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return state_.tick;
}

PartitionPolicy PartitionRuntime::policy() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return policy_;
}

RuntimeStatus PartitionRuntime::status() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  RuntimeStatus status;
  status.epoch = state_.epoch;
  status.boot = state_.boot;
  status.incarnation = state_.incarnation;
  status.partition_generation = state_.partition_generation;
  status.authority_sequence = state_.authority_sequence;
  status.decision_sequence = state_.decision_sequence;
  status.attempt_sequence = state_.attempt_sequence;
  status.lineage_sequence = state_.lineage_sequence;
  status.fence_sequence = state_.fence_sequence;
  status.tick = state_.tick;
  status.partition_count = partitions_.size();
  status.decision_history = decisions_.size();
  status.decisions_dropped = decisions_.dropped();
  status.lineage_records = lineage_.size();
  status.fence_records = fences_.size();
  status.evidence_bundles = ledger_.retained_count();
  status.interrupted_authorities = interrupted_.size();
  status.active_isolations = isolations_.size();
  status.durable = store_ != nullptr;
  status.closed = closed_;
  status.store = store_ != nullptr ? store_->stats() : StoreStats{};
  return status;
}

bool PartitionRuntime::advance_ticks(std::uint64_t delta) {
  const std::lock_guard<std::mutex> guard(mutex_);
  if (closed_) {
    return false;
  }
  const auto next = checked_add(state_.tick, delta);
  if (!next.has_value()) {
    return false;
  }
  state_.tick = *next;
  return true;
}

bool PartitionRuntime::closed() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return closed_;
}

// ---------------------------------------------------------------------------
// Decision plumbing
// ---------------------------------------------------------------------------

void PartitionRuntime::assign_decision_identity(Decision& decision) {
  if (!decision.id.is_nil()) {
    return;
  }
  const auto sequence = state_.decision_sequence.next();
  if (!sequence.has_value()) {
    throw PartitionError(ErrorCode::ArithmeticOverflow, "runtime.decision",
                         "the decision sequence is exhausted");
  }
  state_.decision_sequence = *sequence;
  decision.id = DecisionId::from_validated("d" + sequence->render() + "-" +
                                           state_.boot.hex().substr(0, 8));
  decision.bindings.decision_sequence = *sequence;
}

Decision PartitionRuntime::make_decision(DecisionKind kind, DecisionVerdict verdict,
                                         const Scope& scope, ReasonCode primary) {
  Decision decision;
  decision.kind = kind;
  decision.verdict = verdict;
  decision.scope = scope;
  decision.bindings.epoch = state_.epoch;
  decision.bindings.boot = state_.boot;
  decision.bindings.incarnation = state_.incarnation;
  decision.bindings.topology_generation = roster_.generation();
  decision.bindings.topology_digest = roster_.digest();
  decision.bindings.reachability_generation = snapshot_.reachability_generation;
  decision.bindings.evidence_digest = snapshot_.evidence_digest;
  decision.bindings.policy_generation = policy_.generation;
  decision.bindings.partition_generation = state_.partition_generation;
  decision.bindings.authority_sequence = state_.authority_sequence;
  decision.bindings.decision_sequence = state_.decision_sequence;
  decision.bindings.evaluated_at_tick = state_.tick;
  decision.revocable = true;
  append_reason(decision.reasons, primary, scope.id.view(), "", options_.limits);
  return decision;
}

void PartitionRuntime::finalize_decision(Decision& decision) {
  assign_decision_identity(decision);
  // A decision is revocable exactly when it depends on an authority-bearing
  // dependency that can change.
  decision.revocation_triggers = {ReasonCode::RestartEpochAdvanced,
                                  ReasonCode::StoreSequenceRegression};
  if (!roster_.is_set()) {
    decision.revocation_triggers.push_back(ReasonCode::SubjectUnknown);
  }
  decisions_.append(decision);
  if (store_ != nullptr && decision.kind != DecisionKind::IngestEvidence) {
    const std::vector<std::byte> encoded = encode_decision(decision);
    (void)commit_record(DurableRecordType::DecisionRecord, encoded);
  }
}

bool PartitionRuntime::commit_record(DurableRecordType type,
                                     const std::vector<std::byte>& payload) {
  if (store_ == nullptr) {
    return true;
  }
  const auto next = checked_add(durable_sequence_, 1);
  if (!next.has_value()) {
    return false;
  }
  if (!store_->append(type, *next, payload)) {
    return false;
  }
  durable_sequence_ = *next;
  return true;
}

bool PartitionRuntime::check_attempt(const AttemptToken& attempt, DecisionKind kind,
                                     Decision& decision) {
  if (attempt.id.is_nil() || attempt.sequence.is_zero()) {
    decision.verdict = DecisionVerdict::Invalid;
    append_reason(decision.reasons, ReasonCode::InvalidRequest, attempt.id.view(),
                  "an attempt identifier and a non-zero attempt sequence are both required",
                  options_.limits);
    return false;
  }
  for (const AttemptRecord& record : attempts_) {
    if (record.attempt.id == attempt.id || record.attempt.sequence == attempt.sequence) {
      decision.verdict = DecisionVerdict::Invalid;
      append_reason(decision.reasons, ReasonCode::AttemptDuplicate, attempt.id.view(),
                    "the attempt identifier or attempt sequence has already been used",
                    options_.limits);
      return false;
    }
  }
  if (attempt.sequence.value() <= state_.attempt_sequence.value()) {
    decision.verdict = DecisionVerdict::Invalid;
    append_reason(decision.reasons, ReasonCode::AttemptRegression, attempt.id.view(),
                  "the attempt sequence regressed below the durable floor", options_.limits);
    return false;
  }
  (void)kind;
  return true;
}

void PartitionRuntime::record_attempt(const AttemptToken& attempt, DecisionKind kind) {
  if (attempts_.size() >= options_.limits.max_attempt_records) {
    attempts_.erase(attempts_.begin());
  }
  AttemptRecord record;
  record.attempt = attempt;
  record.kind = kind;
  record.tick = state_.tick;
  attempts_.push_back(record);
  state_.attempt_sequence = attempt.sequence;
}

// ---------------------------------------------------------------------------
// Restore
// ---------------------------------------------------------------------------

void PartitionRuntime::restore_from_records(const std::vector<DurableRecord>& records) {
  bool epoch_seen = false;
  std::vector<LineageRecord> pending_lineage;
  for (const DurableRecord& record : records) {
    switch (record.type) {
      case DurableRecordType::EpochState: {
        const auto decoded = decode_epoch_state(record.payload, options_.limits);
        if (!decoded.has_value()) {
          throw PartitionError(ErrorCode::IntegrityFailure, "runtime.store",
                               "a durable epoch state record could not be decoded");
        }
        EpochState next = *decoded;
        // The restored mark set describes what WAS live. It is converted into
        // interrupted authority and never into current authority.
        interrupted_.clear();
        for (const AuthorityGrantMark& mark : next.live_grants) {
          InterruptedAuthority entry;
          entry.partition = mark.partition;
          entry.lineage = mark.lineage;
          entry.authority_sequence = mark.authority_sequence;
          entry.epoch = next.epoch;
          entry.boot = next.boot;
          entry.tick = next.tick;
          interrupted_.push_back(entry);
        }
        next.live_grants.clear();
        state_ = next;
        epoch_seen = true;
        break;
      }
      case DurableRecordType::PolicyDefinition: {
        const auto decoded = decode_policy(record.payload, options_.limits);
        if (!decoded.has_value()) {
          throw PartitionError(ErrorCode::IntegrityFailure, "runtime.store",
                               "a durable policy record could not be decoded");
        }
        if (decoded->generation.value() > policy_.generation.value()) {
          policy_ = *decoded;
        }
        break;
      }
      case DurableRecordType::TopologyDefinition: {
        const auto decoded = decode_topology(record.payload, options_.limits);
        if (!decoded.has_value()) {
          throw PartitionError(ErrorCode::IntegrityFailure, "runtime.store",
                               "a durable topology record could not be decoded");
        }
        const auto roster = ComponentRoster::create(*decoded, options_.limits);
        if (!roster.has_value()) {
          throw PartitionError(ErrorCode::IntegrityFailure, "runtime.store",
                               "a durable topology record is not a valid roster");
        }
        if (!roster_set_ || decoded->generation.value() > topology_.generation.value()) {
          roster_ = *roster;
          topology_ = *decoded;
          roster_set_ = true;
        }
        break;
      }
      case DurableRecordType::LineageRecord: {
        const auto decoded = decode_lineage(record.payload, options_.limits);
        if (!decoded.has_value()) {
          throw PartitionError(ErrorCode::IntegrityFailure, "runtime.store",
                               "a durable lineage record could not be decoded");
        }
        if (!lineage_.append(*decoded)) {
          throw PartitionError(ErrorCode::IntegrityFailure, "runtime.store",
                               "the durable lineage is not self-consistent");
        }
        break;
      }
      case DurableRecordType::DecisionRecord: {
        const auto decoded = decode_decision(record.payload, options_.limits);
        if (!decoded.has_value()) {
          throw PartitionError(ErrorCode::IntegrityFailure, "runtime.store",
                               "a durable decision record could not be decoded");
        }
        decisions_.append(*decoded);
        break;
      }
      case DurableRecordType::FenceRecord: {
        const auto decoded = decode_fence(record.payload, options_.limits);
        if (!decoded.has_value()) {
          throw PartitionError(ErrorCode::IntegrityFailure, "runtime.store",
                               "a durable fence record could not be decoded");
        }
        fences_.push_back(*decoded);
        if (fences_.size() > options_.limits.max_fence_records) {
          fences_.erase(fences_.begin());
        }
        break;
      }
    }
  }
  (void)epoch_seen;
  (void)pending_lineage;
  if (!interrupted_.empty()) {
    for (const InterruptedAuthority& entry : interrupted_) {
      // Rebuild the fence ledger so a pre-restart grant is durably recorded as
      // revoked in this incarnation.
      const auto next = state_.fence_sequence.next();
      if (!next.has_value()) {
        break;
      }
      state_.fence_sequence = *next;
      FenceRecord fence;
      fence.id = FenceId::from_validated("f" + next->render() + "-" +
                                         state_.boot.hex().substr(0, 8));
      fence.sequence = *next;
      fence.scope = partition_scope(entry.partition);
      fence.partition = entry.partition;
      fence.fenced_authority_sequence = entry.authority_sequence;
      fence.fenced_epoch = entry.epoch;
      fence.fenced_boot = entry.boot;
      fence.issuing_epoch = state_.epoch;
      fence.issuing_boot = state_.boot;
      fence.tick = state_.tick;
      fence.cause = ReasonCode::AuthorityInterruptedByRestart;
      store_fence(fence);
    }
  }
}

// ---------------------------------------------------------------------------
// Fences and isolation
// ---------------------------------------------------------------------------

void PartitionRuntime::store_fence(const FenceRecord& fence) {
  if (fences_.size() >= options_.limits.max_fence_records) {
    return;
  }
  fences_.push_back(fence);
  if (store_ != nullptr) {
    const std::vector<std::byte> encoded = encode_fence(fence);
    (void)commit_record(DurableRecordType::FenceRecord, encoded);
  }
}

FenceRecord PartitionRuntime::make_fence(const Partition& partition, AuthoritySequence sequence,
                                         ReasonCode cause) {
  FenceRecord fence;
  const auto next = state_.fence_sequence.next();
  if (!next.has_value()) {
    throw PartitionError(ErrorCode::ArithmeticOverflow, "runtime.fence",
                         "the fence sequence is exhausted");
  }
  state_.fence_sequence = *next;
  fence.id = FenceId::from_validated("f" + next->render() + "-" + state_.boot.hex().substr(0, 8));
  fence.sequence = *next;
  fence.scope = partition_scope(partition.id);
  fence.partition = partition.id;
  fence.fenced_authority_sequence = sequence;
  fence.fenced_epoch = state_.epoch;
  fence.fenced_boot = state_.boot;
  fence.issuing_epoch = state_.epoch;
  fence.issuing_boot = state_.boot;
  fence.tick = state_.tick;
  fence.cause = cause;
  return fence;
}

bool PartitionRuntime::is_isolated(const LineageId& lineage) const {
  for (const IsolationDirective& directive : isolations_) {
    if (!(directive.lineage == lineage)) {
      continue;
    }
    if (directive.expires_at_tick != 0 && state_.tick >= directive.expires_at_tick) {
      continue;
    }
    return true;
  }
  return false;
}

void PartitionRuntime::interrupt_all_authority(ReasonCode cause) {
  for (const Partition& partition : partitions_) {
    if (partition.authority.authorized.empty()) {
      continue;
    }
    InterruptedAuthority entry;
    entry.partition = partition.id;
    entry.lineage = partition.lineage;
    entry.authority_sequence = partition.authority_sequence;
    entry.epoch = state_.epoch;
    entry.boot = state_.boot;
    entry.tick = state_.tick;
    interrupted_.push_back(entry);
    const auto next = state_.authority_sequence.next();
    if (!next.has_value()) {
      return;
    }
    state_.authority_sequence = *next;
    FenceRecord fence = make_fence(partition, partition.authority_sequence, cause);
    store_fence(fence);
  }
}

// ---------------------------------------------------------------------------
// Inputs
// ---------------------------------------------------------------------------

Decision PartitionRuntime::adopt_topology(const TopologyDefinition& definition) {
  const std::lock_guard<std::mutex> guard(mutex_);
  Decision decision = make_decision(DecisionKind::AdoptTopology, DecisionVerdict::Observed,
                                    named_scope(ScopeKind::Fabric, "fabric"),
                                    ReasonCode::SubjectUnknown);
  decision.reasons.clear();
  if (closed_) {
    decision.verdict = DecisionVerdict::Invalid;
    append_reason(decision.reasons, ReasonCode::InvalidRequest, "runtime",
                  "the runtime is closed", options_.limits);
    finalize_decision(decision);
    return decision;
  }
  const auto roster = ComponentRoster::create(definition, options_.limits);
  if (!roster.has_value()) {
    decision.verdict = DecisionVerdict::Invalid;
    append_reason(decision.reasons, ReasonCode::InvalidRequest, definition.id.view(),
                  "the topology definition is not a valid canonical roster", options_.limits);
    finalize_decision(decision);
    return decision;
  }
  if (roster_set_ && definition.generation.value() < topology_.generation.value()) {
    decision.verdict = DecisionVerdict::Invalid;
    append_reason(decision.reasons, ReasonCode::EvidenceRejectedGeneration, definition.id.view(),
                  "the topology generation regressed below the adopted generation",
                  options_.limits);
    finalize_decision(decision);
    return decision;
  }
  if (roster_set_ && definition.generation.value() == topology_.generation.value()) {
    if (roster->digest() == roster_.digest()) {
      decision.verdict = DecisionVerdict::Observed;
      append_reason(decision.reasons, ReasonCode::DeterministicOrdering, definition.id.view(),
                    "the identical roster is already adopted at this generation", options_.limits);
      finalize_decision(decision);
      return decision;
    }
    decision.verdict = DecisionVerdict::Conflict;
    append_reason(decision.reasons, ReasonCode::EvidenceConflictingPairs, definition.id.view(),
                  "two different rosters claim the same topology generation", options_.limits);
    finalize_decision(decision);
    return decision;
  }

  const std::vector<std::byte> encoded = encode_topology(definition);
  if (store_ != nullptr && !commit_record(DurableRecordType::TopologyDefinition, encoded)) {
    decision.verdict = DecisionVerdict::Indeterminate;
    append_reason(decision.reasons, ReasonCode::StoreIoFailure, definition.id.view(),
                  "the topology definition could not be committed durably", options_.limits);
    finalize_decision(decision);
    return decision;
  }
  if (roster_set_) {
    // A new authoritative roster supersedes every current partition and fences
    // the authority that was derived from the previous roster.
    interrupt_all_authority(ReasonCode::StoreSequenceRegression);
    for (Partition& partition : partitions_) {
      partition.lifecycle = PartitionLifecycle::Superseded;
    }
  }
  roster_ = *roster;
  topology_ = definition;
  roster_set_ = true;
  ledger_.drop_dynamic_state();
  decision.bindings.topology_generation = roster_.generation();
  decision.bindings.topology_digest = roster_.digest();
  decision.verdict = DecisionVerdict::Granted;
  append_reason(decision.reasons, ReasonCode::EvidenceAccepted, definition.id.view(),
                std::to_string(roster_.size()) + " components adopted as the authoritative roster",
                options_.limits);
  append_reason(decision.reasons, ReasonCode::RestartEvidenceDropped, "evidence",
                "reachability evidence bound to the previous topology generation was dropped",
                options_.limits);
  finalize_decision(decision);
  return decision;
}

Decision PartitionRuntime::set_policy(const PartitionPolicy& policy) {
  const std::lock_guard<std::mutex> guard(mutex_);
  Decision decision = make_decision(DecisionKind::SetPolicy, DecisionVerdict::Observed,
                                    named_scope(ScopeKind::Fabric, "policy"),
                                    ReasonCode::SubjectUnknown);
  decision.reasons.clear();
  if (closed_) {
    decision.verdict = DecisionVerdict::Invalid;
    append_reason(decision.reasons, ReasonCode::InvalidRequest, "runtime", "the runtime is closed",
                  options_.limits);
    finalize_decision(decision);
    return decision;
  }
  const auto canonical = policy.canonicalised(options_.limits);
  if (!canonical.has_value()) {
    decision.verdict = DecisionVerdict::Invalid;
    append_reason(decision.reasons, ReasonCode::InvalidRequest, policy.id.view(),
                  "the policy definition is not valid or carries an unsupported schema version",
                  options_.limits);
    finalize_decision(decision);
    return decision;
  }
  if (canonical->generation.value() <= policy_.generation.value()) {
    decision.verdict = DecisionVerdict::Invalid;
    append_reason(decision.reasons, ReasonCode::EvidenceRejectedGeneration, policy.id.view(),
                  "the policy generation did not advance", options_.limits);
    finalize_decision(decision);
    return decision;
  }
  const std::vector<std::byte> encoded = encode_policy(*canonical);
  if (store_ != nullptr && !commit_record(DurableRecordType::PolicyDefinition, encoded)) {
    decision.verdict = DecisionVerdict::Indeterminate;
    append_reason(decision.reasons, ReasonCode::StoreIoFailure, policy.id.view(),
                  "the policy definition could not be committed durably", options_.limits);
    finalize_decision(decision);
    return decision;
  }
  // A policy change is an authority-bearing dependency change: every live
  // authority is fenced and must be re-derived.
  interrupt_all_authority(ReasonCode::AuthorityDeniedFailClosed);
  policy_ = *canonical;
  decision.bindings.policy_generation = policy_.generation;
  decision.verdict = DecisionVerdict::Granted;
  append_reason(decision.reasons, ReasonCode::AuthorityDeniedFailClosed, policy.id.view(),
                "live authority was fenced because the authority policy changed",
                options_.limits);
  finalize_decision(decision);
  return decision;
}

Decision PartitionRuntime::ingest_evidence(const ReachabilityEvidence& evidence) {
  const std::lock_guard<std::mutex> guard(mutex_);
  Decision decision = make_decision(DecisionKind::IngestEvidence, DecisionVerdict::Observed,
                                    fabric_scope(), ReasonCode::EvidenceAccepted);
  decision.reasons.clear();
  if (closed_) {
    decision.verdict = DecisionVerdict::Invalid;
    append_reason(decision.reasons, ReasonCode::InvalidRequest, evidence.id.view(),
                  "the runtime is closed", options_.limits);
    finalize_decision(decision);
    return decision;
  }
  if (!roster_set_) {
    decision.verdict = DecisionVerdict::Invalid;
    append_reason(decision.reasons, ReasonCode::SubjectUnknown, evidence.id.view(),
                  "no authoritative roster has been adopted", options_.limits);
    finalize_decision(decision);
    return decision;
  }
  const EvidenceAcceptance acceptance = ledger_.accept(evidence, roster_, policy_, state_.tick);
  for (const Reason& reason : acceptance.reasons) {
    if (decision.reasons.size() < options_.limits.max_reasons_per_decision) {
      decision.reasons.push_back(reason);
    }
  }
  switch (acceptance.status) {
    case EvidenceAcceptanceStatus::Accepted:
      decision.verdict = DecisionVerdict::Granted;
      break;
    case EvidenceAcceptanceStatus::RejectedUnsupported:
      decision.verdict = DecisionVerdict::Unsupported;
      break;
    case EvidenceAcceptanceStatus::RejectedMalformed:
    case EvidenceAcceptanceStatus::RejectedEmpty:
    case EvidenceAcceptanceStatus::RejectedUnknownComponent:
    case EvidenceAcceptanceStatus::RejectedFutureDated:
    case EvidenceAcceptanceStatus::RejectedGeneration:
      decision.verdict = DecisionVerdict::Invalid;
      break;
    default:
      decision.verdict = DecisionVerdict::Denied;
      break;
  }
  decision.bindings.reachability_generation = ledger_.highest_generation();
  finalize_decision(decision);
  return decision;
}


// ---------------------------------------------------------------------------
// Authority evaluation
// ---------------------------------------------------------------------------

PartitionRuntime::AuthorityEvaluation PartitionRuntime::evaluate_authority(
    const ReachabilitySnapshot& snapshot, std::vector<Partition>& partitions, bool allow_restore,
    std::vector<Reason>& reasons) {
  AuthorityEvaluation evaluation;
  const CapabilitySet physically_able = CapabilitySet::all();
  const std::size_t count = partitions.size();
  std::vector<PartitionAuthorityClass> quorum_class(count, PartitionAuthorityClass::Unassigned);
  std::vector<PartitionAuthorityClass> final_class(count, PartitionAuthorityClass::Unassigned);
  std::vector<AuthorityBasis> basis(count, AuthorityBasis::None);

  for (std::size_t index = 0; index < count; ++index) {
    Partition& partition = partitions[index];
    const std::uint32_t members = static_cast<std::uint32_t>(
        std::min<std::size_t>(partition.members.size(), 0xFFFF'FFFFu));
    const bool full = policy_.full_authority.satisfied_by(members, partition.voter_count,
                                                          partition.total_weight);
    const bool degraded = policy_.degraded_authority.satisfied_by(members, partition.voter_count,
                                                                  partition.total_weight);
    PartitionAuthorityClass klass = PartitionAuthorityClass::Isolated;
    AuthorityBasis current_basis = AuthorityBasis::QuorumNotSatisfied;
    if (full) {
      klass = PartitionAuthorityClass::Primary;
      current_basis = AuthorityBasis::QuorumSatisfied;
      append_reason(reasons, ReasonCode::AuthorityQuorumSatisfied, partition.id.view(),
                    requirement_detail(policy_.full_authority, members, partition.voter_count,
                                       partition.total_weight),
                    options_.limits);
    } else if (degraded) {
      klass = PartitionAuthorityClass::Degraded;
      current_basis = AuthorityBasis::PolicyDegradation;
      append_reason(reasons, ReasonCode::AuthorityDegraded, partition.id.view(),
                    requirement_detail(policy_.full_authority, members, partition.voter_count,
                                       partition.total_weight),
                    options_.limits);
    } else {
      klass = PartitionAuthorityClass::Isolated;
      current_basis = AuthorityBasis::QuorumNotSatisfied;
      append_reason(reasons, ReasonCode::AuthorityIsolated, partition.id.view(),
                    requirement_detail(policy_.degraded_authority, members, partition.voter_count,
                                       partition.total_weight),
                    options_.limits);
    }
    quorum_class[index] = klass;

    if (!partition.pending_merge_parents.empty()) {
      klass = weaker(klass, PartitionAuthorityClass::ObserveOnly);
      current_basis = AuthorityBasis::PartialPartition;
      append_reason(reasons, ReasonCode::MergeRejectedLineageConflict, partition.id.view(),
                    "the component set spans multiple live lineages; authority is withheld until "
                    "the merge is decided",
                    options_.limits);
    }
    if (!snapshot.any_evidence_fresh) {
      // Nothing about the fabric has been observed at all, so no determination
      // exists to build authority on. This is the fail-closed floor.
      klass = weaker(klass, PartitionAuthorityClass::ObserveOnly);
      current_basis = AuthorityBasis::UnknownEvidence;
      append_reason(reasons, ReasonCode::AuthorityDowngradedUnknownEvidence, partition.id.view(),
                    "no fresh reachability evidence exists; only observation is permitted",
                    options_.limits);
    }
    if (!snapshot.confirmed) {
      if (policy_.unknown_evidence_downgrades) {
        klass = weaker(klass, PartitionAuthorityClass::Degraded);
        current_basis = AuthorityBasis::UnknownEvidence;
        append_reason(reasons, ReasonCode::AuthorityDowngradedUnknownEvidence, partition.id.view(),
                      "the component decomposition is not confirmed: unknown connectivity could "
                      "still merge components",
                      options_.limits);
      }
      if (policy_.require_confirmed_partition_for_write) {
        klass = weaker(klass, PartitionAuthorityClass::ReadOnly);
        current_basis = AuthorityBasis::PartialPartition;
        append_reason(reasons, ReasonCode::AuthorityDowngradedPartialPartition, partition.id.view(),
                      "write authority requires a confirmed component decomposition",
                      options_.limits);
      }
      if (policy_.require_confirmed_partition_for_read) {
        klass = weaker(klass, PartitionAuthorityClass::ObserveOnly);
        current_basis = AuthorityBasis::PartialPartition;
        append_reason(reasons, ReasonCode::AuthorityDowngradedPartialPartition, partition.id.view(),
                      "read authority requires a confirmed component decomposition", options_.limits);
      }
    }
    if (is_isolated(partition.lineage)) {
      klass = PartitionAuthorityClass::Isolated;
      current_basis = AuthorityBasis::ExplicitIsolation;
      append_reason(reasons, ReasonCode::AuthorityIsolated, partition.id.view(),
                    "an explicit isolation directive applies to this lineage", options_.limits);
    }
    bool interrupted = false;
    for (const InterruptedAuthority& entry : interrupted_) {
      if (entry.lineage == partition.lineage) {
        interrupted = true;
        break;
      }
    }
    if (interrupted && !allow_restore) {
      klass = weaker(klass, PartitionAuthorityClass::ObserveOnly);
      current_basis = AuthorityBasis::RestartInterrupted;
      append_reason(reasons, ReasonCode::AuthorityInterruptedByRestart, partition.id.view(),
                    "authority held before the last restart is interrupted until it is revalidated",
                    options_.limits);
    }
    final_class[index] = klass;
    basis[index] = current_basis;
  }

  std::size_t primary_count = 0;
  for (std::size_t index = 0; index < count; ++index) {
    if (final_class[index] == PartitionAuthorityClass::Primary) {
      ++primary_count;
    }
  }
  if (primary_count > 1 && !policy_.allow_multiple_authoritative_partitions) {
    evaluation.split_brain = true;
    for (std::size_t index = 0; index < count; ++index) {
      if (final_class[index] != PartitionAuthorityClass::Primary) {
        continue;
      }
      final_class[index] = PartitionAuthorityClass::ObserveOnly;
      basis[index] = AuthorityBasis::SplitBrainConflict;
      append_reason(reasons, ReasonCode::AuthoritySplitBrainConflict, partitions[index].id.view(),
                    std::to_string(primary_count) +
                        " partitions independently satisfy the full authority quorum",
                    options_.limits);
    }
  }

  bool any_primary = false;
  bool any_degraded = false;
  bool any_readonly = false;
  bool any_observe = false;
  bool all_isolated = true;

  for (std::size_t index = 0; index < count; ++index) {
    Partition& partition = partitions[index];
    std::vector<Reason> authority_reasons;
    const CapabilitySet eligible = effective_capabilities(policy_, quorum_class[index]);
    const CapabilitySet authorized = effective_capabilities(policy_, final_class[index]);
    partition.authority = authorized_authority(final_class[index], basis[index], physically_able,
                                               eligible, authorized, authority_reasons);
    partition.authority.authority_class = final_class[index];
    partition.authority.basis = basis[index];
    partition.authority.revocable = !authorized.empty();
    if (authorized.empty()) {
      partition.authority.application_state = AuthorityApplicationState::NotApplicable;
      partition.authority_sequence = AuthoritySequence{};
    } else {
      const auto next = state_.authority_sequence.next();
      if (!next.has_value()) {
        partition.authority.authorized = CapabilitySet::none();
        partition.authority.application_state = AuthorityApplicationState::NotApplicable;
        partition.authority_sequence = AuthoritySequence{};
        append_reason(reasons, ReasonCode::ArithmeticRefused, partition.id.view(),
                      "the authority sequence is exhausted; authority was withheld", options_.limits);
      } else {
        state_.authority_sequence = *next;
        partition.authority_sequence = *next;
        partition.authority.application_state = AuthorityApplicationState::AuthorizationIssued;
      }
    }

    switch (final_class[index]) {
      case PartitionAuthorityClass::Primary:
        any_primary = true;
        all_isolated = false;
        evaluation.authoritative = partition.id;
        partition.authoritative = true;
        break;
      case PartitionAuthorityClass::Degraded:
        any_degraded = true;
        all_isolated = false;
        break;
      case PartitionAuthorityClass::ReadOnly:
        any_readonly = true;
        all_isolated = false;
        break;
      case PartitionAuthorityClass::ObserveOnly:
        any_observe = true;
        all_isolated = false;
        break;
      case PartitionAuthorityClass::Isolated:
      case PartitionAuthorityClass::Unassigned:
        break;
    }

    if (allow_restore) {
      for (const InterruptedAuthority& entry : interrupted_) {
        if (entry.lineage == partition.lineage && !partition.authority.authorized.empty()) {
          evaluation.restored_lineages.push_back(entry.lineage);
        }
      }
    }
  }

  if (evaluation.split_brain) {
    evaluation.verdict = DecisionVerdict::Conflict;
  } else if (any_primary) {
    evaluation.verdict = DecisionVerdict::Granted;
  } else if (any_degraded || any_readonly) {
    evaluation.verdict = DecisionVerdict::Degraded;
  } else if (all_isolated) {
    evaluation.verdict = DecisionVerdict::Isolated;
  } else if (any_observe) {
    evaluation.verdict = DecisionVerdict::Denied;
  } else {
    evaluation.verdict = DecisionVerdict::Denied;
  }
  return evaluation;
}

// ---------------------------------------------------------------------------
// Assessment
// ---------------------------------------------------------------------------

PartitionAssessment PartitionRuntime::assess() {
  const std::lock_guard<std::mutex> guard(mutex_);
  return assess_locked(false);
}

PartitionAssessment PartitionRuntime::assess_locked(bool allow_restore) {
  if (closed_) {
    Decision denial = make_decision(DecisionKind::AssessFabric, DecisionVerdict::Invalid,
                                    fabric_scope(), ReasonCode::InvalidRequest);
    denial.reasons.clear();
    append_reason(denial.reasons, ReasonCode::InvalidRequest, "runtime", "the runtime is closed",
                  options_.limits);
    finalize_decision(denial);
    assessment_.decision = denial;
    assessment_.partitions.clear();
    return assessment_;
  }
  if (!roster_set_) {
    Decision denial = make_decision(DecisionKind::AssessFabric, DecisionVerdict::Invalid,
                                    fabric_scope(), ReasonCode::SubjectUnknown);
    denial.reasons.clear();
    append_reason(denial.reasons, ReasonCode::SubjectUnknown, "fabric",
                  "no authoritative roster has been adopted", options_.limits);
    finalize_decision(denial);
    assessment_.decision = denial;
    assessment_.partitions.clear();
    assessment_.reachability = ReachabilitySummary{};
    return assessment_;
  }
  const auto next_generation = state_.partition_generation.next();
  if (!next_generation.has_value()) {
    throw PartitionError(ErrorCode::ArithmeticOverflow, "runtime.partition",
                         "the partition generation is exhausted");
  }
  state_.partition_generation = *next_generation;

  ReachabilityBuildInput input;
  input.roster = &roster_;
  input.evidence = &ledger_.retained();
  input.policy = &policy_;
  input.now_tick = state_.tick;
  snapshot_ = build_reachability_snapshot(input, options_.limits);

  Decision decision = make_decision(DecisionKind::AssessFabric, DecisionVerdict::Observed,
                                    fabric_scope(), ReasonCode::DeterministicOrdering);
  decision.reasons.clear();
  decision.bindings.partition_generation = state_.partition_generation;

  PartitionAssessment outcome;
  outcome.generation = state_.partition_generation;

  if (!snapshot_.valid) {
    decision.verdict = DecisionVerdict::Invalid;
    append_reason(decision.reasons, ReasonCode::SubjectUnknown, "fabric",
                  "the reachability snapshot could not be built", options_.limits);
    finalize_decision(decision);
    outcome.decision = decision;
    partitions_.clear();
    assessment_ = outcome;
    last_assessment_confirmed_ = false;
    return outcome;
  }

  PartitionBuildInput build;
  build.snapshot = &snapshot_;
  build.generation = state_.partition_generation;
  build.policy = &policy_;
  build.lineage = &lineage_;
  build.tick = state_.tick;
  std::vector<Partition> built = build_partitions(build, options_.limits);

  std::vector<Reason> authority_reasons;
  const AuthorityEvaluation evaluation =
      evaluate_authority(snapshot_, built, allow_restore, authority_reasons);

  decision.verdict = evaluation.verdict;
  decision.subjects.reserve(built.size());
  for (const Partition& partition : built) {
    decision.subjects.push_back(partition.id);
  }
  for (const Reason& reason : snapshot_.reasons) {
    if (decision.reasons.size() >= options_.limits.max_reasons_per_decision) {
      break;
    }
    decision.reasons.push_back(reason);
  }
  for (const Reason& reason : authority_reasons) {
    if (decision.reasons.size() >= options_.limits.max_reasons_per_decision) {
      break;
    }
    decision.reasons.push_back(reason);
  }
  if (evaluation.authoritative.is_nil()) {
    decision.authority = observed_only_authority(CapabilitySet::all(), {});
    decision.authority.revocable = false;
  } else {
    for (const Partition& partition : built) {
      if (partition.id == evaluation.authoritative) {
        decision.authority = partition.authority;
        break;
      }
    }
  }
  decision.bindings.authority_sequence = state_.authority_sequence;
  finalize_decision(decision);

  // Durable lineage and the epoch watermark are committed before the assessment
  // is published, so a crash can never leave an observable grant that was not
  // preceded by its durable record.
  for (Partition& partition : built) {
    if (!snapshot_.any_evidence_fresh) {
      // With no fresh observation the decomposition is an artefact of having
      // seen nothing, not a statement about the fabric. Recording it as lineage
      // would later make an ordinary reconnect look like a reconciliation of
      // independent lineages.
      append_reason(partition.reasons, ReasonCode::EvidenceIncompleteCoverage, partition.id.view(),
                    "no fresh reachability evidence exists; membership is not durably recorded",
                    options_.limits);
      continue;
    }
    if (!partition.pending_merge_parents.empty()) {
      append_reason(partition.reasons, ReasonCode::RevalidationRequired, partition.id.view(),
                    "a governed merge is required before this membership becomes durable lineage",
                    options_.limits);
      continue;
    }
    const LineageRecord* existing = lineage_.latest(partition.lineage);
    if (existing != nullptr && existing->membership == partition.membership) {
      partition.lifecycle = PartitionLifecycle::Committed;
      continue;
    }
    const auto sequence = state_.lineage_sequence.next();
    if (!sequence.has_value()) {
      append_reason(partition.reasons, ReasonCode::ArithmeticRefused, partition.id.view(),
                    "the lineage sequence is exhausted; lineage was not committed", options_.limits);
      continue;
    }
    LineageRecord record;
    record.sequence = *sequence;
    record.lineage = partition.lineage;
    record.generation = partition.generation;
    record.membership = partition.membership;
    record.members = partition.members;
    record.event = partition.parent_lineage.is_nil() ? LineageEventKind::Genesis
                                                     : LineageEventKind::Split;
    record.parent_left = partition.parent_lineage;
    record.epoch = state_.epoch;
    record.boot = state_.boot;
    record.decision = decision.id;
    record.tick = state_.tick;
    record.digest = compute_lineage_digest(record);

    bool committed = true;
    if (store_ != nullptr) {
      const std::vector<std::byte> encoded = encode_lineage(record);
      if (!commit_record(DurableRecordType::LineageRecord, encoded)) {
        committed = false;
      }
    }
    if (!committed || !lineage_.append(record)) {
      append_reason(partition.reasons, ReasonCode::LimitExceededReason, partition.id.view(),
                    "the lineage store refused the record; the partition is not durably committed",
                    options_.limits);
      continue;
    }
    state_.lineage_sequence = *sequence;
    partition.lifecycle = PartitionLifecycle::Committed;
  }

  state_.live_grants.clear();
  for (const Partition& partition : built) {
    if (partition.authority.authorized.empty()) {
      continue;
    }
    AuthorityGrantMark mark;
    mark.partition = partition.id;
    mark.lineage = partition.lineage;
    mark.authority_sequence = partition.authority_sequence;
    state_.live_grants.push_back(mark);
  }
  if (store_ != nullptr) {
    const std::vector<std::byte> encoded = encode_epoch_state(state_);
    (void)commit_record(DurableRecordType::EpochState, encoded);
  }

  partitions_ = built;
  last_assessment_confirmed_ = snapshot_.confirmed;
  outcome.decision = decision;
  outcome.partitions = partitions_;
  outcome.confirmed = snapshot_.confirmed;
  outcome.split_brain = evaluation.split_brain;
  outcome.authoritative_partition = evaluation.authoritative;
  outcome.fabric_authority = evaluation.authoritative.is_nil()
                                 ? (evaluation.verdict == DecisionVerdict::Isolated
                                        ? PartitionAuthorityClass::Isolated
                                        : PartitionAuthorityClass::ObserveOnly)
                                 : PartitionAuthorityClass::Primary;
  outcome.reachability.valid = snapshot_.valid;
  outcome.reachability.confirmed = snapshot_.confirmed;
  outcome.reachability.complete_coverage_conflict = snapshot_.complete_coverage_conflict;
  outcome.reachability.topology_generation = snapshot_.topology_generation;
  outcome.reachability.reachability_generation = snapshot_.reachability_generation;
  outcome.reachability.evidence_digest = snapshot_.evidence_digest;
  outcome.reachability.counters = snapshot_.counters;
  outcome.reachability.component_count = snapshot_.component_count();
  outcome.reachability.fresh_bundle_count = snapshot_.fresh_bundle_count;
  outcome.reachability.stale_bundle_count = snapshot_.stale_bundle_count;
  assessment_ = outcome;
  return outcome;
}

// ---------------------------------------------------------------------------
// Merge governance
// ---------------------------------------------------------------------------

MergeOutcome PartitionRuntime::request_merge(const MergeRequest& request) {
  const std::lock_guard<std::mutex> guard(mutex_);
  MergeOutcome outcome;
  Decision decision = make_decision(DecisionKind::MergePartitions, DecisionVerdict::Invalid,
                                    partition_scope(request.subject), ReasonCode::InvalidRequest);
  decision.reasons.clear();
  const auto finish = [&](MergeOutcome& result) {
    result.verdict = decision.verdict;
    finalize_decision(decision);
    result.decision = decision;
    return result;
  };
  if (closed_) {
    append_reason(decision.reasons, ReasonCode::InvalidRequest, "runtime", "the runtime is closed",
                  options_.limits);
    return finish(outcome);
  }
  if (!check_attempt(request.attempt, DecisionKind::MergePartitions, decision)) {
    return finish(outcome);
  }
  record_attempt(request.attempt, DecisionKind::MergePartitions);
  if (!(request.expected_epoch == state_.epoch)) {
    decision.verdict = DecisionVerdict::Denied;
    append_reason(decision.reasons, ReasonCode::EpochMismatchReason, request.subject.view(),
                  "the request names epoch " + request.expected_epoch.render() +
                      " but the current epoch is " + state_.epoch.render(),
                  options_.limits);
    return finish(outcome);
  }
  const std::optional<Partition> subject = partition_by_id(request.subject);
  if (!subject.has_value()) {
    decision.verdict = DecisionVerdict::Denied;
    append_reason(decision.reasons, ReasonCode::MergeRejectedUnknownSubject, request.subject.view(),
                  "no partition with this identity exists in the current assessment",
                  options_.limits);
    return finish(outcome);
  }
  if (!(subject->generation == state_.partition_generation)) {
    decision.verdict = DecisionVerdict::Denied;
    append_reason(decision.reasons, ReasonCode::MergeRejectedGenerationMismatch,
                  request.subject.view(),
                  "the subject partition belongs to an earlier assessment generation",
                  options_.limits);
    return finish(outcome);
  }
  if (subject->pending_merge_parents.empty()) {
    decision.verdict = DecisionVerdict::Denied;
    append_reason(decision.reasons, ReasonCode::MergeRejectedAlreadyMerged, request.subject.view(),
                  "the partition does not span multiple live lineages", options_.limits);
    return finish(outcome);
  }
  const auto in_parents = [&subject](const LineageId& value) {
    return std::find(subject->pending_merge_parents.begin(), subject->pending_merge_parents.end(),
                     value) != subject->pending_merge_parents.end();
  };
  if (!in_parents(request.left) || !in_parents(request.right) ||
      request.left == request.right) {
    decision.verdict = DecisionVerdict::Invalid;
    append_reason(decision.reasons, ReasonCode::MergeRejectedUnknownSubject,
                  request.left.view(),
                  "both named lineages must be distinct live parents of the subject partition",
                  options_.limits);
    return finish(outcome);
  }
  if (!snapshot_.any_evidence_fresh) {
    decision.verdict = DecisionVerdict::Denied;
    append_reason(decision.reasons, ReasonCode::MergeRejectedStaleEvidence, request.subject.view(),
                  "no fresh reachability evidence supports this reconciliation", options_.limits);
    return finish(outcome);
  }
  if (!snapshot_.confirmed) {
    decision.verdict = DecisionVerdict::Denied;
    append_reason(decision.reasons, ReasonCode::MergeRejectedNotReachable, request.subject.view(),
                  "the merged component set is not confirmed by current evidence", options_.limits);
    return finish(outcome);
  }

  bool limit_reached = false;
  const LineageRelation relation = lineage_.relate(request.left, request.right, limit_reached);
  if (limit_reached || relation == LineageRelation::Indeterminate) {
    decision.verdict = DecisionVerdict::Indeterminate;
    append_reason(decision.reasons, ReasonCode::SearchLimitReached, request.left.view(),
                  "the bounded lineage ancestry walk could not resolve the relation between the "
                  "two parents",
                  options_.limits);
    append_reason(decision.reasons, ReasonCode::MergeRejectedIndeterminate,
                  request.subject.view(), "an unresolved ancestry relation is a refusal, not a "
                                          "permission",
                  options_.limits);
    return finish(outcome);
  }
  if (relation == LineageRelation::Identical || relation == LineageRelation::AncestorOf ||
      relation == LineageRelation::DescendantOf) {
    decision.verdict = DecisionVerdict::Conflict;
    append_reason(decision.reasons, ReasonCode::MergeRejectedLineageConflict, request.left.view(),
                  std::string("the two parents are related as ") +
                      std::string(lineage_relation_name(relation)) +
                      "; conflicting lineage may not be unioned",
                  options_.limits);
    return finish(outcome);
  }

  decision.verdict = DecisionVerdict::Granted;
  append_reason(decision.reasons, ReasonCode::MergeAccepted, request.subject.view(),
                "pairwise reconciliation committed with a new merged lineage", options_.limits);
  append_reason(decision.reasons, ReasonCode::MergeNewLineageEstablished,
                merge_lineage_id(request.left, request.right).view(),
                "the merged lineage supersedes both parents", options_.limits);
  assign_decision_identity(decision);

  const LineageId merged = merge_lineage_id(request.left, request.right);
  const auto sequence = state_.lineage_sequence.next();
  if (!sequence.has_value()) {
    decision.verdict = DecisionVerdict::Indeterminate;
    append_reason(decision.reasons, ReasonCode::ArithmeticRefused, request.subject.view(),
                  "the lineage sequence is exhausted", options_.limits);
    return finish(outcome);
  }
  LineageRecord record;
  record.sequence = *sequence;
  record.lineage = merged;
  record.generation = state_.partition_generation;
  record.membership = subject->membership;
  record.members = subject->members;
  record.event = LineageEventKind::Merge;
  record.parent_left = request.left < request.right ? request.left : request.right;
  record.parent_right = request.left < request.right ? request.right : request.left;
  record.epoch = state_.epoch;
  record.boot = state_.boot;
  record.decision = decision.id;
  record.tick = state_.tick;
  record.digest = compute_lineage_digest(record);

  if (store_ != nullptr) {
    const std::vector<std::byte> encoded = encode_lineage(record);
    if (!commit_record(DurableRecordType::LineageRecord, encoded)) {
      decision.verdict = DecisionVerdict::Indeterminate;
      append_reason(decision.reasons, ReasonCode::StoreIoFailure, request.subject.view(),
                    "the merge lineage record could not be committed durably", options_.limits);
      return finish(outcome);
    }
  }
  if (!lineage_.append(record)) {
    decision.verdict = DecisionVerdict::Invalid;
    append_reason(decision.reasons, ReasonCode::MergeRejectedLineageConflict, request.subject.view(),
                  "the lineage store refused the merge record", options_.limits);
    return finish(outcome);
  }
  state_.lineage_sequence = *sequence;

  if (!subject->authority.authorized.empty()) {
    FenceRecord fence =
        make_fence(*subject, subject->authority_sequence, ReasonCode::MergeFencedPriorAuthority);
    store_fence(fence);
    decision.fences_issued.push_back(fence.id);
    decision.fenced_partitions.push_back(subject->id);
    outcome.fenced_partitions.push_back(subject->id);
  }
  for (std::size_t index = 0; index < interrupted_.size();) {
    if (interrupted_[index].lineage == request.left || interrupted_[index].lineage == request.right) {
      interrupted_.erase(interrupted_.begin() + static_cast<std::ptrdiff_t>(index));
      continue;
    }
    ++index;
  }

  // The merge decision is published before the assessment it triggers so the
  // audit log remains monotonic in decision sequence.
  outcome.verdict = decision.verdict;
  finalize_decision(decision);
  outcome.decision = decision;

  outcome.merged_lineage = merged;
  const PartitionAssessment refreshed = assess_locked(false);
  for (const Partition& partition : refreshed.partitions) {
    if (partition.lineage == merged) {
      outcome.merged_partition = partition.id;
      break;
    }
  }
  return outcome;
}

// ---------------------------------------------------------------------------
// Isolation and retirement
// ---------------------------------------------------------------------------

IsolationOutcome PartitionRuntime::isolate(const IsolationRequest& request) {
  const std::lock_guard<std::mutex> guard(mutex_);
  IsolationOutcome outcome;
  Decision decision = make_decision(DecisionKind::IsolateComponents, DecisionVerdict::Invalid,
                                    fabric_scope(), ReasonCode::InvalidRequest);
  decision.reasons.clear();
  const auto finish = [&]() {
    outcome.verdict = decision.verdict;
    finalize_decision(decision);
    outcome.decision = decision;
    return outcome;
  };
  if (closed_) {
    append_reason(decision.reasons, ReasonCode::InvalidRequest, "runtime", "the runtime is closed",
                  options_.limits);
    return finish();
  }
  if (!check_attempt(request.attempt, DecisionKind::IsolateComponents, decision)) {
    return finish();
  }
  record_attempt(request.attempt, DecisionKind::IsolateComponents);
  if (!(request.expected_epoch == state_.epoch)) {
    decision.verdict = DecisionVerdict::Denied;
    append_reason(decision.reasons, ReasonCode::EpochMismatchReason, request.lineage.view(),
                  "the request names an epoch that is not current", options_.limits);
    return finish();
  }
  if (!lineage_.contains(request.lineage)) {
    decision.verdict = DecisionVerdict::Denied;
    append_reason(decision.reasons, ReasonCode::IsolationRejectedUnknownSubject,
                  request.lineage.view(), "the lineage is not present in durable lineage",
                  options_.limits);
    return finish();
  }
  if (isolations_.size() >= options_.limits.max_revalidations_retained) {
    decision.verdict = DecisionVerdict::Indeterminate;
    append_reason(decision.reasons, ReasonCode::CapacityExhausted, request.lineage.view(),
                  "the isolation table is full", options_.limits);
    return finish();
  }
  IsolationDirective directive;
  directive.lineage = request.lineage;
  directive.cause = request.cause;
  directive.expires_at_tick =
      request.duration_ticks == 0 ? 0 : state_.tick + request.duration_ticks;
  isolations_.push_back(directive);
  decision.scope = named_scope(ScopeKind::Domain, request.lineage.view());
  decision.verdict = DecisionVerdict::Isolated;
  append_reason(decision.reasons, ReasonCode::IsolationApplied, request.lineage.view(),
                request.duration_ticks == 0
                    ? "isolation applies until it is explicitly cleared"
                    : "isolation expires at tick " + std::to_string(directive.expires_at_tick),
                options_.limits);
  // The directive decision is appended in sequence order before the assessment
  // it triggers, so the audit log is always monotonic in decision sequence.
  outcome.verdict = decision.verdict;
  finalize_decision(decision);
  outcome.decision = decision;
  (void)assess_locked(false);
  return outcome;
}

IsolationOutcome PartitionRuntime::clear_isolation(const IsolationRequest& request) {
  const std::lock_guard<std::mutex> guard(mutex_);
  IsolationOutcome outcome;
  Decision decision = make_decision(DecisionKind::IsolateComponents, DecisionVerdict::Invalid,
                                    fabric_scope(), ReasonCode::InvalidRequest);
  decision.reasons.clear();
  const auto finish = [&]() {
    outcome.verdict = decision.verdict;
    finalize_decision(decision);
    outcome.decision = decision;
    return outcome;
  };
  if (closed_) {
    append_reason(decision.reasons, ReasonCode::InvalidRequest, "runtime", "the runtime is closed",
                  options_.limits);
    return finish();
  }
  if (!check_attempt(request.attempt, DecisionKind::IsolateComponents, decision)) {
    return finish();
  }
  record_attempt(request.attempt, DecisionKind::IsolateComponents);
  if (!(request.expected_epoch == state_.epoch)) {
    decision.verdict = DecisionVerdict::Denied;
    append_reason(decision.reasons, ReasonCode::EpochMismatchReason, request.lineage.view(),
                  "the request names an epoch that is not current", options_.limits);
    return finish();
  }
  std::size_t removed = 0;
  for (std::size_t index = 0; index < isolations_.size();) {
    if (isolations_[index].lineage == request.lineage) {
      isolations_.erase(isolations_.begin() + static_cast<std::ptrdiff_t>(index));
      ++removed;
      continue;
    }
    ++index;
  }
  decision.scope = named_scope(ScopeKind::Domain, request.lineage.view());
  decision.verdict = removed == 0 ? DecisionVerdict::Denied : DecisionVerdict::Granted;
  append_reason(decision.reasons,
                removed == 0 ? ReasonCode::IsolationRejectedUnknownSubject
                             : ReasonCode::RevalidationGranted,
                request.lineage.view(),
                removed == 0 ? "no isolation directive applied to this lineage"
                             : "isolation directive cleared",
                options_.limits);
  outcome.verdict = decision.verdict;
  finalize_decision(decision);
  outcome.decision = decision;
  (void)assess_locked(false);
  return outcome;
}

Decision PartitionRuntime::retire(const RetireRequest& request) {
  const std::lock_guard<std::mutex> guard(mutex_);
  Decision decision = make_decision(DecisionKind::Retire, DecisionVerdict::Invalid,
                                    named_scope(ScopeKind::Domain, request.lineage.view()),
                                    ReasonCode::InvalidRequest);
  decision.reasons.clear();
  const auto finish = [&]() {
    finalize_decision(decision);
    return decision;
  };
  if (closed_) {
    append_reason(decision.reasons, ReasonCode::InvalidRequest, "runtime", "the runtime is closed",
                  options_.limits);
    return finish();
  }
  if (!check_attempt(request.attempt, DecisionKind::Retire, decision)) {
    return finish();
  }
  record_attempt(request.attempt, DecisionKind::Retire);
  if (!(request.expected_epoch == state_.epoch)) {
    decision.verdict = DecisionVerdict::Denied;
    append_reason(decision.reasons, ReasonCode::EpochMismatchReason, request.lineage.view(),
                  "the request names an epoch that is not current", options_.limits);
    return finish();
  }
  const LineageRecord* latest = lineage_.latest(request.lineage);
  if (latest == nullptr) {
    decision.verdict = DecisionVerdict::Denied;
    append_reason(decision.reasons, ReasonCode::SubjectUnknown, request.lineage.view(),
                  "the lineage is not present in durable lineage", options_.limits);
    return finish();
  }
  if (latest->event == LineageEventKind::Retire) {
    decision.verdict = DecisionVerdict::Observed;
    append_reason(decision.reasons, ReasonCode::PartitionRetired, request.lineage.view(),
                  "the lineage is already retired", options_.limits);
    return finish();
  }
  const auto sequence = state_.lineage_sequence.next();
  if (!sequence.has_value()) {
    decision.verdict = DecisionVerdict::Indeterminate;
    append_reason(decision.reasons, ReasonCode::ArithmeticRefused, request.lineage.view(),
                  "the lineage sequence is exhausted", options_.limits);
    return finish();
  }
  decision.verdict = DecisionVerdict::Granted;
  append_reason(decision.reasons, ReasonCode::PartitionRetired, request.lineage.view(),
                "the lineage is retired and every grant derived from it is fenced", options_.limits);
  assign_decision_identity(decision);
  LineageRecord record;
  record.sequence = *sequence;
  record.lineage = request.lineage;
  record.generation = latest->generation;
  record.membership = latest->membership;
  record.members = latest->members;
  record.event = LineageEventKind::Retire;
  record.epoch = state_.epoch;
  record.boot = state_.boot;
  record.decision = decision.id;
  record.tick = state_.tick;
  record.digest = compute_lineage_digest(record);
  if (store_ != nullptr) {
    const std::vector<std::byte> encoded = encode_lineage(record);
    if (!commit_record(DurableRecordType::LineageRecord, encoded)) {
      decision.verdict = DecisionVerdict::Indeterminate;
      append_reason(decision.reasons, ReasonCode::StoreIoFailure, request.lineage.view(),
                    "the retirement record could not be committed durably", options_.limits);
      return finish();
    }
  }
  if (!lineage_.append(record)) {
    decision.verdict = DecisionVerdict::Invalid;
    append_reason(decision.reasons, ReasonCode::InvalidRequest, request.lineage.view(),
                  "the lineage store refused the retirement record", options_.limits);
    return finish();
  }
  state_.lineage_sequence = *sequence;
  for (std::size_t index = 0; index < partitions_.size(); ++index) {
    if (!(partitions_[index].lineage == request.lineage)) {
      continue;
    }
    if (!partitions_[index].authority.authorized.empty()) {
      const FenceRecord fence = make_fence(partitions_[index], partitions_[index].authority_sequence,
                                           ReasonCode::PartitionRetired);
      store_fence(fence);
      decision.fences_issued.push_back(fence.id);
      decision.fenced_partitions.push_back(partitions_[index].id);
    }
  }
  for (std::size_t index = 0; index < interrupted_.size();) {
    if (interrupted_[index].lineage == request.lineage) {
      interrupted_.erase(interrupted_.begin() + static_cast<std::ptrdiff_t>(index));
      continue;
    }
    ++index;
  }
  return finish();
}

// ---------------------------------------------------------------------------
// Revalidation
// ---------------------------------------------------------------------------

RevalidationOutcome PartitionRuntime::revalidate(const RevalidationRequest& request) {
  const std::lock_guard<std::mutex> guard(mutex_);
  RevalidationOutcome outcome;
  Decision decision = make_decision(DecisionKind::Revalidate, DecisionVerdict::Invalid,
                                    fabric_scope(), ReasonCode::InvalidRequest);
  decision.reasons.clear();
  const auto finish = [&]() {
    outcome.verdict = decision.verdict;
    finalize_decision(decision);
    outcome.decision = decision;
    return outcome;
  };
  if (closed_) {
    append_reason(decision.reasons, ReasonCode::InvalidRequest, "runtime", "the runtime is closed",
                  options_.limits);
    return finish();
  }
  if (!check_attempt(request.attempt, DecisionKind::Revalidate, decision)) {
    return finish();
  }
  record_attempt(request.attempt, DecisionKind::Revalidate);
  if (!(request.expected_epoch == state_.epoch)) {
    decision.verdict = DecisionVerdict::Denied;
    append_reason(decision.reasons, ReasonCode::EpochMismatchReason, "revalidate",
                  "the request names an epoch that is not current", options_.limits);
    return finish();
  }
  if (!roster_set_) {
    decision.verdict = DecisionVerdict::Invalid;
    append_reason(decision.reasons, ReasonCode::SubjectUnknown, "fabric",
                  "no authoritative roster has been adopted", options_.limits);
    return finish();
  }
  const std::size_t interrupted_before = interrupted_.size();
  const PartitionAssessment refreshed = assess_locked(true);
  outcome.partitions = refreshed.partitions;

  for (const Partition& partition : refreshed.partitions) {
    for (const InterruptedAuthority& entry : interrupted_) {
      if (entry.lineage != partition.lineage) {
        continue;
      }
      if (partition.authority.authorized.empty()) {
        outcome.still_interrupted.push_back(partition.id);
        append_reason(decision.reasons, ReasonCode::RevalidationDenied, partition.id.view(),
                      "current evidence does not support restoring the interrupted authority",
                      options_.limits);
        break;
      }
      outcome.restored.push_back(partition.id);
      append_reason(decision.reasons, ReasonCode::RevalidationGranted, partition.id.view(),
                    "authority was re-derived from fresh evidence under epoch " +
                        state_.epoch.render(),
                    options_.limits);
      FenceRecord fence = make_fence(partition, entry.authority_sequence,
                                     ReasonCode::AuthorityInterruptedByRestart);
      store_fence(fence);
      decision.fences_issued.push_back(fence.id);
      decision.fenced_partitions.push_back(partition.id);
      break;
    }
  }

  for (const InterruptedAuthority& entry : interrupted_) {
    if (std::find(outcome.restored.begin(), outcome.restored.end(), entry.partition) ==
        outcome.restored.end()) {
      bool present = false;
      for (const Partition& partition : refreshed.partitions) {
        if (partition.lineage == entry.lineage) {
          present = true;
          break;
        }
      }
      if (!present) {
        append_reason(decision.reasons, ReasonCode::RevalidationIndeterminate, entry.partition.view(),
                      "the interrupted lineage is not present in the current decomposition",
                      options_.limits);
      }
    }
  }

  for (std::size_t index = 0; index < interrupted_.size();) {
    bool restored = false;
    for (const PartitionId& id : outcome.restored) {
      if (interrupted_[index].partition == id) {
        restored = true;
        break;
      }
    }
    if (!restored) {
      for (const Partition& partition : refreshed.partitions) {
        if (partition.lineage == interrupted_[index].lineage &&
            !partition.authority.authorized.empty()) {
          restored = true;
          break;
        }
      }
    }
    if (restored) {
      interrupted_.erase(interrupted_.begin() + static_cast<std::ptrdiff_t>(index));
      continue;
    }
    ++index;
  }

  if (!refreshed.reachability.valid || refreshed.reachability.fresh_bundle_count == 0) {
    decision.verdict = DecisionVerdict::Indeterminate;
    append_reason(decision.reasons, ReasonCode::RevalidationIndeterminate, "fabric",
                  "no fresh reachability evidence is available; authority cannot be revalidated",
                  options_.limits);
  } else if (!outcome.restored.empty()) {
    decision.verdict = DecisionVerdict::Granted;
  } else if (interrupted_before == 0) {
    decision.verdict = DecisionVerdict::Observed;
    append_reason(decision.reasons, ReasonCode::RevalidationGranted, "fabric",
                  "no authority was interrupted; current authority stands", options_.limits);
  } else {
    decision.verdict = DecisionVerdict::Denied;
  }
  decision.authority = refreshed.decision.authority;
  return finish();
}

// ---------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------

Decision PartitionRuntime::record_acknowledgement(const AuthorityAcknowledgement& acknowledgement) {
  const std::lock_guard<std::mutex> guard(mutex_);
  Decision decision = make_decision(DecisionKind::AcknowledgeAuthority, DecisionVerdict::Invalid,
                                    partition_scope(acknowledgement.partition),
                                    ReasonCode::InvalidRequest);
  decision.reasons.clear();
  const auto finish = [&]() {
    finalize_decision(decision);
    return decision;
  };
  if (closed_) {
    append_reason(decision.reasons, ReasonCode::InvalidRequest, "runtime", "the runtime is closed",
                  options_.limits);
    return finish();
  }
  const std::optional<Partition> partition = partition_by_id(acknowledgement.partition);
  if (!partition.has_value()) {
    append_reason(decision.reasons, ReasonCode::SubjectUnknown,
                  acknowledgement.partition.view(),
                  "no partition with this identity holds a grant in the current assessment",
                  options_.limits);
    return finish();
  }
  if (partition->authority.authorized.empty()) {
    append_reason(decision.reasons, ReasonCode::AuthorityDeniedFailClosed,
                  acknowledgement.partition.view(),
                  "the partition holds no authority to acknowledge", options_.limits);
    return finish();
  }
  for (const AuthorityAcknowledgement& existing : acknowledgements_) {
    if (existing.attempt == acknowledgement.attempt &&
        existing.partition == acknowledgement.partition) {
      append_reason(decision.reasons, ReasonCode::AttemptDuplicate,
                    acknowledgement.partition.view(),
                    "this attempt has already been acknowledged", options_.limits);
      return finish();
    }
  }
  if (!partition->authority.authorized.contains_all(acknowledgement.applied)) {
    append_reason(decision.reasons, ReasonCode::AuthorityDeniedFailClosed,
                  acknowledgement.partition.view(),
                  "the acknowledgement claims capabilities that were never authorized",
                  options_.limits);
    return finish();
  }
  if (acknowledgements_.size() >= options_.limits.max_attempt_records) {
    append_reason(decision.reasons, ReasonCode::CapacityExhausted, acknowledgement.partition.view(),
                  "the acknowledgement table is full", options_.limits);
    return finish();
  }
  acknowledgements_.push_back(acknowledgement);
  decision.verdict = DecisionVerdict::Observed;
  append_reason(decision.reasons, ReasonCode::DeterministicOrdering,
                acknowledgement.partition.view(),
                acknowledgement.accepted
                    ? "the subject reported applying the authorization; this is a report, not a "
                      "verified effect"
                    : "the subject reported refusing the authorization",
                options_.limits);
  return finish();
}

Decision PartitionRuntime::record_verified_effect(const VerifiedEffect& effect) {
  const std::lock_guard<std::mutex> guard(mutex_);
  Decision decision = make_decision(DecisionKind::VerifyEffect, DecisionVerdict::Invalid,
                                    partition_scope(effect.partition), ReasonCode::InvalidRequest);
  decision.reasons.clear();
  const auto finish = [&]() {
    finalize_decision(decision);
    return decision;
  };
  if (closed_) {
    append_reason(decision.reasons, ReasonCode::InvalidRequest, "runtime", "the runtime is closed",
                  options_.limits);
    return finish();
  }
  const std::optional<Partition> partition = partition_by_id(effect.partition);
  if (!partition.has_value()) {
    append_reason(decision.reasons, ReasonCode::SubjectUnknown, effect.partition.view(),
                  "no partition with this identity exists in the current assessment",
                  options_.limits);
    return finish();
  }
  bool acknowledged = false;
  for (const AuthorityAcknowledgement& existing : acknowledgements_) {
    if (existing.attempt == effect.attempt && existing.partition == effect.partition &&
        existing.accepted) {
      acknowledged = true;
      break;
    }
  }
  if (!acknowledged) {
    append_reason(decision.reasons, ReasonCode::AttemptUnknown, effect.partition.view(),
                  "no accepted acknowledgement exists for this attempt; a verified effect cannot "
                  "skip acknowledgement",
                  options_.limits);
    return finish();
  }
  if (!partition->authority.authorized.contains_all(effect.confirmed)) {
    append_reason(decision.reasons, ReasonCode::AuthorityDeniedFailClosed, effect.partition.view(),
                  "the observed effect exceeds the authorized capability set", options_.limits);
    return finish();
  }
  for (const VerifiedEffect& existing : verified_effects_) {
    if (existing.partition == effect.partition && existing.attempt == effect.attempt) {
      append_reason(decision.reasons, ReasonCode::AttemptDuplicate, effect.partition.view(),
                    "this effect has already been verified", options_.limits);
      return finish();
    }
  }
  if (verified_effects_.size() >= options_.limits.max_attempt_records) {
    append_reason(decision.reasons, ReasonCode::CapacityExhausted, effect.partition.view(),
                  "the verified effect table is full", options_.limits);
    return finish();
  }
  verified_effects_.push_back(effect);
  decision.verdict = DecisionVerdict::Observed;
  append_reason(decision.reasons, ReasonCode::DeterministicOrdering, effect.partition.view(),
                "an independent observation confirmed the authorized effect", options_.limits);
  return finish();
}

AuthorityApplicationState PartitionRuntime::application_state(const PartitionId& partition) const {
  const std::lock_guard<std::mutex> guard(mutex_);
  const std::optional<Partition> current = partition_by_id(partition);
  if (!current.has_value()) {
    for (const InterruptedAuthority& entry : interrupted_) {
      if (entry.partition == partition) {
        return AuthorityApplicationState::Interrupted;
      }
    }
    return AuthorityApplicationState::Revoked;
  }
  if (current->authority.authorized.empty()) {
    for (const InterruptedAuthority& entry : interrupted_) {
      if (entry.lineage == current->lineage) {
        return AuthorityApplicationState::Interrupted;
      }
    }
    return AuthorityApplicationState::NotApplicable;
  }
  for (const VerifiedEffect& effect : verified_effects_) {
    if (effect.partition == partition) {
      return AuthorityApplicationState::VerifiedByObservation;
    }
  }
  for (const AuthorityAcknowledgement& acknowledgement : acknowledgements_) {
    if (acknowledgement.partition == partition && acknowledgement.accepted) {
      return AuthorityApplicationState::AcknowledgedBySubject;
    }
  }
  return AuthorityApplicationState::AuthorizationIssued;
}

// ---------------------------------------------------------------------------
// Queries and shutdown
// ---------------------------------------------------------------------------

std::optional<Partition> PartitionRuntime::partition_by_id(const PartitionId& id) const {
  for (const Partition& partition : partitions_) {
    if (partition.id == id) {
      return partition;
    }
  }
  return std::nullopt;
}

std::optional<Partition> PartitionRuntime::find_partition(const PartitionId& id) const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return partition_by_id(id);
}

PartitionAssessment PartitionRuntime::last_assessment() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return assessment_;
}

std::vector<Partition> PartitionRuntime::current_partitions() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return partitions_;
}

std::vector<Decision> PartitionRuntime::decision_history() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return decisions_.entries();
}

std::optional<Decision> PartitionRuntime::find_decision(const DecisionId& id) const {
  const std::lock_guard<std::mutex> guard(mutex_);
  const Decision* found = decisions_.find(id);
  if (found == nullptr) {
    return std::nullopt;
  }
  return *found;
}

std::vector<FenceRecord> PartitionRuntime::fences() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return fences_;
}

std::vector<LineageRecord> PartitionRuntime::lineage_records() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return lineage_.records();
}

std::vector<InterruptedAuthority> PartitionRuntime::interrupted_authorities() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return interrupted_;
}

std::vector<Reason> PartitionRuntime::restart_reasons() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return restart_reasons_;
}

ReachabilitySummary PartitionRuntime::reachability_summary() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return assessment_.reachability;
}

ComponentRoster PartitionRuntime::roster() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return roster_;
}

std::optional<TopologyDefinition> PartitionRuntime::adopted_topology() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  if (!roster_set_) {
    return std::nullopt;
  }
  return topology_;
}

void PartitionRuntime::close() {
  const std::lock_guard<std::mutex> guard(mutex_);
  if (closed_) {
    return;
  }
  closed_ = true;
  if (store_ != nullptr) {
    if (options_.compact_on_close) {
      (void)store_->compact();
    }
    store_->close();
  }
}

}  // namespace fabric_partition_manager
