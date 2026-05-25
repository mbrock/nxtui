#!/usr/bin/env bash
cd /home/mbrock/nxtui
set -a
[ -f .env ] && . ./.env
set +a
export NXT_STDOUT_TRACE=/home/mbrock/nxtui/tmp/repro-terminal-20260525-180306/stdout-trace.log
build/nxtllm -m gpt-5.4-nano 'please use the shell tool with short sleeps and stdout lines; this is a terminal rendering harness test'
status=$?
printf '\n@@NXT_DONE:%s\n' "$status"
tmux wait-for -S "nxtrepro-done-20260525-180306"
sleep 5
exit "$status"
