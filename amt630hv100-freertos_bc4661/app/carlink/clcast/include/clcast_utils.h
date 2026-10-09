/**
 * clcast_net.h
 * 网络收发辅助 API（跨平台，兼容 POSIX / Windows）
 */

#ifndef CLCAST_NET_H
#define CLCAST_NET_H

#include "clcast_protocol.h"
#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
   typedef SOCKET clcast_fd_t;
#  define CLCAST_INVALID_FD  INVALID_SOCKET
#else
#ifdef __linux__
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#else
//#  include <sys/socket.h>
#  include "lwip/sockets.h"
//#  include <arpa/inet.h>
#  include <unistd.h>
#endif
   typedef int clcast_fd_t;
#  define CLCAST_INVALID_FD  (-1)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------
 * 平台初始化 / 清理（仅 Windows 需要 WSAStartup）
 * ----------------------------------------------------------------------- */
int  clcast_net_init(void);
void clcast_net_cleanup(void);

/* -----------------------------------------------------------------------
 * 基础 socket 操作
 * ----------------------------------------------------------------------- */
/** 创建 TCP socket，设置 SO_REUSEADDR，非阻塞可选 */
clcast_fd_t clcast_tcp_socket(void);

/** 绑定并监听，port=0 由系统分配,返回值为端口号 */
int clcast_tcp_listen(clcast_fd_t fd, uint16_t port, int backlog);

/** 获取 socket 实际绑定端口（用于 port=0 时查询） */
uint16_t clcast_tcp_local_port(clcast_fd_t fd);

/** 阻塞连接到 host:port，host 为点分十进制或域名 */
int clcast_tcp_connect(clcast_fd_t fd, const char *host, uint16_t port);

/** 关闭并置为 INVALID */
void clcast_fd_close(clcast_fd_t *fd);

/** 设置 fd 为非阻塞模式，返回 0 成功，-1 失败 */
int clcast_set_nonblocking(clcast_fd_t fd);

int clcast_accept_client(clcast_fd_t fd, struct sockaddr * saddr, socklen_t saddrlen);

/* -----------------------------------------------------------------------
 * 可靠收发（处理 EINTR / 短读写）
 * ----------------------------------------------------------------------- */
/** 精确发送 len 字节，返回 0 成功，-1 失败 */
int clcast_send_all(clcast_fd_t fd, const void *buf, size_t len);

/** 精确接收 len 字节，返回 0 成功，-1 失败/连接关闭 */
int clcast_recv_all(clcast_fd_t fd, void *buf, size_t len);

/* -----------------------------------------------------------------------
 * 协议帧收发
 * ----------------------------------------------------------------------- */
/**
 * 发送协议帧
 * @param fd       目标 socket
 * @param command  命令码
 * @param payload  payload 缓冲（可为 NULL，此时 payload_len=0）
 * @param payload_len payload 字节数
 * @return 0 成功，-1 失败
 */
int clcast_send_frame(clcast_fd_t fd,
                      uint32_t command,
                      const void *payload, uint32_t payload_len);

/**
 * 接收一个协议帧（先读 8 字节头，再按 length 分配/读取 payload）
 * @param fd         来源 socket
 * @param cmd_out    接收命令码
 * @param payload_out 动态分配的 payload 缓冲（调用方 free()）
 * @param len_out    payload 字节数
 * @return 0 成功，-1 失败
 */
int clcast_recv_frame(clcast_fd_t fd,
                      uint32_t *cmd_out,
                      uint8_t **payload_out, uint32_t *len_out);

int	clcast_connect_loopback_socket();

int	clcast_send_loopback_msg( int inSock, const void *inMsg, size_t inLen );
/* -----------------------------------------------------------------------
 * 字节序辅助（小端 <-> 主机）
 * ----------------------------------------------------------------------- */
uint16_t clcast_le16(uint16_t v);
uint32_t clcast_le32(uint32_t v);
uint64_t clcast_le64(uint64_t v);

#ifdef __cplusplus
}
#endif

#endif /* CLCAST_NET_H */
