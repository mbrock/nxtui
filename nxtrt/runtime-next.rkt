#lang rdf-forge

ontology nxt-next "https://swa.sh/nxt-next#"
  class TASK
  class GAME
  class CARD
  class SYNC

  a GAME runs some TASK
  a GAME considers some CARD

  a GAME varyingly chooses a CARD or not
  a TASK varyingly awaits one SYNC

  a SYNC posts some CARD
  a SYNC waits some CARD
  a SYNC halts some CARD
  a SYNC links one SYNC

model runtime-next-model
  nxt-next

  predicate P4
    ∀ g ∈ GAME, c ∈ g/chooses:
      ∧ c ∈ g/considers
      ∧ ∃ b ∈ g/runs:
        ∧ c ∈ b/awaits/posts
        ∧ no (intersect (matching halts c) (g/runs/awaits))

  predicate P5
    ∀ game ∈ GAME, task ∈ game/runs, sync ∈ task/awaits, card ∈ game/chooses:
      next-state
        ∧ (card ∈ sync/posts ∨ card ∈ sync/waits) ⇒ task/awaits = sync/links
        ∧ (no (intersect card (sync/posts)) ∧ no (intersect card (sync/waits))) ⇒ task/awaits = sync

  predicate P6
    ∃ g ∈ GAME, b ∈ g/runs, c ∈ CARD, s1 ∈ SYNC, s2 ∈ SYNC:
      ∧ c ∈ g/considers
      ∧ g/runs = TASK
      ∧ b/awaits = s1
      ∧ s1/links = s2
      ∧ no (intersect s1 s2)
      ∧ c ∈ s1/posts
      ∧ no (intersect (matching halts c) (g/runs/awaits))
      ∧ g/chooses = c

  run R3:
    exactly 1 of GAME, CARD
    exactly 2 of TASK, SYNC
    for 4 steps
    always P4
    always P5
    show P6
