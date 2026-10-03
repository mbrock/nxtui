#lang rdf-forge

ontology nxt "https://swa.sh/nxt#"
  class deck
  class task
  class pool
  class pool-slot
  class pool-close
  class fixed-group
  class group-stop
  class group-return
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

  property has-ready
  property has-lifecycle
  property has-parked-phase
  property has-settled-phase
  property has-continuation
  property realizes
  property admitted
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
  property children
  property started
  property completed-at-entry
  property finished
  property stop-requested
  property stopping
  property returned
  property stop-trigger

model runtime-model
  signature deck
    has-ready var set task
  // Fixed task-only groups directly own their children; bounded streams
  // admit a feed of ideas into a pool. Stop belongs to tasks: stopping an
  // owner drains its started children/jobs before returning. Once
  // wishes exist, both timeouts and outside stops drain their execs under
  // the same lifecycle below. HTTP connections use a pool; Wisp callbacks
  // await native tasks directly, and guest continuations stay in the Wisp
  // heap. Coroutine frames use the ordinary allocator and are not modeled.
  // This model describes ownership and close/drain. Group results stay in
  // child promises; values and positional collection are not modeled.
  // slots is fixed capacity; consuming/discarding are events on this step.
  // admitted records job provenance, not current occupancy; slot.job is the
  // live/result identity. Result values, cancellation delivery,
  // and eventual completion/close progress are intentionally abstracted out.
  // Upstream input reservation is folded into admission here; C++ reserves
  // capacity before reading an idea, and tests cover that additional wait.
  signature pool
    admitted set task
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
  signature fixed-group
    children set task
    // Already-completed handles keep their results without being scheduled.
    completed-at-entry set task
    started var set task
    finished var set task
    stop-requested var set task
    stopping var lone group-stop
    returned var lone group-return
    // Abstract completion events accepted by a completion-driven stop rule.
    // An empty set represents a rule that waits for all children.
    stop-trigger set task
  signature group-stop
  signature group-return

  predicate group-shape
    all ([g fixed-group])
      in (g started) (g children)
      in (g completed-at-entry) (g children)
      no (intersect (g started) (g completed-at-entry))
      in (g completed-at-entry) (g finished)
      in (g finished) (union (g started) (g completed-at-entry))
      in (g stop-requested) (g started)
      in (g stop-trigger) (g children)
    all ([g fixed-group] [t (g children)])
      one (matching children t)
      no (matching admitted t)

  predicate groups-start
    all ([g fixed-group])
      no (g started)
      == (g finished) (g completed-at-entry)
      no (g stop-requested)
      no (g stopping)
      no (g returned)

  predicate group-transitions
    all ([g fixed-group])
      in (g started) (g (prime started))
      in (g finished) (g (prime finished))
      in (g stop-requested) (g (prime stop-requested))
      // Outside stop is nondeterministic; completion-driven stop uses the
      // same drain path. Delivery and child execution are abstract events.
      (=> (some (intersect (g finished) (g stop-trigger)))
          (next-state (some (g stopping))))
      (=> (some (g stopping))
          (block
            (next-state (some (g stopping)))
            (== (g started) (g (prime started)))
            (all ([t (g started)])
              (=> (no (intersect t (g finished)))
                  (next-state (in t (g stop-requested)))))))
      // Return is permitted, not required: no fairness/progress assumption.
      (=> (next-state (some (g returned)))
          (block
            (in (g (prime started)) (g (prime finished)))
            (either
              (next-state (some (g stopping)))
              (== (g (prime finished)) (g children)))))
      (=> (some (g returned))
          (block
            (next-state (some (g returned)))
            (== (g started) (g (prime started)))))

  predicate group-return-drained
    all ([g fixed-group])
      (=> (some (g returned)) (in (g started) (g finished)))

  predicate group-stop-freezes-starts
    all ([g fixed-group])
      (=> (some (g stopping))
          (== (g started) (g (prime started))))

  predicate group-stop-requests-siblings
    all ([g fixed-group])
      (=> (some (g stopping))
          (all ([t (g started)])
            (=> (no (intersect t (g finished)))
                (next-state (in t (g stop-requested))))))

  check fixed-group-return-drains :for ([1 fixed-group group-stop group-return] [2 task] [0 pool]) :trace-length 6
    assume groups-start
    assume always group-shape
    assume always group-transitions
    show always group-return-drained

  check fixed-group-stop-prevents-start :for ([1 fixed-group group-stop group-return] [2 task] [0 pool]) :trace-length 6
    assume groups-start
    assume always group-shape
    assume always group-transitions
    show always group-stop-freezes-starts

  check fixed-group-stop-requests-siblings :for ([1 fixed-group group-stop group-return] [2 task] [0 pool]) :trace-length 6
    assume groups-start
    assume always group-shape
    assume always group-transitions
    show always group-stop-requests-siblings

  // a is a completed failing input accepted by the stop rule; b is a
  // later unfinished input and c a later already-completed input. Ordering
  // and failure values are abstracted by stop-trigger, not modeled here.
  run fixed-group-completed-input-stop-witness :for ([1 fixed-group group-stop group-return] [3 task] [0 pool]) :trace-length 4
    groups-start
    always group-shape
    always group-transitions
    some ([g fixed-group] [a (g children)] [b (g children)] [c (g children)])
      no (intersect a b)
      no (intersect a c)
      no (intersect b c)
      == (g completed-at-entry) (union a c)
      == (g stop-trigger) a
      next-state
        some (g stopping)
        no (g started)
        no (g returned)
        next-state
          some (g returned)
          no (g started)
          == (g finished) (union a c)
          no (g stop-requested)

  run fixed-group-completion-stop-drains-witness :for ([1 fixed-group group-stop group-return] [2 task] [0 pool]) :trace-length 6
    groups-start
    always group-shape
    always group-transitions
    some ([g fixed-group] [a (g children)] [b (g children)])
      no (intersect a b)
      == (g stop-trigger) a
      next-state
        == (g started) (g children)
        == (g finished) a
        next-state
          some (g stopping)
          no (g returned)
          == (g finished) a
          next-state
            in b (g stop-requested)
            == (g finished) a
            no (g returned)
            next-state
              == (g finished) (g children)
              some (g returned)

  run fixed-group-never-started-cancels-witness :for ([1 fixed-group group-stop group-return] [2 task] [0 pool]) :trace-length 4
    groups-start
    always group-shape
    always group-transitions
    some ([g fixed-group])
      some (g children)
      next-state
        some (g stopping)
        no (g started)
        next-state
          some (g returned)
          no (g started)

  run fixed-group-outside-stop-drains-witness :for ([1 fixed-group group-stop group-return] [2 task] [0 pool]) :trace-length 5
    groups-start
    always group-shape
    always group-transitions
    some ([g fixed-group] [a (g children)] [b (g children)])
      no (intersect a b)
      no (g stop-trigger)
      next-state
        == (g started) a
        some (g stopping)
        no (g finished)
        next-state
          in a (g stop-requested)
          no (g returned)
          no (g finished)
          next-state
            == (g finished) a
            some (g returned)
            no (intersect b (g started))

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
    all ([p pool] [t (p admitted)])
      one (matching admitted t)
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

  run pool-reuse-witness :for ([1 pool pool-slot pool-close] [2 task] [0 deck wish exec]) :trace-length 8
    always structural-invariants
    always pool-shape
    pools-start-free
    always pool-transitions
    pool-reuse

  // Two stream jobs occupy distinct ordinary-pool slots. At close
  // start one result is ready while the other job is still running; close
  // then discards both results as they become ready and returns both slots.
  // This witnesses bounded-stream close/drain. It makes no fairness or general
  // cancellation-liveness claim.
  predicate stream-close-drains
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

  run stream-close-drains-witness :for ([1 pool pool-close] [2 pool-slot task] [0 deck wish exec]) :trace-length 6
    always structural-invariants
    always pool-shape
    always pool-transitions
    stream-close-drains

  // Bounded safety checks: at most two slots/jobs, eight steps; ownership
  // and slot shape are premises, as are the transition rules under test.
  // The pool-only scopes contain one pool and no execs.
  // Exec retirement is independent and checked below, not assumed here.
  check pool-capacity-only-after-release :for ([1 pool pool-close] [2 pool-slot task] [0 deck wish exec]) :trace-length 8
    assume always structural-invariants
    assume always pool-shape
    assume pools-start-free
    assume always pool-transitions
    show always pool-capacity-returned-only-by-release

  check pool-ready-not-readmitted :for ([1 pool pool-close] [2 pool-slot task] [0 deck wish exec]) :trace-length 8
    assume always structural-invariants
    assume always pool-shape
    assume pools-start-free
    assume always pool-transitions
    show always pool-ready-cannot-be-readmitted

  check pool-release-retires :for ([1 pool pool-close] [2 pool-slot task] [0 deck wish exec]) :trace-length 8
    assume always structural-invariants
    assume always pool-shape
    assume pools-start-free
    assume always pool-transitions
    show always pool-release-retires-result

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
    some ([d deck] [p pool])
      some (d has-ready)
      ge (count (p admitted)) 2
      all ([a exec])
        in (a has-lifecycle) prepared-state
      some ([a exec] [t (p admitted)])
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

  check lifecycle-can-complete :for ([1 deck pool wish exec prepared-state parked-state settled-state retired-state queued-phase submitted-phase cancelling-phase ready-to-retire-phase draining-phase] [2 task]) :trace-length 6 :expect sat
    assume execs-start-prepared
    assume always structural-invariants
    assume always lifecycle-transitions
    show some-exec-retires

  check cancelled-exec-can-drain-and-retire :for ([1 deck pool wish exec prepared-state parked-state settled-state retired-state queued-phase submitted-phase cancelling-phase ready-to-retire-phase draining-phase] [2 task]) :trace-length 6 :expect sat
    assume execs-start-prepared
    assume always structural-invariants
    assume always lifecycle-transitions
    show some-exec-drains-then-retires

  check retire-only-when-ready :for ([1 deck pool wish prepared-state parked-state settled-state retired-state queued-phase submitted-phase cancelling-phase ready-to-retire-phase draining-phase] [2 exec task]) :trace-length 6
    assume execs-start-prepared
    assume always structural-invariants
    assume always lifecycle-transitions
    show always retires-only-when-ready

  check drain-only-after-cancel :for ([1 deck pool wish prepared-state parked-state settled-state retired-state queued-phase submitted-phase cancelling-phase ready-to-retire-phase draining-phase] [2 exec task]) :trace-length 6
    assume execs-start-prepared
    assume always structural-invariants
    assume always lifecycle-transitions
    show always drains-only-after-cancel

  check settled-exec-never-reparks :for ([1 deck pool wish prepared-state parked-state settled-state retired-state queued-phase submitted-phase cancelling-phase ready-to-retire-phase draining-phase] [2 exec task]) :trace-length 6
    assume execs-start-prepared
    assume always structural-invariants
    assume always lifecycle-transitions
    show always settled-never-reparks

  run rich-runtime-shape-witness :for ([1 deck pool wish exec prepared-state parked-state settled-state retired-state queued-phase submitted-phase cancelling-phase ready-to-retire-phase draining-phase] [2 task])
    structural-invariants
    rich-runtime-shape
  run rich-runtime-trace-witness :for ([1 deck pool wish exec prepared-state parked-state settled-state retired-state queued-phase submitted-phase cancelling-phase ready-to-retire-phase draining-phase] [2 task]) :trace-length 5
    always structural-invariants
    always lifecycle-transitions
    rich-runtime-shape
    phase-changes-once
