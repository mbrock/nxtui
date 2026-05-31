#lang racket/base

(require rackunit
         racket/string
         "../bfo-sketch.rkt"
         (except-in "../model.rkt" check)
         "../ontology.rkt")

(define turtle (ontology->turtle bfo-sketch))

(define (check-turtle pattern description)
  (check-true (regexp-match? pattern turtle) description))

(check-turtle #rx"owl:equivalentClass" "exports equivalent class axioms")
(check-turtle #rx"owl:unionOf" "exports equivalent union lists")
(check-turtle #rx"owl:intersectionOf" "exports equivalent intersection lists")
(check-turtle #rx"owl:Restriction" "exports restriction axioms")
(check-turtle #rx"owl:onProperty" "exports restriction properties")
(check-turtle #rx"owl:someValuesFrom" "exports existential restrictions")
(check-turtle #rx"owl:allValuesFrom" "exports universal restrictions")
(check-turtle #rx"owl:propertyChainAxiom" "exports property chain axioms")
(check-turtle #rx"owl:disjointWith" "exports pairwise disjointness")
(check-turtle #rx"owl:inverseOf" "exports inverse properties")
(check-turtle #rx"rdfs:subPropertyOf" "exports subproperties")
(check-turtle #rx"owl:TransitiveProperty" "exports transitive properties")
(check-turtle #rx"owl:SymmetricProperty" "exports symmetric properties")
(check-turtle #rx"rdfs:label" "exports RDFS labels")
(check-turtle #rx"skos:prefLabel" "exports SKOS preferred labels")
(check-turtle #rx"skos:altLabel" "exports SKOS alternative labels")
(check-turtle #rx"skos:definition" "exports definitions and elucidations")
(check-turtle #rx"skos:example" "exports examples")
(check-turtle #rx"bfo-sketch:bfo-owl-label" "exports ontology-local annotation predicates")
(check-turtle #rx"bfo-sketch:bfo-clif-label" "exports CLIF label annotations")
(check-turtle #rx"bfo-sketch:has-associated-axiom-nl" "exports natural language axiom annotations")
(check-turtle #rx"bfo-sketch:has-associated-axiom-fol" "exports FOL axiom annotations")
(check-turtle #rx"bfo-sketch:has-axiom-label" "exports BFO-style axiom labels")
(check-turtle #rx"bfo-sketch:axiom-008-002" "exports named logical axiom resources")
(check-turtle #rx"owl:NamedIndividual" "exports logical axiom label individuals")
(check-turtle #rx"owl:Axiom" "exports reified axiom annotations")
(check-turtle #rx"owl:annotatedSource bfo-sketch:continuant" "exports axiom annotated sources")
(check-turtle #rx"owl:annotatedProperty bfo-sketch:has-associated-axiom-fol" "exports axiom annotated properties")
(check-turtle #rx"owl:annotatedTarget" "exports axiom annotated targets")
(check-turtle #rx"\\(forall \\(x\\) \\(if \\(Continuant x\\) \\(Entity x\\)\\)\\)" "exports CLIF axiom text")
(check-turtle #rx"owl:AnnotationProperty" "exports annotation property declarations")
(check-turtle #rx"bfo-sketch:bfo-owl-label rdfs:subPropertyOf rdfs:label" "exports annotation subproperties")
(check-turtle #rx"bfo-sketch:continuant-part-of" "exports continuant parthood")
(check-turtle #rx"bfo-sketch:occurrent-part-of" "exports occurrent parthood")
(check-turtle #rx"bfo-sketch:proper-continuant-part-of" "exports proper continuant parthood")
(check-turtle #rx"bfo-sketch:proper-occurrent-part-of" "exports proper occurrent parthood")
(check-turtle #rx"bfo-sketch:temporal-part-of" "exports temporal parthood")
(check-turtle #rx"continuantPartOfAt" "exports exact BFO continuant parthood CLIF")
(check-turtle #rx"occurrentPartOf" "exports exact BFO occurrent parthood CLIF")
(check-turtle #rx"properTemporalPartOf" "exports exact BFO proper temporal parthood CLIF")
(check-turtle #rx"bfo-sketch:axiom-009-002" "exports BFO continuant parthood closure axiom")
(check-turtle #rx"bfo-sketch:axiom-101-001" "exports BFO temporal region parthood closure axiom")

(define bfo-model
  (model bfo-sketch
         (run 'bfo-generated-logic-smoke
              (some entity)
              #:for 3)))

(define generated-predicate-names
  (map symbol->string (map forge-predicate-name (forge-model-predicates bfo-model))))

(check-true
 (ormap (lambda (name)
          (string-prefix? name "%ontology-bfo-sketch-part-of-transitive"))
        generated-predicate-names)
 "generates Forge logic predicates for transitive BFO-style properties")

(check-true
 (ormap (lambda (name)
          (string-prefix? name "%ontology-bfo-sketch-independent-continuant-some-located-in"))
        generated-predicate-names)
 "generates Forge logic predicates for existential BFO-style restrictions")

(check-true
 (ormap (lambda (name)
          (string-prefix? name "%ontology-bfo-sketch-continuant-part-of-transitive"))
        generated-predicate-names)
 "generates Forge logic predicates for continuant parthood transitivity")

(check-true
 (ormap (lambda (name)
          (string-prefix? name "%ontology-bfo-sketch-occurrent-part-of-transitive"))
        generated-predicate-names)
 "generates Forge logic predicates for occurrent parthood transitivity")

(check-true
 (ormap (lambda (name)
          (string-prefix? name "%ontology-bfo-sketch-continuant-only-continuant-part-of"))
        generated-predicate-names)
 "generates Forge logic predicates for continuant parthood range closure")

(check-true
 (ormap (lambda (name)
          (string-prefix? name "%ontology-bfo-sketch-occurrent-only-occurrent-part-of"))
        generated-predicate-names)
 "generates Forge logic predicates for occurrent parthood range closure")

(check-not-exn
 (lambda ()
   (model->forge-runs bfo-model))
 "compiles generated ontology logic into Forge runs")
