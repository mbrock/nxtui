#lang rdf-forge

ontology nxt "https://swa.sh/nxt#"
  class deck
  class firm
  class task
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

model runtime-model
  signature deck
    has-ready var set task
  signature firm
    spawned set task
    issued set deed
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
    observes one task

  predicate structural-invariants
    all ([t task])
      some ([z firm])
        in t (z spawned)
    all ([z firm] [t (z spawned)])
      some ([d (z issued)])
        == (d observes) t
    all ([z firm] [t (z spawned)])
      lone ([d (z issued)])
        == (d observes) t
    all ([z firm] [d (z issued)])
      in (d observes) (z spawned)
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
  // while a cancel SQE is in flight.
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
