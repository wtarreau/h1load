#!/bin/bash

# This script will read one or multiple load output files from h1load and
# produce two-axis graphs showing the request per second and the latency.
# The output will be one PNG file per input file, under the same name but
# with the extension replaced with ".png".
#

if [ $# -eq 0 -o "$1" = "-h" -o "$1" = "--help" ]; then
  echo "Usage: ${0##*/} [-h|--help] [-t title] <file>..."
  echo "   -h --help      display this help"
  echo "   -t title       set the graph's title"
  echo "   file...        file(s) to convert"
  echo
  echo "If no title is set and the file name contains a colon, then everything between"
  echo "the first colon and the last dot (if any) will be used as the graph's title."
  echo "Otherwise the title passed with -t will be used for all files if not empty,"
  echo "Otherwise the file's name without what follows the last dot will be used."
  exit 0
fi

if [ "$1" = "-t" ]; then
  title="$2"
  shift; shift
fi

for i in "$@"; do
  t="${i##*/}"
  if [ -n "$t" -a -z "${t##*:*}" ]; then
    title="${t#*:}"
    title="${name%.*}"
  elif [ -z "$title" ]; then
    title="${t%.*}"
  fi

gnuplot <<EOF
  set title "$title"
  set grid lt 0 lw 1 ls 1 lc rgb "#d0d0d0"
  set yrange [0:]
  set ytics nomirror
  set y2range [0:]
  set y2tics 200
  set xlabel "Time(s)" offset 0,0.5
  set ylabel "Requests per second"
  set y2label "Latency (microseconds)"
  #set key inside bottom center box
  set key outside bottom center horizontal spacing 1.5 reverse Left
  #set terminal png font courbi 9 size 800,400
  set terminal pngcairo size 800,400 background rgb "#f0f0f0"
  set style fill transparent solid 0.20 noborder
  set format y "%.0f"
  set format y2 "%.0f"
  set output "${i%.*}.png"

  autorange(x)=(scale=(x<=10?1:(10**(int(log10(x)-1)))), x/scale<=20?step=2:x/scale<=50?step=5:step=10, int((x-0.0001)/scale/step+1)*step*scale)

  stats "$i" using 1 nooutput; min_time_run=STATS_min
  x_offset=min_time_run

  #stats "$i" using 1:2 nooutput
  conmax=0

  stats "$i" using 1:9 nooutput
  rpsmax=autorange(STATS_max_y)

  stats "$i" using 1:12 nooutput
  latmax=autorange(STATS_max_y)

  y2max=(latmax>conmax)?latmax:conmax
  set y2range[0:y2max]
  set y2tics y2max/10

  set yrange[0:rpsmax]
  set ytics rpsmax/10

  # reminder on LT: 1=magenta, 2=green, 3=light blue, 4=dark yellow, 5=light yellow, 6=dark blue, 7=red, 8=black
  plot \
    "$i" using (\$1-x_offset):12 with filledcurves x1 notitle axis x1y2 lt 1, \
     ""  using (\$1-x_offset):9  with filledcurves x1 notitle lt 2, \
     ""  using (\$1-x_offset):9  with lines title "<- Req/s" lt 2 lw 3, \
     ""  using (\$1-x_offset):12 with lines title "Latency ->" axis x1y2 lt 1 lw 3
EOF
done
