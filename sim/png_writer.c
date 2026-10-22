#include "png_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* RGB565 分量扩展到 8bit */
#define R565(c) (uint8_t)((((c) >> 11) & 0x1F) * 255 / 31)
#define G565(c) (uint8_t)((((c) >> 5) & 0x3F) * 255 / 63)
#define B565(c) (uint8_t)(((c) & 0x1F) * 255 / 31)

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/** 写一个 PNG chunk：length + type + data + crc(type+data) */
static int write_chunk(FILE *f, const char *type, const uint8_t *data, size_t len)
{
    uint8_t hdr[8];
    put_be32(hdr, (uint32_t)len);
    memcpy(hdr + 4, type, 4);
    if (fwrite(hdr, 1, 8, f) != 8) return 1;
    if (len && fwrite(data, 1, len, f) != len) return 1;

    uint32_t crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (const Bytef *)type, 4);
    if (len) crc = crc32(crc, data, (uInt)len);

    uint8_t trailer[4];
    put_be32(trailer, crc);
    return fwrite(trailer, 1, 4, f) == 4 ? 0 : 1;
}

int png_write_rgb565(const char *path, const uint16_t *px, int w, int h)
{
    if (!path || !px || w <= 0 || h <= 0) return 1;

    /* 每行: 1 字节 filter + w*3 字节 RGB */
    const size_t stride = (size_t)w * 3 + 1;
    const size_t raw_len = stride * (size_t)h;

    uint8_t *raw = malloc(raw_len);
    if (!raw) return 1;

    for (int y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * stride;
        row[0] = 0; /* filter: None */
        const uint16_t *src = px + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            row[1 + x * 3 + 0] = R565(src[x]);
            row[1 + x * 3 + 1] = G565(src[x]);
            row[1 + x * 3 + 2] = B565(src[x]);
        }
    }

    uLongf bound = compressBound((uLong)raw_len);
    uint8_t *deflated = malloc(bound);
    if (!deflated) {
        free(raw);
        return 1;
    }
    if (compress2(deflated, &bound, raw, (uLong)raw_len, 6) != Z_OK) {
        free(raw);
        free(deflated);
        return 1;
    }
    free(raw);

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(deflated);
        return 1;
    }

    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    int rc = 0;
    if (fwrite(sig, 1, 8, f) != 8) rc = 1;

    if (!rc) {
        uint8_t ihdr[13];
        put_be32(ihdr + 0, (uint32_t)w);
        put_be32(ihdr + 4, (uint32_t)h);
        ihdr[8] = 8;  /* bit depth */
        ihdr[9] = 2;  /* color type: truecolor RGB */
        ihdr[10] = 0; /* deflate */
        ihdr[11] = 0; /* adaptive filtering */
        ihdr[12] = 0; /* interlace: none */
        rc = write_chunk(f, "IHDR", ihdr, sizeof(ihdr));
    }
    if (!rc) rc = write_chunk(f, "IDAT", deflated, (size_t)bound);
    if (!rc) rc = write_chunk(f, "IEND", NULL, 0);

    fclose(f);
    free(deflated);
    return rc;
}
