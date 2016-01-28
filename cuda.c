/* CUDA engine
 *
 * Copyright 2013 Constantin Baranov
 *
 * This file is part of GLGrab.
 *
 * GLGrab is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * GLGrab is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GLGrab.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "cuda.h"
#include "rgba2yuv420p.h"

#include <cuda_runtime_api.h>
#include <cuda_gl_interop.h>
#include <stdio.h>

static cudaError_t cuda(cudaError_t error, const char label[]) {
	if (error != cudaSuccess) {
		fprintf(stderr, "glgrab: CUDA: %s failed with %s\n", label, cudaGetErrorName(error));
	}

	return error;
}

static void cuda_cleanup(struct glgrab *g) {
	cuda(cudaFreeHost(g->cuda.p_host), "FreeHost");
	g->cuda.p_host = NULL;

	cuda(cudaGraphicsUnregisterResource(g->cuda.resource), "GraphicsUnregisterResource");
	g->cuda.resource = NULL;

	cuda(cudaStreamDestroy(g->cuda.stream), "StreamDestroy");
	g->cuda.stream = NULL;
}

static bool cuda_shoot(struct glgrab *g, size_t width, size_t height, size_t pitch) {
	cudaError_t error = cuda(cudaGraphicsMapResources(1, &g->cuda.resource, g->cuda.stream),
		"GraphicsMapResources");

	cudaArray_t array;
	if (error == cudaSuccess) {
		error = cuda(cudaGraphicsSubResourceGetMappedArray(&array, g->cuda.resource, 0, 0),
			"GraphicsSubResourceGetMappedArray");
	}

	void *p_dev = NULL;
	if (error == cudaSuccess) {
		error = cuda(cudaHostGetDevicePointer(&p_dev, g->cuda.p_host, 0), "HostGetDevicePointer");
	}

	if (error == cudaSuccess) {
		error = cuda(cudaMemcpy2DFromArrayAsync(p_dev, pitch * 4, array,
				0, 0, width * 4, height, cudaMemcpyDeviceToDevice, g->cuda.stream),
			"Memcpy2DFromArrayAsync");
	}

	if (error == cudaSuccess) {
		error = cuda(cudaGraphicsUnmapResources(1, &g->cuda.resource, g->cuda.stream),
			"GraphicsUnmapResources");
	}

	return error == cudaSuccess;
}

static bool cuda_copy(struct glgrab *g) {
	cudaError_t error = cuda(cudaStreamSynchronize(g->cuda.stream), "StreamSynchronize");

	if (error == cudaSuccess) {
		rgba2yuv420p(g->cuda.p_host, g->frame->data,
			g->frame->padded_width >> width_align,
			g->frame->padded_height >> height_align);
	}

	return error == cudaSuccess;
}

bool cuda_init(struct glgrab *g, size_t size) {
	cudaError_t error = cuda(cudaStreamCreate(&g->cuda.stream), "StreamCreate");

	if (error == cudaSuccess) {
		error = cuda(cudaGraphicsGLRegisterImage(&g->cuda.resource,
				g->tex, GL_TEXTURE_RECTANGLE, cudaGraphicsRegisterFlagsReadOnly),
			"GraphicsGLRegisterImage");
	}

	if (error == cudaSuccess) {
		error = cuda(cudaHostAlloc(&g->cuda.p_host, size, cudaHostAllocMapped), "HostAlloc");
	}

	if (error != cudaSuccess) {
		return false;
	}

	g->engine_shoot = cuda_shoot;
	g->engine_copy = cuda_copy;
	g->engine_cleanup = cuda_cleanup;
	return true;
}
