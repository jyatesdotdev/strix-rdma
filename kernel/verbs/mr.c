// SPDX-License-Identifier: GPL-2.0-only
#include <linux/scatterlist.h>
#include "strix_nhi.h"

struct sn_mr *sn_mr_get(struct sn_qp *q, u32 key, u64 address, u32 length, u32 access)
{
	struct sn_device *d = sn_dev(q->ib.device);
	struct sn_mr *m;

	lockdep_assert_held(&d->lock);
	list_for_each_entry(m, &d->mrs, entry) {
		if (m->ib.lkey != key || m->ib.pd != q->ib.pd)
			continue;
		if ((m->access & access) != access ||
		    !sn_range(m->base, m->size, address, length))
			return NULL;
		m->users++;
		return m;
	}
	return NULL;
}
void sn_mr_put(struct sn_mr *m)
{
	if (m) {
		lockdep_assert_held(&sn_dev(m->ib.device)->lock);
		WARN_ON(!m->users);
		m->users--;
	}
}
int sn_mr_copy(struct sn_mr *m, u64 address, void *buffer, u32 length, bool to_mr)
{
	size_t copied, skip;

	lockdep_assert_held(&sn_dev(m->ib.device)->lock);
	if (!m->users || !sn_range(m->base, m->size, address, length) ||
	    (to_mr && !(m->access & IB_ACCESS_LOCAL_WRITE)))
		return -EACCES;
	if (!length)
		return 0;
	if (m->dmabuf) {
		/* GPU memory: through the CPU mapping made at registration. An
		 * importer must not touch the exporter's scatterlist pages. */
		size_t off = address - m->base;

		if (to_mr)
			iosys_map_memcpy_to(&m->vmap, off, buffer, length);
		else
			iosys_map_memcpy_from(buffer, &m->vmap, off, length);
		return 0;
	}
	skip = address - m->base + ib_umem_offset(m->umem);
	/* VIRT_DMA umem owns pinned CPU pages; only separate coherent staging
	 * buffers are ever mapped into the NHI. sg_pcopy handles page boundaries. */
	if (to_mr)
		copied = sg_pcopy_from_buffer(m->umem->sgt_append.sgt.sgl,
			m->umem->sgt_append.sgt.orig_nents, buffer, length, skip);
	else
		copied = sg_pcopy_to_buffer(m->umem->sgt_append.sgt.sgl,
			m->umem->sgt_append.sgt.orig_nents, buffer, length, skip);
	return copied == length ? 0 : -EFAULT;
}
/* Lazily DMA-map the MR's pinned pages for the NHI device (first zero-copy
 * use); unmapped again at deregistration. The mapping device is the ring DMA
 * device, same as the coherent staging buffers. */
int sn_mr_ensure_mapped(struct sn_device *d, struct sn_mr *m)
{
	struct device *dev;
	int n;

	lockdep_assert_held(&d->lock);
	/* A GPU MR is mapped for the NHI by its exporter at registration. */
	if (m->map_nents || m->dmabuf)
		return 0;
	if (!d->rx_ring)
		return -ENODEV;
	dev = tb_ring_dma_device(d->rx_ring);
	n = dma_map_sg(dev, m->umem->sgt_append.sgt.sgl,
		m->umem->sgt_append.sgt.orig_nents, DMA_BIDIRECTIONAL);
	if (!n)
		return -ENOMEM;
	m->map_dev = dev;
	m->map_nents = n;
	/* The segment list identity may have changed; drop any stale cursor. */
	m->cur_sg = NULL; m->cur_idx = 0; m->cur_off = 0;
	return 0;
}
/* NHI DMA address for an iova inside the MR, and the contiguous bytes
 * available there (bounded by the DMA segment). Never spans segments. */
