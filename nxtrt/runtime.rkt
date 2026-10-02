#lang rdf-forge

ontology nxt "https://swa.sh/nxt#"
  class deck
  class firm
  class task
  class pool
  class pool-slot
  class pool-close
  class blocking-work
  class blocking-worker
  class blocking-stop
  class wish
  class exec
  class exec-state :abstract
  class prepared-state :subclass-of exec-state
  class parked-state :subclass-of exec-state
  class settled-state :subclass-of exec-state
  class retired-state :subclass-of exec-state
  class parked-phase :abstract
  class queued-phase :subclass-of parked-phase
  class submitted-phase :subclass-of parked-phase
  class cancelling-phase :subclass-of parked-phase
  class settled-phase :abstract
  class ready-to-retire-phase :subclass-of settled-phase
  class draining-phase :subclass-of settled-phase
  class deed

  property has-ready
  property has-lifecycle
  property has-parked-phase
  property has-settled-phase
  property spawned
  property issued
  property observes
  property has-continuation
  property realizes
  property admitted
  property frame-source
  property slots
  property free-slots
  property running-slots
  property ready-slots
  property consuming
  property discarding
  property closing
  property job
  property blocking-lifecycle
  property worker-access
  property blocking-stopped

model runtime-model
  signature deck
    has-ready var set task
  // Ownership here is a snapshot, not a model of admission or exception
  // selection. A stopped C++ firm rejects new forks: timeout scope
  // bodies must check stop before spawning if cancelled before their first
  // turn. Once wishes exist, both timeout and external stop drain their
  // execs under the same lifecycle below, before the owning scope returns.
  // HTTP connection tasks use the bounded pool below; Wisp callbacks await
  // native tasks directly. Guest continuations stay in the Wisp heap, not
  // in this deck. Neither layer needs permanent evaluator/connection workers.
  // Firms provide frame memory and cancellation context; core no longer
  // offers ambient free fork/join. A firm may be an empty frame/cancel scope,
  // or a callback may explicitly use its firm reference to own children.
  // The concurrent tuple combinator admits its finite input of N indexed
  // void recipes into an N-slot ordinary pool: main work stays pool-owned,
  // with no firm child/deed records. Any additional fork needs an explicit
  // owner reference. This model describes pool ownership and close/drain;
  // tuple value types and positional writes are intentionally not modeled.
  signature firm
    // Optional explicit child ownership, not what makes the firm exist.
    spawned set task
    issued set deed
  // Admission ownership is separate from frame allocation: pool jobs use
  // the ambient firm's frame storage, but have no firm child/deed record.
  // Frame-allocation provenance (a preconstructed task's allocation scope
  // must outlive its target, while a factory runs in the target scope) is
  // enforced by C++ and is outside this exec/ownership model.
  // slots is fixed capacity; consuming/discarding are events on this step.
  // admitted records job provenance, not current occupancy; slot.job is the
  // live/result identity. Frame bytes, result values, cancellation delivery,
  // and eventual completion/close progress are intentionally abstracted out.
  // Upstream input reservation is folded into admission here; C++ reserves
  // capacity before reading an idea, and tests cover that additional wait.
  signature pool
    admitted set task
    frame-source one firm
    slots set pool-slot
    free-slots var set pool-slot
    running-slots var set pool-slot
    ready-slots var set pool-slot
    consuming var set pool-slot
    discarding var set pool-slot
    closing var lone pool-close
  signature pool-slot
    job var lone task
  signature pool-close
  // Blocking work has ordinary heap-owned input, not a migrating task.
  // prepared = waiting for credit; parked = admitted/queued or running;
  // settled = publication complete; retired = owner delivered/discarded.
  // A stop makes the result unwanted, but does not revoke worker access.
  // The fd poll exec is separate and resumes only on the original deck.
  signature blocking-work
    blocking-lifecycle var one exec-state
    worker-access var lone blocking-worker
    blocking-stopped var lone blocking-stop
  signature blocking-worker
  signature blocking-stop
  signature task
    has-continuation lone task
  signature wish
  signature exec
    realizes one wish
    has-continuation one task
    has-lifecycle var one exec-state
    has-parked-phase var lone parked-phase
    has-settled-phase var lone settled-phase
  signature exec-state
  signature prepared-state
  signature parked-state
  signature settled-state
  signature retired-state
  signature parked-phase
  signature queued-phase
  signature submitted-phase
  signature cancelling-phase
  signature settled-phase
  signature ready-to-retire-phase
  signature draining-phase
  signature deed
    // Observation survives task-frame evacuation. C++ keeps the deed and
    // settlement record linked until either is destroyed, retargeting the
    // record when the deed moves. This is not ownership of a live frame.
    // issued/observes are semantic relations, not separate C++ ledgers.
    observes one task

  predicate explicitly-owned-child
    all ([t task])
      (=> (some (matching spawned t))
          (== (count (matching spawned t)) 1))

  predicate blocking-starts
    all ([b blocking-work])
      in (b blocking-lifecycle) prepared-state
      no (b worker-access)
      no (b blocking-stopped)

  predicate blocking-transitions
    all ([b blocking-work])
      (=> (some (b blocking-stopped))
          (next-state (some (b blocking-stopped))))
      (=> (in (b blocking-lifecycle) prepared-state)
          (block
            (next-state (no (b worker-access)))
            (either
              (next-state (in (b blocking-lifecycle) prepared-state))
              (block (no (b blocking-stopped))
                     (next-state (in (b blocking-lifecycle) parked-state)))
              (block (some (b blocking-stopped))
                     (next-state (in (b blocking-lifecycle) settled-state))))))
      (=> (in (b blocking-lifecycle) parked-state)
          (either
            (block (next-state (in (b blocking-lifecycle) parked-state))
              // Only uncancelled queued work may acquire a worker. Already
              // running work retains access even after stop is requested.
              (=> (no (b worker-access))
                  (either
                    (next-state (no (b worker-access)))
                    (no (b blocking-stopped))))
              (=> (some (b worker-access))
                  (== (b worker-access) (b (prime worker-access)))))
            (block
              (either (some (b worker-access)) (some (b blocking-stopped)))
              (next-state (block
                (in (b blocking-lifecycle) settled-state)
                (no (b worker-access)))))))
      (=> (in (b blocking-lifecycle) settled-state)
          (next-state (block
            (no (b worker-access))
            (in (b blocking-lifecycle) (union settled-state retired-state)))))
      (=> (in (b blocking-lifecycle) retired-state)
          (next-state (block
            (no (b worker-access))
            (in (b blocking-lifecycle) retired-state))))

  predicate blocking-release-only-after-settlement
    all ([b blocking-work])
      (=> (next-state (in (b blocking-lifecycle) retired-state))
          (in (b blocking-lifecycle) (union settled-state retired-state)))

  predicate stopped-queue-never-starts
    all ([b blocking-work])
      (=> (block (some (b blocking-stopped)) (no (b worker-access)))
          (next-state (no (b worker-access))))

  check blocking-storage-retained-until-settlement :for ([1 blocking-work blocking-worker blocking-stop prepared-state parked-state settled-state retired-state]) :trace-length 6
    assume blocking-starts
    assume always blocking-transitions
    show always blocking-release-only-after-settlement

  check blocking-queued-stop-prevents-start :for ([1 blocking-work blocking-worker blocking-stop prepared-state parked-state settled-state retired-state]) :trace-length 6
    assume blocking-starts
    assume always blocking-transitions
    show always stopped-queue-never-starts

  run blocking-running-stop-settles-witness :for ([1 blocking-work blocking-worker blocking-stop prepared-state parked-state settled-state retired-state]) :trace-length 6
    blocking-starts
    always blocking-transitions
    some ([b blocking-work])
      eventually
        some (b worker-access)
        some (b blocking-stopped)
      eventually (in (b blocking-lifecycle) retired-state)

  predicate structural-invariants
    all ([t task])
      (either
        (some ([z firm]) (in t (z spawned)))
        (some ([p pool]) (in t (p admitted))))
    all ([p pool] [t (p admitted)])
      no (matching spawned t)
      one (matching admitted t)
    all ([z firm] [t (z spawned)])
      some ([d (z issued)])
        == (d observes) t
    all ([z firm] [t (z spawned)])
      lone ([d (z issued)])
        == (d observes) t
    all ([z firm] [d (z issued)])
      in (d observes) (z spawned)
    all ([t task])
      (=> (some (matching spawned t))
          (== (count (matching spawned t)) 1))
    all ([s pool-slot])
      one (matching slots s)
    all ([a exec] [s (intersect (a has-lifecycle) prepared-state)])
      no (a has-parked-phase)
      no (a has-settled-phase)
    all ([a exec] [s (intersect (a has-lifecycle) parked-state)])
      one (a has-parked-phase)
      no (a has-settled-phase)
    all ([a exec] [s (intersect (a has-lifecycle) settled-state)])
      no (a has-parked-phase)
      one (a has-settled-phase)
    all ([a exec] [s (intersect (a has-lifecycle) retired-state)])
      no (a has-parked-phase)
      no (a has-settled-phase)
    all ([a exec] [s (intersect (a has-lifecycle) parked-state)])
      no (matching has-ready (a (has-continuation (task exec))))

  predicate pool-shape
    all ([p pool])
      == (p slots) (union (p free-slots) (union (p running-slots) (p ready-slots)))
      no (intersect (p free-slots) (p running-slots))
      no (intersect (p free-slots) (p ready-slots))
      no (intersect (p running-slots) (p ready-slots))
      in (p consuming) (p ready-slots)
      in (p discarding) (p ready-slots)
      no (intersect (p consuming) (p discarding))
      (=> (some (p discarding)) (some (p closing)))
      all ([s (p free-slots)])
        no (s job)
      all ([s (union (p running-slots) (p ready-slots))])
        one (s job)
        in (s job) (p admitted)
    all ([t task])
      lone (matching job t)

  predicate pools-start-free
    all ([p pool])
      == (p free-slots) (p slots)
      no (p closing)

  predicate pool-transitions
    all ([p pool])
      // Close is explicit and terminal; it drains rather than admits.
      (=> (some (p closing)) (next-state (some (p closing))))
      all ([s (p free-slots)])
        (=> (some (p closing)) (next-state (in s (p free-slots))))
      // Free slots may admit either an asynchronous job (running) or an
      // immediately settled hope (ready). Completion never returns capacity.
      all ([s (p running-slots)])
        next-state
          in s (union (p running-slots) (p ready-slots))
        == (s job) (s (prime job))
      all ([s (p ready-slots)])
        (either
          (block (in s (union (p consuming) (p discarding)))
              (next-state (in s (p free-slots))))
          (block (no (intersect s (union (p consuming) (p discarding))))
              (next-state (in s (p ready-slots)))
              (== (s job) (s (prime job)))))

  predicate pool-capacity-returned-only-by-release
    all ([p pool] [s (union (p running-slots) (p ready-slots))])
      (=> (next-state (in s (p free-slots)))
          (in s (union (p consuming) (p discarding))))

  predicate pool-ready-cannot-be-readmitted
    all ([p pool] [s (p ready-slots)])
      no (intersect s (p (prime running-slots)))
      (=> (next-state (in s (p ready-slots)))
          (== (s job) (s (prime job))))

  predicate pool-release-retires-result
    all ([p pool] [s (union (p consuming) (p discarding))])
      next-state
        in s (p free-slots)
        no (s job)

  // One slot is used asynchronously, held ready, consumed, reused for an
  // immediate result, then discarded by explicit close. Both release paths
  // and the capacity-boundary states occur in this satisfiable witness.
  predicate pool-reuse
    some ([p pool] [s (p slots)] [a (p admitted)] [b (p admitted)])
      no (intersect a b)
      next-state
        in s (p running-slots)
        == (s job) a
        next-state
          in s (p ready-slots)
          no (p consuming)
          no (p discarding)
          next-state
            in s (p consuming)
            next-state
              in s (p free-slots)
              next-state
                in s (p ready-slots)
                == (s job) b
                some (p closing)
                in s (p discarding)
                next-state
                  in s (p free-slots)

  run pool-reuse-witness :for ([1 pool pool-slot pool-close firm] [2 task] [0 deck deed wish exec]) :trace-length 8
    always structural-invariants
    always pool-shape
    pools-start-free
    always pool-transitions
    pool-reuse

  // A scope is valid even when the callback retains no forked children.
  predicate empty-firm-scope
    some ([z firm])
      no (z spawned)
      no (z issued)

  run empty-firm-scope-witness :for ([1 firm] [0 deck task deed wish exec pool pool-slot pool-close]) :trace-length 2
    structural-invariants
    empty-firm-scope

  // Two indexed tuple jobs occupy distinct ordinary-pool slots. At close
  // start one result is ready while the other job is still running; close
  // then discards both results as they become ready and returns both slots.
  // This witnesses early policy completion using the same pool drain, not a
  // second tuple/nursery ownership mechanism. It makes no fairness or general
  // cancellation-liveness claim.
  predicate tuple-close-drains
    some ([p pool] [s1 (p slots)] [s2 (p slots)] [a (p admitted)] [b (p admitted)])
      no (intersect s1 s2)
      no (intersect a b)
      == (p free-slots) (p slots)
      no (p closing)
      next-state
        in s1 (p running-slots)
        in s2 (p running-slots)
        == (s1 job) a
        == (s2 job) b
        next-state
          in s1 (p ready-slots)
          in s2 (p running-slots)
          some (p closing)
          next-state
            in s1 (p ready-slots)
            in s2 (p running-slots)
            in s1 (p discarding)
            next-state
              in s1 (p free-slots)
              in s2 (p ready-slots)
              in s2 (p discarding)
              next-state
                in s1 (p free-slots)
                in s2 (p free-slots)

  run tuple-close-drains-witness :for ([1 pool pool-close firm] [2 pool-slot task] [0 deck deed wish exec]) :trace-length 6
    always structural-invariants
    always pool-shape
    always pool-transitions
    tuple-close-drains

  // Bounded safety checks: at most two slots/jobs, eight steps; ownership
  // and slot shape are premises, as are the transition rules under test.
  // The pool-only scopes contain one pool/ambient firm and no execs/deeds.
  // Exec retirement is independent and checked below, not assumed here.
  check pool-capacity-only-after-release :for ([1 pool pool-close firm] [2 pool-slot task] [0 deck deed wish exec]) :trace-length 8
    assume always structural-invariants
    assume always pool-shape
    assume pools-start-free
    assume always pool-transitions
    show always pool-capacity-returned-only-by-release

  check pool-ready-not-readmitted :for ([1 pool pool-close firm] [2 pool-slot task] [0 deck deed wish exec]) :trace-length 8
    assume always structural-invariants
    assume always pool-shape
    assume pools-start-free
    assume always pool-transitions
    show always pool-ready-cannot-be-readmitted

  check pool-release-retires :for ([1 pool pool-close firm] [2 pool-slot task] [0 deck deed wish exec]) :trace-length 8
    assume always structural-invariants
    assume always pool-shape
    assume pools-start-free
    assume always pool-transitions
    show always pool-release-retires-result

  // Removing explicitly-owned-child from structural-invariants makes this
  // check fail: the remaining ownership facts allow two firms to own a task.
  check explicit-child-has-one-firm-owner :for ([2 firm task deed] [0 deck wish exec pool pool-slot pool-close]) :trace-length 2
    assume always structural-invariants
    show always explicitly-owned-child

  predicate lifecycle-transitions
    all ([a exec] [s (intersect (a has-lifecycle) prepared-state)])
      next-state
        (either
          (in (a has-lifecycle) prepared-state)
          (in (a has-lifecycle) parked-state))
    all ([a exec] [s (intersect (a has-lifecycle) parked-state)])
      next-state
        (either
          (in (a has-lifecycle) parked-state)
          (in (a has-lifecycle) settled-state))
    all ([a exec] [s (intersect (a has-lifecycle) settled-state)])
      next-state
        (either
          (in (a has-lifecycle) settled-state)
          (in (a has-lifecycle) retired-state))
    all ([a exec] [s (intersect (a has-lifecycle) retired-state)])
      next-state
        in (a has-lifecycle) retired-state
    // Only a parked exec with a cancel in flight settles into draining
    // (uring: op CQE before cancel CQE); everything else settles ready.
    all ([a exec] [s (intersect (a has-lifecycle) parked-state)])
      (=> (next-state (some (intersect (a has-settled-phase) draining-phase)))
          (some (intersect (a has-parked-phase) cancelling-phase)))
    // A draining exec waits for its cancel CQE: it stays settled and may
    // only become ready to retire.
    all ([a exec] [p (intersect (a has-settled-phase) draining-phase)])
      next-state
        (either
          (some (intersect (a has-settled-phase) draining-phase))
          (some (intersect (a has-settled-phase) ready-to-retire-phase)))
    // Ready to retire never goes back to draining (is_retirable).
    all ([a exec] [p (intersect (a has-settled-phase) ready-to-retire-phase)])
      next-state
        (either
          (some (intersect (a has-settled-phase) ready-to-retire-phase))
          (in (a has-lifecycle) retired-state))

  predicate phase-changes-once
    some ([a exec])
      in (a has-lifecycle) prepared-state
      next-state
        in (a has-lifecycle) parked-state

  predicate rich-runtime-shape
    some ([d deck] [z firm])
      some (d has-ready)
      ge (count (z spawned)) 2
      all ([a exec])
        in (a has-lifecycle) prepared-state
      some ([a exec] [t (z spawned)])
        == (a (has-continuation (task exec))) t

  // Properties the C++ wands rely on. Execs are constructed `prepared{}`.
  predicate execs-start-prepared
    all ([a exec])
      in (a has-lifecycle) prepared-state

  // wand_exec::lifecycle::is_retirable: only settled + ready_to_retire.
  predicate retires-only-when-ready
    all ([a exec])
      (=> (next-state (in (a has-lifecycle) retired-state))
          (either
            (in (a has-lifecycle) retired-state)
            (some (intersect (a has-settled-phase) ready-to-retire-phase))))

  // uring handle_op_cqe: the op CQE settles into waiting_cancel_cqe only
  // while a cancel SQE is in flight. A wait-child exec uses a pidfd poll
  // as its op SQE; successful, uncancelled completion reaps with ordinary
  // nonblocking waitid before settling. Cancellation only drains the poll
  // and cancel CQEs, leaving the child waitable. No extra exec phase is
  // needed for reaping, so the same lifecycle rules apply.
  predicate drains-only-after-cancel
    all ([a exec])
      (=> (some (intersect (a has-settled-phase) draining-phase))
          (prev-state
            (either
              (some (intersect (a has-parked-phase) cancelling-phase))
              (some (intersect (a has-settled-phase) draining-phase)))))

  predicate settled-never-reparks
    all ([a exec])
      (=> (in (a has-lifecycle) settled-state)
          (always (no (intersect (a has-lifecycle) (union prepared-state parked-state)))))

  predicate some-exec-retires
    some ([a exec])
      eventually (in (a has-lifecycle) retired-state)

  predicate some-exec-drains-then-retires
    some ([a exec])
      eventually (some (intersect (a has-settled-phase) draining-phase))
      eventually (in (a has-lifecycle) retired-state)

  check lifecycle-can-complete :for ([1 deck firm wish exec prepared-state parked-state settled-state retired-state queued-phase submitted-phase cancelling-phase ready-to-retire-phase draining-phase] [2 task deed]) :trace-length 6 :expect sat
    assume execs-start-prepared
    assume always structural-invariants
    assume always lifecycle-transitions
    show some-exec-retires

  check cancelled-exec-can-drain-and-retire :for ([1 deck firm wish exec prepared-state parked-state settled-state retired-state queued-phase submitted-phase cancelling-phase ready-to-retire-phase draining-phase] [2 task deed]) :trace-length 6 :expect sat
    assume execs-start-prepared
    assume always structural-invariants
    assume always lifecycle-transitions
    show some-exec-drains-then-retires

  check retire-only-when-ready :for ([1 deck firm wish prepared-state parked-state settled-state retired-state queued-phase submitted-phase cancelling-phase ready-to-retire-phase draining-phase] [2 exec task deed]) :trace-length 6
    assume execs-start-prepared
    assume always structural-invariants
    assume always lifecycle-transitions
    show always retires-only-when-ready

  check drain-only-after-cancel :for ([1 deck firm wish prepared-state parked-state settled-state retired-state queued-phase submitted-phase cancelling-phase ready-to-retire-phase draining-phase] [2 exec task deed]) :trace-length 6
    assume execs-start-prepared
    assume always structural-invariants
    assume always lifecycle-transitions
    show always drains-only-after-cancel

  check settled-exec-never-reparks :for ([1 deck firm wish prepared-state parked-state settled-state retired-state queued-phase submitted-phase cancelling-phase ready-to-retire-phase draining-phase] [2 exec task deed]) :trace-length 6
    assume execs-start-prepared
    assume always structural-invariants
    assume always lifecycle-transitions
    show always settled-never-reparks

  run rich-runtime-shape-witness :for ([1 deck firm wish exec prepared-state parked-state settled-state retired-state queued-phase submitted-phase cancelling-phase ready-to-retire-phase draining-phase] [2 task deed])
    structural-invariants
    rich-runtime-shape
  run rich-runtime-trace-witness :for ([1 deck firm wish exec prepared-state parked-state settled-state retired-state queued-phase submitted-phase cancelling-phase ready-to-retire-phase draining-phase] [2 task deed]) :trace-length 5
    always structural-invariants
    always lifecycle-transitions
    rich-runtime-shape
    phase-changes-once
