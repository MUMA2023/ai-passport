// main/demo.h —— 玩法页的统一接口。
//
// 本固件是单一玩法固件:main.c 上电后直接进入球场,没有 BSP 参考示例的菜单,
// 因此不需要 demo_entry_t 注册表。玩法页仍按 enter / exit / key 三段式实现,
// 便于将来增删页面时保持同一套页面契约。
#pragma once

#include "bsp_button.h"

// 迷你高尔夫(定义在 demo_golf.c)
void demo_golf_enter(void);                          // 建屏、起定时器、载入
void demo_golf_exit(void);                           // 停定时器、删屏、释放资源
void demo_golf_key(bsp_btn_t btn, bsp_btn_ev_t ev);  // 收按键
