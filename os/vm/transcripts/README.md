# vm/transcripts — what a live model actually did, kept verbatim

These are recordings of REAL sessions: a live model, over TLS, driving real
emulated hardware, with the operator's key. They are evidence, not tests. Nothing
in the build reads them. They are here because the claim "a model wrote a driver
for a card the kernel had never heard of, and sound came out" is worth nothing
without the transcript and the samples beside it.

Every pair is produced by `tests/qemu/live_audio.py` and measured by
`tests/qemu/wavcheck.py`:

    NAME.log        the whole serial transcript, kernel voice included
    NAME.wav.txt    wavcheck.py's analysis of what the codec was really fed
    NAME.png        the framebuffer at the end, where an app was involved

## The headline result

`ac97-fullloop.*` — one boot, three sentences, the entire loop:

    driver_targets -> driver_run -> driver_install -> audio_tone -> app + gui_click

The model found an unclaimed 8086:2415 at 00:04.0, brought the codec up, wrote a
second short play program, installed it as the machine's audio sink ("vmaudio"),
played a 440 Hz note through the ordinary `audio_tone` tool, then authored a
two-widget app and pressed its own button. The WAV holds two clean 440 Hz sine
tones at **100.0% tonality and peak exactly 9000**, which is the amplitude
`audio_tone` was asked for.

## Read this one for the bug, not just the win

The same transcript is the best bug report in the tree. At line 100 the model
says, in its own words:

> the contract body is rendering blank (a `%*s` formatting bug in the kernel), so
> I can't read the exact install-program requirements from it

It was right. `tools/audio_tools.c` printed the play contract with `%.*s`, and
the kernel's reduced vsnprintf (`lib/libc_shim.c`) has no star precision — it
emitted the literal text `%*s` instead of the contract. Denied the one document a
play program cannot be written without, the model guessed, and the guess is
visible on the framebuffer in `ac97-fullloop.png`:

> One BDL entry: addr=r7, count=98304 samples, IOC+BUP set.

98304 is the whole 192 KiB PCM region, not `r9` (the bytes actually written for
this sound). AC'97's descriptor length field is 16 bits, so 98304 truncated to
32768 samples = 16384 stereo frames = **341.3 ms**, and `ac97-fullloop.wav.txt`
shows every note lasting exactly 341.3 ms regardless of the duration requested,
followed by a DC hold at 8081 — which is what `BUP` means on that part. A kernel
formatting bug, a model's guess, and a measurable acoustic artefact, joined end
to end.

That line is fixed. `tests/qemu/lint_printf.py` is the guard, and it still
reports 25 more instances in `vm/dvm.c`'s assembler diagnostics — see that
script's header for why those are worse.

## The earlier single-card runs

Made before `driver_install` existed, so they stop at `driver_run`: the model
drove the hardware directly and never became the machine's sink.

| file | card | outcome |
|---|---|---|
| `ac97.*` | AC97 | silent — an early run that never started the engine |
| `ac97b.*` | AC97 | three 444 Hz triangle notes, 81.6% tonal |
| `ac97-final.*` | AC97 | 1000.0 Hz triangle, 98.5% tonal, peak 8000 |
| `hda.*` | intel-hda | silent |
| `hda2.*` | intel-hda | 445.4 Hz square, 81.6% tonal, ran 51 s |
| `adlib.*` | adlib | silent, and necessarily so — an ISA card has no PCI config space, so it can never be a driver target (`tests/qemu/cases/audio-isa-invisible.case`) |

The measured frequencies are worth more than the logs, because each one is a
fingerprint of the model's own integer arithmetic rather than of anything the
kernel would produce. `ac97-final` asked for a 48-sample triangle period at
48 kHz and 48000/48 is exactly the 1000.0 Hz measured; `hda2` used a 54-sample
half period and 48000/108 = 444.4 Hz against 445.4 measured. The kernel's
synthesiser plays sines at the requested pitch, so neither number could have
come from it.

## Reproducing

    tests/qemu/live_audio.py --card ac97 --out /tmp/run \
        --turn "there's a sound card in this machine with no driver..."
    tests/qemu/wavcheck.py /tmp/run.wav

It costs the operator real money, and `live_audio.py` never reads `os/.env` —
`make run-nox` does that, and passes the key to the guest over fw_cfg.
