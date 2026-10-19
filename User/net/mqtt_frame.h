#ifndef MQTT_FRAME_H
#define MQTT_FRAME_H

#include <stdint.h>
#include <string.h>

/* ============================================================
 *  MQTT 报文帧层的纯函数（固件与主机侧单测共用同一份实现）
 *
 *  为什么单独抽出来：透传模式下收到的是**裸字节流**，所有报文连在一起，
 *  没有长度前缀、没有分隔符。要在流里切出报文，必须：
 *    ① 读固定头第 1 字节（报文类型 + flags）
 *    ② 解析"剩余长度"（变长编码，1~4 字节）
 *    ③ 按剩余长度把 body 整段消费掉
 *
 *  **核心不变量：任何一条报文，要么完整消费，要么一个字节都不碰。**
 *  只消费一部分（比如"发现不是 PUBLISH 就丢掉头字节"）会让后续所有报文
 *  从错误位置开始解析——而且**不会报错**，表现为"跑一阵就收到乱码"。
 *
 *  这些函数不碰硬件、不持锁、不做 IO，纯计算，可以在 PC 上跑边界用例。
 * ============================================================ */

/* ---- MQTT 控制报文类型（固定头高 4 位） ---- */
#define MQTT_PKT_CONNECT      1
#define MQTT_PKT_CONNACK      2
#define MQTT_PKT_PUBLISH      3
#define MQTT_PKT_PUBACK       4
#define MQTT_PKT_SUBSCRIBE    8
#define MQTT_PKT_SUBACK       9
#define MQTT_PKT_PINGREQ      12
#define MQTT_PKT_PINGRESP     13
#define MQTT_PKT_DISCONNECT   14

/* 解析结果 */
typedef enum {
    MQTT_OK = 0,           /* 完整解析出一条报文 */
    MQTT_NEED_MORE,        /* 数据不够，还没收全 */
    MQTT_BAD               /* 格式非法（剩余长度超 4 字节 / 非法编码） */
} MqttParseResult;

/* 报文描述：只描述"这条报文占据多少字节"，不含 payload 语义 */
typedef struct {
    uint8_t  type;         /* 控制报文类型（1~14） */
    uint8_t  flags;        /* 固定头低 4 位（PUBLISH 里是 QoS/retain） */
    uint32_t remaining;    /* 剩余长度（可变头 + payload） */
    uint32_t total;        /* 固定头 + 剩余长度字段 + remaining = 整条报文长度 */
    uint8_t  hdr_len;      /* 固定头字节数（1 + 长度字段字节数） */
} MqttFrame;

/* 读固定头 + 剩余长度，算出整条报文总长度。
 *
 * 返回：
 *   MQTT_OK        解析成功，*out 有效
 *   MQTT_NEED_MORE 数据不够（可能剩余长度字段还没收全）
 *   MQTT_BAD       剩余长度编码非法（超过 4 字节仍未结束）
 *
 * 注意：本函数**只读 len 范围内的字节**，不假设 buf 后面还有空间。 */
static inline MqttParseResult mqtt_parse_header(const uint8_t *buf, uint32_t len,
                                                MqttFrame *out)
{
    if (buf == NULL || out == NULL) return MQTT_BAD;
    if (len < 2) return MQTT_NEED_MORE;

    uint8_t b0 = buf[0];
    out->type  = (uint8_t)(b0 >> 4);
    out->flags = (uint8_t)(b0 & 0x0F);

    /* 剩余长度：每字节低 7 位有效，最高位是"还有后续" */
    uint32_t rem = 0, mult = 1;
    uint32_t i;
    int done = 0;
    for (i = 1; i < len && i <= 4; i++) {
        uint8_t b = buf[i];
        rem += (uint32_t)(b & 0x7F) * mult;
        mult *= 128;
        if ((b & 0x80) == 0) { done = 1; break; }
    }

    if (!done) {
        /* 已经读满 4 个字节还没结束 → 非法；否则只是还没收全 */
        return (len >= 5) ? MQTT_BAD : MQTT_NEED_MORE;
    }

    out->remaining = rem;
    out->hdr_len   = (uint8_t)(i + 1);          /* 1 字节类型 + i 字节长度 */
    out->total     = out->hdr_len + rem;
    return MQTT_OK;
}

/* 判断一条已解析的帧"是否可以整条跳过"（不关心内容时用）。
 * 供上层：遇到不认识/不关心的报文，按 frame->total 整段丢弃，保持流对齐。 */
static inline int mqtt_frame_total(const MqttFrame *f, uint32_t *total)
{
    if (f == NULL || total == NULL) return 0;
    *total = f->total;
    return 1;
}

/* 从一条完整的 PUBLISH 帧里取出 payload（QoS1 会跳过 Packet ID）。
 *
 * 入参 buf 指向报文起始，len 应为 frame->total（至少 >= hdr_len + remaining）。
 * 出参 out/out_max：payload 拷贝到 out（加 '\0'），返回 payload 实际长度。
 *
 * 返回：
 *   0  成功
 *  -1  不是 PUBLISH
 *  -2  body 不完整（可变头越界）
 *  -3  数据还没收全
 *
 * 说明：**QoS 0 没有 Packet ID**，这里按 flags 里的 QoS 位区分——
 * 老实现无脑跳 2 字节，遇到 QoS0 的下行会把 payload 前 2 字节吃掉。 */
static inline int mqtt_publish_payload(const uint8_t *buf, uint32_t len,
                                       const MqttFrame *f,
                                       char *out, uint32_t out_max,
                                       uint32_t *payload_len)
{
    if (buf == NULL || f == NULL || out == NULL || out_max == 0) return -1;
    if (f->type != MQTT_PKT_PUBLISH) return -1;
    if (len < f->total) return -3;
    if (f->total < f->hdr_len) return -2;

    uint32_t body = f->hdr_len;
    uint32_t end  = f->total;

    /* 可变头：topic 长度(2) + topic */
    if (body + 2 > end) return -2;
    uint32_t tlen = ((uint32_t)buf[body] << 8) | buf[body + 1];
    body += 2;
    if (body + tlen > end) return -2;
    body += tlen;

    /* QoS > 0 才有 Packet ID */
    uint8_t qos = (uint8_t)((f->flags >> 1) & 0x03);
    if (qos > 0) {
        if (body + 2 > end) return -2;
        body += 2;
    }

    uint32_t plen = end - body;
    if (plen > out_max - 1) plen = out_max - 1;
    memcpy(out, buf + body, plen);
    out[plen] = '\0';
    if (payload_len) *payload_len = plen;
    return 0;
}

#endif /* MQTT_FRAME_H */
