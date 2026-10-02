;; SPDX-License-Identifier: AGPL-3.0-or-later
;; Optional executable-host policy. The portable base has no I/O authority.

;; A description selects a native task, which may await one wish or compose
;; many operations. It starts when awaited; no guest worker or future is born.
(defun await (task-description)
  (send! :host task-description))

(defun sleep-ms (milliseconds)
  (await (vector :timer milliseconds)))

(set! %host-write
  (fn (&rest strings) (await (vector :stdout strings))))
(set! %host-write-error
  (fn (&rest strings) (await (vector :stderr strings))))
(set! %host-read-line
  (fn () (await (vector :read-line nil))))
(set! %host-read-bytes
  (fn (count) (await (vector :read-bytes count))))

;; Entry: [:NXT-WISP-3 source byte-offset run pending last-result serial].
;; Callback activations share its run/pending/result field positions.
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
  (await (vector :request-header (vector *request* name))))

(defun %nxt-http-handle (handler request)
  (binding ((*request* request) (*response* (response 200 nil nil)))
    (call-with-prompt :respond
      (fn () (call handler) *response*)
      (fn (value continuation) value))))

;; Files beneath directories the command line granted with --dir NAME=PATH,
;; like WASI preopens: there is no ambient filesystem. Guest paths are
;; NAME/relative/path; empty, "." and ".." segments are rejected and symlinks
;; are never followed. Failures raise :NOT-CAPABLE, :NOT-FOUND,
;; :PERMISSION-DENIED, :INVALID-ARGUMENT or :IO.
(defun read-file (path)
  (await (vector :read-file path)))

;; [kind size modified-unix-ms], kind one of :file :directory :symlink
;; :other, or NIL when nothing is there. Size and time are decimal strings,
;; since fixnums are 31 bits.
(defun file-status (path)
  (await (vector :file-status path)))

;; Sorted entry names, without "." and "..".
(defun list-directory (path)
  (await (vector :list-directory path)))

(defvar *content-types*
  '(("html" "text/html; charset=utf-8")
    ("htm" "text/html; charset=utf-8")
    ("css" "text/css; charset=utf-8")
    ("js" "text/javascript; charset=utf-8")
    ("mjs" "text/javascript; charset=utf-8")
    ("json" "application/json")
    ("txt" "text/plain; charset=utf-8")
    ("wisp" "text/plain; charset=utf-8")
    ("wasm" "application/wasm")
    ("svg" "image/svg+xml")
    ("png" "image/png")
    ("jpg" "image/jpeg")
    ("jpeg" "image/jpeg")
    ("gif" "image/gif")
    ("webp" "image/webp")
    ("ico" "image/x-icon")
    ("pdf" "application/pdf")
    ("woff2" "font/woff2")))

(defun content-type (path)
  (let* ((name (head (last (split-string path "/"))))
         (parts (split-string name "."))
         (found (and (tail parts)
                     (find-result *content-types*
                       (fn (entry)
                         (when (equal? (head entry) (head (last parts)))
                           (second entry)))))))
    (if found (tail found) "application/octet-stream")))

;; A response body of [:file path] is streamed by the native server with
;; Content-Length, instead of being buffered in the heap. The file is opened
;; when the handler finishes; a missing one then fails the request with 500.
;; SERVE-FILE checks first and answers 404 for anything but a regular file,
;; including invalid and ungranted paths.
(defun serve-file (path &optional type)
  (let ((status (try (file-status path) (catch (e k) nil))))
    (if (and status (eq? (vector-get status 0) :file))
        (do (add-header! "Content-Type" (or type (content-type path)))
            (set-response-body! (vector :file path)))
      (send! :respond (response 404 nil "Not Found\n")))))