dma_addr_t sn_mr_dma(struct sn_mr *m, u64 address, u32 *max_len)
{
	struct scatterlist *sg;
	u64 skip = address - m->base + ib_umem_offset(m->umem), off;
	unsigned int i;

	*max_len = 0;
	/* Resume from the cursor when the walk moves forward, which is the normal
	 * case within a burst; otherwise restart from the head. */
	if (m->cur_sg && skip >= m->cur_off) {
		sg = m->cur_sg; off = m->cur_off; i = m->cur_idx;
	} else {
		sg = m->umem->sgt_append.sgt.sgl; off = 0; i = 0;
	}
	for (; i < (unsigned int)m->map_nents && sg; i++, sg = sg_next(sg)) {
		if (skip < off + sg_dma_len(sg)) {
			u64 inner = skip - off;

			*max_len = sg_dma_len(sg) - inner;
			m->cur_sg = sg; m->cur_idx = i; m->cur_off = off;
			return sg_dma_address(sg) + inner;
		}
		off += sg_dma_len(sg);
	}
	return 0;
}
struct ib_mr *sn_reg_user_mr(struct ib_pd *pd, u64 start, u64 length,
			   u64 iova, int access, struct ib_dmah *dmah,
			   struct ib_udata *udata)
{
	struct sn_device *d = sn_dev(pd->device);
	struct sn_mr *m;
	int ret = 0;
	u64 pinned = PAGE_ALIGN((start & ~PAGE_MASK) + length);

	if (!udata || udata->inlen || udata->outlen || dmah || access & ~SN_ACCESS_FLAGS)
		return ERR_PTR(-EOPNOTSUPP);
	if (!length || length > SN_MR_BYTES || start > ULONG_MAX ||
	    !sn_range(start, length, start, length) ||
	    !sn_range(iova, length, iova, length) ||
	    ((access & IB_ACCESS_REMOTE_WRITE) && !(access & IB_ACCESS_LOCAL_WRITE)))
		return ERR_PTR(-EINVAL);
	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return ERR_PTR(-ENOMEM);
	mutex_lock(&d->lock);
	if (d->dead) ret = -ENODEV;
	else if (d->mr_count == SN_MAX_MRS || pinned > SN_PIN_BYTES - d->pinned)
		ret = -ENOMEM;
	else if (d->key_serial == U32_MAX) ret = -EOVERFLOW;
	if (ret)
		goto fail;
	m->umem = ib_umem_get_va(&d->ib, start, length, access);
	if (IS_ERR(m->umem)) {
		ret = PTR_ERR(m->umem);
		goto fail;
	}
	m->ib.device = &d->ib;
	m->ib.pd = pd;
	m->ib.lkey = ++d->key_serial;
	m->ib.rkey = m->ib.lkey;
	m->base = iova; m->size = length; m->pinned = pinned; m->access = access;
	m->ib.iova = iova; m->ib.length = length;
	m->map_nents = 0; m->map_dev = NULL;
	m->cur_sg = NULL; m->cur_idx = 0; m->cur_off = 0;
	list_add_tail(&m->entry, &d->mrs);
	d->mr_count++; d->pinned += pinned;
	mutex_unlock(&d->lock);
	return &m->ib;
fail:
	mutex_unlock(&d->lock);
	kfree(m);
	return ERR_PTR(ret);
}
/* Register GPU memory exported as a DMA-BUF (docs/GPU_DIRECT_VERBS.md). The
 * attachment is made for the NHI's DMA device -- this IB device has none of
 * its own -- pinned, mapped and trimmed to [offset, offset + length) by the
 * core helper; the data path then uses that mapping as is. A CPU mapping is
 * made too, because the staged paths (small or unaligned operations, and the
 * zero-copy READ fallback's snapshot) copy through sn_mr_copy(). */
struct ib_mr *sn_reg_user_mr_dmabuf(struct ib_pd *pd, u64 offset, u64 length,
				   u64 iova, int fd, int access,
				   struct ib_dmah *dmah,
				   struct uverbs_attr_bundle *attrs)
{
	struct sn_device *d = sn_dev(pd->device);
	struct ib_umem_dmabuf *umem;
	struct sn_mr *m;
	u64 pinned = PAGE_ALIGN(length);
	int ret = 0;

