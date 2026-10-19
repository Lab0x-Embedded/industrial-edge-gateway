/*
 * XPT2046 电阻触摸驱动（GPIO 模拟 SPI）
 * TP_Get_Calibrated 为 LVGL indev 的对接接口（原仓库缺失，此处补全）。
 */
#include "touch.h"
#include "main.h"
#include "w25qxx.h"
#include "log.h"        /* Log_Write：校准超时/成功/无效 落盘（内有 flash_mutex 保护） */
#include "FreeRTOS.h"
#include "task.h"

#define TP_CLK(x)  HAL_GPIO_WritePin(TP_CLK_GPIO_Port,  TP_CLK_Pin,  (GPIO_PinState)(x))
#define TP_DIN(x)  HAL_GPIO_WritePin(TP_MOSI_GPIO_Port, TP_MOSI_Pin, (GPIO_PinState)(x))
#define TP_CS(x)   HAL_GPIO_WritePin(TP_CS_GPIO_Port,   TP_CS_Pin,   (GPIO_PinState)(x))
#define TP_DOUT()  HAL_GPIO_ReadPin (TP_MISO_GPIO_Port, TP_MISO_Pin)
#define TP_PEN()   HAL_GPIO_ReadPin (TP_PEN_GPIO_Port,  TP_PEN_Pin)

/* —— 时序对齐厂商例程 xpt2046.c（启明欣欣 HAL 库版本）：——
 * 每个时钟沿约 1µs、命令后 6µs 建立、busy 时钟后读 16 位取高 12 位(>>4)。
 * 之前自写版本无延时且 >>3，读数整体错位。 */

static void tp_delay_us(uint32_t us)
{
    while (us--)
        for (volatile uint32_t i = 0; i < 100; i++)
            __NOP();
}

static void TP_WriteByte(uint8_t d)             /* MSB 先行，上升沿写入 */
{
    for (uint8_t i = 0; i < 8; i++) {
        TP_CLK(0);
        TP_DIN((d & 0x80) ? 1 : 0);
        d <<= 1;
        tp_delay_us(1);
        TP_CLK(1);
        tp_delay_us(1);
    }
}

static uint16_t TP_Read_AD(uint8_t cmd)         /* 差分模式读 12bit */
{
    uint16_t v = 0;

    TP_CLK(0);
    TP_DIN(0);
    TP_CS(0);
    TP_WriteByte(cmd);
    tp_delay_us(6);                             /* 转换建立时间 */
    TP_CLK(0); tp_delay_us(1);
    TP_CLK(1); tp_delay_us(1);                  /* busy 时钟 */
    TP_CLK(0);
    for (uint8_t i = 0; i < 16; i++) {          /* 16 位，仅高 12 位有效 */
        v <<= 1;
        TP_CLK(0); tp_delay_us(1);
        TP_CLK(1); tp_delay_us(1);
        if (TP_DOUT()) v |= 1;
    }
    v >>= 4;
    TP_CS(1);
    return v;
}

uint16_t TP_ReadX(void)      { return TP_Read_AD(0xD0); }
uint16_t TP_ReadY(void)      { return TP_Read_AD(0x90); }

uint16_t TP_ReadPressure(void)
{
    uint16_t z1 = TP_Read_AD(0xB1);
    uint16_t z2 = TP_Read_AD(0xC1);
    if (z1 == 0) return 0;
    return (z2 * 100) / z1;
}

void TP_Init(void)
{
    TP_CS(1);
    TP_ReadX();                                /* 丢弃首次读数 */
}

/* ================= 两点校准 =================
 * 开机在屏幕 左上/右下 各画一个十字，用户依次按中心，
 * 用两次按压的原始读数推算整屏映射，镜像/量程自动适配。
 * 横屏假定 XPT 的 Y(0x90) 对应面板 X、X(0xD0) 对应面板 Y；
 * 若实测方向不对，改 TP_SWAP_XY 后重新校准即可。 */
#define TP_SWAP_XY     1

