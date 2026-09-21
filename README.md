# Fabric Partition Manager

**Fabric Partition Manager is the authoritative representation and governance runtime for partially
disconnected fabric components of the Distributed Fabric Infrastructure / Fabric OS stack.** It
answers exactly one question:

> Given authoritative topology and reachability evidence showing a partially disconnected fabric,
> which components exist now, what authority may each retain, what must be isolated or degraded, and
> when may partitions merge or be revalidated?

Fabric Partition Manager 1.0.0 is a C++20 library, a framed wire protocol, a coordinator process, a
publisher process and a command-line client. It is vendor-neutral, has no third-party runtime
dependency, and builds with CMake.

The core invariants are:

> **Reachability is not authority.**
> **Absence of evidence is not evidence of absence.**
> **Persistence is not liveness, and a matching identity is not a matching generation.**

---

## 1. Systems boundary

### Fabric Partition Manager owns

* the canonical decomposition of an authoritative component roster into partitions under current
  reachability evidence;
* canonical, generation-bound partition identity and membership digests;
* the resolution of contradictory, stale, asymmetric and incomplete reachability evidence;
* the authority vector for every partition: observation, eligibility, recommendation,
  authorization, acknowledgement and verified effect, kept apart;
* quorum, degradation, read-only, observe-only and isolation policy enforcement;
* governed split, merge and revalidation of partition lineage;
* the coordinator epoch, coordinator boot identity, process incarnation and monotonic attempt
  sequence that bound every decision;
* durable lineage, fences and completed decisions, with conservative restart semantics;
* deterministic, bounded explanations for every externally visible outcome;
* a bounded framed protocol, a coordinator service and a client;
* partition snapshots with deterministic semantic digests.

### Fabric Partition Manager does not own

* canonical device identity (Fabric Registry);
* structural topology discovery (Fabric Topology);
* operational link state (Link State Fabric);
* port configuration (Port Fabric);
* routing, path planning or path authority;
* traffic engineering, scheduling, congestion control, queue or buffer allocation;
* application leader election or distributed consensus (not implemented; see section 16);
* healing physical links.

Topology and reachability arrive as explicit typed inputs. This runtime never discovers raw
topology, never routes, never elects an application leader, and never repairs a link. It integrates
through evidence, references and authority boundaries only.

---

## 2. The problem the runtime solves

A fabric is partially disconnected. Some component pairs were observed reachable, some were observed
unreachable, some were never observed, some observations disagree, and some are old. From that the
runtime must produce, deterministically and truthfully:

1. **Which components exist now** - a canonical decomposition into partitions.
2. **What authority each may retain** - an explicit authority vector derived from policy and quorum.
3. **What must be isolated or degraded** - enforced caps with recorded bases.
4. **When partitions may merge or be revalidated** - governed reconciliation events, not set unions.

Every one of those answers is a Decision that names its exact subject, binds every generation and
evidence digest that made it legal, states a verdict that distinguishes a grant from a denial, a
degradation, a conflict and an indeterminate result, explains itself with stable reason codes, and
records whether it is revocable and what would revoke it.

---

## 3. Evidence and its resolution

Reachability evidence is a bounded set of independently published bundles. A bundle carries a
publisher identity, a publisher incarnation, a publisher sequence, the topology generation it was
observed against, a reachability generation, an observation tick, a validity horizon, a completeness
claim and a list of directed observations.

Each observation is one of REACHABLE, UNREACHABLE or UNKNOWN. An explicit UNKNOWN is refused: an
observer cannot observe that it did not observe.

### The resolution rule

For each ordered pair (a, b) the runtime folds every fresh observation and every fresh completeness
claim:

| condition | resolution |
| --- | --- |
| a is reachable to b and b is proven unreachable | CONFLICTING |
| a is reachable to b and a fresh complete claim covering a is silent about b | CONFLICTING |
| a is reachable to b, nothing contradicts it | REACHABLE |
| b was observed unreachable, or every fresh complete claim covering a is silent about b | UNREACHABLE |
| the only observations of the pair are outside their validity horizon | STALE |
| nothing fresh was ever observed | UNKNOWN |

