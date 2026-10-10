#ifndef IPV4_PARSE_H
#define IPV4_PARSE_H

#include <stdint.h>

/* ============================================================
 *  在一段文本里抓**最后一个** IPv4 字面量（借鉴 stm32mx407test_xinxin
 *  的 wifi_parse_ip，抽成纯函数：固件与主机侧单测共用同一份实现）。
 *
 *  为什么是"最后一个"：AP+STA 模式下应答里 APIP 在前、STAIP 在后，
 *  我们要的是 STAIP；为什么不认 "+CIFSR:STAIP" 固定格式：新旧 AT 固件
 *  应答格式不同（新版带前缀，旧版裸 IP），纯字面量扫描两者通吃。
 *
 *  规则：
 *    - 4 段、每段 1~3 位数字、'.' 分隔；
 *    - 段后紧跟数字 → 整段作废（"1.2.3.4001" 不是合法 IP）；
 *    - 数字/点的中间不作起点（"1234.1.1.1" 不许截出 "4.1.1.1"）。
 *
 *  返回：匹配长度（0=没找到）。out 非 NULL 且装得下（含 '\0'）时
 *        同时拷出并加 '\0'；装不下只返回长度、不动 out。
 * ============================================================ */
static inline uint16_t ipv4_find_last(const char *text, uint16_t len,
                                      char *out, uint16_t max_len)
{
    uint16_t found_off = 0, found_len = 0;
    uint16_t i = 0;

    while (i < len) {
        if (!(text[i] >= '0' && text[i] <= '9')) { i++; continue; }
        /* 前一字符还是数字或点：这里不是一个 IP 的合法起点 */
        if (i > 0 && (text[i-1] == '.' ||
                      (text[i-1] >= '0' && text[i-1] <= '9'))) { i++; continue; }

        uint16_t q = i;
        int seg, ok = 1;
        for (seg = 0; seg < 4 && ok; seg++) {
            int digits = 0;
            while (q < len && text[q] >= '0' && text[q] <= '9' && digits < 3) {
                q++; digits++;
            }
            if (digits == 0) { ok = 0; break; }
            if (seg < 3) {
                if (q >= len || text[q] != '.') { ok = 0; break; }
                q++;
            }
        }
        if (ok && (q >= len || text[q] < '0' || text[q] > '9')) {
            found_off = i;
            found_len = (uint16_t)(q - i);
            i = q;                      /* 继续往后扫，保持"最后一个" */
        } else {
            i++;
        }
    }

    if (found_len == 0)
        return 0;
    if (out != NULL && max_len > 0) {
        if (found_len + 1 <= max_len) {
            for (uint16_t k = 0; k < found_len; k++)
                out[k] = text[found_off + k];
            out[found_len] = '\0';
        } else {
            out[0] = '\0';              /* 装不下：不留半个 IP */
        }
    }
    return found_len;
}

#endif /* IPV4_PARSE_H */
