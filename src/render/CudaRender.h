#pragma once

#include "MotionBlur.h"

/** @brief Launch the transform kernel on the host-supplied stream.
 *
 * Pointers are device memory owned by the host application; Resolve hands the
 * plugin CUDA pointers directly when kOfxImageEffectPropCudaEnabled is set.
 * Asynchronous: the plugin declares setSupportsCudaStream(true), so Resolve
 * handles synchronisation.
 *
 * @param st per-shutter-sample inverse transforms, built once per render by
 *           BuildSampleTransforms. A count of 1 means no motion blur.
 */
void RunMultiTransformCuda(void* pStream,
                           const float* src, int srcWidth, int srcHeight, int srcRowFloats,
                           float* dst, int dstWidth, int dstHeight, int dstRowFloats,
                           const mtx::SampleTransforms& st,
                           int filterMode, int edgeMode);

/** @brief Host-memory, fully synchronous variant used by the parity test. */
void RunMultiTransformCudaSync(const float* hostSrc, int srcWidth, int srcHeight,
                               float* hostDst, int dstWidth, int dstHeight,
                               const mtx::SampleTransforms& st,
                               int filterMode, int edgeMode);
