/**
 * 极简 PNG 输出（依赖系统 zlib，无其他依赖）
 * 用于无头模拟器把 LVGL 帧缓冲直接落成 PNG，供 AI 视觉自检。
 */
#ifndef PNG_WRITER_H
#define PNG_WRITER_H

#include <stdint.h>
#include <stddef.h>

/**
 * 把 RGB565 帧缓冲写成 PNG 文件。
 *
 * @param path   输出路径
 * @param px     RGB565 像素数组，行主序，长度 w*h
 * @param w      宽（像素）
 * @param h      高（像素）
 * @return 0 成功，非 0 失败
 */
int png_write_rgb565(const char *path, const uint16_t *px, int w, int h);

#endif /* PNG_WRITER_H */
