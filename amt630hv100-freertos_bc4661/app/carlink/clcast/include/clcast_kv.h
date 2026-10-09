/**
 * clcast_kv.h
 * KV 二进制编解码 API
 *
 * KV 单条记录格式:
 *   [1 byte]  key_len
 *   [key_len] key  (无终止符)
 *   [2 bytes] val_len (小端)
 *   [val_len] value
 */

#ifndef CLCAST_KV_H
#define CLCAST_KV_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------
 * KV 编码器（写缓冲区）
 * ----------------------------------------------------------------------- */
typedef struct clcast_kv_encoder_t{
    uint8_t  *buf;      /* 外部提供的缓冲区 */
    size_t    cap;      /* 缓冲区容量 */
    size_t    pos;      /* 当前写入位置 */
} clcast_kv_encoder_t;

/**
 * 初始化编码器
 * @param enc  编码器
 * @param buf  外部缓冲区
 * @param cap  缓冲区大小
 */
void clcast_kv_encoder_init(clcast_kv_encoder_t *enc, uint8_t *buf, size_t cap);

/** 返回已编码的字节数 */
size_t clcast_kv_encoder_size(const clcast_kv_encoder_t *enc);

/**
 * 写入任意 value
 * @return 0 成功, -1 空间不足
 */
int clcast_kv_put(clcast_kv_encoder_t *enc,
                  const char *key,
                  const void *value, uint16_t val_len);

/* 便捷写入函数 */
int clcast_kv_put_u8 (clcast_kv_encoder_t *enc, const char *key, uint8_t  v);
int clcast_kv_put_u16(clcast_kv_encoder_t *enc, const char *key, uint16_t v);
int clcast_kv_put_u32(clcast_kv_encoder_t *enc, const char *key, uint32_t v);
int clcast_kv_put_u64(clcast_kv_encoder_t *enc, const char *key, uint64_t v);
int clcast_kv_put_str(clcast_kv_encoder_t *enc, const char *key, const char *str);

/* -----------------------------------------------------------------------
 * KV 解码器（读缓冲区）
 * ----------------------------------------------------------------------- */
typedef struct {
    const uint8_t *buf;
    size_t         len;
    size_t         pos;
} clcast_kv_decoder_t;

void clcast_kv_decoder_init(clcast_kv_decoder_t *dec,
                             const uint8_t *buf, size_t len);

/**
 * 遍历下一个 KV 对
 * @param key_out   key 指针（指向 buf 内部，非零终止）
 * @param key_len   key 长度
 * @param val_out   value 指针
 * @param val_len   value 长度
 * @return 1 有数据, 0 遍历完毕, -1 格式错误
 */
int clcast_kv_next(clcast_kv_decoder_t *dec,
                   const char **key_out, uint8_t  *key_len,
                   const uint8_t **val_out, uint16_t *val_len);

/**
 * 按 key 查找 value（线性扫描，重置 pos 后搜索）
 * @return 1 找到, 0 未找到, -1 格式错误
 */
int clcast_kv_find(const uint8_t *buf, size_t len,
                   const char *key,
                   const uint8_t **val_out, uint16_t *val_len_out);

/* 便捷读取（找到且长度匹配则写入 out，返回 0；否则返回 -1） */
int clcast_kv_get_u8 (const uint8_t *buf, size_t len, const char *key, uint8_t  *out);
int clcast_kv_get_u16(const uint8_t *buf, size_t len, const char *key, uint16_t *out);
int clcast_kv_get_u32(const uint8_t *buf, size_t len, const char *key, uint32_t *out);
int clcast_kv_get_u64(const uint8_t *buf, size_t len, const char *key, uint64_t *out);
/**
 * 读取字符串，写入 out_buf（含终止符），out_buf_size 含终止符空间
 * @return 0 成功，-1 失败
 */
int clcast_kv_get_str(const uint8_t *buf, size_t len, const char *key,
                      char *out_buf, size_t out_buf_size);

#ifdef __cplusplus
}
#endif

#endif /* CLCAST_KV_H */
