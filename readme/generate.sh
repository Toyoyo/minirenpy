#!/bin/bash
if [ "$1" == "" ]
then
  echo "Usage: $0 [image.png]"
  exit 1
fi
python3 -I ../tools/cp437.py miwa.png logo 50 adapt16 0 0 MAXCOVERAGE
python3 -I ../tools/mkreadme.py logo.bin logo.pal readme.txt readmdat.h
wcl -zq -bt=dos -l=dos -ms -0 -os -w4 -fe=readme.exe ../tools/readme.c
rm -f logo.bin logo.pal logo.png readmdat.h readme.o