The two directions of a pair are then joined into the undirected resolution with a commutative,
associative and idempotent lattice, which is why the result is independent of observation order:

* both directions reachable -> REACHABLE, and only then is there a usable edge;
* both directions unreachable -> UNREACHABLE;
* one reachable and the other unreachable -> CONFLICTING;
* one direction proven and the other never observed -> ASYMMETRIC;
* both unobserved -> UNKNOWN.

### What makes a decomposition CONFIRMED

The component decomposition is built from proven bidirectional reachability only. It is a refinement
of the truth: unknown connectivity can still merge components, never split them. The decomposition is
CONFIRMED exactly when **every ordered pair that crosses a component boundary is proven
unreachable**. Otherwise it is PARTIAL and carries the exact number of unresolved crossing pairs.

Consequences that the property suite pins down:

* A single component set is trivially confirmed, because no unobserved pair could split it.
* A roster with no evidence is never confirmed, however many components it contains.
* UNKNOWN is never reported as UNREACHABLE, and PARTIAL is never reported as clean.

### Exact pair accounting

The snapshot reports exact counters: ordered pairs classified into reachable, unreachable,
conflicting, stale and unknown, plus the subsets that are asymmetric (one direction proven, the
reverse unobserved) and contradictory (one direction reachable, the reverse unreachable), the usable
edge count, and the crossing-pair accounting that decides confirmedness. The five ordered categories
always sum to N * (N - 1).

Nothing in the resolution iterates over the N^2 pairs of the fabric. The classification is linear in
the observation count, and a slow independent O(N^2) reference implementation in the test tree is
differential-tested against it on seeded instances.

---

## 4. Authority model

Reachability is not authority. Six things are kept apart, each with its own field and its own name:

| stage | meaning |
| --- | --- |
| observed | what the component set is physically able to do. Confers nothing. |
| eligible | what the policy would permit this subject to hold if the evidence supported it. |
| recommended | what the assessment proposes. Not a grant. |
| authorized | what this runtime actually confers. The only field that carries authority. |
| acknowledgement | a separate type: the subject reported applying a grant. |
| verified effect | a separate type: independent observation confirmed the effect. |

The authority class is one of PRIMARY, DEGRADED, READ_ONLY, OBSERVE_ONLY or ISOLATED, each with a
recorded AuthorityBasis. Fail-closed rules, applied in a fixed order:

1. The full quorum must be satisfied for PRIMARY; otherwise the degraded quorum decides between
   DEGRADED and ISOLATED.
2. A component set that spans two or more live lineages is capped at OBSERVE_ONLY until a governed
   merge is committed.
3. If no fresh evidence exists at all, every component set is capped at OBSERVE_ONLY. Nothing may
   read, write or mutate on the strength of an observation that does not exist.
4. If the decomposition is not confirmed, unknown_evidence_downgrades caps at DEGRADED,
   require_confirmed_partition_for_write caps at READ_ONLY, and
   require_confirmed_partition_for_read caps at OBSERVE_ONLY.
5. An explicit isolation directive applied to the lineage forces ISOLATED.
6. An authority that existed before the last restart is capped at OBSERVE_ONLY until it is
   revalidated.
7. If two or more component sets independently satisfy the full quorum, that is a split brain: the
   verdict is CONFLICT and every one of them is reduced to OBSERVE_ONLY.

withheld is what the policy would permit but did not confer. denied is everything the subject is
physically able to do that was not authorized, so an isolated subject denies every observed
capability.

An acknowledgement is a subject's report. A verified effect is this runtime's own observation, and it
is refused unless an accepted acknowledgement exists for the same attempt and partition, and unless
the confirmed capability set is inside the authorized set.

---

## 5. Identities and generations

All identities are distinct C++ types. None of them is an interchangeable integer or string, none can
be constructed from a malformed encoding, and none converts implicitly into another domain.

