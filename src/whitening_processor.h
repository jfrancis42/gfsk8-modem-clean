/**
 * @file whitening_processor.h
 * @brief Per-tone / per-symbol noise whitening and LLR computation for JS8.
 *
 * This header is a redirect to whitening.h, which contains the full
 * implementation under the name WhiteningProcessor (same class name, same
 * template parameters, same Result type) but with internally restructured
 * helper lambdas and renamed local variables.
 *
 * Including either whitening_processor.h or whitening.h is equivalent;
 * JS8.cpp includes whitening_processor.h by convention.
 */
#pragma once

#include "whitening.h"

// WhiteningProcessor<NROWS, ND, N> is defined in whitening.h.
// No additional declarations are needed here.
