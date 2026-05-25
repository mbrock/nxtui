#!/usr/bin/env bash
cd /home/mbrock/nxtui
set -a
[ -f .env ] && . ./.env
[ -f .envrc ] && . ./.envrc 2>/dev/null || true
set +a
export NXT_STDOUT_TRACE=/home/mbrock/nxtui/tmp/strace-stdio-20260525-182416/stdout-trace.log
exec strace -ff -tt -o /home/mbrock/nxtui/tmp/strace-stdio-20260525-182416/trace -e trace=write,read,poll,ppoll,select,pselect6,io_uring_setup,io_uring_enter,io_uring_register,ioctl,fcntl ./build/nxtllm -m gpt-5.4-nano 'say hello briefly; do not use tools'
