#lang rdf-forge

ontology nxt "https://swa.sh/nxt#"
  class resource :abstract
  class deck :subclass-of resource
  class wand :subclass-of resource
  class zone :subclass-of resource
  class task :subclass-of resource
  class wish :subclass-of resource
  class waiter :subclass-of resource
  class deed :subclass-of resource

  property waves
    deck wand
  property belongs-to
    task zone
    waiter task
    deed task
    deed zone
  property is-ready-on
    task deck
  property continues-as
    task task
  property wants
    waiter wish
  property holds
    waiter wand
  property staged-on
    waiter wand
  property parked-on
    waiter wand

model runtime-model
  option verbose 0
  option min_tracelength 3
  option max_tracelength 3

  signature deck
    waves one wand
  signature wand
  signature zone
  signature task
    belongs-to lone zone
    is-ready-on lone deck
    continues-as lone task
  signature wish
  signature waiter
    wants one wish
    holds one wand
    belongs-to one task
    staged-on var lone wand
    parked-on var lone wand
  signature deed
    belongs-to one task
    belongs-to one zone

  predicate structural-invariants
    all ([a waiter])
      in (union (a staged-on) (a parked-on)) (a holds)
    all ([a waiter])
      lone (union (a staged-on) (a parked-on))
    all ([d deed])
      == (d (belongs-to (task deed)) (belongs-to (zone task))) (d (belongs-to (zone deed)))
    all ([z zone] [t (matching (belongs-to (zone task)) z)])
      lone ([d (matching (belongs-to (zone deed)) z)])
        == (d (belongs-to (task deed))) t
    all ([a waiter])
      no (a (belongs-to (task waiter)) is-ready-on)
    all ([a waiter])
      => (some (a parked-on)) (no (a (belongs-to (task waiter)) is-ready-on))

  predicate ready-task-has-a-deck
    all ([t task])
      => (some (t is-ready-on)) (one (t is-ready-on))

  predicate waiting-task-has-a-wand
    all ([a waiter])
      one (a holds)

  predicate child-task-has-at-most-one-deed-in-its-zone
    all ([z zone] [t (matching (belongs-to (zone task)) z)])
      lone ([d (matching (belongs-to (zone deed)) z)])
        == (d (belongs-to (task deed))) t

  predicate parked-waiter-identifies-suspended-task
    all ([a waiter])
      => (some (a parked-on)) (no (a (belongs-to (task waiter)) is-ready-on))

  predicate ready-task-on-deck
    some ([t task])
      some (t is-ready-on)

  predicate task-awaiting-wish
    some ([t task] [a waiter] [w wand])
      == (a (belongs-to (task waiter))) t
      == (a holds) w
      == (a parked-on) w

  predicate zone-with-children
    some ([z zone])
      ge (count (matching (belongs-to (zone task)) z)) 2
      ge (count (matching (belongs-to (zone deed)) z)) 2

  predicate staged-but-not-parked-yet
    some ([a waiter] [w wand])
      == (a staged-on) w
      no (a parked-on)

  predicate parent-child-continuation
    some ([parent task] [child task])
      == (child continues-as) parent

  predicate staged-to-parked-to-idle
    some ([a waiter] [w wand])
      == (a holds) w
      == (a staged-on) w
      no (a parked-on)
      next-state
        no (a staged-on)
        == (a parked-on) w
        next-state
          no (a staged-on)
          no (a parked-on)

  predicate rich-runtime-shape
    some ([d deck] [w wand] [z zone])
      == (d waves) w
      ge (count (matching is-ready-on d)) 1
      ge (count (matching (belongs-to (zone task)) z)) 2
      some ([a waiter])
        either (== (a staged-on) w) (== (a parked-on) w)
      some ([a waiter] [t (matching (belongs-to (zone task)) z)])
        == (a (belongs-to (task waiter))) t

  check ready-task-has-a-deck-checked
    structural-invariants
    ready-task-has-a-deck
  check waiting-task-has-a-wand-checked
    structural-invariants
    waiting-task-has-a-wand
  check child-task-has-at-most-one-deed-in-its-zone-checked
    structural-invariants
    child-task-has-at-most-one-deed-in-its-zone
  check parked-waiter-identifies-suspended-task-checked
    structural-invariants
    parked-waiter-identifies-suspended-task

  run ready-task-witness :for 4
    structural-invariants
    ready-task-on-deck
  run awaiting-wish-witness :for 4
    structural-invariants
    task-awaiting-wish
  run zone-with-children-witness :for 5
    structural-invariants
    zone-with-children
  run staged-but-not-parked-witness :for 4
    structural-invariants
    staged-but-not-parked-yet
  run rich-runtime-shape-witness :for 6
    structural-invariants
    rich-runtime-shape
  run rich-runtime-trace-witness :for 6
    always structural-invariants
    rich-runtime-shape
    staged-to-parked-to-idle
  run rich-runtime-shape-always :for 6
    structural-invariants
    always rich-runtime-shape
