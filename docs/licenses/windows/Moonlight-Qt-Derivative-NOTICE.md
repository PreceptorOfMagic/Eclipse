# Moonlight Qt derivative notice

The Eclipse desktop video module at
`third_party/ss4s/modules/ffmpeg/ffmpegvid.c` incorporates and adapts decoding,
hardware-format selection and presentation work from the following Moonlight
Qt source revision:

- Project: [Moonlight Qt](https://github.com/moonlight-stream/moonlight-qt)
- Revision: [`032529d782242e3833e0b3b147dbbf96e878e3ca`](https://github.com/moonlight-stream/moonlight-qt/commit/032529d782242e3833e0b3b147dbbf96e878e3ca)
- Referenced sources: `app/streaming/video/ffmpeg.cpp`,
  `app/streaming/video/ffmpeg-renderers/d3d11va.cpp` and
  `app/streaming/video/ffmpeg-renderers/sdlvid.cpp`
- Upstream authorship: Moonlight Game Streaming Project contributors
- Licence: GNU GPL version 3; the complete, byte-identical upstream text is
  packaged as `licenses/source/Moonlight-Qt-GPL-3.0.txt`

## Modification record

Relevant modification date: **2026-09-28**.

The referenced C++/Qt renderer design was adapted into a C SS4S video-driver
module and the SS4S `SS4S_VideoDriver` lifecycle. The local derivative:

- replaces Qt/session integration with SS4S callbacks and the client-owned SDL
  renderer/window contract;
- adds a fail-closed Windows D3D11VA capability and physical-adapter check;
- downloads D3D11VA frames and converts NV12/P010 output to IYUV for the current
  SDL presentation boundary;
- adds host-render lifetime synchronisation, frame/corruption telemetry and
  SS4S decoder-latency reporting; and
- retains a non-Windows FFmpeg software-decoding path while restricting the
  Windows software path to an explicitly labelled diagnostic mode.

This notice describes the working-tree derivative. A public binary release
must include this file and the module in the exact Corresponding Source; the
release process must update the date and description if the derivative changes.
