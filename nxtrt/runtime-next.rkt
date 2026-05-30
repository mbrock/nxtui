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
    ∀ game ∈ GAME, card ∈ game/chooses:
      ∧ card ∈ game/considers
      ∧ ∃ task ∈ game/runs:
        ∧ card ∈ task/awaits/posts
        ∧ no ((halts ∋ card) ∩ (game/runs/awaits))

  predicate P5
    ∀ game ∈ GAME, task ∈ game/runs, sync ∈ task/awaits, card ∈ game/chooses:
      next-state
        ∧ ∨ ∧ no (card ∩ sync/posts)
            ∧ no (card ∩ sync/waits)
          ∨ task/awaits = sync/links
        ∧ ∨ card ∈ sync/posts
          ∨ card ∈ sync/waits
          ∨ task/awaits = sync

  predicate P6
    ∃ game ∈ GAME, task ∈ game/runs, card ∈ CARD, before ∈ SYNC, after ∈ SYNC:
      ∧ card ∈ game/considers
      ∧ game/runs = TASK
      ∧ task/awaits = before
      ∧ before/links = after
      ∧ no (before ∩ after)
      ∧ card ∈ before/posts
      ∧ no ((halts ∋ card) ∩ (game/runs/awaits))
      ∧ game/chooses = card

  run R3:
    exactly 1 of GAME, CARD
    exactly 2 of TASK, SYNC
    for 4 steps
    always P4
    always P5
    show P6
