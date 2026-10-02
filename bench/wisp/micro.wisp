;; SPDX-License-Identifier: AGPL-3.0-or-later
;; Extracted from mbrock/wisp core/benchmark.zig at
;; a282b936867fcf7d5d2926ebee9afbaf3e153f5b. Same programs and arguments.

(defun %bench-call-1 (count)
  (if (eq? count 0) 0 (%bench-call-1 (- count 1))))
(defun %bench-call-2 (count a)
  (if (eq? count 0) 0 (%bench-call-2 (- count 1) 0)))
(defun %bench-call-5 (count a b c d)
  (if (eq? count 0) 0 (%bench-call-5 (- count 1) 0 0 0 0)))
(defun %bench-call-16 (count a b c d e f g h i j k l m n o)
  (if (eq? count 0) 0
    (%bench-call-16 (- count 1) 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0)))
(defun %bench-jet-add-2 (count)
  (if (eq? count 0) 0
    (do (+ 1 2) (%bench-jet-add-2 (- count 1)))))
(defun %bench-leaf-2 (a b) b)
(defun %bench-closure-leaf-2 (count)
  (if (eq? count 0) 0
    (do (%bench-leaf-2 1 2) (%bench-closure-leaf-2 (- count 1)))))
(defun %bench-lookup-first-16 (target a b c d e f g h i j k l m n o)
  (if (eq? target 0) 0
    (%bench-lookup-first-16 (- target 1) 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0)))
(defun %bench-lookup-last-16 (a b c d e f g h i j k l m n o target)
  (if (eq? target 0) 0
    (%bench-lookup-last-16 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 (- target 1))))
(defun %bench-lookup-inner-8-leaf ()
  (let ((a 0))
    (let ((b 0))
      (let ((c 0))
        (let ((d 0))
          (let ((e 0))
            (let ((f 0))
              (let ((g 0))
                (let ((target 1)) target)))))))))
(defun %bench-lookup-inner-8 (count)
  (if (eq? count 0) 0
    (do (%bench-lookup-inner-8-leaf) (%bench-lookup-inner-8 (- count 1)))))
(defun %bench-lookup-outer-8-leaf ()
  (let ((target 1))
    (let ((a 0))
      (let ((b 0))
        (let ((c 0))
          (let ((d 0))
            (let ((e 0))
              (let ((f 0))
                (let ((g 0)) target)))))))))
(defun %bench-lookup-outer-8 (count)
  (if (eq? count 0) 0
    (do (%bench-lookup-outer-8-leaf) (%bench-lookup-outer-8 (- count 1)))))

(defun %bench-effect-depth (depth)
  (if (eq? depth 0) (send! 'pulse 5)
    (+ 1 (%bench-effect-depth (- depth 1)))))
(defun %bench-effect-series (count depth)
  (if (eq? count 1) (%bench-effect-depth depth)
    (do (%bench-effect-depth depth)
        (%bench-effect-series (- count 1) depth))))
(defun %bench-effects (count depth)
  (call-with-effect-handler 'pulse
    (fn () (%bench-effect-series count depth))
    (fn (request resume raise) (call resume request))))
