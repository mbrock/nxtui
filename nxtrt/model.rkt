#lang racket/base

(require racket/cmdline
         "../rdf-forge/model.rkt"
         "ontology.rkt")

(provide runtime-model)

(define runtime-model
  (model
   #:language 'forge/temporal

   (option 'verbose 0)
   (option 'min_tracelength 3)
   (option 'max_tracelength 3)

   (signature deck
     (waves one wand))
   (signature wand)
   (signature zone)
   (signature task
     (belongs-to lone zone)
     (belongs-to lone deck)
     (belongs-to lone waiter)
     (continues-as lone task))
   (signature wish)
   (signature waiter
     (wants one wish)
     (holds one wand)
     (belongs-to one task)
     (staged-on var lone wand)
     (parked-on var lone wand))
   (signature deed
     (belongs-to one task)
     (belongs-to one zone))

   (predicate 'structural-invariants
              (block
               (all ([a waiter])
                 (in (union (a staged-on)
                            (a parked-on))
                     (a holds)))
               (all ([a waiter])
                 (lone (union (a staged-on)
                              (a parked-on))))
               (all ([t task] [a waiter])
                 (=> (== (t (belongs-to waiter)) a)
                     (== (a (belongs-to (task waiter))) t)))
               (all ([d deed])
                 (== (d (belongs-to (task deed)) (belongs-to (zone task)))
                     (d (belongs-to (zone deed)))))
               (all ([z zone] [t (matching (belongs-to (zone task)) z)])
                 (lone ([d (matching (belongs-to (zone deed)) z)])
                   (== (d (belongs-to (task deed))) t)))
               (all ([t task] [d deck])
                 (=> (== (t (belongs-to deck)) d)
                     (no (t (belongs-to waiter)))))
               (all ([a waiter])
                 (=> (some (a parked-on))
                     (== (a (belongs-to (task waiter)) (belongs-to waiter)) a)))))

   (predicate 'ready-task-has-a-deck
              (all ([t task])
                (=> (some (t (belongs-to deck)))
                    (one (t (belongs-to deck))))))

   (predicate 'waiting-task-has-a-wand
              (all ([t task])
                (=> (some (t (belongs-to waiter)))
                    (one (t (belongs-to waiter) holds)))))

   (predicate 'child-task-has-at-most-one-deed-in-its-zone
              (all ([z zone] [t (matching (belongs-to (zone task)) z)])
                (lone ([d (matching (belongs-to (zone deed)) z)])
                  (== (d (belongs-to (task deed))) t))))

   (predicate 'parked-waiter-identifies-suspended-task
              (all ([a waiter])
                (=> (some (a parked-on))
                    (== (a (belongs-to (task waiter)) (belongs-to waiter)) a))))

   (predicate 'ready-task-on-deck
              (some ([t task])
                (some (t (belongs-to deck)))))

   (predicate 'task-awaiting-wish
              (some ([t task] [a waiter] [w wand])
                (block
                 (== (t (belongs-to waiter)) a)
                 (== (a holds) w)
                 (== (a parked-on) w))))

   (predicate 'zone-with-children
              (some ([z zone])
                (block
                 (ge (count (matching (belongs-to (zone task)) z)) 2)
                 (ge (count (matching (belongs-to (zone deed)) z)) 2))))

   (predicate 'staged-but-not-parked-yet
              (some ([a waiter] [w wand])
                (block
                 (== (a staged-on) w)
                 (no (a parked-on)))))

   (predicate 'parent-child-continuation
              (some ([parent task] [child task])
                (== (child continues-as) parent)))

   (predicate 'staged-to-parked-to-idle
              (some ([a waiter] [w wand])
                (block
                 (== (a holds) w)
                 (== (a staged-on) w)
                 (no (a parked-on))
                 (next-state
                  (block
                   (no (a staged-on))
                   (== (a parked-on) w)
                   (next-state
                    (block
                     (no (a staged-on))
                     (no (a parked-on)))))))))

   (predicate 'rich-runtime-shape
              (some ([d deck] [w wand] [z zone])
                (block
                 (== (d waves) w)
                 (ge (count (matching (belongs-to deck) d)) 1)
                 (ge (count (matching (belongs-to (zone task)) z)) 2)
                 (some ([a waiter])
                   (|| (== (a staged-on) w)
                       (== (a parked-on) w)))
                 (some ([t (matching (belongs-to (zone task)) z)])
                   (some (t (belongs-to waiter)))))))

   (check 'ready-task-has-a-deck-checked
          (=> 'structural-invariants 'ready-task-has-a-deck))
   (check 'waiting-task-has-a-wand-checked
          (=> 'structural-invariants 'waiting-task-has-a-wand))
   (check 'child-task-has-at-most-one-deed-in-its-zone-checked
          (=> 'structural-invariants 'child-task-has-at-most-one-deed-in-its-zone))
   (check 'parked-waiter-identifies-suspended-task-checked
          (=> 'structural-invariants 'parked-waiter-identifies-suspended-task))

   (run 'ready-task-witness
        (block 'structural-invariants
               'ready-task-on-deck)
        #:for 4)
   (run 'awaiting-wish-witness
        (block 'structural-invariants
               'task-awaiting-wish)
        #:for 4)
   (run 'zone-with-children-witness
        (block 'structural-invariants
               'zone-with-children)
        #:for 5)
   (run 'staged-but-not-parked-witness
        (block 'structural-invariants
               'staged-but-not-parked-yet)
        #:for 4)
   (run 'rich-runtime-shape-witness
        (block 'structural-invariants
               'rich-runtime-shape)
        #:for 6)
   (run 'rich-runtime-trace-witness
        (block (always 'structural-invariants)
               'rich-runtime-shape
               'staged-to-parked-to-idle)
        #:for 6)
   (run 'rich-runtime-shape-always
        (block 'structural-invariants
               (always 'rich-runtime-shape))
        #:for 6)))

(module+ main
  (define direct-xml-path #f)
  (define run-name 'rich-runtime-shape-witness)
  (define run-checks? #f)
  (command-line
   #:program "nxtrt-model"
   #:once-each
   [("--direct-xml") path "Run the model through Forge directly and write Alloy XML"
                    (set! direct-xml-path path)]
   [("--run") name "Run name to execute with --direct-xml"
              (set! run-name (string->symbol name))]
   [("--check") "Run the model checks through Forge directly"
                (set! run-checks? #t)])
  (cond
    [run-checks?
     (void (check-forge-model runtime-model))]
    [direct-xml-path
     (void (run-forge-model runtime-model run-name
                            #:run-sterling 'off
                            #:export-xml direct-xml-path))]
    [else
     (error 'nxtrt-model "choose --direct-xml or --check")]))
