/* 时间服务实现：单调 tick 走时 + RTC/编译时间定初值 */
#include "clock.h"
#include "rtc.h"
#include "stm32f4xx.h"

/* 备份寄存器标记（"CLKP"）：写在 RTC 备份域里。
 * 有 VBAT 电池的板子断电后备份域不丢 → 下次上电看到这个标记，
 * 说明 RTC 的时间是"断电期间也在走"的，可以拿来当初值；
 * 没电池的板子断电即清零 → 走下面的编译时间分支。 */
#define CLOCK_BKP_MAGIC  0x504C4B43UL

static uint32_t t0_tick;   /* 校时/播种那一刻的 HAL_GetTick() */
static uint32_t t0_sec;    /* 那一刻的当日秒数 */
static uint8_t  synced;    /* 1 = 已经由 SNTP/手动校过时 */

static uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v >> 4) * 10U + (v & 0x0FU)); }
static uint8_t bin2bcd(uint8_t v) { return (uint8_t)(((v / 10U) << 4) | (v % 10U)); }

static void clock_seed(uint8_t h, uint8_t m, uint8_t s, uint8_t is_synced)
{
    t0_sec  = (uint32_t)h * 3600U + (uint32_t)m * 60U + (uint32_t)s;
    t0_tick = HAL_GetTick();
    synced  = is_synced;
}

void Clock_Init(void)
{
    if (RTC->BKP0R == CLOCK_BKP_MAGIC) {
        RTC_TimeTypeDef t = {0};
        RTC_DateTypeDef d = {0};
        HAL_RTC_GetTime(&hrtc, &t, RTC_FORMAT_BCD);
        /* 影子寄存器坑：读完 Time 必须再读一次 Date，否则 RTC 的秒不会更新，
         * 读出来永远是同一个值（见参考手册 RTC 影子寄存器章节）。 */
        HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BCD);
        clock_seed(bcd2bin(t.Hours), bcd2bin(t.Minutes), bcd2bin(t.Seconds), 1);
        return;
    }

    /* 无备份域：用编译时间播种（"__TIME__" = "HH:MM:SS"）。
     * 不等于真实时间，但比 00:00:00 像样，且未校时状态在界面里是暗色的。 */
    const char *bt = __TIME__;
    uint8_t h = 0, m = 0, s = 0;
    if (bt[0] >= '0' && bt[0] <= '9') {
        h = (uint8_t)((bt[0] - '0') * 10 + (bt[1] - '0'));
        m = (uint8_t)((bt[3] - '0') * 10 + (bt[4] - '0'));
        s = (uint8_t)((bt[6] - '0') * 10 + (bt[7] - '0'));
    }
    clock_seed(h, m, s, 0);
}

void Clock_SetTime(uint8_t h, uint8_t m, uint8_t s)
{
    clock_seed(h, m, s, 1);

    RTC_TimeTypeDef t;
    t.Hours          = bin2bcd(h);
    t.Minutes        = bin2bcd(m);
    t.Seconds        = bin2bcd(s);
    t.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    t.StoreOperation = RTC_STOREOPERATION_RESET;
    if (HAL_RTC_SetTime(&hrtc, &t, RTC_FORMAT_BCD) == HAL_OK)
    {
        /* 坑：HAL_RTC_SetTime 返回时会把备份域写保护重新打开（清 PWR_CR_DBP），
         * 此时直接写 BKP0R 是**无效**的（写被忽略，不报错）。必须显式放开。 */
        HAL_PWR_EnableBkUpAccess();
        RTC->BKP0R = CLOCK_BKP_MAGIC;      /* 让下次上电能直接信任 RTC */
        HAL_PWR_DisableBkUpAccess();
    }
}

void Clock_GetTime(uint8_t *h, uint8_t *m, uint8_t *s)
{
    /* 无符号差值天然处理 HAL_GetTick() 49.7 天回绕 */
    uint32_t elapsed = (HAL_GetTick() - t0_tick) / 1000U;
    uint32_t sod     = (t0_sec + elapsed) % 86400U;
    if (h) *h = (uint8_t)(sod / 3600U);
    if (m) *m = (uint8_t)((sod % 3600U) / 60U);
    if (s) *s = (uint8_t)(sod % 60U);
}

uint8_t Clock_IsSynced(void)
{
    return synced;
}
