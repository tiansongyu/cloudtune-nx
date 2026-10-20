#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
test_binary="${1:?usage: test-aac.sh test-binary [fixture-directory]}"
fixture_dir="${2:-build/validation/aac/fixtures}"
mkdir -p "$fixture_dir"
source_args=(-f lavfi -i "sine=frequency=440:sample_rate=48000:duration=3"
             -af "pan=stereo|c0=c0|c1=0.5*c0" -c:a aac -b:a 128k)
ffmpeg -nostdin -hide_banner -loglevel error -y "${source_args[@]}" -f adts "$fixture_dir/adts.bin"
ffmpeg -nostdin -hide_banner -loglevel error -y "${source_args[@]}" -movflags +faststart \
    -f ipod "$fixture_dir/faststart.flac"
ffmpeg -nostdin -hide_banner -loglevel error -y "${source_args[@]}" -f ipod "$fixture_dir/endmoov.mp3"
ffmpeg -nostdin -hide_banner -loglevel error -y -f lavfi \
    -i "sine=frequency=880:sample_rate=44100:duration=3" -c:a aac -b:a 96k -f adts "$fixture_dir/mono.bin"
for layout in 51 71; do
    mapping="pan=5.1|FL=c0|FR=0.5*c0|FC=0.3*c0|LFE=0.1*c0|BL=0.2*c0|BR=0.4*c0"
    reference="pan=stereo|FL<FL+0.707106781*FC+0.5*LFE+0.707106781*BL|FR<FR+0.707106781*FC+0.5*LFE+0.707106781*BR"
    if [[ "$layout" == 71 ]]; then
        mapping="pan=7.1|FL=c0|FR=0.5*c0|FC=0.3*c0|LFE=0.1*c0|BL=0.2*c0|BR=0.4*c0|SL=0.6*c0|SR=0.7*c0"
        reference="pan=stereo|FL<FL+0.707106781*FC+0.5*LFE+0.707106781*BL+0.707106781*SL|FR<FR+0.707106781*FC+0.5*LFE+0.707106781*BR+0.707106781*SR"
    fi
    ffmpeg -nostdin -hide_banner -loglevel error -y -f lavfi \
        -i "sine=frequency=660:sample_rate=48000:duration=3" -af "$mapping" \
        -c:a aac -b:a 384k -movflags +faststart -f ipod "$fixture_dir/surround$layout.bin"
    ffmpeg -nostdin -hide_banner -loglevel error -y -i "$fixture_dir/surround$layout.bin" \
        -af "$reference" -c:a pcm_s16le -f s16le "$fixture_dir/surround$layout.bin.s16"
done
for fixture in adts.bin faststart.flac endmoov.mp3 mono.bin; do
    ffmpeg -nostdin -hide_banner -loglevel error -y -i "$fixture_dir/$fixture" \
        -c:a pcm_s16le -f s16le "$fixture_dir/$fixture.s16"
done
"$test_binary" "$fixture_dir"