| identity | kind | bound to |
| --- | --- | --- |
| ComponentId | bounded canonical text | the authoritative roster |
| PartitionId | hash of lineage, generation and membership digest | one generation |
| LineageId | hash of parents and membership digest | the reconciliation history |
| MembershipDigest | SHA-256 over the canonically sorted member set | the member set alone |
| TopologyGeneration, ReachabilityGeneration, PolicyGeneration, PartitionGeneration | checked monotonic counters | - |
| CoordinatorEpoch, CoordinatorBootId, ProcessIncarnationId | counters and 128-bit boot identities | one coordinator incarnation |
| DecisionId, FenceId, EvidenceId, SessionId, AttemptId with AttemptSequence | textual and monotonic | one attempt |

Canonical ordering is lexicographic over the canonical text, so every ordering is independent of
container, insertion and discovery order. Every counter refuses to wrap and refuses to regress.

**Partition identity is canonical and generation bound.** Equivalent component membership yields the
same identity whatever order it was observed in, and the same membership under a later generation is
a different partition identity that shares a lineage.

---

## 6. Lineage, merge and revalidation

Lineage is the only part of a partition that legitimately survives a restart. A lineage record names
the event (GENESIS, SPLIT, MERGE, REVALIDATE, RETIRE), the membership, the parents, the coordinator
epoch and boot identity, and the decision that produced it, with an integrity digest over all of it.
Lineage membership is stored exactly, never sampled, so ancestry is never indeterminate merely
because a partition is large.

**Merge is a governed reconciliation event, not a set union.**

* A component set that spans two or more live lineages is detected and receives a provisional
  identity. It is not granted authority, and its membership is not recorded as lineage.
* request_merge names exactly two live parent lineages of that component set. Reconciliation in
  1.0.0 is pairwise: a component set spanning three lineages requires two sequential governed
  merges.
* The two parents must be either unrelated or divergent. Identical, ancestor or descendant parents
  are a lineage conflict and are refused: conflicting authority may not be unioned.
* The ancestry relation is computed by a bounded upward walk whose frontier is expanded in canonical
  lineage order. When the node or depth budget is exhausted the outcome is INDETERMINATE
  (SEARCH_LIMIT_REACHED), never unrelated.
* The merge requires fresh evidence and a confirmed decomposition.
* A committed merge creates a new lineage that supersedes both parents, fences every grant derived
  from them, and is recorded durably before the reconciled assessment is published.

**Revalidation** re-derives authority from fresh evidence under the current epoch. It restores
nothing on its own. With no fresh evidence it returns INDETERMINATE; with fresh evidence it clears
exactly the interruptions whose lineage is present in the new decomposition and whose authority is
re-derived, fences the superseded grant, and reports anything still interrupted.

---

## 7. Restart, durability and crash semantics

### What is durable

The adopted topology definition and component roster, the authority policy, committed lineage
records, completed decisions, fence records, and the epoch and sequence floors. Nothing else.

### What is not restored

Reachability evidence, evidence freshness, live authority, attempts in flight, acknowledgements and
verified effects. A restart creates a fresh process incarnation, advances the coordinator epoch, and
takes a new coordinator boot identity, and the epoch advance is committed durably **before** any
authority can be issued under it.

Every authority that existed before the restart becomes INTERRUPTED: it is reported, it stays
observable, it is durably fenced, and it grants nothing until revalidate re-derives it from fresh
evidence. A restart does not turn a durable record into current authority, and a matching identity is
never treated as a matching generation.

### Store layout

* **journal** - a 24-byte header (magic, format version, header checksum) followed by a stream of
  self-describing records. Each record carries a type, flags, a declared payload length, a monotonic
  sequence, the payload and a CRC-64/XZ integrity field over the header and payload.
* **snapshot** - a 24-byte header, a bounded payload holding the sequence watermark, the record count
  and a compacted record stream, and a trailing checksum. Snapshots are written to a temporary file,
  flushed, synced, and renamed over the live file: transactional replacement, never an in-place edit.

### Crash semantics

* A torn tail - a final record whose bytes were never fully committed - is discarded and reported,
  and the journal is truncated to the last good offset before the next append.
