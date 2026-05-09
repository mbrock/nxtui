#!/bin/sh
# Generate all libvterm include files
# Args: $1=tbl2inc_c.pl $2=DECdrawing.tbl $3=uk.tbl $4=find-wide-chars.pl $5=output_dir

TBL2INC="$1"
DECDRAWING_TBL="$2"
UK_TBL="$3"
FULLWIDTH_PL="$4"
OUTDIR="$5"

# Create encoding subdirectory
mkdir -p "$OUTDIR/encoding"

# Generate encoding tables
perl -CSD "$TBL2INC" "$DECDRAWING_TBL" > "$OUTDIR/encoding/DECdrawing.inc"
perl -CSD "$TBL2INC" "$UK_TBL" > "$OUTDIR/encoding/uk.inc"

# Generate fullwidth table
perl "$FULLWIDTH_PL" > "$OUTDIR/fullwidth.inc"

# Create dummy output files (meson needs them)
touch "$OUTDIR/encoding_DECdrawing.inc"
touch "$OUTDIR/encoding_uk.inc"
