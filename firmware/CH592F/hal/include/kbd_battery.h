#ifndef KBD_BATTERY_H
#define KBD_BATTERY_H

#include "CH59x_common.h"

/**
 * @file    kbd_battery.h
 * @brief   电池电压 ADC 采样 + TP4054 充电状态检测
 *
 * 硬件:
 * - PA14 (AIN4): VBAT 经 100K+100K 分压 (1/2) 后的 ADC 输入
 * - ADC PGA: 1x (0dB)，与 WCH 外部分压参考实现一致
 * - PA15: VBAT_AD_EN, 高电平启动分压电路
 *   当 KBD_VBAT_DIVIDER_ALWAYS_ON=1 时持续保持高电平，便于板级测量
 * - PA13: TP4054 CHRG 引脚 (开漏输出)
 *   - 低电平: 正在充电
 *   - 高阻态: 未充电 (内部上拉读高)
 *
 * 软件:
 * - 采样采用 TMOS 非阻塞状态机
 * - 单次采样去掉极值，周期结果采用低通滤波，并复核异常大跳变
 * - 电量按单节 LiPo 电压曲线估算；充电时补偿端电压抬升且不直接报 100%
 * - 高压区经连续采样确认后锁定 100%，并通过回差避免满电百分比抖动
 * - CHRG 稳定状态采用非对称滤波，避免充电末段补充充电造成状态灯跳变
 * - 读取接口返回最近一次缓存值
 * - 查询时会在后台补发刷新请求，不阻塞主循环
 *
 * 限制:
 * - 本板没有电流采样或库仑计，百分比是电压估算值，不是精确 SOC
 * - CHRG 高电平只能表示“当前未充电”，不能区分未插电和已经充满
 */

/*============================================================================*/
/**
 * @defgroup BAT_ChargeState 充电状态
 * @{
 */

typedef enum {
  BAT_CHG_NONE = 0,     /**< 未充电 (CHRG 高阻 → 上拉读高) */
  BAT_CHG_CHARGING = 1, /**< 充电中 (CHRG 拉低) */
} kbd_charge_state_t;

/** @} */

/*============================================================================*/
/**
 * @defgroup BAT_API 电池接口
 * @{
 */

/**
 * @brief 初始化电池检测 (ADC + 充电引脚)
 * @note 在 KBD_Storage_Init 之后调用
 */
void KBD_Battery_Init(void);

/**
 * @brief 请求后台刷新一次电池采样
 * @note 非阻塞；若已有采样在途则忽略
 */
void KBD_Battery_RequestRefresh(void);

/**
 * @brief 获取电池电量百分比
 * @return 0-100
 */
uint8_t KBD_Battery_GetLevel(void);

/**
 * @brief 获取电池电压 (毫伏)
 * @return 电压 mV, 如 4200 = 4.2V
 */
uint16_t KBD_Battery_GetVoltage_mV(void);

/**
 * @brief 获取最近一次采样的 ADC 原始平均值
 * @return 未应用 ADC 校准偏移的 12 位原始计数 (0-4095)
 */
uint16_t KBD_Battery_GetAdcRaw(void);

/**
 * @brief 获取滤波后的充电状态
 * @note 进入充电约延迟 300ms，退出充电约延迟 10s
 * @return kbd_charge_state_t
 */
kbd_charge_state_t KBD_Battery_GetChargeState(void);

/**
 * @brief 获取 TP4054 CHRG 引脚瞬时原始电平
 * @return 0=低电平（充电中），1=高电平（未充电）
 */
uint8_t KBD_Battery_GetChargePinRaw(void);

/**
 * @brief 获取电压 (0.1V 单位, 用于 HID 协议)
 * @return 如 42 = 4.2V
 */
uint8_t KBD_Battery_GetVoltage_dV(void);

/**
 * @brief 进入低功耗：停止周期性采样
 * @note 常开诊断版不会关闭 VBAT 分压
 */
void KBD_Battery_Suspend(void);

/**
 * @brief 退出低功耗：立即刷新一次电量并重新启动周期性采样
 */
void KBD_Battery_Resume(void);

/** @} */

#endif /* KBD_BATTERY_H */
