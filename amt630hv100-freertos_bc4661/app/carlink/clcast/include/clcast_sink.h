#ifndef CLCAST_SINK_H
#define CLCAST_SINK_H

#include "clcast_protocol.h"
#include "clcast_utils.h"
#include "clcast_kv.h"

typedef struct clcast_sink_stream{
    clcast_fd_t server_fd;
    clcast_fd_t client_fd;
    clcast_fd_t comand_fd;
    CLCAST_STREAM_TYPE    stream_type;
    uint32_t    stream_format;
    pthread_t * recv_thread_ptr;
    pthread_t   recv_thread;
    void      * session;
}clcast_sink_stream;

typedef struct clcast_sink_session{
    char         device_name[64];
    char         mac_address[32];
    clcast_fd_t  session_fd;
    clcast_sink_stream * streams[STREAM_TYPE_END];
    uint8_t      props[2048];
    uint16_t     props_datalen;
    uint64_t     last_ping_time;  /* 最后收到心跳的时间戳（毫秒） */
}clcast_sink_session;

typedef void (* clcast_sink_session_create)(clcast_sink_session * session);
typedef void (* clcast_sink_session_connected)(clcast_sink_session * session);
typedef void (* clcast_sink_session_disconnected)(clcast_sink_session * session);
typedef void (* clcast_sink_session_destroy)(clcast_sink_session * session);
typedef void (* clcast_sink_session_getprop)(clcast_sink_session * session, const char * key, clcast_kv_encoder_t * kv_encoder);

typedef void (* clcast_sink_session_audio_start)(clcast_sink_session * session, CLCAST_STREAM_TYPE streamType, int sampleRate, int format, int channel);
typedef void (* clcast_sink_session_audio_frame)(clcast_sink_session * session, CLCAST_STREAM_TYPE streamType, const uint8_t * frame, uint32_t len, uint64_t pts);
typedef void (* clcast_sink_session_audio_stop)(clcast_sink_session * session, CLCAST_STREAM_TYPE streamType);
typedef void (* clcast_sink_session_video_start)(clcast_sink_session * session, CLCAST_STREAM_TYPE streamType, uint32_t format);
typedef void (* clcast_sink_session_video_frame)(clcast_sink_session * session, CLCAST_STREAM_TYPE streamType, const uint8_t * frame, uint32_t len, uint64_t pts);
typedef void (* clcast_sink_session_video_stop)(clcast_sink_session * session, CLCAST_STREAM_TYPE streamType);

typedef struct _clcast_sink_session_delegate{
    clcast_sink_session_create create;
    clcast_sink_session_connected connected;
    clcast_sink_session_disconnected disconnected;
    clcast_sink_session_destroy destory;
    clcast_sink_session_getprop getprop;

    clcast_sink_session_audio_start audio_start;
    clcast_sink_session_audio_frame audio_frame;
    clcast_sink_session_audio_stop  audio_stop;

    clcast_sink_session_video_start video_start;
    clcast_sink_session_video_frame video_frame;
    clcast_sink_session_video_stop  video_stop;
}clcast_sink_session_delegate;

typedef struct clcast_sink_server{
    clcast_fd_t  server_fd;
    clcast_fd_t  comand_fd;
    uint8_t      server_is_running;
    pthread_t * runloop_thread_ptr;
    pthread_t   runloop_thread;
    clcast_sink_session * session;
    clcast_sink_session_delegate delegate;
}clcast_sink_server;

clcast_sink_server * clcast_sink_start(int port, clcast_sink_session_delegate delegate);

int clcast_sink_runloop(clcast_sink_server * server);

int clcast_sink_stop(clcast_sink_server * server);

int clcast_sink_request_teardown_session(clcast_sink_server * server);
/* -----------------------------------------------------------------------
 * 触摸/按键事件 API
 * ----------------------------------------------------------------------- */

/**
 * 发送触摸事件到 source 端
 * @param server   sink server 句柄
 * @param action   动作：0=down, 1=move, 2=up
 * @param x        x 坐标
 * @param y        y 坐标
 * @param id       触点 id（多点触控）
 * @return 0 成功，-1 失败
 */
int clcast_sink_send_touch_event(clcast_sink_server * server, uint8_t action, uint32_t x, uint32_t y, uint8_t id);

/**
 * 发送按键事件到 source 端
 * @param server   sink server 句柄
 * @param action   动作：0=down, 1=up
 * @param keycode  平台无关键码
 * @return 0 成功，-1 失败
 */
int clcast_sink_send_key_event(clcast_sink_server * server, uint8_t action, uint32_t keycode);

#endif // CLCAST_SINK_H
