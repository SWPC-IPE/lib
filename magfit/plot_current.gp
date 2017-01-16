#!/usr/bin/env gnuplot

set term pngcairo enh col size 800,600
set out "current.png"

unset key

set cbrange [-25:25]
set cbtics 5

set cntrparam levels inc -40,2,40

#lonA = -46.017235
#lonB = -26.378850
#lonC = -44.581345
lonA = -10.549855
lonB = 36.054322
lonC = -9.123493

idx=0

file = 'chi.txt'
tfile = 'test.dat'
cfile = 'cont.dat'

set table tfile
splot file us 1:2:3 index idx
unset table

set contour base
unset surface
set table cfile
splot file us 1:2:3 index idx
unset table
unset contour
set surface

load 'moreland.pal'
load 'xlonon.cfg'
load 'ylaton.cfg'
set cblabel "kA"

set arrow 1 from lonA,-90 to lonA,90 front nohead lt 6 lw 2
set arrow 2 from lonC,-90 to lonC,90 front nohead lt 6 lw 2
set arrow 3 from lonB,-90 to lonB,90 front nohead lt 6 lw 2

set label 1 "A,C" at lonA,93 center font ",8"
set label 2 "B" at lonB,93 center font ",8"

plot tfile w image, cfile w li lt -1 lw 1.5