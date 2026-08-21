#pragma once

#include "MotionBlur.h"

/** @brief Launch the transform kernel on the host-supplied stream.
 *
 * Pointers are device memory owned by the host application; the host hands the
 * plugin CUDA pointers directly when kOfxImageEffectPropCudaEnabled is set.
 *
 * Asynchronous when the host supplies a stream, since setSupportsCudaStream(true)
 * makes synchronisation the host's job. When @p pStream is null the call
 * synchronises before returning, because otherwise the host could read the
 * output while the kernel was still writing it.
 *
 * @param st per-shutter-sample inverse transforms, built once per render by
 *           BuildSampleTransforms. A count of 1 means no motion blur.
 * @param srcOriginX,srcOriginY where the source's pixel (0,0) sits in the
 *           destination's frame -- non-zero when a host crops the input to its
 *           domain of definition, as Fusion does. See ImageView::originX.
 *
 * A null or empty source clears the destination to transparent rather than
 * leaving it untouched; see the definition for why that matters.
 *
 * @return nullptr on success, or a static CUDA error string.
 */
const char* RunMultiTransformCuda(void* pStream,
                                  const float* src, int srcWidth, int srcHeight, int srcRowFloats,
                                  float* dst, int dstWidth, int dstHeight, int dstRowFloats,
                                  const mtx::SampleTransforms& st,
                                  int filterMode, int edgeMode,
                                  int srcOriginX = 0, int srcOriginY = 0);

/** @brief Host-memory, fully synchronous variant used by the parity test. */
void RunMultiTransformCudaSync(const float* hostSrc, int srcWidth, int srcHeight,
                               float* hostDst, int dstWidth, int dstHeight,
                               const mtx::SampleTransforms& st,
                               int filterMode, int edgeMode,
                               int srcOriginX = 0, int srcOriginY = 0);
