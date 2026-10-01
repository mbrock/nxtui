;; SPDX-License-Identifier: AGPL-3.0-or-later
;; Optional executable-host policy. The portable base has no I/O authority.

(defun sleep-ms (milliseconds)
  (send! :host (vector :timer milliseconds)))

(set! %host-write
  (fn (&rest strings) (send! :host (vector :stdout strings))))
(set! %host-write-error
  (fn (&rest strings) (send! :host (vector :stderr strings))))
(set! %host-read-line
  (fn () (send! :host (vector :read-line nil))))
(set! %host-read-bytes
  (fn (count) (send! :host (vector :read-bytes count))))

;; One entry owns the whole job, including the request and both callbacks.
;; Entry: [:NXT-WISP-1 source byte-offset run pending last-result].
;; Pending: [id [operation arguments] deadline resume raise].
(defun %nxt-start (thunk state)
  (call-with-effect-handler :host thunk
    (fn (request resume raise)
      (vector-set! state 4
        (vector (genkey!) request nil resume raise))
      nil)))
