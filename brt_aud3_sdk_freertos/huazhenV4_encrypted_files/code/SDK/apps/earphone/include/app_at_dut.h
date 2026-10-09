#ifndef _APP_AT_DUT_H_
#define _APP_AT_DUT_H_

#include "generic/typedef.h"

/*
 * 产测 AT 指令模块总开关。
 * 收到 AT+TEST=1 前模块静默、不改变任何正常行为；
 * 收到后进入测试模式：拉起蓝牙、切换测试唤醒词、放行全部产测指令。
 */
#ifndef TCFG_AT_DUT_ENABLE
#define TCFG_AT_DUT_ENABLE 1
#endif

/* 产测全链路调试日志开关：0 关闭，1 打开 */
#ifndef AT_DUT_DEBUG_LOG
#define AT_DUT_DEBUG_LOG 0
#endif

#if TCFG_AT_DUT_ENABLE

/* 测试模式状态查询：未进测试模式返回 0 */
u8 at_dut_test_mode_get(void);

/* 串口字节分流入口（由语音串口 RX 路径在中断里调用，仅组行并转交 AT 任务） */
void at_dut_rx_byte(u8 c);

/* 串口 RX_TIMEOUT 时冲刷未以换行结束的行 */
void at_dut_rx_flush(void);

/* 串口空闲轮询：超过 100ms 无数据时冲刷未换行结束的行（任务里周期调用） */
void at_dut_rx_idle_tick(void);

/* AT 回复发送（走语音串口发送队列，与二进制语音帧排队） */
void at_dut_send(const char *s);

/* 蓝牙初始化完成钩子：AT+TEST 等待蓝牙就绪后回 OK */
void at_dut_bt_init_ok_hook(void);

#endif /* TCFG_AT_DUT_ENABLE */

#endif /* _APP_AT_DUT_H_ */
