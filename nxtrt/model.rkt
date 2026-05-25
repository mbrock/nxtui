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
     (field waves #:one wand))
   (signature wand)
   (signature zone)
   (signature task
     (field (belongs-to zone #:domain task) #:lone zone)
     (field (belongs-to deck) #:lone deck)
     (field (belongs-to waiter) #:lone waiter)
     (field continues-as #:lone task))
   (signature wish)
   (signature waiter
     (field wants #:one wish)
     (field holds #:one wand)
     (field (belongs-to task #:domain waiter) #:one task)
     (field staged-on #:lone wand)
     (field parked-on #:lone wand))
   (signature deed
     (field (belongs-to task #:domain deed) #:one task)
     (field (belongs-to zone #:domain deed) #:one zone))

   (predicate 'structural-invariants
              (block
               (all ([a waiter])
                 (in (union (follow a staged-on)
                            (follow a parked-on))
                     (follow a holds)))
               (all ([a waiter])
                 (lone (union (follow a staged-on)
                              (follow a parked-on))))
               (all ([t task] [a waiter])
                 (=> (== (follow t (belongs-to waiter)) a)
                     (== (follow a (belongs-to task #:domain waiter)) t)))
               (all ([d deed])
                 (== (follow d (belongs-to task #:domain deed) (belongs-to zone #:domain task))
                     (follow d (belongs-to zone #:domain deed))))
               (all ([z zone] [t (matching (belongs-to zone #:domain task) z)])
                 (lone ([d (matching (belongs-to zone #:domain deed) z)])
                   (== (follow d (belongs-to task #:domain deed)) t)))
               (all ([t task] [d deck])
                 (=> (== (follow t (belongs-to deck)) d)
                     (no (follow t (belongs-to waiter)))))
               (all ([a waiter])
                 (=> (some (follow a parked-on))
                     (== (follow a (belongs-to task #:domain waiter) (belongs-to waiter)) a)))))

   (predicate 'ready-task-has-a-deck
              (all ([t task])
                (=> (some (follow t (belongs-to deck)))
                    (one (follow t (belongs-to deck))))))

   (predicate 'waiting-task-has-a-wand
              (all ([t task])
                (=> (some (follow t (belongs-to waiter)))
                    (one (follow t (belongs-to waiter) holds)))))

   (predicate 'child-task-has-at-most-one-deed-in-its-zone
              (all ([z zone] [t (matching (belongs-to zone #:domain task) z)])
                (lone ([d (matching (belongs-to zone #:domain deed) z)])
                  (== (follow d (belongs-to task #:domain deed)) t))))

   (predicate 'parked-waiter-identifies-suspended-task
              (all ([a waiter])
                (=> (some (follow a parked-on))
                    (== (follow a (belongs-to task #:domain waiter) (belongs-to waiter)) a))))

   (predicate 'ready-task-on-deck
              (some ([t task])
                (some (follow t (belongs-to deck)))))

   (predicate 'task-awaiting-wish
              (some ([t task] [a waiter] [w wand])
                (block
                 (== (follow t (belongs-to waiter)) a)
                 (== (follow a holds) w)
                 (== (follow a parked-on) w))))

   (predicate 'zone-with-children
              (some ([z zone])
                (block
                 (ge (count (matching (belongs-to zone #:domain task) z)) 2)
                 (ge (count (matching (belongs-to zone #:domain deed) z)) 2))))

   (predicate 'staged-but-not-parked-yet
              (some ([a waiter] [w wand])
                (block
                 (== (follow a staged-on) w)
                 (no (follow a parked-on)))))

   (predicate 'parent-child-continuation
              (some ([parent task] [child task])
                (== (follow child continues-as) parent)))

   (predicate 'rich-runtime-shape
              (some ([d deck] [w wand] [z zone])
                (block
                 (== (follow d waves) w)
                 (ge (count (matching (belongs-to deck) d)) 1)
                 (ge (count (matching (belongs-to zone #:domain task) z)) 2)
                 (some ([a waiter])
                   (|| (== (follow a staged-on) w)
                       (== (follow a parked-on) w)))
                 (some ([t (matching (belongs-to zone #:domain task) z)])
                   (some (follow t (belongs-to waiter)))))))

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