	if (dmah || access & ~SN_ACCESS_FLAGS)
		return ERR_PTR(-EOPNOTSUPP);
	/* Page-aligned offset and iova keep a frame-aligned iova frame-aligned in
	 * DMA space; the core helper trims the table to whole pages. */
	if (!length || length > SN_MR_BYTES ||
	    (offset | iova) & ~PAGE_MASK || offset > ULONG_MAX - length ||
	    !sn_range(iova, length, iova, length) ||
	    ((access & IB_ACCESS_REMOTE_WRITE) && !(access & IB_ACCESS_LOCAL_WRITE)))
		return ERR_PTR(-EINVAL);
	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return ERR_PTR(-ENOMEM);
	mutex_lock(&d->lock);
	if (d->dead) ret = -ENODEV;
	else if (d->mr_count == SN_MAX_MRS || pinned > SN_PIN_BYTES - d->pinned)
		ret = -ENOMEM;
	else if (d->key_serial == U32_MAX) ret = -EOVERFLOW;
	if (ret)
		goto fail;
	/* Reserve the slot and quota before dropping the lock for the import,
	 * which may wait on the exporter's fences. */
	d->mr_count++; d->pinned += pinned;
	mutex_unlock(&d->lock);

	umem = ib_umem_dmabuf_get_pinned_with_dma_device(&d->ib,
		tb_service_parent(d->service)->tb->nhi->dev, offset, length,
		fd, access);
	if (IS_ERR(umem)) {
		ret = PTR_ERR(umem);
		goto unreserve;
	}
	ret = dma_buf_vmap_unlocked(umem->attach->dmabuf, &m->vmap);
	if (ret) {
		ib_umem_release(&umem->umem);
		goto unreserve;
	}
	mutex_lock(&d->lock);
	if (d->dead || d->key_serial == U32_MAX) {
		ret = d->dead ? -ENODEV : -EOVERFLOW;
		mutex_unlock(&d->lock);
		dma_buf_vunmap_unlocked(umem->attach->dmabuf, &m->vmap);
		ib_umem_release(&umem->umem);
		goto unreserve;
	}
	m->dmabuf = umem;
	m->umem = &umem->umem;
	m->ib.device = &d->ib;
	m->ib.pd = pd;
	m->ib.lkey = ++d->key_serial;
	m->ib.rkey = m->ib.lkey;
	m->base = iova; m->size = length; m->pinned = pinned; m->access = access;
	m->ib.iova = iova; m->ib.length = length;
	/* The exporter's mapping, trimmed to the MR, is the NHI mapping. */
	m->map_dev = tb_service_parent(d->service)->tb->nhi->dev;
	m->map_nents = umem->umem.sgt_append.sgt.nents;
	m->cur_sg = NULL; m->cur_idx = 0; m->cur_off = 0;
	list_add_tail(&m->entry, &d->mrs);
	mutex_unlock(&d->lock);
	return &m->ib;
unreserve:
	mutex_lock(&d->lock);
	d->mr_count--; d->pinned -= pinned;
fail:
	mutex_unlock(&d->lock);
	kfree(m);
	return ERR_PTR(ret);
}
/* Undo a registration's mappings and drop its memory: the NHI mapping of a
 * CPU MR, or the CPU mapping of a GPU MR (whose NHI mapping, pin, attachment
 * and DMA-BUF reference ib_umem_release() drops). */
static void sn_mr_release(struct sn_mr *m)
{
	if (m->dmabuf)
		dma_buf_vunmap_unlocked(m->dmabuf->attach->dmabuf, &m->vmap);
	else if (m->map_nents)
		dma_unmap_sg(m->map_dev, m->umem->sgt_append.sgt.sgl,
			m->umem->sgt_append.sgt.orig_nents, DMA_BIDIRECTIONAL);
	ib_umem_release(m->umem);
}
int sn_dereg_mr(struct ib_mr *ib, struct ib_udata *udata)
{
	struct sn_mr *m = container_of(ib, struct sn_mr, ib);
	struct sn_device *d = sn_dev(ib->device);

	mutex_lock(&d->lock);
	if (m->users) {
		mutex_unlock(&d->lock);
		return -EBUSY;
	}
	list_del(&m->entry);
	d->mr_count--; d->pinned -= m->pinned;
	sn_mr_release(m);
	mutex_unlock(&d->lock);
	kfree(m);
	return 0;
}
