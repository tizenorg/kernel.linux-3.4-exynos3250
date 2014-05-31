/* exynos_drm_buf.c
 *
 * Copyright (c) 2011 Samsung Electronics Co., Ltd.
 * Author: Inki Dae <inki.dae@samsung.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * VA LINUX SYSTEMS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "drmP.h"
#include "drm.h"
#include "exynos_drm.h"

#include "exynos_drm_drv.h"
#include "exynos_drm_gem.h"
#include "exynos_drm_buf.h"
#include "exynos_drm_iommu.h"

static struct sg_table *exynos_pages_to_sg(struct page **pages, int nr_pages)
{
	struct sg_table *sgt = NULL;
	int ret;

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	ret = sg_alloc_table_from_pages(sgt, pages, nr_pages,
			0, PAGE_SIZE * nr_pages, GFP_KERNEL);
	if (ret) {
		kfree(sgt);
		return ERR_PTR(ret);
	}

	return sgt;
}

static int lowlevel_buffer_allocate(struct drm_device *dev,
		unsigned int flags, struct exynos_drm_gem_buf *buf)
{
	int ret = 0;
	enum dma_attr attr = DMA_ATTR_SKIP_BUFFER_CLEAR;
	unsigned int nr_pages;

	DRM_DEBUG_KMS("%s\n", __FILE__);

	if (buf->dma_addr) {
		DRM_DEBUG_KMS("already allocated.\n");
		return 0;
	}

	init_dma_attrs(&buf->dma_attrs);
	dma_set_attr(attr, &buf->dma_attrs);

	/*
	 * if EXYNOS_BO_CONTIG, fully physically contiguous memory
	 * region will be allocated else physically contiguous
	 * as possible.
	 */
	if (!(flags & EXYNOS_BO_NONCONTIG)) {
		attr = DMA_ATTR_FORCE_CONTIGUOUS;
		dma_set_attr(attr, &buf->dma_attrs);
	}

	/*
	 * if EXYNOS_BO_WC or EXYNOS_BO_NONCACHABLE, writecombine mapping
	 * else cachable mapping.
	 */
	if (flags & EXYNOS_BO_WC || !(flags & EXYNOS_BO_CACHABLE)) {
		dma_set_attr(DMA_ATTR_WRITE_COMBINE, &buf->dma_attrs);
		dma_set_attr(DMA_ATTR_SKIP_CPU_SYNC, &buf->dma_attrs);
	} else
		dma_set_attr(DMA_ATTR_NON_CONSISTENT, &buf->dma_attrs);

	dma_set_attr(DMA_ATTR_NO_KERNEL_MAPPING, &buf->dma_attrs);

	nr_pages = buf->size >> PAGE_SHIFT;

	if (is_drm_iommu_supported(dev)) {
		buf->pages = dma_alloc_attrs(dev->dev, buf->size,
					&buf->dma_addr, GFP_KERNEL,
					&buf->dma_attrs);
		if (!buf->pages) {
			DRM_ERROR("failed to allocate buffer.\n");
			return -ENOMEM;
		}
	} else {
		dma_addr_t start_addr;
		unsigned int i = 0;

		buf->pages = drm_calloc_large(nr_pages, sizeof(struct page *));
		if (!buf->pages) {
			DRM_ERROR("failed to allocate pages.\n");
			return -ENOMEM;
		}

		buf->kvaddr = dma_alloc_attrs(dev->dev, buf->size,
					&buf->dma_addr, GFP_KERNEL,
					&buf->dma_attrs);
		if (!buf->kvaddr) {
			DRM_ERROR("failed to allocate buffer.\n");
			drm_free_large(buf->pages);
			return -ENOMEM;
		}

		start_addr = buf->dma_addr;
		while (i < nr_pages) {
			buf->pages[i] = phys_to_page(start_addr);
			start_addr += PAGE_SIZE;
			i++;
		}
	}

	buf->sgt = exynos_pages_to_sg(buf->pages, nr_pages);
	if (IS_ERR(buf->sgt)) {
		DRM_ERROR("failed to get sg table.\n");
		ret = PTR_ERR(buf->sgt);
		goto err_free_attrs;
	}

	DRM_DEBUG_KMS("dma_addr(0x%lx), size(0x%lx)\n",
			(unsigned long)buf->dma_addr, buf->size);

	return ret;

err_free_attrs:
	dma_free_attrs(dev->dev, buf->size, buf->pages,
			(dma_addr_t)buf->dma_addr, &buf->dma_attrs);
	buf->dma_addr = (dma_addr_t)NULL;

	if (!is_drm_iommu_supported(dev))
		drm_free_large(buf->pages);

	return ret;
}

static void lowlevel_buffer_deallocate(struct drm_device *dev,
		unsigned int flags, struct exynos_drm_gem_buf *buf)
{
	DRM_DEBUG_KMS("%s.\n", __FILE__);

	if (!buf->dma_addr) {
		DRM_DEBUG_KMS("dma_addr is invalid.\n");
		return;
	}

	DRM_DEBUG_KMS("dma_addr(0x%lx), size(0x%lx)\n",
			(unsigned long)buf->dma_addr, buf->size);

	sg_free_table(buf->sgt);

	kfree(buf->sgt);
	buf->sgt = NULL;

	if (!is_drm_iommu_supported(dev)) {
		dma_free_attrs(dev->dev, buf->size, buf->kvaddr,
				(dma_addr_t)buf->dma_addr, &buf->dma_attrs);
		drm_free_large(buf->pages);
	} else
		dma_free_attrs(dev->dev, buf->size, buf->pages,
				(dma_addr_t)buf->dma_addr, &buf->dma_attrs);

	buf->dma_addr = (dma_addr_t)NULL;
}

struct exynos_drm_gem_buf *exynos_drm_init_buf(struct drm_device *dev,
						unsigned int size)
{
	struct exynos_drm_gem_buf *buffer;

	DRM_DEBUG_KMS("%s.\n", __FILE__);
	DRM_DEBUG_KMS("desired size = 0x%x\n", size);

	buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer) {
		DRM_ERROR("failed to allocate exynos_drm_gem_buf.\n");
		return NULL;
	}

	buffer->size = size;
	return buffer;
}

void exynos_drm_fini_buf(struct drm_device *dev,
				struct exynos_drm_gem_buf *buffer)
{
	DRM_DEBUG_KMS("%s.\n", __FILE__);

	if (!buffer) {
		DRM_DEBUG_KMS("buffer is null.\n");
		return;
	}

	kfree(buffer);
	buffer = NULL;
}

int exynos_drm_alloc_buf(struct drm_device *dev,
		struct exynos_drm_gem_buf *buf, unsigned int flags)
{

	/*
	 * allocate memory region and set the memory information
	 * to dma_addr of a buffer object.
	 */
	if (lowlevel_buffer_allocate(dev, flags, buf) < 0)
		return -ENOMEM;

	return 0;
}

void exynos_drm_free_buf(struct drm_device *dev,
		unsigned int flags, struct exynos_drm_gem_buf *buffer)
{

	lowlevel_buffer_deallocate(dev, flags, buffer);
}
