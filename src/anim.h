// 动画曲线，取自 WacOS 的 src/shell/anim.cpp，仅保留界面使用的部分。
#pragma once

namespace anim {
double Now();  // 返回高精度时间，单位为毫秒。

float Bezier(float t, float x1, float y1, float x2, float y2);
float Ease(float t);  // 缓动曲线，参数取自 Core Animation 的默认值。
float EaseInOut(float t);  // 缓入缓出曲线。
float EaseOut(float t);  // 缓出曲线。
// 弹簧曲线由响应周期和阻尼比定义，阻尼比小于 1 时会出现回弹。
float Spring(float tSec, float response = 0.55f, float damping = 0.825f);
float SpringDuration(float response, float damping);  // 返回弹簧稳定所需的时间，用于决定动画时钟的停止时机。

extern bool reduceMotion;  // 系统减少动画开关，置位时动画直接跳到终点。
}
