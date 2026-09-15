// main/pet_audio.h —— 桌宠的提示音。
//
// 音效全部由代码合成（正弦/三角/方波 + 包络），不需要额外的音频素材文件，
// 也就不占用 Flash 与 RAM 做资源解码。播放跑在独立工作线程里，按键回调只入队。
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PET_SOUND_BOOT = 0,  // 开机问好
    PET_SOUND_PET,       // 撸一撸
    PET_SOUND_FEED,      // 喂食
    PET_SOUND_PLAY,      // 玩耍
    PET_SOUND_HUNGRY,    // 饿了提醒
    PET_SOUND_SLEEP,     // 睡着
    PET_SOUND_WAKE,      // 醒来
    PET_SOUND_MOVE,      // 界面移动
    PET_SOUND_CONFIRM,   // 确认
    PET_SOUND_DENY,      // 操作无效
    PET_SOUND_COUNT,
} pet_sound_t;

// 启动音频与播放线程。失败时后续 play 调用静默丢弃（无声音但不影响使用）。
bool pet_audio_init(void);

// 输出音量 0..100。0 表示静音。
void pet_audio_set_volume(uint8_t percent);
uint8_t pet_audio_volume(void);

// 请求播放一个音效；非阻塞，队列满时直接丢弃。
void pet_audio_play(pet_sound_t sound);
