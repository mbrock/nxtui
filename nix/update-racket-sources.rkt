#lang racket/base

;; Explicit, networked lock update. Normal Nix builds never consult a catalog.
;; Run with the flake's Racket; see README.md for the atomic lock update command.
(require json
         net/url
         pkg/lib
         racket/list
         racket/port
         racket/runtime-path
         racket/string
         racket/system
         setup/getinfo)

(define-runtime-path forge "../vendor/racket/forge")
(define-runtime-path something "../vendor/racket/something-src")
;; The minimal distribution supplies just these packages. Keep the lock
;; independent of the updater's installed packages (including Darwin's full
;; distribution), so it remains complete for the Linux headless build.
(define bundled '("base" "racket-lib"))
(define sources (make-hash))
(define seen (make-hash))

(define (archive-url source rev)
  (define u (string->url source))
  (cond
    ;; Release-catalog packages are already archives, not Git repositories.
    [(and (equal? (url-scheme u) "https") (regexp-match? #rx"[.]zip$" source)) source]
    [else
     (unless (and (member (url-scheme u) '("https" "git" "github"))
                  (regexp-match? #px"^[0-9a-f]{40}$" rev))
       (error 'archive-url "expected an HTTPS archive or Git source and commit: ~a ~a" source rev))
     (define parts (map path/param-path (url-path u)))
     (define owner (car parts))
     (define repo (regexp-replace #rx"[.]git$" (cadr parts) ""))
     (define host (url-host u))
     (if (equal? host "gitlab.com")
         (format "https://~a/~a/~a/-/archive/~a/~a-~a.tar.gz" host owner repo rev repo rev)
         (format "https://~a/~a/~a/archive/~a.tar.gz" host owner repo rev))]))

(define (visit dep)
  (define name (if (pair? dep) (car dep) dep))
  (unless (or (member name bundled) (hash-has-key? seen name))
    (hash-set! seen name #t)
    (define details (get-pkg-details-from-catalogs name))
    (unless details (error 'visit "package not in catalog: ~a" name))
    (define source (hash-ref details 'source))
    (define url (archive-url source (hash-ref details 'checksum)))
    (define packages (hash-ref! sources url make-hash))
    (define subdir (cond [(assq 'path (url-query (string->url source))) => cdr]
                        [else "."]))
    (hash-set! packages (string->symbol name) subdir)
    (for-each visit (hash-ref details 'dependencies))))

(parameterize ([current-pkg-catalogs
                (list (string->url (format "https://download.racket-lang.org/releases/~a/catalog/" (version)))
                      (string->url "https://pkgs.racket-lang.org"))])
  (visit "compiler-lib") ; supplies `raco make` in the minimal environment
  (for ([dir (list forge something)])
    (for-each visit (extract-pkg-dependencies (get-info/full dir) #:build-deps? #t))))

(define locked
  (for/list ([url (sort (hash-keys sources) string<?)])
    (eprintf "Pinning ~a\n" url)
    (define result
      (string->jsexpr
       (with-output-to-string
         (lambda ()
           (unless (system* (find-executable-path "nix") "store" "prefetch-file"
                            "--unpack" "--json" url)
             (error 'prefetch "failed: ~a" url))))))
    (hash 'url url 'hash (hash-ref result 'hash) 'packages (hash-ref sources url))))
(write-json locked #:indent 2)
(newline)
