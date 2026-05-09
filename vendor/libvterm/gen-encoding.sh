#!/bin/sh
# Generate encoding table include file
# Usage: gen-encoding.sh <input.tbl> <output-dir> <basename>

mkdir -p "$2/encoding"
perl -CSD tbl2inc_c.pl "$1" > "$2/encoding/$3.inc"
