;; SPDX-License-Identifier: AGPL-3.0-or-later
;; Optional executable-host policy. The portable base has no I/O authority.

(defun sleep-ms (milliseconds)
  (send! :host (vector :timer milliseconds)))

(defun spawn (thunk)
  (send! :host (vector :spawn thunk)))

(defun join (job)
  (send! :host (vector :join job)))

(set! %host-write
  (fn (&rest strings) (send! :host (vector :stdout strings))))
(set! %host-write-error
  (fn (&rest strings) (send! :host (vector :stderr strings))))
(set! %host-read-line
  (fn () (send! :host (vector :read-line nil))))
(set! %host-read-bytes
  (fn (count) (send! :host (vector :read-bytes count))))

;; Entries and child jobs share the run/pending/result field positions.
;; Entry: [:NXT-WISP-2 source byte-offset run pending last-result jobs serial].
;; Job: [:NXT-JOB nil slot run pending result status]. Treat handles as opaque.
;; Pending: [id [operation arguments] deadline resume raise].
;; The host assigns a decimal request ID. Interning a GENKEY per effect
;; would retain every request identity forever in the KEY package.
(defun %nxt-start (thunk state)
  (call-with-effect-handler :host thunk
    (fn (request resume raise)
      (vector-set! state 4
        (vector nil request nil resume raise))
      nil)))

;; Adapted from mbrock/wisp web/http.wisp at
;; 223535633179cdf2a49391820bdab16a5db5bf4e (AGPL-3.0-or-later).
;; The Deno/JS bridge is replaced by heap records and NXT socket operations.
(defparameter *request* nil)
(defparameter *response* nil)

(defun response (status headers &optional body)
  (vector status headers body))

(defun set-response-status! (status)
  (vector-set! *response* 0 status))

(defun set-response-body! (body)
  (vector-set! *response* 2 body))

;; Headers are a list of [name value] vectors; duplicates preserve order.
(defun add-header! (name value)
  (vector-set! *response* 1
    (append (vector-get *response* 1) (list (vector name value)))))

(defun request-method () (vector-get *request* 0))
(defun request-path () (vector-get *request* 1))
(defun request-query-string () (vector-get *request* 2))
(defun request-text () (vector-get *request* 4))
(defun request-header (name)
  (send! :host (vector :request-header (vector *request* name))))

(defun %nxt-http-handle (handler request)
  (binding ((*request* request) (*response* (response 200 nil nil)))
    (call-with-prompt :respond
      (fn () (call handler) *response*)
      (fn (value continuation) value))))

;; Returns a job handle. The session stays alive while the listener runs.
;; Plain HTTP on loopback only; TLS belongs to the reverse proxy.
(defun serve-http (port handler)
  (spawn (fn () (send! :host (vector :http-serve (vector port handler))))))

;; Waits in the calling job; spawn/join can make requests concurrent.
;; Returns [status headers body], like RESPONSE. Headers are a list of
;; [name value] vectors, and the decoded body is a binary-safe string.
(defun fetch-http (url &optional method headers body)
  (send! :host (vector :http-fetch (vector url (or method "GET") headers body))))
