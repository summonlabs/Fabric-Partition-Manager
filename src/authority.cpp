// Fabric Partition Manager 1.0.0 - Summon Software Labs
#include "fabric_partition_manager/authority.hpp"

namespace fabric_partition_manager {

std::string_view partition_authority_class_name(PartitionAuthorityClass value) noexcept {
  switch (value) {
    case PartitionAuthorityClass::Unassigned: return "UNASSIGNED";
    case PartitionAuthorityClass::Primary: return "PRIMARY";
    case PartitionAuthorityClass::Degraded: return "DEGRADED";
    case PartitionAuthorityClass::ReadOnly: return "READ_ONLY";
    case PartitionAuthorityClass::ObserveOnly: return "OBSERVE_ONLY";
    case PartitionAuthorityClass::Isolated: return "ISOLATED";
  }
  return "UNRECOGNIZED";
}

std::string_view authority_basis_name(AuthorityBasis value) noexcept {
  switch (value) {
    case AuthorityBasis::None: return "NONE";
    case AuthorityBasis::QuorumSatisfied: return "QUORUM_SATISFIED";
    case AuthorityBasis::QuorumNotSatisfied: return "QUORUM_NOT_SATISFIED";
    case AuthorityBasis::PartialPartition: return "PARTIAL_PARTITION";
    case AuthorityBasis::UnknownEvidence: return "UNKNOWN_EVIDENCE";
    case AuthorityBasis::SplitBrainConflict: return "SPLIT_BRAIN_CONFLICT";
    case AuthorityBasis::PolicyDegradation: return "POLICY_DEGRADATION";
    case AuthorityBasis::ExplicitIsolation: return "EXPLICIT_ISOLATION";
    case AuthorityBasis::RestartInterrupted: return "RESTART_INTERRUPTED";
    case AuthorityBasis::SubjectOutOfScope: return "SUBJECT_OUT_OF_SCOPE";
  }
  return "UNRECOGNIZED";
}

std::string_view authority_application_state_name(AuthorityApplicationState value) noexcept {
  switch (value) {
    case AuthorityApplicationState::NotApplicable: return "NOT_APPLICABLE";
    case AuthorityApplicationState::AuthorizationIssued: return "AUTHORIZATION_ISSUED";
    case AuthorityApplicationState::AcknowledgedBySubject: return "ACKNOWLEDGED_BY_SUBJECT";
    case AuthorityApplicationState::VerifiedByObservation: return "VERIFIED_BY_OBSERVATION";
    case AuthorityApplicationState::Interrupted: return "INTERRUPTED";
    case AuthorityApplicationState::Revoked: return "REVOKED";
  }
  return "UNRECOGNIZED";
}

CapabilitySet capabilities_for_class(const PartitionPolicy& policy,
                                     PartitionAuthorityClass authority_class) noexcept {
  switch (authority_class) {
    case PartitionAuthorityClass::Unassigned:
    case PartitionAuthorityClass::Isolated:
      return CapabilitySet::none();
    case PartitionAuthorityClass::ObserveOnly:
      return CapabilitySet::of(AuthorityCapability::Observe);
    case PartitionAuthorityClass::ReadOnly:
      return policy.readonly_capabilities;
    case PartitionAuthorityClass::Degraded:
      return policy.degraded_capabilities;
    case PartitionAuthorityClass::Primary:
      return CapabilitySet::all();
  }
  return CapabilitySet::none();
}

bool AuthorityVector::matches_class_capabilities(const PartitionPolicy& policy) const noexcept {
  const CapabilitySet expected = capabilities_for_class(policy, authority_class);
  if (!authorized.contains_all(expected) && authority_class != PartitionAuthorityClass::Unassigned) {
    return false;
  }
  // An isolated or unassigned subject must never hold authority.
  if ((authority_class == PartitionAuthorityClass::Isolated ||
       authority_class == PartitionAuthorityClass::Unassigned) &&
      !authorized.empty()) {
    return false;
  }
  // Authority must always be a subset of what is eligible: a runtime may not
  // grant something it declared ineligible.
  return eligible.contains_all(authorized);
}

std::string AuthorityVector::render() const {
  std::string result;
  result.append(partition_authority_class_name(authority_class));
  result.append(" basis=");
  result.append(authority_basis_name(basis));
  result.append(" stage=");
  result.append(authority_application_state_name(application_state));
  result.append(" observed=[");
  result.append(observed.render());
  result.append("] eligible=[");
  result.append(eligible.render());
  result.append("] recommended=[");
  result.append(recommended.render());
  result.append("] authorized=[");
  result.append(authorized.render());
  result.append("] withheld=[");
  result.append(withheld.render());
  result.append("] denied=[");
  result.append(denied.render());
  result.append("]");
  return result;
}

AuthorityVector observed_only_authority(const CapabilitySet& observed_capabilities,
                                        const std::vector<Reason>& reasons) {
  AuthorityVector vector;
  vector.authority_class = PartitionAuthorityClass::Unassigned;
  vector.basis = AuthorityBasis::None;
  vector.application_state = AuthorityApplicationState::NotApplicable;
  vector.observed = observed_capabilities;
  vector.revocable = false;
  vector.reasons = reasons;
  return vector;
}

AuthorityVector isolated_authority(AuthorityBasis basis,
                                   const CapabilitySet& observed_capabilities,
                                   const std::vector<Reason>& reasons) {
  AuthorityVector vector;
  vector.authority_class = PartitionAuthorityClass::Isolated;
  vector.basis = basis;
  vector.application_state = AuthorityApplicationState::AuthorizationIssued;
  vector.observed = observed_capabilities;
  vector.eligible = CapabilitySet::none();
  vector.recommended = CapabilitySet::none();
  vector.authorized = CapabilitySet::none();
  vector.denied = observed_capabilities;
  vector.revocable = true;
  vector.reasons = reasons;
  return vector;
}

AuthorityVector authorized_authority(PartitionAuthorityClass authority_class,
                                     AuthorityBasis basis,
                                     const CapabilitySet& observed_capabilities,
                                     const CapabilitySet& eligible,
                                     const CapabilitySet& authorized,
                                     const std::vector<Reason>& reasons) {
  AuthorityVector vector;
  vector.authority_class = authority_class == PartitionAuthorityClass::Unassigned
                               ? PartitionAuthorityClass::Unassigned
                               : authority_class;
  vector.basis = basis;
  vector.observed = observed_capabilities;
  vector.eligible = eligible;
  vector.recommended = authorized;
  vector.authorized = authorized;
  // withheld  : the policy would permit it, but it was not conferred.
  // denied    : everything the subject is physically able to do that this
  //             runtime does NOT authorize, whether or not the policy would
  //             ever permit it. An isolated subject therefore denies every
  //             observed capability, which is the fail-closed reading.
  vector.withheld = eligible.without(authorized);
  vector.denied = observed_capabilities.without(authorized);
  vector.revocable = true;
  vector.application_state = AuthorityApplicationState::AuthorizationIssued;
  vector.reasons = reasons;
  return vector;
}

}  // namespace fabric_partition_manager
