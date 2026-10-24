# WASAPI drain hardware regression probe

Build explicitly (the target is excluded from normal builds):

```powershell
cmake --build <build-directory> --config Release --target am_exclusive_drain_probe
<build-directory>\Release\am_exclusive_drain_probe.exe --run-on-default-device
```

This acquires the current default render endpoint in exclusive mode and sends
only silence. Stop other playback first. The device must support stereo packed
24-bit and 16-bit PCM at 192, 96 and 44.1 kHz. Unsupported formats are reported
as failures, not skipped. It is deliberately not registered as an unattended
CTest.

The 36 cases cover empty, partial and full terminal packets, source starvation,
pause/resume during drain, initially empty sources, and producer notifications
as well as polling. The source rejects any request after its terminal packet.
Each case requires clock-certified completion without underruns or errors.

Motivation: on a C-Media-based USB endpoint (Fosi Audio DA2120C), end-of-stream
drains that followed a source starvation wait timed out intermittently (3-4 of
the 36 cases per run, always `wait=1` cases). The device loses its event/clock
cycle when a packet is released into a stopped, previously run exclusive
stream: IAudioClock parks below the committed end-of-stream position and the
drain can only fault. Restarting the preserved endpoint queue before releasing
the next packet (with a readiness check first, so the restarted stream is
filled on the same wake) made all 36 cases pass repeatedly on the same
hardware.

These are transport/control tests. They do not establish audible correctness
or Apple Music lifecycle behavior.
