#!/bin/sh
# Make the first multi-frame VDEC fixture from Blender's 30 fps 1080p release.
# Big Buck Bunny: (c) Blender Foundation | blender.org, CC BY 3.0.
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: sh $0 SOURCE.mp4 OUTPUT_DIRECTORY" >&2
    exit 2
fi
source=$1
out=$2
ffmpeg=${FFMPEG:-ffmpeg}
ffprobe=${FFPROBE:-ffprobe}
mkdir -p "$out"
for file in bbb-1080p-ip.h264 bbb-1080p-ip.mp4 bbb-1080p-ip.framemd5; do
    if [ -e "$out/$file" ]; then
        echo "refusing to overwrite $out/$file" >&2
        exit 1
    fi
done
"$ffmpeg" -hide_banner -loglevel error -ss 60 -i "$source" -t 10 -an \
    -c:v libx264 -preset veryfast -profile:v baseline -pix_fmt yuv420p \
    -r 30 -bf 0 -refs 1 -g 300 \
    -x264-params scenecut=0:threads=1:repeat-headers=1:aud=1 \
    -b:v 6M -maxrate 8M -bufsize 16M -f h264 "$out/bbb-1080p-ip.h264"
"$ffmpeg" -hide_banner -loglevel error -framerate 30 -i "$out/bbb-1080p-ip.h264" \
    -c copy -movflags +faststart "$out/bbb-1080p-ip.mp4"
"$ffmpeg" -hide_banner -loglevel error -i "$out/bbb-1080p-ip.mp4" \
    -pix_fmt nv12 -f framemd5 "$out/bbb-1080p-ip.framemd5"
"$ffprobe" -v error -select_streams v:0 -count_frames \
    -show_entries stream=codec_name,profile,width,height,pix_fmt,has_b_frames,nb_read_frames,avg_frame_rate \
    -of default=nw=1 "$out/bbb-1080p-ip.mp4"
echo 'Verify 300 frames, no B-frames and 1920x1080 above before use.'
