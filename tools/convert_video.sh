#!/bin/bash

if [ "$2" == "" ]
then
    echo "usage: $(basename $0): source dest.avi"
    exit 1
fi

ffmpeg -i "$1" -vf scale=640x360 -c:v libxvid -q:v 5 -bf 0 -g 125 -vtag XVID -c:a libmp3lame -b:a 128k -ar 44100 -ac 2 "$2"
