#!/bin/bash

for i in "$@"; do
  # take only what's after ':' if present in the file name, and stop
  # before the last dot (file name extension).
  name="${i%.*}"
  name="${name##*:}"
gnuplot <<EOF
  set title "$name"
  set grid lt 0 lw 1 ls 1 lc rgb "#d0d0d0"
  set yrange [0:]
  set ytics nomirror
  set xtics nomirror
  set xlabel "Percentile" #offset 0,0.5
  set ylabel "Latency (milliseconds)"
  #set key inside bottom center box
  set key outside bottom center horizontal spacing 1.5 reverse Left
  #set terminal png font courbi 9 size 800,400
  set terminal pngcairo size 800,400 background rgb "#f0f0f0"
  set style fill transparent solid 0.3 noborder
  set output "${i%.*}.png"
  set logscale x 10
  set xrange [1:1000000]
  set xtics ("0%%" 1, "90%%" 10, "99%%" 100, "99.9%%" 1000, "99.99%%" 10000, "99.999%%" 100000, "99.9999%%" 1000000)
  set format y "%.1f"
  plot "$i" using 3:5 with lines title "Latency (ms)" lt 1 lw 3, "" using 3:5 with filledcurves x1 notitle lt 1
EOF
done
