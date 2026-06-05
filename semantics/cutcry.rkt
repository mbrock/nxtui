#lang racket
(require redex/reduction-semantics)

(provide BP
         harel->
         stopped?
         enabled
         participates?
         resume-waiting)

(define-language BP
  (y (yield (post v ...) (wait v ...) (stop v ...)))
  (e y halt (loop e))
  (thread (e ...))
  (waiting (at y thread))
  (P (run (running thread ...)
          (yielding waiting ...)))
  (v number))

(define-metafunction BP
  stopped? : v (y ...) -> boolean
  [(stopped? v
             (y_before ...
              (yield (post v_p ...)
                     (wait v_w ...)
                     (stop v_s_before ... v v_s_after ...))
              y_after ...))
   #t]
  [(stopped? v (y ...))
   #f])

(define-judgment-form BP
  #:mode (enabled I O)
  #:contract (enabled (y ...) v)
  [(where (y_before ...
           (yield (post v_p_before ... v v_p_after ...)
                  (wait v_w0 ...)
                  (stop v_s0 ...))
           y_middle ...
           (yield (post v_p1 ...)
                  (wait v_w_before ... v v_w_after ...)
                  (stop v_s1 ...))
           y_after ...)
          (y ...))
   (where #f (stopped? v (y ...)))
   ----------------
   (enabled (y ...) v)])

(define-metafunction BP
  participates? : v y -> boolean
  [(participates? v
                  (yield (post v_p_before ... v v_p_after ...)
                         (wait v_w ...)
                         (stop v_s ...)))
   #t]
  [(participates? v
                  (yield (post v_p ...)
                         (wait v_w_before ... v v_w_after ...)
                         (stop v_s ...)))
   #t]
  [(participates? v y)
   #f])

(define-metafunction BP
  add-running : thread P -> P
  [(add-running thread
                (run (running thread_r ...)
                     (yielding waiting_y ...)))
   (run (running thread thread_r ...)
        (yielding waiting_y ...))])

(define-metafunction BP
  add-yielding : waiting P -> P
  [(add-yielding waiting
                 (run (running thread_r ...)
                      (yielding waiting_y ...)))
   (run (running thread_r ...)
        (yielding waiting waiting_y ...))])

(define-metafunction BP
  resume-waiting : v (waiting ...) -> P
  [(resume-waiting v ())
   (run (running) (yielding))]
  [(resume-waiting v ((at y thread) waiting ...))
   (add-running thread (resume-waiting v (waiting ...)))
   (side-condition (term (participates? v y)))]
  [(resume-waiting v (waiting_0 waiting ...))
   (add-yielding waiting_0 (resume-waiting v (waiting ...)))])

(define harel->
  (reduction-relation
   BP
   #:domain P
   (-->
    (run (running thread_before ...
                  (y e_rest ...)
                  thread_after ...)
         (yielding waiting ...))
    (run (running thread_before ...
                  thread_after ...)
         (yielding waiting ...
                   (at y (e_rest ...))))
    yield)
   (-->
    (run (running)
         (yielding (at y thread) ...))
    (resume-waiting v ((at y thread) ...))
    (judgment-holds (enabled (y ...) v))
    resume)
   (-->
    (run (running thread_before ...
                  ((loop e) e_after ...)
                  thread_after ...)
         (yielding waiting ...))
    (run (running thread_before ...
                  (e (loop e) e_after ...)
                  thread_after ...)
         (yielding waiting ...))
    loop)
   (-->
    (run (running thread_before ...
                  (halt e_after ...)
                  thread_after ...)
         (yielding waiting ...))
    (run (running thread_before ...
                  (e_after ...)
                  thread_after ...)
         (yielding waiting ...))
    halt)))

(module+ main
  (define args (vector->list (current-command-line-arguments)))
  (define launch-stepper? (member "--stepper" args))
  (define empty-yield (term (yield (post) (wait) (stop))))
  (define post-1 (term (yield (post 1) (wait) (stop))))
  (define wait-1 (term (yield (post) (wait 1) (stop))))
  (define stop-1 (term (yield (post) (wait) (stop 1))))
  (define wait-2 (term (yield (post) (wait 2) (stop))))
  (define loop-post-1 (term (loop ,post-1)))
  (define stepper-seed
    (term (run (running (,loop-post-1) (,wait-1))
               (yielding))))

  (printf "Is (run (running (halt halt)) (yielding)) a P? ~s~n"
          (redex-match? BP P (term (run (running (halt halt))
                                        (yielding)))))
  (printf "stopped? 1: ~s~n"
          (term (stopped? 1 (,post-1 ,wait-1 ,stop-1))))
  (printf "enabled post/wait values: ~s~n"
          (judgment-holds (enabled (,post-1 ,wait-1) v) v))
  (printf "enabled post/wait/stop values: ~s~n"
          (judgment-holds (enabled (,post-1 ,wait-1 ,stop-1) v) v))
  (printf "participates? poster: ~s~n"
          (term (participates? 1 ,post-1)))
  (printf "participates? bystander: ~s~n"
          (term (participates? 1 ,wait-2)))

  (for ([p (in-list (list (term (run (running (halt))
                                      (yielding)))
                          (term (run (running (,empty-yield halt))
                                      (yielding)))
                          (term (run (running (,loop-post-1) (,wait-1))
                                      (yielding)))
                          (term (run (running (,post-1 halt) (,wait-1))
                                      (yielding)))
                          (term (run (running (,post-1 halt) (,wait-1) (,stop-1))
                                      (yielding)))
                          (term (run (running)
                                      (yielding (at ,post-1 (halt))
                                                (at ,wait-1 ())
                                                (at ,wait-2 (halt)))))
                          (term (run (running)
                                      (yielding (at ,post-1 (halt))
                                                (at ,wait-1 ())
                                                (at ,stop-1 ()))))
                          (term (run (running)
                                      (yielding)))))])
    (printf "~s  -->  ~s~n"
            p
            (apply-reduction-relation/tag-with-names harel-> p)))

  (when launch-stepper?
    (with-handlers ([exn:fail?
                     (lambda (e)
                       (eprintf "could not open Redex stepper: ~a~n"
                                (exn-message e)))])
      ((dynamic-require 'redex 'stepper) harel-> stepper-seed))))
