# 完成 CLCast MJPEG 单帧显示

Status: in-progress - implementation and IAR build complete; target JPEG display validation pending

## Implementation status (2026-07-10)

- Added `mjpeg_video_player_init/proc/uninit` to the existing common Carlink
  video module. Existing H.264 APIs and EC/EY call sites are unchanged.
- Player initialization allocates one 32-byte-aligned compressed JPEG buffer
  capped at `LCD_WIDTH * LCD_HEIGHT` and one aligned YUV buffer sized for the
  maximum aligned YUV422 output. No frame-time allocation is performed.
- Each frame validates pointer, length, SOI/EOI markers, decode result,
  configured maximum dimensions, and YUV420/YUV422 format. Input cache is
  cleaned before `mfc_jpegdec`.
- Decoded YUV is scaled through the proven UVC PXP path into an RGB565 display
  buffer, submitted to `LCD_VIDEO_LAYER`, and the UI layer is hidden.
- Player shutdown disables the video layer, restores the UI layer, releases
  MFC and both buffers. Session disconnect/destroy also invokes this cleanup.
- CLCast main/map capabilities now advertise MJPEG only. Video callbacks call
  only the common MJPEG player; CLCast no longer owns H.264/JPEG decode buffers.
- Per user request, no command-line build or test was run.

## Next target validation

1. Build and flash the AWTK configuration from IAR.
2. Confirm startup and BLE behavior remain unchanged.
3. Start one CLCast MJPEG stream or inject one complete baseline JPEG through
   the CLCast frame callback.
4. Expected initialization log for the current panel:

```text
carlink mjpeg: init max=1024x600 jpeg=614400 yuv=1245184
clcast: MJPEG video stream ready
```

5. A successful frame should log detected JPEG dimensions, aligned decode
   dimensions, `YUV420` or `YUV422`, and `display=1024x600`, then appear on the
   video layer.
6. Stop the stream and confirm the native UI returns.
7. Record any `decode failed`, `unsupported dimensions`, `unsupported YUV
   format`, `PXP failed`, or allocation diagnostics with the input JPEG's
   dimensions, subsampling, and byte length.

## Parent

`.scratch/clcast-ble/PRD.md`

## What to build

为公共视频层增加 EC 风格的 MJPEG 播放器接口，并让 CLCast 会话只宣告 MJPEG。CLCast 的视频 start、frame、stop 回调同步调用公共播放器；播放器把完整 JPEG 帧复制到固定对齐缓冲区，通过平台 JPEG 硬件解码成 YUV420/YUV422，按需要缩放后显示，并在停止时恢复本机 UI。

## Acceptance criteria

- [ ] 当前 CLCast 产品的主视频和地图视频能力均只宣告 MJPEG，不再宣告 H.264。
- [ ] CLCast start/frame/stop 回调不直接操作 JPEG 解码器、PXP 或 LCD，而是调用公共 MJPEG 播放器接口。
- [ ] 播放器初始化固定对齐的 JPEG 输入缓冲和有界 YUV 输出缓冲，不按帧动态分配内存。
- [ ] 每个输入帧在复制前检查空指针、长度和固定上限，并完成硬件解码所需的缓存处理。
- [ ] JPEG 宽高不超过配置的 B3/session 尺寸；较小尺寸可以缩放到目标显示区域；较大尺寸被拒绝。
- [ ] 已知 YUV420 和 YUV422 JPEG 测试图均可成功解码并显示。
- [ ] YUV444、异常 JPEG、超长帧和硬件解码错误被安全拒绝并记录诊断。
- [ ] 视频停止后释放播放器资源、关闭视频层并恢复本机 UI。
- [ ] 现有 H.264 播放器接口和 EC/EY 使用方式保持不变。
- [ ] 目标板测试记录 JPEG 尺寸、色度格式、解码返回值、缩放结果和显示结果。

## Blocked by

None - can start immediately and run in parallel with BLE work.
