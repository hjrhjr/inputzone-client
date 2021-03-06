/*
 *  Copyright (c) 2013 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <math.h>

#include "vp9/encoder/vp9_aq_variance.h"

#include "vp9/common/vp9_seg_common.h"

#include "vp9/encoder/vp9_ratectrl.h"
#include "vp9/encoder/vp9_rd.h"
#include "vp9/encoder/vp9_segmentation.h"
#include "vp9/common/vp9_systemdependent.h"

#define ENERGY_MIN (-1)
#define ENERGY_MAX (1)
#define ENERGY_SPAN (ENERGY_MAX - ENERGY_MIN +  1)
#define ENERGY_IN_BOUNDS(energy)\
  assert((energy) >= ENERGY_MIN && (energy) <= ENERGY_MAX)

static const double rate_ratio[MAX_SEGMENTS] =
  {1.143, 1.0, 0.875, 1.0, 1.0, 1.0, 1.0, 1.0};
static const int segment_id[ENERGY_SPAN] = {0, 1, 2};

#define SEGMENT_ID(i) segment_id[(i) - ENERGY_MIN]

DECLARE_ALIGNED(16, static const uint8_t, vp9_64_zeros[64]) = {0};
#if CONFIG_VP9_HIGHBITDEPTH
DECLARE_ALIGNED(16, static const uint16_t, vp9_highbd_64_zeros[64]) = {0};
#endif

unsigned int vp9_vaq_segment_id(int energy) {
  ENERGY_IN_BOUNDS(energy);
  return SEGMENT_ID(energy);
}

void vp9_vaq_frame_setup(VP9_COMP *cpi) {
  VP9_COMMON *cm = &cpi->common;
  struct segmentation *seg = &cm->seg;
  int i;

  if (cm->frame_type == KEY_FRAME ||
      cpi->refresh_alt_ref_frame ||
      (cpi->refresh_golden_frame && !cpi->rc.is_src_frame_alt_ref)) {
    vp9_enable_segmentation(seg);
    vp9_clearall_segfeatures(seg);

    seg->abs_delta = SEGMENT_DELTADATA;

    vp9_clear_system_state();

    for (i = 0; i < MAX_SEGMENTS; ++i) {
      int qindex_delta =
          vp9_compute_qdelta_by_rate(&cpi->rc, cm->frame_type, cm->base_qindex,
                                     rate_ratio[i], cm->bit_depth);

      // We don't allow qindex 0 in a segment if the base value is not 0.
      // Q index 0 (lossless) implies 4x4 encoding only and in AQ mode a segment
      // Q delta is sometimes applied without going back around the rd loop.
      // This could lead to an illegal combination of partition size and q.
      if ((cm->base_qindex != 0) && ((cm->base_qindex + qindex_delta) == 0)) {
        qindex_delta = -cm->base_qindex + 1;
      }

      // No need to enable SEG_LVL_ALT_Q for this segment.
      if (rate_ratio[i] == 1.0) {
        continue;
      }

      vp9_set_segdata(seg, i, SEG_LVL_ALT_Q, qindex_delta);
      vp9_enable_segfeature(seg, i, SEG_LVL_ALT_Q);
    }
  }
}


static unsigned int block_variance(VP9_COMP *cpi, MACROBLOCK *x,
                                   BLOCK_SIZE bs) {
  MACROBLOCKD *xd = &x->e_mbd;
  unsigned int var, sse;
  int right_overflow = (xd->mb_to_right_edge < 0) ?
      ((-xd->mb_to_right_edge) >> 3) : 0;
  int bottom_overflow = (xd->mb_to_bottom_edge < 0) ?
      ((-xd->mb_to_bottom_edge) >> 3) : 0;

  if (right_overflow || bottom_overflow) {
    const int bw = 8 * num_8x8_blocks_wide_lookup[bs] - right_overflow;
    const int bh = 8 * num_8x8_blocks_high_lookup[bs] - bottom_overflow;
    int avg;
#if CONFIG_VP9_HIGHBITDEPTH
    if (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) {
      highbd_variance(x->plane[0].src.buf, x->plane[0].src.stride,
                      CONVERT_TO_BYTEPTR(vp9_highbd_64_zeros), 0, bw, bh,
                      &sse, &avg);
      sse >>= 2 * (xd->bd - 8);
      avg >>= (xd->bd - 8);
    } else {
      variance(x->plane[0].src.buf, x->plane[0].src.stride,
               vp9_64_zeros, 0, bw, bh, &sse, &avg);
    }
#else
    variance(x->plane[0].src.buf, x->plane[0].src.stride,
             vp9_64_zeros, 0, bw, bh, &sse, &avg);
#endif  // CONFIG_VP9_HIGHBITDEPTH
    var = sse - (((int64_t)avg * avg) / (bw * bh));
    return (256 * var) / (bw * bh);
  } else {
#if CONFIG_VP9_HIGHBITDEPTH
    if (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) {
      var = cpi->fn_ptr[bs].vf(x->plane[0].src.buf,
                               x->plane[0].src.stride,
                               CONVERT_TO_BYTEPTR(vp9_highbd_64_zeros),
                               0, &sse);
    } else {
      var = cpi->fn_ptr[bs].vf(x->plane[0].src.buf,
                               x->plane[0].src.stride,
                               vp9_64_zeros, 0, &sse);
    }
#else
    var = cpi->fn_ptr[bs].vf(x->plane[0].src.buf,
                             x->plane[0].src.stride,
                             vp9_64_zeros, 0, &sse);
#endif  // CONFIG_VP9_HIGHBITDEPTH
    return (256 * var) >> num_pels_log2_lookup[bs];
  }
}

double vp9_log_block_var(VP9_COMP *cpi, MACROBLOCK *x, BLOCK_SIZE bs) {
  unsigned int var = block_variance(cpi, x, bs);
  vp9_clear_system_state();
  return log(var + 1.0);
}

#define DEFAULT_E_MIDPOINT 10.0
int vp9_block_energy(VP9_COMP *cpi, MACROBLOCK *x, BLOCK_SIZE bs) {
  double energy;
  double energy_midpoint;
  vp9_clear_system_state();
  energy_midpoint =
    (cpi->oxcf.pass == 2) ? cpi->twopass.mb_av_energy : DEFAULT_E_MIDPOINT;
  energy = vp9_log_block_var(cpi, x, bs) - energy_midpoint;
  return clamp((int)round(energy), ENERGY_MIN, ENERGY_MAX);
}
