# Video preview fixture

These generated, one-frame, 640×360 blue FFV1 videos contain no user material
and are dedicated to the public domain (CC0). They are intentionally small so
tests require neither downloads nor AI services. `blue.mkv` uses square pixels;
`anamorphic.mkv` uses a 4:3 sample aspect ratio to verify display proportions.

Generation command:

```sh
ffmpeg -nostdin -hide_banner -loglevel error \
  -f lavfi -i color=c=blue:s=640x360:r=1:d=1 -an -c:v ffv1 -threads 1 \
  -fflags +bitexact -flags:v +bitexact -map_metadata -1 -f matroska blue.mkv
```

Length: 1158 bytes. SHA-256:
`bf6dc4e2dd82e6f5c3cbfe0472fde2d064b174c5ace2e430dd116bc49cd0f6b2`.
Container output can vary with FFmpeg versions; update the pinned digest and
tests deliberately if regenerating this fixture.

`anamorphic.mkv` is generated with the same command plus `-vf setsar=4/3`.
Length: 1170 bytes. SHA-256:
`ff6462e5764c21f63c2878aa3e66d6d95626528fc3ca3f295cc1d4b1c36f1225`.
