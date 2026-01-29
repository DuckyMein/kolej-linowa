#!/bin/bash
timeout 10 strace -f -e trace=msgrcv,msgsnd -p $1 2>&1 | head -100
