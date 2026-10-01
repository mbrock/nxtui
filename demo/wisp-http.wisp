;; SPDX-License-Identifier: AGPL-3.0-or-later
;; Run with: build/wisp run demo/wisp-http.wisp
;; Loopback port 8080. Put a TLS-terminating reverse proxy in front.

(serve-http 8080
  (fn ()
    (add-header! "Content-Type" "text/plain; charset=utf-8")
    (cond
      ((equal? (request-path) "/")
       (set-response-body! "Hello from Wisp on NXT.\nTry /slow or POST /echo.\n"))
      ((equal? (request-path) "/slow")
       (do (sleep-ms 1000)
           (set-response-body! "Other requests kept running during that timer.\n")))
      ((equal? (request-path) "/echo")
       (set-response-body! (request-text)))
      (t (send! :respond (response 404 nil "Not Found\n"))))))
