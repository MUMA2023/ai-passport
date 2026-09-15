// main/pet_store.h —— 桌宠状态的断电持久化。
//
// 采用「双槽 + CRC」策略：每次保存交替写入 slot_a / slot_b，读取时选序号更大且
// 校验通过的一份。这样即使掉电发生在写入中途，也总有一份完好数据可用。
#pragma once

#include <stdbool.h>

#include "pet_model.h"

// 初始化 NVS 并启动后台保存任务。可重复调用。
bool pet_store_init(void);

// 读回最近一次有效状态；无有效数据或数据不合法时返回 false（调用方应新建）。
bool pet_store_load(pet_model_t *model);

// 把状态投递给后台任务，立即返回，不阻塞按键与 LVGL 任务。
bool pet_store_save_async(const pet_model_t *model);

// 是否有一次保存尚未落盘（退出或深度休眠前可用于判断）。
bool pet_store_pending(void);
