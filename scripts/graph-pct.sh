#!/bin/bash

# This script will read two load output files from h1load and produce a graph
# showing the response time distribution long a percentile axis.
# The default output will be a PNG file under the same name but with the
# extension replaced with ".png".
#

# show usage with $1=command name and optional exit code in $2
usage() {
  echo "Usage: ${1##*/} [-h|--help] [-t title] [-l] [-o outfile] <pctfile>"
  echo "   -h --help      display this help"
  echo "   -l             use a log scale on y (increases the dynamic range)"
  echo "   -o outfile     output PNG file (default: same as pctfile with .png)"
  echo "   -t title       set the graph's title"
  echo "   pctfile        h1load percentile output from the main worker"
  echo
  echo "If no title is set and the file name contains a colon, then everything between"
  echo "the first colon and the last dot (if any) will be used as the graph's title."
  echo "Otherwise the title passed with -t will be used for all files if not empty,"
  echo "otherwise the file's name without what follows the last dot will be used."
  exit $2
}


out=""; log=""
while [ -n "$1" -a -z "${1##-*}" ]; do
  if [ "$1" = "-t" ]; then
    title="$2"
    shift; shift
  elif [ "$1" = "-l" ]; then
    log=1
    shift;
  elif [ "$1" = "-o" ]; then
    out="$2"
    shift; shift
  elif [ "$1" = "-h" -o "$1" = "--help" ]; then
    usage "$0" 0
  else
    usage "$0" 1
  fi
done

if [ $# -eq 0 ]; then
  usage "$0" 1
fi

pct="$1"
if [ -z "$out" ]; then
  out=${pct%.*}.png
fi

t="${pct##*/}"
if [ -n "$t" -a -z "${t##*:*}" ]; then
  title="${t#*:}"
  title="${name%.*}"
elif [ -z "$title" ]; then
  title="${t%.*}"
fi

gnuplot << EOF
  set title "$title"
  set grid lt 0 lw 1 ls 1 lc rgb "#d0d0d0"
  ${log:+set logscale y2}
  set yrange [0:1]
  unset ytics
  set y2range [0:]
  set y2tics nomirror
  set xtics nomirror border
  set xlabel "Percentile" #offset 0,0.5
  set y2label "Latency (milliseconds)"
  #set key inside bottom center box
  set key outside bottom center horizontal spacing 1.5 reverse Left
  #set terminal png font courbi 9 size 800,400
  set terminal pngcairo size 800,400 background rgb "#f0f0f0"
  set style fill transparent solid 0.3 noborder
  set output "${out%.*}.png"
  set logscale x 10
  set xrange [1:1000000]
  set xtics ("0%%" 1, "|\nmean (50%%)" 2, "90%%" 10, "99%%" 100, "99.9%%" 1000, "99.99%%" 10000, "99.999%%" 100000, "99.9999%%" 1000000)
  set format y2 "%.1f"
  plot "$pct" using 3:5 with lines title "Latency (ms)" lt 1 lw 3 axis x1y2, \
           "" using 3:5 with filledcurves x1 notitle lt 1 axis x1y2
EOF
