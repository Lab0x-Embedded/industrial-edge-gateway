/* mqtt_frame.h 的主机侧单测
 *
 * 目的：把"透传字节流里切出 MQTT 报文"这件事从 cloud.c 里抽出来，
 * 在 PC 上跑边界用例。这些用例对应真实踩过的坑，先写测试再改实现。
 *
 * 编译运行：
 *   cc -I ../User/net mqtt_frame_test.c -o /tmp/mqtt_frame_test && /tmp/mqtt_frame_test
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "mqtt_frame.h"

static int fails = 0;
static int total = 0;

static void ok(const char *name, int cond)
{
    total++;
    printf("  %-52s %s\n", name, cond ? "PASS" : "FAIL");
    if (!cond) fails++;
}

/* 构造一条 PUBLISH 帧到 out，返回总长度 */
static uint32_t build_publish(uint8_t *out, uint8_t qos,
                              const char *topic, uint16_t pid,
                              const char *payload)
{
    uint8_t *w = out;
    uint16_t tl = (uint16_t)strlen(topic);
    uint16_t pl = (uint16_t)strlen(payload);

    /* 可变头 + payload 长度 */
    uint32_t rem = 2 + tl + (qos ? 2 : 0) + pl;

    *w++ = (uint8_t)(0x30 | (qos << 1));
    /* 剩余长度（本测试只用到 <128 的情况） */
    do {
        uint8_t b = (uint8_t)(rem % 128);
        rem /= 128;
        if (rem) b |= 0x80;
        *w++ = b;
    } while (rem);

    *w++ = (uint8_t)(tl >> 8);
    *w++ = (uint8_t)(tl & 0xFF);
    memcpy(w, topic, tl); w += tl;
    if (qos) {
        *w++ = (uint8_t)(pid >> 8);
        *w++ = (uint8_t)(pid & 0xFF);
    }
    memcpy(w, payload, pl); w += pl;
    return (uint32_t)(w - out);
}