* A **complete** record that fails its integrity check is an integrity failure. The store refuses to
  load and the coordinator exits with a diagnostic. It is never silently truncated, because a
  complete record with a broken checksum can represent tampering.
* Corrupt headers, unsupported versions, impossible declared lengths, type values outside their
  domain, reserved fields that are not zero, padded or trailing bytes after a declared record stream,
  and regressed sequences are all refused.
* A declared length is validated against the configured bound before any allocation.
* A journal that exists but holds less than a complete header was never committed and is treated as
  an empty store; nothing was ever acknowledged from it.

### Proven restart semantics

The restart and revalidation behaviour is proven with **real process termination**, not serialization
round trips: a coordinator process is hard-killed with no orderly close, restarted against the same
store, and checked for an advanced epoch, a new boot identity, retained lineage and fences, zero
retained evidence, and interrupted authority that refuses to reappear without fresh evidence and an
explicit revalidation.

---

## 8. Framed protocol and the service

fabric-partition-coordinator owns one PartitionRuntime and exposes it over a bounded framed protocol
on a loopback address. A frame is 48 header bytes plus a bounded payload plus an 8-byte trailer:

| offset | field |
| --- | --- |
| 0 | magic FPM1 |
| 4 | protocol version (u16) |
| 6 | message type (u16) |
| 8 | flags (u16) |
| 10 | reserved (u16, must be zero) |
| 12 | declared payload length (u32) |
| 16 | monotonic per-session sequence (u64) |
| 24 | coordinator epoch the sender believes is current (u64) |
| 32 | opaque session handle (16 bytes) |
| 48 | payload |
| 48 + length | CRC-64/XZ over the header and payload |

Decoding is total and sticky-failure. Every truncated prefix is NEED_MORE; a corrupt magic, version,
reserved field, type, declared length or checksum has its own rejection status; an oversized declared
payload is refused before any allocation.

**Session binding.** A session establishes its authority model with a handshake: the server issues an
opaque handle and records the epoch and coordinator boot identity that were current when it was
issued. Every later frame must name that handle and that epoch, and its sequence must advance past
the last accepted sequence. A frame that names a different handle, a different epoch, or a replayed
sequence is refused with a PROTOCOL_ERROR frame and the session is closed. One session can never act
under another session's identity, boot or epoch.

**Shutdown.** The acceptor waits on readability with a short poll so it re-checks the stop flag, and
stopping shuts down every session socket before joining. A session thread blocked in a read is
released immediately, and no thread closes a descriptor another thread may still be using.

**Trust boundary.** This protocol is **not authenticated and not encrypted**. The integrity field
detects corruption, not a hostile peer; reaching the port is enough to send a well-formed frame.
Session binding prevents one session from acting under another's handle, epoch or boot identity, and
nothing more is claimed. See section 16.

---

## 9. Bounded resources and exact accounting

Every externally reachable allocation is derived from a value already compared against a configured
limit, and every size computation uses checked addition and multiplication. A hostile or corrupt
input produces a deterministic refusal, never a wraparound, over-allocation or process instability.

Bounded tables include: components, observations per evidence bundle, total retained evidence bytes,
covered components per bundle, partitions, reasons per decision, decision history, fence records,
lineage records, lineage member entries, authority grants, attempt records, isolation directives,
snapshot bytes, record bytes, journal records, frame bytes, sessions, pending connections and
in-flight requests per session.

Reason text is truncated at a configured bound and the truncation is marked, so a clipped explanation
is never mistaken for a complete one. The decision history is a bounded window and reports how many
entries it dropped.

UNKNOWN, STALE, CONFLICT, INVALID, UNSUPPORTED and INDETERMINATE are first-class outcomes. None of
them is reported as success, and none is reported as an ordinary absence.

---

## 10. Build

Requires CMake 3.21 or newer and a C++20 compiler. There are no third-party runtime dependencies.

    cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build/release

CMake presets are provided: release, debug, analyze (MSVC /analyze on first-party code) and asan
(AddressSanitizer).

