#lang rdf-forge

ontology bfo-sketch "https://example.test/bfo-sketch#"
  annotation-property bfo-owl-label
    label "BFO OWL specification label"
    subproperty-of label
  annotation-property bfo-clif-label
    label "BFO CLIF specification label"
    subproperty-of label
  annotation-property has-associated-axiom-nl
    label "has associated axiom(nl)"
  annotation-property has-associated-axiom-fol
    label "has associated axiom(fol)"
  annotation-property has-axiom-label
    label "has axiom label"

  class entity
    label "entity"
    pref-label "entity"
    annotation bfo-owl-label "entity"
    clif-label "Entity"
    elucidation "An entity is anything that exists, has existed, or will exist."
  class continuant :subclass-of entity
    label "continuant"
    pref-label "continuant"
    alt-label "endurant"
    clif-label "Continuant"
    nl-axiom "if b is a continuant, then b is an entity. (axiom label in BFO2 Reference: [008-002])"
    fol-axiom "(forall (x) (if (Continuant x) (Entity x))) // axiom label in BFO2 CLIF: [008-002]"
  class occurrent :subclass-of entity
    label "occurrent"
    clif-label "Occurrent"
    fol-axiom "(forall (x) (if (Occurrent x) (exists (r) (and (SpatioTemporalRegion r) (occupiesSpatioTemporalRegion x r))))) // axiom label in BFO2 CLIF: [108-001]"
    fol-axiom "(forall (x) (iff (Occurrent x) (and (Entity x) (exists (y) (temporalPartOf y x))))) // axiom label in BFO2 CLIF: [079-001]"
  class process :subclass-of occurrent
    label "process"
    clif-label "Process"
    fol-axiom "(iff (Process a) (and (Occurrent a) (exists (b) (properTemporalPartOf b a)) (exists (c t) (and (MaterialEntity c) (specificallyDependsOnAt a c t))))) // axiom label in BFO2 CLIF: [083-003]"
  class process-boundary :subclass-of occurrent
    label "process boundary"
    clif-label "ProcessBoundary"
    fol-axiom "(iff (ProcessBoundary a) (exists (p) (and (Process p) (temporalPartOf a p) (not (exists (b) (properTemporalPartOf b a)))))) // axiom label in BFO2 CLIF: [084-001]"
  class temporal-region :subclass-of occurrent
    label "temporal region"
    clif-label "TemporalRegion"
    fol-axiom "(forall (r) (if (TemporalRegion r) (occupiesTemporalRegion r r))) // axiom label in BFO2 CLIF: [119-002]"
    fol-axiom "(forall (x) (if (TemporalRegion x) (Occurrent x))) // axiom label in BFO2 CLIF: [100-001]"
  class spatiotemporal-region :subclass-of occurrent
    label "spatiotemporal region"
    clif-label "SpatioTemporalRegion"
    fol-axiom "(forall (r) (if (SpatioTemporalRegion r) (occupiesSpatioTemporalRegion r r))) // axiom label in BFO2 CLIF: [107-002]"
  class spatial-region :subclass-of continuant
    label "spatial region"
    clif-label "SpatialRegion"
    example "the interior of your mouth"
    fol-axiom "(forall (x y t) (if (and (SpatialRegion x) (continuantPartOfAt y x t)) (SpatialRegion y))) // axiom label in BFO2 CLIF: [036-001]"
  class dependent-continuant :subclass-of continuant
    label "dependent continuant"
  class specifically-dependent-continuant :subclass-of dependent-continuant
    label "specifically dependent continuant"
    clif-label "SpecificallyDependentContinuant"
  class realizable-entity :subclass-of specifically-dependent-continuant
    label "realizable entity"
    clif-label "RealizableEntity"
    elucidation "A specifically dependent continuant whose instances are realized in processes."
  class disposition :subclass-of realizable-entity
    label "disposition"
    clif-label "Disposition"
  class independent-continuant :subclass-of continuant
    label "independent continuant"
    clif-label "IndependentContinuant"
    fol-axiom "(forall (x t) (if (IndependentContinuant x) (exists (r) (and (SpatialRegion r) (locatedInAt x r t))))) // axiom label in BFO2 CLIF: [134-001]"
    fol-axiom "(iff (IndependentContinuant a) (and (Continuant a) (not (exists (b t) (specificallyDependsOnAt a b t))))) // axiom label in BFO2 CLIF: [017-002]"
  class material-entity :subclass-of independent-continuant
    label "material entity"
    clif-label "MaterialEntity"
    fol-axiom "(forall (x) (if (MaterialEntity x) (IndependentContinuant x))) // axiom label in BFO2 CLIF: [019-002]"
  class object :subclass-of material-entity
    label "object"
    clif-label "Object"
  class object-aggregate :subclass-of material-entity
    label "object aggregate"
    clif-label "ObjectAggregate"
  disjoint continuant occurrent
  equivalent-union entity continuant occurrent
  equivalent-union continuant dependent-continuant independent-continuant spatial-region
  equivalent-intersection material-entity independent-continuant continuant
  equivalent-union material-entity object object-aggregate

  property related-to
    label "related to"
    symmetric

  property part-of
    entity set entity
    label "part of"
    transitive
    fol-axiom "(transitive part_of)"

  property has-part
    entity set entity
    transitive
    inverse-of part-of
    fol-axiom "(inverse_of has_part part_of)"

  property continuant-part-of
    continuant set continuant
    label "continuant part of"
    transitive
    reflexive
    subproperty-of part-of
    fol-axiom "(forall (x y z t) (if (and (continuantPartOfAt x y t) (continuantPartOfAt y z t)) (continuantPartOfAt x z t)))"

  property has-continuant-part
    continuant set continuant
    label "has continuant part"
    transitive
    reflexive
    inverse-of continuant-part-of

  property proper-continuant-part-of
    continuant set continuant
    label "proper continuant part of"
    transitive
    subproperty-of continuant-part-of
    fol-axiom "(iff (properContinuantPartOfAt a b t) (and (continuantPartOfAt a b t) (not (= a b)))) // axiom label in BFO2 CLIF: [004-001]"

  property occurrent-part-of
    occurrent set occurrent
    label "occurrent part of"
    transitive
    reflexive
    subproperty-of part-of
    fol-axiom "(forall (x y z) (if (and (occurrentPartOf x y) (occurrentPartOf y z)) (occurrentPartOf x z)))"

  property has-occurrent-part
    occurrent set occurrent
    label "has occurrent part"
    transitive
    reflexive
    inverse-of occurrent-part-of

  property proper-occurrent-part-of
    occurrent set occurrent
    label "proper occurrent part of"
    transitive
    subproperty-of occurrent-part-of
    fol-axiom "(iff (properOccurrentPartOf a b) (and (occurrentPartOf a b) (not (= a b)))) // axiom label in BFO2 CLIF: [005-001]"

  property temporal-part-of
    occurrent set occurrent
    label "temporal part of"
    transitive
    subproperty-of occurrent-part-of
    fol-axiom "(forall (x y) (if (temporalPartOf x y) (occurrentPartOf x y)))"

  property proper-temporal-part-of
    occurrent set occurrent
    label "proper temporal part of"
    transitive
    subproperty-of temporal-part-of
    fol-axiom "(iff (properTemporalPartOf a b) (and (temporalPartOf a b) (not (= a b)))) // axiom label in BFO2 CLIF: [117-001]"

  property participates-in
    continuant set occurrent
    label "participates in"
    subproperty-of related-to
    fol-axiom "(forall (c p) (if (participatesIn c p) (and (Continuant c) (Occurrent p)))) // RO participates_in; BFO renders it time-indexed as has_participant(p, c, t)"
  property has-participant
    occurrent set continuant
    label "has participant"
    inverse-of participates-in

  property realized-in
    realizable-entity set occurrent
    label "realized in"
    subproperty-of related-to
    fol-axiom "(forall (r p) (if (realizedIn r p) (and (RealizableEntity r) (Occurrent p)))) // BFO is_realized_in: a realizable is realized in a process"
  property realizes
    occurrent set realizable-entity
    label "realizes"
    inverse-of realized-in

  property located-in
    continuant set spatial-region
    label "located in"
    transitive
    subproperty-of related-to

  property-chain located-in (part-of located-in)
  property-chain continuant-part-of (proper-continuant-part-of continuant-part-of)
  property-chain occurrent-part-of (proper-occurrent-part-of occurrent-part-of)

  subclass-some independent-continuant located-in spatial-region
  subclass-only material-entity has-part material-entity
  subclass-only continuant continuant-part-of continuant
  subclass-only occurrent occurrent-part-of occurrent

  logical-axiom axiom-008-002 continuant
    label "BFO2 Reference [008-002]"
    nl "if b is a continuant, then b is an entity."
    fol "(forall (x) (if (Continuant x) (Entity x)))"

  logical-axiom axiom-017-002 independent-continuant
    label "BFO2 Reference [017-002]"
    nl "b is an independent continuant iff b is a continuant that does not specifically depend on anything at any time."
    fol "(iff (IndependentContinuant a) (and (Continuant a) (not (exists (b t) (specificallyDependsOnAt a b t)))))"

  logical-axiom axiom-134-001 independent-continuant
    label "BFO2 Reference [134-001]"
    nl "For any independent continuant b and any time t there is some spatial region r such that b is located_in r at t."
    fol "(forall (x t) (if (IndependentContinuant x) (exists (r) (and (SpatialRegion r) (locatedInAt x r t)))))"

  logical-axiom axiom-002-001 continuant-part-of
    label "BFO2 Reference [002-001]"
    nl "continuantPartOfAt(a, b, t) means a is a part of b at time t, where a and b are continuants."
    fol "(forall (a b t) (if (continuantPartOfAt a b t) (and (Continuant a) (Continuant b))))"

  logical-axiom axiom-003-002 occurrent-part-of
    label "BFO2 Reference [003-002]"
    nl "occurrentPartOf(a, b) means a is a part of b, where a and b are occurrents."
    fol "(forall (a b) (if (occurrentPartOf a b) (and (Occurrent a) (Occurrent b))))"

  logical-axiom axiom-004-001 proper-continuant-part-of
    label "BFO2 Reference [004-001]"
    nl "a is a proper continuant part of b at t iff a is a continuant part of b at t and a is not identical to b."
    fol "(iff (properContinuantPartOfAt a b t) (and (continuantPartOfAt a b t) (not (= a b))))"

  logical-axiom axiom-005-001 proper-occurrent-part-of
    label "BFO2 Reference [005-001]"
    nl "a is a proper occurrent part of b iff a is an occurrent part of b and a is not identical to b."
    fol "(iff (properOccurrentPartOf a b) (and (occurrentPartOf a b) (not (= a b))))"

  logical-axiom axiom-009-002 continuant
    label "BFO2 Reference [009-002]"
    nl "if b is a continuant and, for some t, c is continuant_part_of b at t, then c is a continuant."
    fol "(forall (x y) (if (and (Continuant x) (exists (t) (continuantPartOfAt y x t))) (Continuant y)))"

  logical-axiom axiom-101-001 temporal-region
    label "BFO2 Reference [101-001]"
    nl "if x is a temporal region and y is an occurrent part of x, then y is a temporal region."
    fol "(forall (x y) (if (and (TemporalRegion x) (occurrentPartOf y x)) (TemporalRegion y)))"

  logical-axiom axiom-part-of-transitive part-of
    label "fol-mungall relation meta-axiom"
    nl "part_of is transitive."
    fol "(transitive part_of)"

  logical-axiom axiom-continuant-part-of-temporal-transitive continuant-part-of
    label "BFO continuant parthood temporal transitivity"
    nl "continuant parthood is transitive at a time."
    fol "(forall (x y z t) (if (and (continuantPartOfAt x y t) (continuantPartOfAt y z t)) (continuantPartOfAt x z t)))"

  logical-axiom axiom-occurrent-part-of-transitive occurrent-part-of
    label "BFO occurrent parthood transitivity"
    nl "occurrent parthood is transitive."
    fol "(forall (x y z) (if (and (occurrentPartOf x y) (occurrentPartOf y z)) (occurrentPartOf x z)))"
