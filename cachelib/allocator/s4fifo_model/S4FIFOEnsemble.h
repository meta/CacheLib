// Ensemble aggregation wrapper for CacheLib S4FIFO
// Include all model files (now header-only, .h)
#include "model_0.h"
#include "model_1.h"
#include "model_2.h"
#include "model_3.h"
#include "model_4.h"

static void ensemble_score(double *input, double *result) {
    // Temporary arrays for each model's output
    double probs_0[18];
    double probs_1[18];
    double probs_2[18];
    double probs_3[18];
    double probs_4[18];

    // Call each model
    model_0_score(input, probs_0);
    model_1_score(input, probs_1);
    model_2_score(input, probs_2);
    model_3_score(input, probs_3);
    model_4_score(input, probs_4);

    // Average probabilities
    for (int c = 0; c < 18; c++) result[c] = 0.0;
    for (int c = 0; c < 18; c++) {
        result[c] += probs_0[c];
        result[c] += probs_1[c];
        result[c] += probs_2[c];
        result[c] += probs_3[c];
        result[c] += probs_4[c];
    }
    for (int c = 0; c < 18; c++) result[c] /= 5.0;
}

static int ensemble_predict(double *input) {
    double probs[18];
    ensemble_score(input, probs);
    int best = 0;
    for (int c = 1; c < 18; c++) {
        if (probs[c] > probs[best]) best = c;
    }
    return best;
}
