;; Save at the timer, then restore in a new process (see the README).
(write "Starting once.\n")
(let ((answer 35))
  (sleep-ms 5000)
  (print (+ answer 7)))
(write "Finished after the saved deadline.\n")
