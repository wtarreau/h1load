#!/bin/sh
#
# This script extracts from h1load's output the part containing only the
# percentiles. It takes the input either from the file name(s) designated
# in arguments, or from stdin.
exec sed -ne '/^#pct/,$p' "$@"
