# DTS test samples

These short audio-only fixtures were extracted from user-supplied demo files.
They are intentionally limited to approximately one second and are used by
`testall.sh`; the test suite does not invoke the `ffmpeg` executable.

| File | Expected stream |
|------|-----------------|
| `dtshd-hra-7.1-96.dtshd` | DTS-HD HRA, 96 kHz, 7.1 |
| `dtshd-ma-7.1-24.dtshd` | DTS-HD MA, 48 kHz, 7.1, 24-bit |
| `dtsx-7.1-24.dtshd` | DTS-HD MA + DTS:X, 48 kHz, 7.1, 24-bit |
| `dtsx-imax-7.1-24.dtshd` | DTS-HD MA + DTS:X IMAX, 48 kHz, 7.1, 24-bit |
| `dts-uhd-p2-imax.mka` | DTS-UHD Profile 2 with IMAX Enhanced, 48 kHz, 10 channels |

The first four files are raw DTS elementary streams extracted without
re-encoding. The DTS-UHD source cannot be represented as a legacy DTS
elementary stream by FFmpeg; it is therefore stored as an audio-only Matroska
passthrough track with the original QuickTime `dtsx` sample entry. It is a
negative fixture that verifies the legacy DTS handler does not misidentify
DTS-UHD Profile 2.

SHA-256:

```text
a6973f67be6a61f6bf6d442ed80805128a216e38991e8c01a5cd7211018aaa73  dts-uhd-p2-imax.mka
ac78ff2eac3b4ff6f67b6bc0364ff0f80bb19d36cfe5265390cfda37868bff60  dtshd-hra-7.1-96.dtshd
abe54e6ac8120ae8d0a299ff74c9b92cecdd1cce767fabe75072cde32ac44205  dtshd-ma-7.1-24.dtshd
e1aed9dffed00d817adbe6fc0779bbdf15adf7c39cdef196f9b33ae31b152b28  dtsx-7.1-24.dtshd
d185eda7879d7cadbbcc2c4196ceecdb3a89b8adb0f88e1879e95c0af8e66cf0  dtsx-imax-7.1-24.dtshd
```