Options: FPM_BUILD_TESTS, FPM_BUILD_EXAMPLES, FPM_BUILD_BENCHMARKS, FPM_BUILD_APPS,
FPM_BUILD_DISTRIBUTED_TESTS, FPM_ENABLE_ANALYZE, FPM_WARNINGS_AS_ERRORS.

First-party targets build with /W4 /permissive- /utf-8 /EHsc /WX on MSVC and
-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror elsewhere.

## 11. Test

    ctest --test-dir build/release --output-on-failure

No CTest TIMEOUT property is set anywhere and no test uses a timeout parameter. A hanging test is a
defect and must surface as a hang rather than be masked by a timer. The one place a wall-clock budget
appears is the benchmark, which fails rather than hangs when a size exceeds its absolute budget.

The suites are: identity, reachability, partition, property, authority, merge, persistence,
corruption, protocol, limits, lifecycle, concurrency, scale, and distributed (built when the
applications are). A single test binary runs all of them and reports executed=<n> failures=<n>; the
suites are also individually selectable with --filter= and --skip=.

## 12. Install and find_package

    cmake --install build/release --prefix install-prefix

The installed package exports SummonSoftwareLabs::FabricPartitionManager, installs the public headers
under include/fabric_partition_manager/ and provides FabricPartitionManagerConfig.cmake with a
fail-fast self-check on the imported target and the umbrella header. A complete independent project
lives in consumer/; it is not part of this build and links only the installed artifact:

    cmake -S consumer -B build/consumer -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build/consumer

Set CMAKE_PREFIX_PATH to the install prefix first.

## 13. Command-line tools

* **fabric-partition-coordinator** - owns a PartitionRuntime and serves it. Options include
  --store=DIR, --bind=ADDR, --port=N, --accept-port-file=FILE, --ready-file=FILE and
  --run-seconds=N. A corrupt store makes it exit 2 with a diagnostic rather than start degraded.
* **fabric-partition-publisher** - builds a SYNTHETIC fabric, adopts its topology once, and publishes
  a stream of distinct reachability observations. Each publish prints
  published <id> sequence=<n> verdict=<VERDICT> decision=<id>.
* **fabric-partition-cli** - status, partitions, assess, fences, lineage, publish, topology, isolate,
  clear-isolation, revalidate, retire and merge. Every subcommand prints key=value lines and exits 0
  on success, 1 on a refusal, 2 on a connection or protocol failure.

## 14. Examples

Each example is a standalone program that exits non-zero when its assertion fails.

| example | what it demonstrates |
| --- | --- |
| ex_detect_partition | two connected groups yield two canonical partitions |
| ex_unknown_is_not_disconnected | partial evidence leaves the decomposition PARTIAL and withholds write authority |
| ex_authority_is_not_reachability | an internally connected component set still gets no authority without a quorum |
| ex_split_brain_conflict | two independent quorums produce CONFLICT and both fall to observe-only |
| ex_governed_merge | reconnection is refused authority until the merge is governed |
| ex_restart_revalidation | authority interrupted by restart is restored only by revalidation with fresh evidence |
| ex_isolation_and_degradation | an isolation directive overrides a satisfied quorum and can be cleared |
| ex_persistence_recovery | lineage and fences survive a reopen; evidence and authority do not |
| ex_distributed_publishers | a real server and client over a real loopback socket |

## 15. Benchmarks

fpm_benchmarks measures **completed work**, not submission latency, on SYNTHETIC fabrics with a fixed
seed, and reports components, edges, wall milliseconds for a full assessment, the ratio to the
previous size, the exact ordered-pair counters, and the retained-state bounds. It exits non-zero when
a size exceeds its absolute wall-clock budget.

