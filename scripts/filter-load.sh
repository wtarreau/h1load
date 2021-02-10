#!/bin/sh
#
# This script extracts from h1load's output the first part which contains the
# real-time load measurements. It takes the input either from the file name(s)
# designated in arguments, or from stdin.
exec sed -ne '/^#=/q;p' "$@"
