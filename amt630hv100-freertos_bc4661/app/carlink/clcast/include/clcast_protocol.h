/**
 * clcast_protocol.h
 * CLCast 投屏协议定义
 *
 * 协议帧格式（小端字节序）:
 *   [4 bytes] command  : 命令码
 *   [4 bytes] length   : payload 字节数
 *   [length bytes] payload : KV 二进制负载
 *
 * KV 二进制格式（每条记录）:
 *   [1 byte]  key_len  : key 字节长度
 *   [key_len] key      : key 字符串（不含终止符）
 *   [2 bytes] val_len  : value 字节长度
 *   [val_len] value    : value 原始字节
 */

#ifndef CLCAST_PROTOCOL_H
#define CLCAST_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------
 * 协议帧头（8 字节）
 * ----------------------------------------------------------------------- */
#define CLCAST_HEADER_SIZE  8u

typedef struct {
    uint32_t command;   /* 命令码，小端 */
    uint32_t length;    /* payload 字节数，小端 */
} clcast_header_t;

/* -----------------------------------------------------------------------
 * 命令码定义
 * ----------------------------------------------------------------------- */
/* ---- 控制通道握手 ---- */
#define CMD_SESSION_SETUP             0x00000001u  /* source -> sink: 握手 */
#define CMD_SESSION_SETUP_ACK         0x00000002u  /* sink -> source: 握手应答并返回车机支持音视频能力*/
#define CMD_SESSION_TEARDOWN          0x00000003u  /* source -> sink / sink -> source: 关闭投屏 */

/* ---- 音视频流控制 ---- */
#define CMD_REQUEST_STREAM_START        0x00000030u  /* sink -> source(CMD_STREAM_SETUP) -> sink(CMD_STREAM_SETUP_ACK) -> source: 请求开始音视频流 */
#define CMD_REQUEST_STREAM_STOP         0x00000031u  /* sink -> source(CMD_SESSION_TEARDOWN) -> sink 请求停止音视频流 */

/* ---- 音视频流建立 ---- */
#define CMD_STREAM_SETUP               0x00000020u  /* source -> sink: 请求建立 AV 流 */
#define CMD_STREAM_SETUP_ACK           0x00000021u  /* sink -> source: 返回 AV 流端口 */
#define CMD_STREAM_TEARDOWN            0x00000022u  /* source -> sink */

/* ---- 音视频流数据 ---- */
#define CMD_STREAM_DATA         0x00000032u  /* source -> sink: 音视频数据帧 */

/* ---- 触摸 / 按键事件 ---- */
#define CMD_TOUCH_EVENT         0x00000040u  /* sink -> source: 触摸事件 */
#define CMD_KEY_EVENT           0x00000041u  /* sink -> source: 按键事件 */

/* ---- 心跳 ---- */
#define CMD_PING                0x0000FF00u
#define CMD_PONG                0x0000FF01u


/* -----------------------------------------------------------------------
 * 流类型定义  (CMD_SETUP key="stream_type", value=uint32_t)
 * ----------------------------------------------------------------------- */
typedef enum {
    STREAM_TYPE_UNKOWN = 0,
    STREAM_TYPE_AUDIO_MAIN,
    STREAM_TYPE_AUDIO_AUX,
    STREAM_TYPE_VIDEO_MAIN,
    STREAM_TYPE_VIDEO_MAP,
    STREAM_TYPE_END,
}CLCAST_STREAM_TYPE;

/* -----------------------------------------------------------------------
 * CMD_SETUP  payload keys
 * ----------------------------------------------------------------------- */
/* "stream_type"  uint8_t  : 流类型，见 STREAM_XXX */
/* "audio_codec"  string   : e.g. "AAC", "PCM" */
/* "video_codec"  string   : e.g. "H264", "H265" */
/* "width"        uint32_t : 视频宽度（仅视频流） */
/* "height"       uint32_t : 视频高度（仅视频流） */
/* "fps"          uint32_t : 帧率（仅视频流） */
/* "sample_rate"  uint32_t : 采样率（仅音频流） */
/* "channels"     uint8_t  : 声道数（仅音频流） */

/* -----------------------------------------------------------------------
 * CMD_SETUP_ACK  payload keys
 * ----------------------------------------------------------------------- */
/* "status"       uint32_t : CLCAST_STATUS_XXX */
/* "stream_type"  uint8_t  : 与请求对应的流类型 */
/* "port"         uint16_t : sink 端监听的 AV 流 TCP 端口 */

/* -----------------------------------------------------------------------
 * CMD_STREAM_DATA  payload keys
 * ----------------------------------------------------------------------- */