#define CAL_CX1  20                    /* 左上十字中心 */
#define CAL_CY1  16
#define CAL_CX2  (LCD_W - 21)          /* 右下十字中心 299 */
#define CAL_CY2  (LCD_H - 17)          /* 223 */

static uint16_t cal_y_tl, cal_x_tl, cal_y_br, cal_x_br; /* 十字处的原始读数 */
static uint8_t  tp_calibrated = 0;

/* 等一次按压，取 16 次读数平均；sx=rawY, sy=rawX（调用方按交换语义使用）
 *
 * 返回 0=采到有效读数，-1=超时或读数无效。
 *
 * ★ 为什么必须带超时：
 *   原实现是 `while (TP_PEN() == GPIO_PIN_SET) vTaskDelay(10);` —— **无限等**。
 *   触发条件是真实存在的：新板首次烧录、Flash 坏块、校准校验失败（魔数/累加和
 *   对不上）时都会走到这里，若现场没人碰屏幕，UI 任务就永远停在开机校准界面：
 *   屏幕全白、主界面出不来、设置页也进不去。
 *
 *   更微妙的是它与看门狗的耦合：当前 Dog_task 是"无条件喂狗"，所以系统不会
 *   被复位，表现为"设备卡在白屏"；一旦哪天把喂狗改成心跳汇聚（各任务报活），
 *   这里就立刻变成"20 秒复位一次"的死循环。所以超时必须先加。
 *
 * 两个等待（等按下 / 等松开）都设了 deadline：
 *   等按下超时 → 失败（没人操作）
 *   等松开超时 → 也失败（一直按住不放属于异常操作；若放行，第二个十字会在
 *                "手指还没抬起"的状态下立刻通过，采到的是错误位置的数据）
 *
 * 关于"按住开机后第一个十字按不进去"的修复见 TP_WaitSample 内的详细说明：
 * 进入采样前**必须先清掉残留按压**（等抬手→等新按下），而不是简单判断
 * "PEN 当前是否为低"——后者会把开机长按当成一次校准点击。 */
#define TP_CAL_TIMEOUT_MS   30000U   /* 每个十字 30s 无有效操作即放弃校准 */
#define TP_CAL_RELEASE_MS    5000U   /* 开机长按后等抬手；超时视为 PEN 卡死 */

/* 等 PEN 抬起。返回 0=已抬起，-1=超时仍在按。
 * 单独抽出来的原因：它和"等按下"是两种不同的语义，混在一个循环里
 * 容易漏掉"必须先抬起、再按下"这个先后关系（下面 TP_WaitSample 里的坑）。 */
static int TP_WaitRelease(TickType_t timeout_ticks)
{
    TickType_t deadline = xTaskGetTickCount() + timeout_ticks;
    while (TP_PEN() == GPIO_PIN_RESET)
    {
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) return -1;
        vTaskDelay(10);
    }
    return 0;
}

/* 注意 timeout 参数的单位是 **tick**，不是 ms。
 * 这样调用方（TP_SampleWithRetry）算出的剩余预算可以直接传进来，
 * 避免"tick → ms → tick"的来回换算：当前 configTICK_RATE_HZ=1000 时
 * pdMS_TO_TICKS 是恒等映射、换算也不会错，但换个 tick 频率就会静默偏掉。 */
