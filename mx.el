;;; mx.el --- emacsclient keyboard driving helpers -*- lexical-binding: t; -*-

(require 'cl-lib)

(defvar server-eval-args-left)

(define-error 'type-buttons-bell "Command rang bell")
(define-error 'type-buttons-unbound-key "Key sequence is not bound")
(define-error 'type-buttons-incomplete-key "Key sequence ended inside a prefix map")

(defun type-buttons--keys (&optional keys)
  "Return KEYS, or consume `server-eval-args-left' as key strings."
  (or keys
      (prog1 server-eval-args-left
        (setq server-eval-args-left nil))))

(defun type-buttons--events (keys)
  "Convert KEYS, a list of `kbd' fragments, to a list of events."
  (append (read-kbd-macro (mapconcat #'identity keys " ")) nil))

(defun type-buttons--event-description (events)
  "Return a `key-description' string for EVENTS."
  (key-description (vconcat events)))

(defun type-buttons--split-one (events)
  "Split the next command-sized key sequence from EVENTS.

Return (CHUNK BINDING REST), where CHUNK is the event list that
resolved to BINDING and REST is the unconsumed event list.  Lookup
uses the currently active keymaps, so callers can run this again
after a command has opened a minibuffer or changed major modes."
  (let ((chunk nil)
        binding)
    (while (and events (or (null binding) (keymapp binding)))
      (push (pop events) chunk)
      (setq binding (key-binding (vconcat (reverse chunk)) t)))
    (let ((chunk (reverse chunk)))
      (cond
       ((null binding)
        (signal 'type-buttons-unbound-key
                (list :keys (type-buttons--event-description chunk))))
       ((keymapp binding)
        (signal 'type-buttons-incomplete-key
                (list :keys (type-buttons--event-description chunk))))
       (t
        (list chunk binding events))))))

(defun type-buttons-split (&rest keys)
  "Split KEYS into command-sized groups in the current keymaps.

This is a static inspection helper.  `type-buttons' performs the
same lookup dynamically after each command, so commands that change
keymaps or enter a minibuffer affect how later events are resolved."
  (let ((remaining (type-buttons--events (type-buttons--keys keys)))
        groups)
    (while remaining
      (pcase-let ((`(,chunk ,command ,rest)
                   (type-buttons--split-one remaining)))
        (push (list :command command
                    :keys (type-buttons--event-description chunk))
              groups)
        (setq remaining rest)))
    (nreverse groups)))

(defun type-buttons--context (&optional chunk command remaining)
  "Return diagnostic context for CHUNK, COMMAND, and REMAINING events."
  (list :command command
        :keys (and chunk (type-buttons--event-description chunk))
        :buffer (buffer-name (current-buffer))
        :point (point)
        :minibuffer-depth (minibuffer-depth)
        :minibuffer-window-active (active-minibuffer-window)
        :remaining (and remaining
                        (type-buttons--event-description remaining))))

(defun type-buttons (&rest keys)
  "Run KEYS as command-sized keyboard interactions.

When called via emacsclient, extra arguments after the first eval
form are consumed from `server-eval-args-left':

  emacsclient --eval \\='(type-buttons)\\=' C-x C-f /tmp/foo.txt RET

Each step resolves the next command using the currently active
keymaps, runs that command, and records context after it returns.
If a command signals an error or rings the bell, stop immediately
and return diagnostic data."
  (let* ((keys (type-buttons--keys keys))
         (spec (mapconcat #'identity keys " "))
         (remaining (type-buttons--events keys))
         history
         current-chunk
         current-command)
    (condition-case err
        (let ((ring-bell-function
               (lambda ()
                 (signal 'type-buttons-bell
                         (type-buttons--context
                          current-chunk current-command remaining)))))
          (while remaining
            (pcase-let ((`(,chunk ,command ,rest)
                         (type-buttons--split-one remaining)))
              (setq current-chunk chunk
                    current-command command
                    remaining rest)
              (let ((unread-command-events remaining))
                (command-execute command 'record)
                (setq remaining unread-command-events))
              (push (type-buttons--context chunk command remaining) history)))
          (list :ok t
                :spec spec
                :commands (nreverse history)))
      (quit
       (list :ok nil
             :spec spec
             :condition (car err)
             :data (cdr err)
             :command-context
             (type-buttons--context current-chunk current-command remaining)
             :commands (nreverse history)))
      (error
       (list :ok nil
             :spec spec
             :condition (car err)
             :data (cdr err)
             :command-context
             (type-buttons--context current-chunk current-command remaining)
             :commands (nreverse history))))))
