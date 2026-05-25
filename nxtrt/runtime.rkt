#lang rdf-forge

ontology nxt "https://swa.sh/nxt#"
  class deck
  class wand
  class zone
  class task
  class wish
  class exec
  class deed

  property waves
  property has-ready
  property has-prepared
  property has-submitted
  property has-parked
  property spawned
  property issued
  property observes
  property has-continuation
  property realizes

model runtime-model
  signature deck
    has-ready var set task
  signature wand
    has-prepared var set exec
    has-submitted var set exec
    has-parked var set exec
  signature zone
    spawned set task
    issued set deed
  signature task
    has-continuation lone task
  signature wish
  signature exec
    realizes one wish
    has-continuation one task
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
    all ([w wand])
      no (intersect (w has-prepared) (w has-submitted))
    all ([w wand])
      no (intersect (w has-prepared) (w has-parked))
    all ([w wand])
      no (intersect (w has-submitted) (w has-parked))
    all ([w wand] [a (w has-parked)])
      no (matching has-ready (a (has-continuation (task exec))))

  predicate phase-changes-once
    next-state
      some ([w wand])
        either (some (w has-submitted)) (some (w has-parked))

  predicate rich-runtime-shape
    some ([d deck] [w wand] [z zone])
      some (d has-ready)
      ge (count (z spawned)) 2
      some (w has-prepared)
      no (w has-submitted)
      no (w has-parked)
      some ([a exec] [t (z spawned)])
        == (a (has-continuation (task exec))) t

  run rich-runtime-shape-witness :for ([1 deck wand zone wish exec] [2 task deed])
    structural-invariants
    rich-runtime-shape
  run rich-runtime-trace-witness :for ([1 deck wand zone wish exec] [2 task deed]) :trace-length 5
    always structural-invariants
    rich-runtime-shape
    phase-changes-once
