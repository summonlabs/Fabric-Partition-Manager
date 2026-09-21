// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// The authority vector.
//
// Reachability is not authority. This header keeps six things apart that are
// routinely conflated:
//
//   OBSERVATION      what the component set could physically exercise
//   ELIGIBILITY      what the policy permits this subject to be granted
//   RECOMMENDATION   what the assessment proposes
//   AUTHORIZATION    what this runtime actually confers
//   ACKNOWLEDGEMENT  what the subject reported applying
//   VERIFIED EFFECT  what independent observation confirmed
//
// Each has its own field and its own type-level name. A caller cannot read the
// recommendation field and mistake it for a grant, and this runtime never
// reports a verified effect it did not observe.
#ifndef FABRIC_PARTITION_MANAGER_AUTHORITY_HPP
#define FABRIC_PARTITION_MANAGER_AUTHORITY_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "fabric_partition_manager/policy.hpp"
#include "fabric_partition_manager/reason.hpp"
#include "fabric_partition_manager/strong_id.hpp"

namespace fabric_partition_manager {

enum class PartitionAuthorityClass : std::uint8_t {
  Unassigned = 0,
  Primary = 1,
  Degraded = 2,
  ReadOnly = 3,
  ObserveOnly = 4,
  Isolated = 5,
};
inline constexpr std::uint8_t partition_authority_class_domain_max = 5;

// Why the authority vector holds the class it holds. A basis is recorded for
// every class, including the classes that grant nothing.
enum class AuthorityBasis : std::uint8_t {
  None = 0,
  QuorumSatisfied = 1,
  QuorumNotSatisfied = 2,
  PartialPartition = 3,
  UnknownEvidence = 4,
  SplitBrainConflict = 5,
  PolicyDegradation = 6,
  ExplicitIsolation = 7,
  RestartInterrupted = 8,
  SubjectOutOfScope = 9,
};
inline constexpr std::uint8_t authority_basis_domain_max = 9;

// How far the authority has actually travelled. AuthorizationIssued is the
// furthest state this runtime can reach on its own.
enum class AuthorityApplicationState : std::uint8_t {
  NotApplicable = 0,
  AuthorizationIssued = 1,
  AcknowledgedBySubject = 2,
  VerifiedByObservation = 3,
  Interrupted = 4,
  Revoked = 5,
};
inline constexpr std::uint8_t authority_application_state_domain_max = 5;

[[nodiscard]] std::string_view partition_authority_class_name(
    PartitionAuthorityClass value) noexcept;
[[nodiscard]] std::string_view authority_basis_name(AuthorityBasis value) noexcept;
[[nodiscard]] std::string_view authority_application_state_name(
    AuthorityApplicationState value) noexcept;

struct AuthorityVector {
  PartitionAuthorityClass authority_class = PartitionAuthorityClass::Unassigned;
  AuthorityBasis basis = AuthorityBasis::None;
  AuthorityApplicationState application_state = AuthorityApplicationState::NotApplicable;

  // OBSERVATION. What the component set is physically able to do. This field
  // never confers anything; it only records what was seen.
  CapabilitySet observed;

  // ELIGIBILITY. What the policy would permit this subject to hold if the
  // evidence supported it.
  CapabilitySet eligible;

  // RECOMMENDATION. What this assessment proposes. Not a grant.
  CapabilitySet recommended;

  // AUTHORIZATION. What this runtime confers. This is the only field that
  // carries authority, and it is set only when the decision verdict grants.
  CapabilitySet authorized;

  // Capabilities the subject is eligible for but that were withheld, and
  // capabilities the policy denies outright.
  CapabilitySet withheld;
  CapabilitySet denied;

  bool revocable = true;
  std::vector<Reason> reasons;

  [[nodiscard]] bool confers_authority() const noexcept { return !authorized.empty(); }

  [[nodiscard]] bool matches_class_capabilities(const PartitionPolicy& policy) const noexcept;

  [[nodiscard]] std::string render() const;
};

// ---------------------------------------------------------------------------
// Acknowledgement and verified effect.
//
// These are reported by the subject and by independent observation
// respectively. They are separate types from AuthorityVector so that a subject
// claiming it applied a grant can never be read as this runtime having verified
// anything.
// ---------------------------------------------------------------------------

struct AuthorityAcknowledgement {
  AttemptId attempt;
  PartitionId partition;
  bool accepted = false;
  CapabilitySet applied;
  std::string note;
};

struct VerifiedEffect {
  PartitionId partition;
  AttemptId attempt;
  CapabilitySet confirmed;
  EvidenceId evidence;
};

// Builds the observation-only view of a component set. The result carries no
// authority at any stage.
[[nodiscard]] AuthorityVector observed_only_authority(const CapabilitySet& observed_capabilities,
                                                      const std::vector<Reason>& reasons);

// Builds an isolation vector: nothing is granted, everything eligible is
// withheld, and the basis records why.
[[nodiscard]] AuthorityVector isolated_authority(AuthorityBasis basis,
                                                 const CapabilitySet& observed_capabilities,
                                                 const std::vector<Reason>& reasons);

// Builds a grant vector at the AUTHORIZATION stage.
[[nodiscard]] AuthorityVector authorized_authority(PartitionAuthorityClass authority_class,
                                                   AuthorityBasis basis,
                                                   const CapabilitySet& observed_capabilities,
                                                   const CapabilitySet& eligible,
                                                   const CapabilitySet& authorized,
                                                   const std::vector<Reason>& reasons);

// The capabilities a policy confers for a class. Used to validate that an
// authority vector is internally consistent.
[[nodiscard]] CapabilitySet capabilities_for_class(const PartitionPolicy& policy,
                                                   PartitionAuthorityClass authority_class) noexcept;

}  // namespace fabric_partition_manager

#endif  // FABRIC_PARTITION_MANAGER_AUTHORITY_HPP
