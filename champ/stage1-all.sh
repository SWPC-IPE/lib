#!/bin/sh
#
# Run stage1 on all data files making a separate screen for each
# year

for year in $(seq 2007 2010); do
  screen -d -m -S p${year} /bin/bash stage1_chaos.sh ${year}
done
