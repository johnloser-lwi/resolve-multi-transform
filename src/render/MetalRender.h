#pragma once

// Metal render path, the macOS counterpart of CudaRender.h -- same entry points,
// same arguments, so the plugin and the parity test treat the two alike.
//
// Plain C++ on purpose: Metal objects cross this boundary as void*, and only
// MetalRender.mm sees Objective-C. When the host enables Metal rendering it
// hands the plugin an id<MTLCommandQueue> (kOfxImageEffectPropMetalCommandQueue)
// and each image's pixel data is an id<MTLBuffer>, not a CPU pointer.
//
// UNTESTED ON HARDWARE: written on Windows ahead of a Mac being available.
// test_render_parity runs every CPU-vs-GPU check against this path on macOS;
// that suite passing is the bar before trusting it in Resolve.

#include "MotionBlur.h"
#include "Sampler.h"

/** @brief Encode the transform (and blur, when @p defocusPx is active) on the
 *  host's command queue and commit. Does not wait: the host orders its own work
 *  on the same queue after ours, as it does for the CUDA stream.
 *
 * @param pCmdQueue  id<MTLCommandQueue> from the render arguments
 * @param src, dst   id<MTLBuffer> pixel data; src may be null (renders clear)
 *
 * @return nullptr on success, or a static error string.
 */
const char* RunMultiTransformMetal(void* pCmdQueue,
                                   const void* src, int srcWidth, int srcHeight,
                                   int srcRowBytes, int srcDepth,
                                   void* dst, int dstWidth, int dstHeight,
                                   int dstRowBytes, int dstDepth,
                                   const mtx::SampleTransforms& st,
                                   int filterMode, int edgeMode,
                                   int srcOriginX = 0, int srcOriginY = 0,
                                   float defocusPx = 0.0f);

/** @brief Host-memory, fully synchronous variant for the parity test: creates
 *  its own device and queue, uploads, renders, waits, downloads. Rows are
 *  assumed tightly packed for the given depth. Same signature as
 *  RunMultiTransformCudaSync. */
void RunMultiTransformMetalSync(const void* hostSrc, int srcWidth, int srcHeight,
                                void* hostDst, int dstWidth, int dstHeight,
                                const mtx::SampleTransforms& st,
                                int filterMode, int edgeMode,
                                int srcOriginX = 0, int srcOriginY = 0,
                                int srcDepth = mtx::kDepthFloat, int dstDepth = mtx::kDepthFloat,
                                float defocusPx = 0.0f);