;; Routing, adapted from the same web/http.wisp. A pattern is a method
;; followed by path segments: strings match exactly, _ skips one segment,
;; other symbols bind one segment, and &REST NAME binds the remaining
;; segments as a list. A symbol method binds the method. Segments stay
;; percent-encoded, like the URL pathname they came from.
;;
;;   (defroute ("GET" "git" repo "info" "refs") ...)
;;   (serve-http 8080 #'route-request)
;;
;; Deliberate differences from Zig Wisp: routes are tried in definition
;; order (redefining a pattern replaces its handler in place, so reloading
;; a file keeps precedence), matching is a pure function, so a handler can
;; no longer abort into the next route, HEAD falls back to GET routes, and a
;; path served under other methods answers 405 with Allow.
(defvar *routes* nil)

(defun %replace-route (routes pattern handler)
  (cond
    ((nil? routes) (list (list pattern handler)))
    ((equal? (head (head routes)) pattern)
     (cons (list pattern handler) (tail routes)))
    (t (cons (head routes)
             (%replace-route (tail routes) pattern handler)))))

(defun install-route (pattern handler)
  (set! *routes* (%replace-route *routes* pattern handler)))

(defmacro defroute (pattern &rest body)
  `(install-route ',pattern
     (fn ,(filter pattern (fn (x) (and (symbol? x) (not (eq? x '_)))))
       ,@body)))

;; Returns (BINDINGS) on a match, so a match without bindings is not NIL.
(defun %match-route (pattern parts acc)
  (cond
    ((nil? pattern) (when (nil? parts) (list (reverse acc))))
    ((eq? (head pattern) '&rest) (list (reverse-append acc parts)))
    ((nil? parts) nil)
    ((eq? (head pattern) '_)
     (%match-route (tail pattern) (tail parts) acc))
    ((symbol? (head pattern))
     (%match-route (tail pattern) (tail parts) (cons (head parts) acc)))
    ((equal? (head pattern) (head parts))
     (%match-route (tail pattern) (tail parts) acc))
    (t nil)))

;; "/" is (""), "/a/" is ("a" ""), and OPTIONS * is ("*").
(defun %route-segments (path)
  (split-string
   (if (eq? 0 (string-search path "/"))
       (string-slice path 1 (string-length path))
     path)
   "/"))

;; Returns (ROUTE BINDINGS) for the first matching route, or NIL.
(defun %find-route (method segments)
  (find-result *routes*
    (fn (route) (%match-route (head route) (cons method segments) nil))))

(defun %allowed-methods (segments)
  (reduce
   (fn (allowed route)
     (let ((method (head (head route))))
       (if (and (string? method)
                (not (includes? allowed method))
                (%match-route (tail (head route)) segments nil))
           (append allowed
                   (if (and (equal? method "GET")
                            (not (includes? allowed "HEAD")))
                       (list method "HEAD")
                     (list method)))
         allowed)))
   *routes* nil))

(defun route-request ()
  (let* ((segments (%route-segments (request-path)))
         (found (or (%find-route (request-method) segments)
                    (and (equal? (request-method) "HEAD")
                         (%find-route "GET" segments)))))
    (if found
        (apply (second (head found)) (second found))
      (let ((allowed (%allowed-methods segments)))
        (send! :respond
          (if allowed
              (response 405 (list (vector "Allow" (join-strings ", " allowed)))
                        "Method Not Allowed\n")
            (response 404 nil "Not Found\n")))))))

;; Await the serving task. Native connection recipes run in a bounded pool;
;; callbacks interleave at awaits on the same thread, without guest jobs.
;; Plain HTTP on loopback only; TLS belongs to the reverse proxy.
(defun serve-http (port handler)
  (await (vector :http-serve (vector port handler))))

;; Await the composite DNS/TCP/TLS/HTTP task in the calling activation.
;; Returns [status headers body], like RESPONSE. Headers are a list of
;; [name value] vectors, and the decoded body is a binary-safe string.
(defun fetch-http (url &optional method headers body)
  (await (vector :http-fetch (vector url (or method "GET") headers body))))
