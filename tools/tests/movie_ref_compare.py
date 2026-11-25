#!/usr/bin/env python3
"""Compare frames decoded by the native MPEG-1 decoder (an I420 file from run_movie_mpeg_diff.sh ... native N
out.yuv, display order) with an independent decoder: FFmpeg's, through PyAV (pip install av).

The two decoders' inverse DCTs differ (Sofdec: a float AAN transform; FFmpeg: an integer one), both within
MPEG-1's accuracy rules, so small differences are expected; this shows the picture, frame order and
motion compensation agree. Prints PSNR (Y, Cb, Cr) and the largest pixel difference.

usage: movie_ref_compare.py native.yuv MOVIE.sfd [frames]
"""
import math
import sys

import av
import numpy as np


def main():
    yuv, movie = sys.argv[1], sys.argv[2]
    limit = int(sys.argv[3]) if len(sys.argv) > 3 else 1 << 30
    raw = open(movie, "rb").read(4)
    fmt = "mpegvideo" if raw == b"\x00\x00\x01\xb3" else "mpeg"  # a bare video stream or a program stream
    cont = av.open(movie, format=fmt)
    stream = next(s for s in cont.streams if s.type == "video")
    stream.thread_type = "NONE"
    data = np.fromfile(yuv, dtype=np.uint8)
    n = 0
    worst = 0
    mse = [0.0, 0.0, 0.0]
    low = (99.0, -1)
    for frame in cont.decode(stream):
        w, h = frame.width, frame.height
        size = w * h * 3 // 2
        if (n + 1) * size > data.size or n >= limit:
            break
        img = frame.to_ndarray(format="yuv420p")  # (h * 3 / 2, w)
        ref = [img[:h], img[h:h + h // 4].reshape(h // 2, w // 2), img[h + h // 4:].reshape(h // 2, w // 2)]
        o = n * size
        mine = [data[o:o + w * h].reshape(h, w), data[o + w * h:o + w * h * 5 // 4].reshape(h // 2, w // 2),
                data[o + w * h * 5 // 4:o + size].reshape(h // 2, w // 2)]
        fm = 0.0
        for k in range(3):
            d = mine[k].astype(np.int32) - ref[k].astype(np.int32)
            e = float(np.mean(d * d))
            mse[k] += e
            worst = max(worst, int(np.abs(d).max()))
            if k == 0:
                fm = e
        p = 99.0 if fm == 0 else 10 * math.log10(255 * 255 / fm)
        if p < low[0]:
            low = (p, n)
        n += 1
    if not n:
        print("no frames compared")
        return 1
    ps = ["inf" if m == 0 else "%.2f dB" % (10 * math.log10(255 * 255 / (m / n))) for m in mse]
    print("%d frames vs FFmpeg %s: PSNR Y %s, Cb %s, Cr %s; lowest frame Y %.2f dB (frame %d); largest pixel difference %d"
          % (n, "libavcodec " + ".".join(str(x) for x in av.library_versions.get("libavcodec", ("?",))), ps[0], ps[1], ps[2], low[0], low[1], worst))
    return 0


if __name__ == "__main__":
    sys.exit(main())