/* "stream_type"  uint8_t  : 流类型 */
/* "pts"          uint64_t : 显示时间戳（微秒） */
/* "data"         bytes    : 编码后的媒体数据 */

/* -----------------------------------------------------------------------
 * CMD_TOUCH_EVENT  payload keys
 * ----------------------------------------------------------------------- */
/* "action"       uint8_t  : 0=down, 1=move, 2=up */
/* "x"            uint32_t : x 坐标 */
/* "y"            uint32_t : y 坐标 */
/* "id"           uint8_t  : 触点 id（多点触控） */

/* -----------------------------------------------------------------------
 * CMD_KEY_EVENT  payload keys
 * ----------------------------------------------------------------------- */
/* "action"       uint8_t  : 0=down, 1=up */
/* "keycode"      uint32_t : 平台无关键码 */

/* -----------------------------------------------------------------------
 * 协议版本
 * ----------------------------------------------------------------------- */
#define CLCAST_KEY_VERSION_MAJOR    1u
#define CLCAST_KEY_VERSION_MINOR    0u

#define CLCAST_KEY_DEVICE_NAME         "device_name"
#define CLCAST_KEY_DEVICE_MACADDR      "device_mac_address"
#define CLCAST_KEY_STREAM_TYPE         "stream_type"
#define CLCAST_KEY_STREAM_PORT         "stream_port"
#define CLCAST_KEY_STREAM_FORMAT       "stream_format"
#define CLCAST_KEY_AUDIO_MAIN_FORMAT   "audio_main_format"
#define CLCAST_KEY_AUDIO_AUX_FORMAT    "audio_aux_format"
#define CLCAST_KEY_STREAM_FRAME_PTS    "frame_pts"
#define CLCAST_KEY_STREAM_FRAME_LEN    "frame_len"

#define STREAM_AUDIO_FORMAT_INVALID						0
//#define STREAM_AUDIO_FORMAT_PCM_8KHZ_16BIT_MONO			( 1 << 1 )
//#define STREAM_AUDIO_FORMAT_PCM_8KHZ_16BIT_STEREO		( 1 << 2 )
#define STREAM_AUDIO_FORMAT_PCM_16KHZ_16BIT_MONO		( 1 << 3 )
#define STREAM_AUDIO_FORMAT_PCM_16KHZ_16BIT_STEREO		( 1 << 4 )
//#define STREAM_AUDIO_FORMAT_PCM_32KHZ_16BIT_MONO		( 1 << 5 )
//#define STREAM_AUDIO_FORMAT_PCM_32KHZ_16BIT_STEREO		( 1 << 6 )
//#define STREAM_AUDIO_FORMAT_PCM_44KHZ_16BIT_MONO		( 1 << 7 )
//#define STREAM_AUDIO_FORMAT_PCM_44KHZ_16BIT_STEREO		( 1 << 8 )
#define STREAM_AUDIO_FORMAT_PCM_48KHZ_16BIT_MONO		( 1 << 9 )
#define STREAM_AUDIO_FORMAT_PCM_48KHZ_16BIT_STEREO		( 1 << 10 )

#define CLCAST_KEY_VIDEO_MAIN_FORMAT   "video_main_format"
#define CLCAST_KEY_VIDEO_MAP_FORMAT    "video_map_format"

#define STREAM_VIDEO_FORMAT_INVALID						0
#define STREAM_VIDEO_FORMAT_MJPEG           			( 1 << 1 )
#define STREAM_VIDEO_FORMAT_H264                		( 1 << 2 )
#define STREAM_VIDEO_FORMAT_H265                		( 1 << 3 )

#define STREAM_FORMAT_HAS_MASK(format, mask)    ((format & mask) == mask)

#define CLCAST_KEY_SCREEN_WIDTH        "screen_width"
#define CLCAST_KEY_SCREEN_HEIGHT       "screen_height"
#define CLCAST_KEY_SCREEN_DENSITY      "screen_density"
#define CLCAST_KEY_VIDEO_MAX_FPS       "max_fps"

/* ---- 触摸事件 key ---- */
#define CLCAST_KEY_TOUCH_ACTION        "action"
#define CLCAST_KEY_TOUCH_X             "x"
#define CLCAST_KEY_TOUCH_Y             "y"
#define CLCAST_KEY_TOUCH_ID            "id"

/* ---- 按键事件 key ---- */
#define CLCAST_KEY_KEY_ACTION          "action"
#define CLCAST_KEY_KEY_KEYCODE         "keycode"

#ifdef __cplusplus
}
#endif

#endif /* CLCAST_PROTOCOL_H */