static int TP_WaitSample(uint16_t *sy, uint16_t *sx, TickType_t timeout_ticks)
{
    uint32_t ax = 0, ay = 0;
    uint8_t n = 0;

    /* 用有符号差值比较，天然处理 tick 回绕（同 clock.c 里 HAL_GetTick 的思路） */
    TickType_t deadline = xTaskGetTickCount() + timeout_ticks;

    /* ★ 必须先等手指抬起，再等"新的按下"。
     *
     * 这是"按住开机重校"与"第一个十字"之间的接缝 bug：
     *   用户按住屏幕开机 → 500ms 检测窗口通过 → 进入校准 → 画左上十字。
     *   此时手指**往往还没松**，PEN 仍是低电平。若"等按下"循环直接放行
     *   （因为它只检查 PEN 是否为低），就会立刻进入采样，采到的是
     *   "用户按住开机的那个位置"（通常不在十字中心）。
     *   结果：十字刚出现就被擦掉 → 用户感觉"根本点不到"，
     *        而且 cal_tl 存的是错误坐标 → 整份校准作废。
     *
     * 所以这里要求一次完整的"抬起 → 按下"边沿，而不是"当前是低电平"。
     * 副作用：十字 2 之前手指本来就是抬起的，这一圈会立刻通过，无额外延迟。 */
    while (TP_PEN() == GPIO_PIN_RESET)         /* 清掉残留按压 */
    {
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0)
        {
            *sx = 0; *sy = 0;
            return -1;
        }
        vTaskDelay(10);
    }

    while (TP_PEN() == GPIO_PIN_SET)           /* 等一次新的按下 */
    {
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0)
        {
            *sx = 0; *sy = 0;
            return -1;
        }
        vTaskDelay(10);
    }
    vTaskDelay(20);                            /* 膜形变稳定 */

    while (n < 16 && TP_PEN() == GPIO_PIN_RESET)
    {
        uint16_t rx = TP_ReadX();
        uint16_t ry = TP_ReadY();
        if (rx > 50 && ry > 50) { ax += rx; ay += ry; n++; }
        vTaskDelay(5);
    }

    /* 等松开，防止连采。
     * 这里**不再用剩余的完整预算**（原来传 timeout_ticks，最坏可到 30s），
     * 而是单独收窄到 TP_CAL_RELEASE_MS：
     *   - 数据已经采完了，"按久了"不影响精度（采样只发生在按下后的前 ~80ms）；
     *   - 但若用户一直按着不放，用 30s 去等会让"这一次十字"看起来像卡死，
     *     而且把下一步的预算也耗光，第二个十字直接超时 —— 正是用户遇到的
     *     "按久了就点不到、校准结束"的一部分原因。
     * 收窄后：正常松手（几十~几百 ms）完全够；真按住不放则 5s 判失败，
     * 由 TP_SampleWithRetry 在外层重试，整轮 30s 预算仍然有效。 */
    if (TP_WaitRelease(pdMS_TO_TICKS(TP_CAL_RELEASE_MS)) != 0)
    {
        *sx = 0; *sy = 0;
        return -1;                             /* 按住不放：本次作废，外层重试 */
    }

    if (n == 0)
    {
        *sx = 0; *sy = 0;
        return -1;                             /* 有按压但读数全无效（<50），同样算失败 */
    }

    *sx = ax / n;
    *sy = ay / n;
    return 0;
}

/* 采一次按压，失败（超时/读数全无效）时重试；最迟 TP_CAL_TIMEOUT_MS 放弃。
 *
 * 为什么还要重试：原实现的 `do { TP_WaitSample(); } while (sx1 == 0);`
 * 语义是"这次没采到有效值就再来一次"，常见于用户按得太轻/太快（PEN 已抬起
 * 但 16 次采样里没有一个 >50）。这层重试要保留——否则正常用户的一次误触
 * 就会导致整个校准被放弃。
 *
 * 但它必须是"有总量上限的重试"，不能是原来的无限 do-while。 */
static int TP_SampleWithRetry(uint16_t *sy, uint16_t *sx)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(TP_CAL_TIMEOUT_MS);

    for (;;)
    {
        uint32_t remain = (uint32_t)(deadline - xTaskGetTickCount());
        if ((int32_t)remain <= 0) return -1;      /* 总预算用完 */

        if (TP_WaitSample(sy, sx, remain) == 0) return 0;
        /* 本次失败但还在预算内 → 继续等下一次按压 */
    }
}

static void TP_DrawCross(uint16_t cx, uint16_t cy, uint16_t color)
{
    LCD_Fill(cx - 8, cy, cx + 8, cy, color);
    LCD_Fill(cx, cy - 8, cx, cy + 8, color);
}

/* —— 校准参数持久化 ——
 * 存 W25Q128 参数区 0xF00000（日志区 <4MB、补传缓存 4~8MB，此处空闲）。
 * 仅 UI 初始化时读写一次，与运行期的日志/补传写入无并发；
 * 读回带魔数+累加和校验，坏块/擦除中断都会被拒绝并重新校准。 */
