/* 时间服务实现：单调 tick 走时 + RTC/编译时间定初值 */
#include "clock.h"      /* CLOCK_BKP_MAGIC 也供 Core/Src/rtc.c 的守卫共用 */
#include "rtc.h"
#include "stm32f4xx.h"

static uint32_t t0_tick;   /* 校时/播种那一刻的 HAL_GetTick() */
static uint32_t t0_sec;    /* 那一刻的当日秒数 */
static uint32_t t0_days;   /* 那一刻距 1970-01-01 的天数（日期随 tick 跨天进位用） */
static uint8_t  synced;    /* 1 = 已经由 SNTP/手动校过时 */

static uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v >> 4) * 10U + (v & 0x0FU)); }
static uint8_t bin2bcd(uint8_t v) { return (uint8_t)(((v / 10U) << 4) | (v % 10U)); }

/* 天数(自1970-01-01) ↔ 民用日期：Howard Hinnant 的算法，
 * 不用大小月/闰年表。日期跨天进位（GetDateTime）和 RTC 星期都要靠它。
 * 本项目年份恒在 2000~2099（RTC 寄存器只有两位年），不用管负 era。 */
static uint32_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int era = y / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (uint32_t)era * 146097U + (uint32_t)doe - 719468U;
}

static void civil_from_days(uint32_t z, int *y, int *m, int *d)
{
    z += 719468U;
    int era = (int)(z / 146097U);
    int doe = (int)(z - (uint32_t)era * 146097U);
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int yy  = yoe + era * 400;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp  = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3 : -9);
    *y = yy + (*m <= 2);
}

/* RTC 的 WeekDay 是 1=周一..7=周日；1970-01-01 是周四（=4） */
static uint8_t weekday_from_days(uint32_t z)
{
    return (uint8_t)((z + 3U) % 7U + 1U);
}

static void clock_seed(uint16_t y, uint8_t mo, uint8_t d,
                       uint8_t h, uint8_t m, uint8_t s, uint8_t is_synced)
{
    t0_sec  = (uint32_t)h * 3600U + (uint32_t)m * 60U + (uint32_t)s;
    t0_days = days_from_civil((int)y, (int)mo, (int)d);
    t0_tick = HAL_GetTick();
    synced  = is_synced;
}

void Clock_Init(void)
{
    if (RTC->BKP0R == CLOCK_BKP_MAGIC) {
        RTC_TimeTypeDef t = {0};
        RTC_DateTypeDef dt = {0};
        HAL_RTC_GetTime(&hrtc, &t, RTC_FORMAT_BCD);
        /* 影子寄存器坑：读完 Time 必须再读一次 Date，否则 RTC 的秒不会更新，
         * 读出来永远是同一个值（见参考手册 RTC 影子寄存器章节）。 */
        HAL_RTC_GetDate(&hrtc, &dt, RTC_FORMAT_BCD);
        clock_seed((uint16_t)(2000U + bcd2bin(dt.Year)), bcd2bin(dt.Month),
                   bcd2bin(dt.Date),
                   bcd2bin(t.Hours), bcd2bin(t.Minutes), bcd2bin(t.Seconds), 1);
        return;
    }

    /* 无备份域：用编译时间播种（"__TIME__" = "HH:MM:SS"）。
     * 不等于真实时间，但比 00:00:00 像样，且未校时状态在界面里是暗色的；
     * 日期没有可信来源，置 2000-01-01 占位——界面未校时时显示 "--" 不会露出它。 */
    const char *bt = __TIME__;
    uint8_t h = 0, m = 0, s = 0;
    if (bt[0] >= '0' && bt[0] <= '9') {
        h = (uint8_t)((bt[0] - '0') * 10 + (bt[1] - '0'));
        m = (uint8_t)((bt[3] - '0') * 10 + (bt[4] - '0'));
        s = (uint8_t)((bt[6] - '0') * 10 + (bt[7] - '0'));
    }
    clock_seed(2000, 1, 1, h, m, s, 0);
}

void Clock_SetDateTime(uint16_t y, uint8_t mo, uint8_t d,
                       uint8_t h, uint8_t m, uint8_t s)
{
    clock_seed(y, mo, d, h, m, s, 1);

    /* ★ 备份域写保护（PWR_CR_DBP）必须先自己放开：RTC 的时间/日期寄存器和
     *   BKP0R 全在备份域里，DBP=0 时所有写入都被**静默丢弃**，而 HAL 不会报
     *   错——SetTime 照样返回 HAL_OK（假成功）。HAL 的 SetTime/SetDate 只开关
     *   RTC 自己的 WPR、从不碰 DBP，工程里（MX_RTC_Init）也没有任何地方开它
     *   —— 2000-01-01 顽固复活的总根源就在这。 */
    HAL_PWR_EnableBkUpAccess();

    RTC_TimeTypeDef t = {0};
    t.Hours          = bin2bcd(h);
    t.Minutes        = bin2bcd(m);
    t.Seconds        = bin2bcd(s);
    t.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    t.StoreOperation = RTC_STOREOPERATION_RESET;
    if (HAL_RTC_SetTime(&hrtc, &t, RTC_FORMAT_BCD) != HAL_OK)
    {
        HAL_PWR_DisableBkUpAccess();
        return;
    }

    /* 日期一起进 RTC：断电兜底时"今天是几号"才不是假的 */
    RTC_DateTypeDef dt = {0};
    dt.Year    = bin2bcd((uint8_t)(y - 2000U));
    dt.Month   = bin2bcd(mo);
    dt.Date    = bin2bcd(d);
    dt.WeekDay = weekday_from_days(days_from_civil((int)y, (int)mo, (int)d));
    HAL_RTC_SetDate(&hrtc, &dt, RTC_FORMAT_BCD);

    RTC->BKP0R = CLOCK_BKP_MAGIC;      /* 让下次上电能直接信任 RTC */
    HAL_PWR_DisableBkUpAccess();
}

void Clock_GetDateTime(uint16_t *y, uint8_t *mo, uint8_t *d,
                       uint8_t *h, uint8_t *m, uint8_t *s)
{
    /* 无符号差值天然处理 HAL_GetTick() 49.7 天回绕 */
    uint32_t total = t0_sec + (HAL_GetTick() - t0_tick) / 1000U;
    uint32_t sod   = total % 86400U;
    int yy, mm, dd;
    civil_from_days(t0_days + total / 86400U, &yy, &mm, &dd);
    if (y)  *y  = (uint16_t)yy;
    if (mo) *mo = (uint8_t)mm;
    if (d)  *d  = (uint8_t)dd;
    if (h) *h = (uint8_t)(sod / 3600U);
    if (m) *m = (uint8_t)((sod % 3600U) / 60U);
    if (s) *s = (uint8_t)(sod % 60U);
}

void Clock_GetTime(uint8_t *h, uint8_t *m, uint8_t *s)
{
    Clock_GetDateTime(NULL, NULL, NULL, h, m, s);
}

uint8_t Clock_IsSynced(void)
{
    return synced;
}
