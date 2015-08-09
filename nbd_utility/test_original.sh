#!/bin/bash

i=0
while [ "$i" != "100" ]
do
  ./read_test /dev/md0
  sleep 2
  i=$(($i+1))
done