#define TP_CAL_FLASH_ADDR  0x00F00000UL
#define TP_CAL_MAGIC       0x54504331UL   /* "TPC1" */

typedef struct {
    uint32_t magic;
    uint16_t y_tl, x_tl, y_br, x_br;
    uint16_t sum;
} TPCal_t;

static uint8_t TP_TryLoadCal(void)
{
    TPCal_t c;
    SPI_FLASH_BufferRead((uint8_t *)&c, TP_CAL_FLASH_ADDR, sizeof(c));
    uint16_t sum = (uint16_t)(c.magic + c.y_tl + c.x_tl + c.y_br + c.x_br);
    if (c.magic != TP_CAL_MAGIC || c.sum != sum) return 0;
    if (c.y_tl == c.y_br || c.x_tl == c.x_br) return 0;

    cal_y_tl = c.y_tl;  cal_x_tl = c.x_tl;
    cal_y_br = c.y_br;  cal_x_br = c.x_br;
    tp_calibrated = 1;
    return 1;
}

static void TP_SaveCal(void)
{
    TPCal_t c = { TP_CAL_MAGIC, cal_y_tl, cal_x_tl, cal_y_br, cal_x_br, 0 };
    c.sum = (uint16_t)(c.magic + c.y_tl + c.x_tl + c.y_br + c.x_br);
    SPI_FLASH_SectorErase(TP_CAL_FLASH_ADDR);
    SPI_FLASH_BufferWrite((uint8_t *)&c, TP_CAL_FLASH_ADDR, sizeof(c));
}

void TP_Do_Calibration(void)
{
    uint16_t sy1, sx1, sy2, sx2;

    LCD_Clear(0xFFFF);

    /* 强制重新校准入口：开机按住屏幕（PEN 为低）→ 忽略 Flash 里的旧校准。
     * 换板/换屏/校准漂移时按住开机即可重校，不必擦 Flash。 */
    {
        uint8_t hold = 0;
        for (uint32_t i = 0; i < 50; i++)      /* 采样 500ms，滤掉上电抖动 */
        {
            if (TP_PEN() == GPIO_PIN_RESET) hold++;
            vTaskDelay(10);
        }
        if (hold < 30)                          /* 没按住 → 尝试装载旧校准 */
            if (TP_TryLoadCal()) return;        /* Flash 里有有效校准，免校准开机 */
    }

    /* ★ 先等用户抬手，再画十字 —— 让"十字出现"本身就等于"现在可以按"。
     *
     * 为什么有这个顺序：走到这里只有两种可能
     *   a) 用户按住屏幕开机（hold≥30 强制重校）——手指此刻往往还按着；
     *   b) Flash 里没有有效校准 —— 用户可能刚点过屏幕。
     * 两种情况的共同点是 PEN 可能已经是低电平。若先画十字再采样，
     * "等按下"会立刻放行，十字刚出现就被这次残留按压消费掉并擦除，
     * 用户看到的就是"十字一闪而过、根本点不到"，而 cal_tl 还会被写成
     * 那个残留按压的坐标 —— 校准错误但流程"成功"，最难查的一类。
     *
     * 放在这里而不是塞进 TP_WaitSample 的原因：TP_WaitSample 里等完再画，
     * 十字已经显示在屏幕上了，用户看着十字却按不进去，体验照样是坏的。
     * 先抬手、再画十字，用户看到的第一个十字就一定是可按状态。 */
    if (TP_WaitRelease(pdMS_TO_TICKS(TP_CAL_RELEASE_MS)) != 0)
    {
        /* 5s 还没抬手：要么用户一直按着，要么 PEN 线卡死（硬件问题）。
         * 不能在这里继续，否则采样会拿到按住位置的错误坐标。 */
        Log_Write(LOG_WARN, "touch cal PEN stuck low");
        LCD_Clear(0xFFFF);
        return;
    }

    TP_DrawCross(CAL_CX1, CAL_CY1, 0xF800);
    if (TP_SampleWithRetry(&sy1, &sx1) != 0)
    {
        /* 左上十字 30s 内没采到 → 放弃校准，直接返回。
         * 这里**不能死等**：UI 任务卡在这里会导致屏幕永久停在白屏，
         * 主界面、设置页都出不来，设备看起来就是"开不了机"。
         * 放弃后走 TP_Get_Calibrated 里的"未校准兜底映射"（线性近似，
         * 精度差但能点亮触摸），用户可按住开机重校。 */
        Log_Write(LOG_WARN, "touch cal timeout TL");
        LCD_Clear(0xFFFF);
        return;
    }

    TP_DrawCross(CAL_CX1, CAL_CY1, 0xFFFF);    /* 擦掉左上 */
    TP_DrawCross(CAL_CX2, CAL_CY2, 0xF800);
    if (TP_SampleWithRetry(&sy2, &sx2) != 0)
    {
        Log_Write(LOG_WARN, "touch cal timeout BR");
        LCD_Clear(0xFFFF);
        return;
    }

    if (sy1 != sy2 && sx1 != sx2)
    {
        cal_y_tl = sy1;  cal_x_tl = sx1;
        cal_y_br = sy2;  cal_x_br = sx2;
        tp_calibrated = 1;
        TP_SaveCal();
        Log_Write(LOG_INFO, "touch calibrated");
    }
    else
    {
        /* 两次读数相同 → 大概率是同一个位置按了两次，不能当校准用
         * （否则除零或映射塌缩）。保持 tp_calibrated=0，走兜底映射。 */
        Log_Write(LOG_WARN, "touch cal invalid");
    }
    LCD_Clear(0xFFFF);
}

