# S4FIFO LightGBM Model

This directory contains the LightGBM ensemble model for S4FIFO configuration prediction.

## Files

| File | Description |
|------|-------------|
| `S4FIFOEnsemble.c` | Ensemble wrapper that combines 5 models |
| `model_0.c` - `model_4.c` | Individual LightGBM model trees |

## Model Details

- **Type**: LightGBM multi-class classifier (18 classes)
- **Ensemble Size**: 5 models
- **Trees per Model**: 50 
- **Max Depth**: 4
- **Total C Code Size**: ~8.3 MB

## Performance

- **Accuracy**: ~47% top-1 classification
- **Mean Improvement vs FIFO**: 15.34%
- **Worst Case vs FIFO**: -3.90%

## Training

Generated from `train_xgb_18class_lite.py` and exported using m2cgen.

## Usage

The model is included via `S4FIFOLightGBMPredictor.h`:

```cpp
#include "cachelib/allocator/S4FIFOLightGBMPredictor.h"

// Set the prediction callback
container.setPredictionCallback(facebook::cachelib::lightGBMPredict);
```


Note: we use lite model since the full model -> over 1GB after conversion
