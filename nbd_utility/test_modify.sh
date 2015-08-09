#!/bin/bash

i=0
while [ "$i" != "100" ]
do
  ./ignoreR.1; ./read_test /dev/md0; ./ignoreR.0
  sleep 1
  i=$(($i+1))
done
