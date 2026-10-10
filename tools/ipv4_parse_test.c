/* ipv4_parse.h 的主机侧单测（手册 #10 的方法：逻辑放主机上跑边界用例）。
 * 编译运行：cc -I ../User/net ipv4_parse_test.c -o /tmp/ipv4_parse_test && /tmp/ipv4_parse_test */
#include <stdio.h>
#include <string.h>
#include "ipv4_parse.h"

static int fails = 0;

static void check(const char *name, const char *text, const char *expect)
{
    char out[16] = {0};
    uint16_t r = ipv4_find_last(text, (uint16_t)strlen(text), out, sizeof out);
    int ok = (expect == NULL) ? (r == 0) : (r > 0 && strcmp(out, expect) == 0);
    printf("%-24s got=%-16s want=%-16s %s\n", name,
           r ? out : "(none)", expect ? expect : "(none)", ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}

int main(void)
{
    /* 1. 标准新固件应答：STAIP 行 + STAMAC 行（MAC 的冒号分组不能误匹配） */
    check("STAIP+STAMAC 标准应答",
          "AT+CIFSR\r\n+CIFSR:STAIP,\"192.168.4.2\"\r\n"
          "+CIFSR:STAMAC,\"24:0a:c4:11:22:33\"\r\n\r\nOK\r\n", "192.168.4.2");
    /* 2. AP+STA 双 IP：APIP 在前 STAIP 在后，取最后一个 */
    check("AP+STA 取最后一个",
          "+CIFSR:APIP,\"192.168.4.1\"\r\n+CIFSR:STAIP,\"192.168.1.100\"\r\nOK",
          "192.168.1.100");
    /* 3. 旧固件裸 IP（没有 +CIFSR: 前缀） */
    check("旧固件裸 IP", "192.168.1.100\r\nOK", "192.168.1.100");
    /* 4. 小写新格式（ESP-AT v2） */
    check("小写新格式", "+CIFSR:staip,\"10.0.0.5\"\r\nOK", "10.0.0.5");
    /* 5. 没有任何 IP */
    check("无 IP 报错", "\r\nERROR\r\n", NULL);
    check("纯文字", "no ip here", NULL);
    /* 6. 长数字串不许从中间截出假 IP（1234.1.1.1 → "4.1.1.1" 是错的） */
    check("长数字串不误切", "1234.1.1.1", NULL);
    /* 7. 段超 3 位整段作废 */
    check("段超3位作废", "1.2.3.4001", NULL);
    /* 8. 3 位段正常 */
    check("3 位段", "172.16.100.200", "172.16.100.200");
    /* 9. 紧跟数字的尾巴不误收（40 后面还有 1） */
    check("尾段截断作废", "ip:10.1.2.40 1 extra", "10.1.2.40");
    /* 10. 乱码混杂（模拟 boot banner 污染的缓冲），目标 IP 在最后 */
    check("乱码中取 IP",
          "\xaf\x13 wdt reset\r\nload 0x401000\r\nSTAIP,\"192.168.4.66\"\r\nOK",
          "192.168.4.66");
    /* 11. 行为契约：应答里若 netmask 排在后面，"取最后一个"取到的是 netmask
     *     （NonOS AT 的 CIFSR 只回 STAIP/STAMAC，不会出现这种排列；这里把
     *      解析器的定义固定下来，防将来无意识改语义） */
    check("netmask 在后取最后",
          "+CFG:192.168.4.66,255.255.255.0\r\nOK", "255.255.255.0");
    /* 12. g_esp_ip 恰好 16 字节：15 字符 IP + '\0' 必须装得下 */
    {
        char out[16] = {0};
        const char *t = "255.255.255.255";
        uint16_t r = ipv4_find_last(t, (uint16_t)strlen(t), out, sizeof out);
        int ok = (r == 15 && strcmp(out, "255.255.255.255") == 0);
        printf("%-24s got=%-16s want=%-16s %s\n", "最长 IP 装满 16B",
               out, "255.255.255.255", ok ? "PASS" : "FAIL");
        if (!ok) fails++;
    }

    if (fails) { printf("== %d FAIL ==\n", fails); return 1; }
    printf("== ALL PASS ==\n");
    return 0;
}