Representative Release run:

    kind      components       edges   assess-ms  time-ratio   ordered-pairs ordered-reachable ordered-unreachable ordered-unknown ordered-conflicting
    sparse         20000       19998      37.963       0.000       399980000             39996           399940004               0                   0
    sparse         60000       59998     139.939       3.686      3599940000            119996          3599820004               0                   0
    sparse        180000      179998     543.970       3.887     32399820000            359996          32399460004               0                   0
    dense            500        3924       5.349       0.000          249500              7848              241652               0                   0
    dense           1000        7927      12.141       2.270          999000             15854              983146               0                   0
    dense           2000       15931      24.077       1.983         3998000             31862             3966138               0                   0

A threefold increase in component count costs about **3.7x** in time for the sparse shape and about
**2.0x** for the dense shape. A quadratic component maintenance step would show roughly **9x** and
**4x**. Retained evidence bundles, lineage records, fence records and decision history stay inside
their configured bounds and do not grow with the fabric.

An earlier revision measured **8.5x** for the last sparse step. That was a genuine accidental
quadratic term: the lineage store resolved latest, earliest and duplicate lookups by scanning every
record, so one assessment over 45,000 component sets was quadratic in the number of lineages. The
store now keeps a per-lineage slot index and a generation set, and the scale suite asserts both the
wall-clock ratio and the structural bound on retained lineage records.

## 16. Validation classification

### REAL

* The C++20 library, the coordinator, publisher and CLI processes, and the client.
* Real operating-system processes for the multiprocess proof: separate coordinator and publisher
  processes over real loopback TCP sockets, hard-killed and restarted.
* Real durable files with real integrity checks and real transactional snapshot replacement.
* Release and Debug builds, MSVC /W4 /permissive- /WX, the MSVC static analyzer, and
  AddressSanitizer.
* The installed package consumed by an independent project outside the source tree.

### SYNTHETIC

* Every component roster and every reachability observation used by the tests, examples and
  benchmarks is generated by fabric_partition_manager/synthetic.hpp and is labelled SYNTHETIC. No
  physical switch, NIC, RDMA transport, DPU, switch ASIC or multi-node deployment was exercised.
* The scale evidence is generated in-process. It demonstrates algorithmic behaviour and bounded
  retention, not the behaviour of any real fabric.

### UNSUPPORTED

* **No physical hardware validation.** No switch, NIC, RDMA/RoCE/InfiniBand, NVLink, DPU, SmartNIC or
  multi-node cluster was available or exercised. No physical-fabric claim is made anywhere.
* **No cryptographic authentication or encryption.** Plain TCP with an unkeyed integrity field.
* **No distributed consensus.** A single coordinator authority. Proof against a network partition
  that separates two independent coordinators is out of scope and not claimed.
* **No HMAC or signed evidence.** Any peer that can reach the port can publish evidence that
  survives every structural and domain check.

---

## 17. Genuine limitations

1. **One coordinator authority.** The protocol and the runtime model a single authoritative
   coordinator. There is no consensus, no leader election and no multi-coordinator reconciliation.
2. **No authentication.** See section 16. Reaching the port is enough to issue protocol requests,
   subject to the authority rules.
3. **Merge reconciliation is pairwise.** A component set spanning three or more live lineages needs
   one governed merge per pair, in sequence. The N-way case is not a single atomic event.
4. **Rollback detection is local.** A restore that replaces the store image and the file it sits on
   is undetectable without external monotonic storage.
5. **Bounded search is bounded.** The ancestry walk can return INDETERMINATE on a lineage graph
   deeper than max_lineage_walk_nodes or max_lineage_walk_depth. That is a refusal, not a no-relation
   answer, and a caller must treat it as such.
6. **The decision history is a window.** It retains the most recent entries and reports how many it
   dropped. Durable lineage and fences are not a window; they are bounded but complete until their
   own bounds are reached, at which point the append is refused and reported rather than silently
   truncated.
7. **Isolation directives do not survive a restart.** The fence records that record the revocation
   do. An isolation must be re-issued after a restart, which is the conservative direction.
8. **A complete coverage claim is a strong assertion.** A publisher that declares complete coverage
   of a source asserts that everything it did not list is unreachable. Publishing contradictory
   complete claims produces CONFLICTING pairs and no edges, which is fail-closed but can look like a
   sudden loss of connectivity until the old claim ages out.
