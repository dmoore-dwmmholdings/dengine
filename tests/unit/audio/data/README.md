`tone-loops.ogg` is a generated 0.1-second, 997 Hz sine at 48 kHz with Vorbis
comments `LOOPSTART=10` and `LOOPEND=90` for the DENG-481 authored-loop override regression.
Generated with:

```sh
ffmpeg -f lavfi -i 'sine=frequency=997:sample_rate=48000:duration=0.1' \
  -ac 2 -c:a vorbis -strict -2 -metadata LOOPSTART=10 -metadata LOOPEND=90 tone-loops.ogg
```

The sound importer inherits embedded loop metadata when `loop` is absent. An explicit
`loop=false` disables automatic looping while retaining the authored loop points;
`loop=true` enables it. Importer version 2 invalidates cooked cache entries from the
previous override behavior.