/* 返回 1=有效坐标（按下），0=未按下；坐标已映射到 LCD_W x LCD_H */
uint8_t TP_Get_Calibrated(uint16_t *x, uint16_t *y)
{
    if (TP_PEN() != GPIO_PIN_RESET) return 0;  /* PEN 高=松开 */

    /* 抗抖（同厂商例程）：连读两对，任一轴差值超 50 视为抖动丢弃 */
    uint16_t rx1 = TP_ReadX(), ry1 = TP_ReadY();
    uint16_t rx2 = TP_ReadX(), ry2 = TP_ReadY();
    if (rx1 < 50 || ry1 < 50 || rx2 < 50 || ry2 < 50) return 0;
    uint16_t ddx = (rx1 > rx2) ? rx1 - rx2 : rx2 - rx1;
    uint16_t ddy = (ry1 > ry2) ? ry1 - ry2 : ry2 - ry1;
    if (ddx > 50 || ddy > 50) return 0;
    uint16_t rx = (rx1 + rx2) / 2;
    uint16_t ry = (ry1 + ry2) / 2;

    if (tp_calibrated)
    {
        /* 十字中心对标：TL→(20,16)，BR→(299,223)，差值小防除零已在采集时保证 */
        int32_t ix = CAL_CX1 + ((int32_t)ry - cal_y_tl) * (CAL_CX2 - CAL_CX1)
                     / ((int32_t)cal_y_br - cal_y_tl);
        int32_t iy = CAL_CY1 + ((int32_t)rx - cal_x_tl) * (CAL_CY2 - CAL_CY1)
                     / ((int32_t)cal_x_br - cal_x_tl);
        if (ix < 0) ix = 0;
        if (ix > LCD_W - 1) ix = LCD_W - 1;
        if (iy < 0) iy = 0;
        if (iy > LCD_H - 1) iy = LCD_H - 1;
        *x = (uint16_t)ix;
        *y = (uint16_t)iy;
        return 1;
    }

    /* 未校准的兜底映射（量程常数可能不准，仅用于链路验证） */
    uint32_t px = (TP_SWAP_XY ? ry : rx);
    uint32_t py = (TP_SWAP_XY ? rx : ry);
    if (px < 200) px = 200;
    if (px > 3900) px = 3900;
    if (py < 200) py = 200;
    if (py > 3900) py = 3900;
    *x = (uint16_t)((px - 200) * LCD_W / 3700);
    *y = (uint16_t)((py - 200) * LCD_H / 3700);
    return 1;
}
