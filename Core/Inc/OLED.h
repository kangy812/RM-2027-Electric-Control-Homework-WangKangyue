#ifndef __OLED_H
#define __OLED_H

#include <stdint-gcc.h>

void OLED_Init(void);

/* 自检结果（开机时 OLED_Init 自动完成）：
 *   OLED_DetectResult  0 = 4 种组合全无应答
 *                      1 = SCL=PB1 SDA=PB0 地址 0x78
 *                      2 = SCL=PB1 SDA=PB0 地址 0x7A
 *                      3 = SCL=PB0 SDA=PB1 地址 0x78（接线接反，已自动纠正）
 *                      4 = SCL=PB0 SDA=PB1 地址 0x7A
 *   OLED_AckAddr       探测到的从机地址，0x00 表示没找到
 */
extern uint8_t OLED_DetectResult;
extern uint8_t OLED_AckAddr;
extern uint8_t OLED_ProbeLog[4];

void OLED_Clear(void);
void OLED_ShowChar(uint8_t Line, uint8_t Column, char Char);
void OLED_ShowString(uint8_t Line, uint8_t Column, char *String);
void OLED_ShowNum(uint8_t Line, uint8_t Column, uint32_t Number, uint8_t Length);
void OLED_ShowSignedNum(uint8_t Line, uint8_t Column, int32_t Number, uint8_t Length);
void OLED_ShowHexNum(uint8_t Line, uint8_t Column, uint32_t Number, uint8_t Length);
void OLED_ShowBinNum(uint8_t Line, uint8_t Column, uint32_t Number, uint8_t Length);

#endif