9. **Observation ticks are a logical clock.** The runtime never reads the wall clock. A caller that
   does not advance the clock keeps every observation fresh forever.
10. **One host.** All multiprocess proof runs on a single machine over loopback.

---

## 18. Concurrency, ownership and lifetime

PartitionRuntime is guarded by a single mutex. Every public operation takes it; the audit below
records why no path can re-enter it. Each item was inspected in the source and either confirmed
correct or fixed, and item 10 was reproduced by a test before it was fixed.

1. **Read-lock then write-lock re-entry on the same lock.** The runtime has one mutex and no
   reader/writer distinction, so no upgrade path exists. The guarded state is only touched through
   the public operations or the *_locked private helpers, which assume the lock is already held and
   never take it again.
2. **A write lock held across a helper that re-enters state.** Fixed. isolate, clear_isolation,
   request_merge and revalidate trigger an assessment. They call the unlocked assess_locked, never
   the public assess, so no operation re-acquires the mutex.
3. **Event, log or callback invocation beneath an internal lock.** There are no callbacks anywhere in
   the runtime, the server or the client. The decision log is a plain bounded vector appended under
   the same lock that guards the state it describes, and nothing is invoked from inside it.
4. **Joining workers while holding state they need.** Fixed. PartitionServer::stop shuts down every
   session socket under the session lock so blocked reads are released, releases the lock, joins the
   acceptor, and only then joins the session threads. A session thread that needs the session lock to
   finish can always get it.
5. **Cancellation or shutdown with reversed lock ordering.** The server takes exactly one lock (the
   session table) and never holds it while performing I/O, joining, or calling the runtime. The
   runtime takes exactly one lock and never calls the server. There is no second lock to order
   against.
6. **Blocked socket or thread teardown.** The acceptor waits on readability with a short poll and
   re-checks the stop flag, so it is never blocked indefinitely and is never closed out from under
   itself. Session reads are released by shutdown before the join. A descriptor is closed only after
   the thread that used it has been joined.
7. **Cross-object mutex order inversion.** Only two objects own a mutex and neither ever acquires the
   other's. The socket layer is lock-free.
8. **Moved-from handle ownership.** Socket is a move-only RAII handle whose moved-from state is
   invalid and whose destructor is a no-op on an invalid handle, so a move cannot double-close.
   PartitionServer and PartitionClient hold a unique_ptr to an incomplete implementation type and
   delete their copy operations, so no partially copied handle exists.
9. **Close and shutdown races, and double close.** PartitionRuntime::close and PartitionServer::stop
   are idempotent and guarded; both were exercised from several threads concurrently in the
   concurrency suite.
10. **Callbacks or references retaining mutable state beyond the lock lifetime.** Fixed. Several
    queries used to hand out a reference or a pointer into mutex-guarded state (last_assessment,
    current_partitions, policy, roster, find_decision). A reader could therefore observe a partially
    updated assessment after the writer released the lock, and the concurrency suite reproduced it.
    All queries now return values, and the suite asserts that concurrent readers only ever observe
    self-consistent assessments.
11. **Counter updates from inside a helper that already holds a lock.** Fixed. Server statistics are
    individual atomics, so no counter increment can re-enter the session lock from a helper that the
    lock holder called.

---

## 19. Determinism

* Canonical serialization is fixed-width big-endian with length-prefixed text, so the byte image of a
  value is a pure function of the value.
* Membership, partition and lineage identities are digests over canonically ordered inputs.
* The component decomposition, the edge list, the partition order and the resolution counters are
  independent of observation order, bundle order and roster order; the property suite asserts this
  over seeded permutations.
* DecisionId and the decision sequence are allocated at the moment a decision is published, so the
  audit log is always monotonic in sequence even when one operation triggers another.
* The bounded ancestry walk expands its frontier in canonical lineage order, so hitting the node
  budget always truncates at the same reproducible point.
* Every property and differential test case is reproducible from its seed.

## License
Apache License 2.0. Copyright 2026 Summon Software Labs.
