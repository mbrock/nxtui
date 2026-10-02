;; SPDX-License-Identifier: AGPL-3.0-or-later
;; Run with: build/wisp run demo/wisp-http.wisp
;; Loopback port 8080. Put a TLS-terminating reverse proxy in front.

(defun text! (body)
  (add-header! "Content-Type" "text/plain; charset=utf-8")
  (set-response-body! body))

(defroute ("GET" "")
  (text! "Hello from Wisp on NXT.\nTry /hello/you, /slow, POST /echo or POST /relay.\n"))

(defroute ("GET" "hello" name)
  (text! (string-append "Hello, " name ".\n")))

(defroute ("GET" "slow")
  (sleep-ms 1000)
  (text! "Other requests kept running during that timer.\n"))

(defroute ("POST" "echo")
  (text! (request-text)))

;; The handler awaits an outbound request while another callback handles
;; /echo on this listener. SEND! returns the fetched response early.
(defroute ("POST" "relay")
  (send! :respond
    (fetch-http "http://127.0.0.1:8080/echo" "POST"
      (list (vector "Content-Type" "text/plain; charset=utf-8"))
      (request-text))))

(serve-http 8080 #'route-request)
