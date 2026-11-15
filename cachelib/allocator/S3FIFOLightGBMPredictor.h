/*
 * S3FIFO LightGBM Ensemble Predictor
 *
 * Shares the same trained ensemble as S4FIFO (see s4fifo_model/). The
 * model predicts 1 of 18 configuration classes, which are mapped to
 * MMS3FIFO Config parameters.
 *
 * NOTE: This header should be included AFTER S3FIFOFeatureVector and
 * S3FIFOPredictedParams are defined (e.g., by MMS3FIFO_feature.h), from
 * inside namespace facebook::cachelib. Do NOT add a namespace declaration
 * here.
 */

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

extern "C" {
#include "s4fifo_model/S4FIFOEnsemble.h"
}

// 18 predicted configuration classes.
//   s -> smallSizePercent (fraction, multiply by 100, clamp [1,50])
//   m -> smallToMainPromoThreshold (1 or 2)
//   t -> (ghostToMainThreshold in S4; unused here, kept for parity)
//   g -> ghostSizePercent (fraction, multiply by 100, clamp [10,600])
//   k -> smallSkipRatio (0 or 0.25)
struct S3FIFOConfigEntry {
  double s;
  int m;
  int t;
  double g;
  double k;
};

constexpr S3FIFOConfigEntry kS3FIFOConfigs[18] = {
    {0.20, 1, 0, 3.0, 0.25}, // Class 0
    {0.05, 1, 0, 0.9, 0.25}, // Class 1
    {0.50, 1, 0, 0.9, 0.25}, // Class 2
    {0.20, 1, 0, 0.9, 0.25}, // Class 3
    {0.05, 2, 0, 6.0, 0.25}, // Class 4
    {0.10, 2, 1, 3.0, 0.25}, // Class 5
    {0.30, 2, 0, 3.0, 0.25}, // Class 6
    {0.05, 2, 0, 3.0, 0.25}, // Class 7
    {0.10, 2, 0, 0.9, 0.25}, // Class 8
    {0.70, 1, 1, 0.9, 0.25}, // Class 9
    {0.20, 1, 1, 0.9, 0.25}, // Class 10
    {0.05, 1, 1, 0.9, 0.25}, // Class 11
    {0.30, 1, 0, 6.0, 0.25}, // Class 12
    {0.20, 2, 0, 0.9, 0.25}, // Class 13
    {0.90, 2, 0, 3.0, 0.25}, // Class 14
    {0.10, 2, 0, 6.0, 0.25}, // Class 15
    {0.30, 2, 1, 3.0, 0.25}, // Class 16
    {0.05, 2, 0, 0.9, 0.25}, // Class 17
};

// Feature layout matches the training pipeline (alphabetical names):
//   0: H_g, 1: H_m, 2: H_s, 3: decay_rate_small, 4: entropy_gap,
//   5: ghost_pressure, 6..25: hist_ghost[HIST_ORDER], 26..45: hist_main,
//   46..65: hist_small, 66: log_C, 67: probation_efficiency,
//   68: ratio_estimate, 69: rho_onehit, 70: rho_unique,
//   71: scan_intensity, 72: tail_heaviness, 73: thrashing_risk,
//   74: total_reqs
inline void prepareS3ModelInput(const S3FIFOFeatureVector& fv, double* input) {
  for (int i = 0; i < 75; i++) {
    input[i] = 0.0;
  }

  const double log_C = fv.logCacheCapacity;

  const double probation_efficiency =
      static_cast<double>(fv.hitsSmall) / (fv.hitsMain + 1e-6);

  const double total_hits_with_ghost =
      static_cast<double>(fv.totalHits + fv.hitsGhost);
  const double ghost_pressure =
      static_cast<double>(fv.hitsGhost) / (total_hits_with_ghost + 1e-6);

  const double entropy_gap = fv.hitRatioMain - fv.hitRatioSmall;
  const double decay_rate_small = fv.histSmall[0] - fv.histSmall[1];

  double tail_heaviness = 0.0;
  for (int i = 10; i < 20; i++) {
    tail_heaviness += fv.histMain[i];
  }

  const double cache_size = std::pow(10.0, log_C);
  const double working_set_size =
      static_cast<double>(fv.totalRequests) * fv.uniqueRatio;
  double ratio_estimate = cache_size / (working_set_size + 1e-6);
  if (ratio_estimate < 0.0001) ratio_estimate = 0.0001;
  if (ratio_estimate > 1.0) ratio_estimate = 1.0;

  const double thrashing_risk =
      fv.uniqueRatio / (ratio_estimate * 100.0 + 1e-6);
  const double scan_intensity = fv.oneHitRatio * (1.0 - ratio_estimate);

  input[0] = fv.hitRatioGhost;
  input[1] = fv.hitRatioMain;
  input[2] = fv.hitRatioSmall;
  input[3] = decay_rate_small;
  input[4] = entropy_gap;
  input[5] = ghost_pressure;

  // Lexicographic order (0,1,10,11,...,19,2,3,...,9) — matches Python
  // training's alphabetical column sort.
  static const int HIST_ORDER[20] = {0, 1, 10, 11, 12, 13, 14, 15, 16, 17,
                                     18, 19, 2, 3, 4, 5, 6, 7, 8, 9};
  for (int i = 0; i < 20; i++) {
    input[6 + i] = fv.histGhost[HIST_ORDER[i]];
  }
  for (int i = 0; i < 20; i++) {
    input[26 + i] = fv.histMain[HIST_ORDER[i]];
  }
  for (int i = 0; i < 20; i++) {
    input[46 + i] = fv.histSmall[HIST_ORDER[i]];
  }

  input[66] = log_C;
  input[67] = probation_efficiency;
  input[68] = ratio_estimate;
  input[69] = fv.oneHitRatio;
  input[70] = fv.uniqueRatio;
  input[71] = scan_intensity;
  input[72] = tail_heaviness;
  input[73] = thrashing_risk;
  input[74] = static_cast<double>(fv.totalRequests);
}

inline S3FIFOPredictedParams s3fifoLightGBMPredict(
    const S3FIFOFeatureVector& features) {
  S3FIFOPredictedParams params;
  if (features.totalRequests < 10000 || features.totalHits < 100) {
    return params;
  }

  double input[75];
  prepareS3ModelInput(features, input);

  const int classId = ensemble_predict(input);
  if (classId < 0 || classId >= 18) {
    return params;
  }

  const S3FIFOConfigEntry& cfg = kS3FIFOConfigs[classId];

  // s_param (0.05-0.90) -> smallSizePercent (5-90%). Pass through; the
  // config allows the full range.
  params.smallSizePercent = static_cast<size_t>(cfg.s * 100);

  // m_param (1 or 2) -> smallToMainPromoThreshold.
  params.smallToMainPromoThreshold = cfg.m;

  // g_param (0.9-6.0) -> ghostSizePercent (90-600%). Pass through.
  params.ghostSizePercent = static_cast<size_t>(cfg.g * 100);

  // k_param (0 or 0.25) -> smallSkipRatio.
  params.smallSkipRatio = cfg.k;

  return params;
}
