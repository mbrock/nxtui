#lang rdf-forge

ontology nxt "https://swa.sh/nxt#"
  class deck
  class zone
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
  signature zone
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
    all ([z zone] [t (z spawned)])
      some ([d (z issued)])
        == (d observes) t
    all ([z zone] [t (z spawned)])
      lone ([d (z issued)])
        == (d observes) t
    all ([z zone] [d (z issued)])
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

  predicate phase-changes-once
    some ([a exec])
      in (a has-lifecycle) prepared-state
      next-state
        in (a has-lifecycle) parked-state

  predicate rich-runtime-shape
    some ([d deck] [z zone])
      some (d has-ready)
      ge (count (z spawned)) 2
      all ([a exec])
        in (a has-lifecycle) prepared-state
      some ([a exec] [t (z spawned)])
        == (a (has-continuation (task exec))) t

  run rich-runtime-shape-witness :for ([1 deck zone wish exec prepared-state parked-state settled-state retired-state queued-phase submitted-phase cancelling-phase ready-to-retire-phase draining-phase] [2 task deed])
    structural-invariants
    rich-runtime-shape
  run rich-runtime-trace-witness :for ([1 deck zone wish exec prepared-state parked-state settled-state retired-state queued-phase submitted-phase cancelling-phase ready-to-retire-phase draining-phase] [2 task deed]) :trace-length 5
    always structural-invariants
    always lifecycle-transitions
    rich-runtime-shape
    phase-changes-once