int main(void)
{
    uint8_t buf[512];
    MqttFrame f;

    /* ========== 1. 固定头 + 剩余长度解析 ========== */
    printf("\n[1] 固定头与剩余长度\n");
    {
        /* PUBACK：0x40 0x02 + 2 字节 pid = 4 字节 */
        uint8_t p[] = {0x40, 0x02, 0x00, 0x01};
        ok("PUBACK: 4 字节整帧", mqtt_parse_header(p, sizeof p, &f) == MQTT_OK);
        ok("PUBACK: type=4", f.type == MQTT_PKT_PUBACK);
        ok("PUBACK: total=4", f.total == 4);
    }
    {
        /* PINGRESP：0xD0 0x00 = 2 字节，剩余长度 0 */
        uint8_t p[] = {0xD0, 0x00};
        ok("PINGRESP: total=2", mqtt_parse_header(p, sizeof p, &f) == MQTT_OK
                                && f.total == 2 && f.type == MQTT_PKT_PINGRESP);
    }
    {
        /* SUBACK：0x90 0x03 + 2 字节 pid + 1 字节 return code = 5 字节
         * 这正是旧实现只丢 1 字节导致错位的典型报文 */
        uint8_t p[] = {0x90, 0x03, 0x00, 0x01, 0x00};
        ok("SUBACK: total=5（可整段跳过）",
           mqtt_parse_header(p, sizeof p, &f) == MQTT_OK
           && f.total == 5 && f.type == MQTT_PKT_SUBACK);
    }
    {
        /* 剩余长度多字节：0x80 0x01 = 128（变长编码） */
        uint8_t p[] = {0x30, 0x80, 0x01};   /* PUBLISH，rem=128，但 body 没给 */
        ok("剩余长度变长编码 128",
           mqtt_parse_header(p, sizeof p, &f) == MQTT_OK
           && f.remaining == 128 && f.total == 131);
    }
    {
        /* 剩余长度 4 字节全是续位 → 非法 */
        uint8_t p[] = {0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F};
        ok("剩余长度超 4 字节 → BAD",
           mqtt_parse_header(p, sizeof p, &f) == MQTT_BAD);
    }
    {
        /* 只收到 1 字节 → 数据不够 */
        uint8_t p[] = {0x30};
        ok("只 1 字节 → NEED_MORE",
           mqtt_parse_header(p, sizeof p, &f) == MQTT_NEED_MORE);
    }
    {
        /* 长度字段还没收全（0x80 表示还有后续，但没了） */
        uint8_t p[] = {0x30, 0x80};
        ok("长度字段没收全 → NEED_MORE",
           mqtt_parse_header(p, sizeof p, &f) == MQTT_NEED_MORE);
    }

    /* ========== 2. PUBLISH payload 提取 ========== */
    printf("\n[2] PUBLISH payload 提取\n");
    {
        uint32_t n = build_publish(buf, 1, "t/1", 0x0001, "{\"code\":0}");
        ok("QoS1 帧能解析", mqtt_parse_header(buf, n, &f) == MQTT_OK);
        char out[128];
        uint32_t plen = 0;
        int r = mqtt_publish_payload(buf, n, &f, out, sizeof out, &plen);
        ok("QoS1 payload 正确", r == 0 && strcmp(out, "{\"code\":0}") == 0);
    }
    {
        /* QoS0：没有 Packet ID。旧实现无脑跳 2 字节会把 payload 前 2 字节吃掉 */
        uint32_t n = build_publish(buf, 0, "t/1", 0, "AB");
        mqtt_parse_header(buf, n, &f);
        char out[128];
        uint32_t plen = 0;
        int r = mqtt_publish_payload(buf, n, &f, out, sizeof out, &plen);
        ok("QoS0 不跳 Packet ID（payload=AB）",
           r == 0 && strcmp(out, "AB") == 0);
    }
    {
        /* 非 PUBLISH */
        uint8_t p[] = {0x40, 0x02, 0x00, 0x01};
        mqtt_parse_header(p, sizeof p, &f);
        char out[16];
        ok("PUBACK 不是 PUBLISH → -1",
           mqtt_publish_payload(p, sizeof p, &f, out, sizeof out, NULL) == -1);
    }
    {
        /* 数据不完整 */
        uint32_t n = build_publish(buf, 1, "t/1", 1, "hello");
        mqtt_parse_header(buf, n, &f);
        char out[128];
        ok("帧没收全 → -3",
           mqtt_publish_payload(buf, n - 1, &f, out, sizeof out, NULL) == -3);
    }

    /* ========== 3. ★ 关键：混合字节流连续消费 ========== */
    printf("\n[3] 混合字节流连续消费（旧 bug 的核心场景）\n");
    {
        /* 构造流：PUBACK(4) + SUBACK(5) + PINGRESP(2) + PUBLISH(带code)
         * 旧实现：drain 时读到 PUBACK 头(0x40)，type!=3 就丢掉 1 字节
         * → 剩下的 02 00 01 留在流里 → 下一条从 02 开始，全错位。
         * 新实现：按 total 整段跳过。 */
        uint8_t stream[512];
        uint32_t n = 0;
        /* PUBACK */
        stream[n++] = 0x40; stream[n++] = 0x02; stream[n++] = 0x00; stream[n++] = 0x01;
        /* SUBACK */
        stream[n++] = 0x90; stream[n++] = 0x03; stream[n++] = 0x00;
        stream[n++] = 0x01; stream[n++] = 0x00;
        /* PINGRESP */
        stream[n++] = 0xD0; stream[n++] = 0x00;
        /* PUBLISH with code */
        n += build_publish(stream + n, 1, "t/1", 1, "{\"code\":10411}");

        /* 逐帧消费，验证每帧都对齐 */
        uint32_t off = 0;
        int frames = 0, found_publish = 0, aligned = 1;
        while (off < n) {
            MqttFrame fr;
            if (mqtt_parse_header(stream + off, n - off, &fr) != MQTT_OK) {
                aligned = 0; break;
            }
            if (fr.total == 0 || off + fr.total > n) { aligned = 0; break; }
            if (fr.type == MQTT_PKT_PUBLISH) {
                char out[128];
                if (mqtt_publish_payload(stream + off, n - off, &fr,
                                         out, sizeof out, NULL) == 0
                    && strstr(out, "10411")) {
                    found_publish = 1;
                }
            }
            off += fr.total;      /* ← 整段跳过，不丢字节 */
            frames++;
        }
        ok("4 条报文全部对齐消费", aligned && frames == 4 && off == n);
        ok("最后那条 PUBLISH 的 code 能取到", found_publish);
    }
    {
        /* 只收到 1 字节：header 都解析不了（模拟 esp_recv_raw 超时）
         * 注意：parse_header 只负责"头+长度"，2 字节就能成功，
         * 所以"半条帧"要在 payload 提取阶段才体现为不完整。 */
        uint8_t stream[512];
        build_publish(stream, 1, "t/1", 1, "{\"code\":0}");
        MqttFrame fr;
        ok("只收到 1 字节 → NEED_MORE",
           mqtt_parse_header(stream, 1, &fr) == MQTT_NEED_MORE);
    }
    {
        /* 头完整但 body 不全：parse_header 成功、payload 提取应报 -3 */
        uint8_t stream[512];
        uint32_t n = build_publish(stream, 1, "t/1", 1, "{\"code\":0}");
        MqttFrame fr;
        char out[128];
        int hdr_ok = (mqtt_parse_header(stream, n, &fr) == MQTT_OK);
        int body_bad = (mqtt_publish_payload(stream, n - 3, &fr,
                                             out, sizeof out, NULL) == -3);
        ok("头完整但 body 缺 3 字节 → payload 返回 -3", hdr_ok && body_bad);
    }

    /* ========== 4. SUBACK 校验（连上即置在线但没有等 SUBACK） ========== */
    printf("\n[4] SUBACK / CONNACK 判定\n");
    {
        uint8_t suback[] = {0x90, 0x03, 0x00, 0x01, 0x00};
        MqttFrame fr;
        mqtt_parse_header(suback, sizeof suback, &fr);
        ok("SUBACK type=9 可识别", fr.type == MQTT_PKT_SUBACK);
        /* SUBACK 最后 1 字节是 return code：0x00=granted QoS0,
         * 0x01=granted QoS1, 0x80=failure */
        uint32_t rc_off = fr.hdr_len + 2 + 0;  /* hdr + pid(2) + rc */
        ok("SUBACK return code 位置正确", rc_off == 4);
        ok("SUBACK 失败码 0x80 能识别",
           (suback[4] = 0x80, suback[4] == 0x80));
    }
    {
        /* CONNACK：0x20 0x02 flags rc —— rc!=0 必须判失败 */
        uint8_t ok_ack[]  = {0x20, 0x02, 0x00, 0x00};
        uint8_t bad_ack[] = {0x20, 0x02, 0x00, 0x05};   /* 5=未授权 */
        MqttFrame f1, f2;
        mqtt_parse_header(ok_ack, sizeof ok_ack, &f1);
        mqtt_parse_header(bad_ack, sizeof bad_ack, &f2);
        ok("CONNACK total=4", f1.total == 4 && f2.total == 4);
        ok("CONNACK rc=0 接受", ok_ack[3] == 0x00);
        ok("CONNACK rc=5 拒绝", bad_ack[3] != 0x00);
    }

    /* ========== 5. 心跳：PUBLISH 先到不能误判 PINGRESP ========== */
    printf("\n[5] 心跳帧循环（PUBLISH 先到的场景）\n");
    {
        /* 模拟缓冲里先来一条 QoS1 PUBLISH，然后才是 PINGRESP。
         * 旧实现"裸读 2 字节"会读到 0x32 0x?? → 判失败 → 假离线。
         * 新实现按帧循环：跳过 PUBLISH，继续等 PINGRESP。 */
        uint8_t stream[512];
        uint32_t n = 0;
        n += build_publish(stream + n, 1, "t/set", 0x0001, "{\"power\":1}");
        stream[n++] = 0xD0; stream[n++] = 0x00;    /* PINGRESP */

        uint32_t off = 0;
        int got_pong = 0, loops = 0;
        while (off < n && loops < 4) {
            MqttFrame fr;
            if (mqtt_parse_header(stream + off, n - off, &fr) != MQTT_OK) break;
            loops++;
            if (fr.type == MQTT_PKT_PINGRESP) { got_pong = 1; }
            off += fr.total;
        }
        ok("跳过 PUBLISH 后能找到 PINGRESP", got_pong && loops == 2);
    }
    {
        /* 只有 PUBLISH、没有 PINGRESP：应判"链路活但没等到 pong" */
        uint8_t stream[512];
        uint32_t n = build_publish(stream, 1, "t/set", 1, "{\"x\":1}");
        MqttFrame fr;
        int alive = 0, pong = 0;
        if (mqtt_parse_header(stream, n, &fr) == MQTT_OK) {
            alive = 1;
            if (fr.type == MQTT_PKT_PINGRESP) pong = 1;
        }
        ok("只有 PUBLISH → 链路活、无 pong", alive && !pong);
    }

    printf("\n%s  (%d/%d 通过)\n",
           fails ? "==> 有失败" : "==> 全部通过", total - fails, total);
    return fails ? 1 : 0;
}
