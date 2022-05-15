#!/bin/bash

# variables
preload=/home/courses/cs3214/bin/sfi/gurthang/gurthang-preload.so
server=/home/ugrads/majors/cpham006/3214/p4/pserv/src/server-fuzz

# input check
if [ $# -lt 1 ]; then
    echo "Usage: $0 /path/to/crash/file"
    exit 1
fi
fpath=$1

# running
LD_PRELOAD=${preload} gdb ${server} -ex "set args -p 55825 -R /home/ugrads/majors/cpham006/3214/p4/pserv/src/fuzz_root  < ${fpath}" -ex "echo [38;2;255;174;52mfuzz-rerun-gdb.sh: Arguments are set. Type 'run' to reproduce the crash/hang.
"[0m
