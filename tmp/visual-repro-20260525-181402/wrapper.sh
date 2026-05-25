#!/usr/bin/env bash
cd /home/mbrock/nxtui
set -a
[ -f .env ] && . ./.env
[ -f .envrc ] && . ./.envrc 2>/dev/null || true
set +a
export NXT_STDOUT_TRACE=/home/mbrock/nxtui/tmp/visual-repro-20260525-181402/stdout-trace.log
build/nxtllm -m gpt-5.4-nano 'please use the shell tool with short sleeps and stdout lines; this is a terminal rendering harness test'
status=$?
printf '\n@@NXT_DONE:%s\n' "$status"
sleep 5
exit "$status"
