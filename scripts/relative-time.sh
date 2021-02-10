#!/bin/sh
#
# This script extracts from h1load's output the first part which contains the
# real-time load measurements, and outputs them with a relative time starting
# at zero, to ease auto-scaling in graph scripts. It takes the input either
# from the file name(s) designated in arguments, or from stdin.
exec awk '/^#=/{exit;} { t=t?t:int($1); if ($1>0) $1-=t; print}' "$@"
