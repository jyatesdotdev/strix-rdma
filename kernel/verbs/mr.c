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
	if (m->map_nents)
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
	return 0;
}
/* NHI DMA address for an iova inside the MR, and the contiguous bytes
 * available there (bounded by the DMA segment). Never spans segments. */
dma_addr_t sn_mr_dma(struct sn_mr *m, u64 address, u32 *max_len)
{
	struct scatterlist *sg;
	u64 skip = address - m->base + ib_umem_offset(m->umem), off = 0;
	int i;

	*max_len = 0;
	for_each_sg(m->umem->sgt_append.sgt.sgl, sg, m->map_nents, i) {
		if (skip < off + sg_dma_len(sg)) {
			u64 inner = skip - off;
			*max_len = sg_dma_len(sg) - inner;
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
	list_add_tail(&m->entry, &d->mrs);
	d->mr_count++; d->pinned += pinned;
	mutex_unlock(&d->lock);
	return &m->ib;
fail:
	mutex_unlock(&d->lock);
	kfree(m);
	return ERR_PTR(ret);
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
	if (m->map_nents)
		dma_unmap_sg(m->map_dev, m->umem->sgt_append.sgt.sgl,
			m->umem->sgt_append.sgt.orig_nents, DMA_BIDIRECTIONAL);
	ib_umem_release(m->umem);
	mutex_unlock(&d->lock);
	kfree(m);
	return 0;
}
