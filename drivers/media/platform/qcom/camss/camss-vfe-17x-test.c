// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the Titan 170 PIX lifecycle: exclusivity, start failure
 * unwind, staging ownership, copy ordering, starvation and stop with and
 * without a confirmed reset. Included by camss-vfe-17x.c so the production
 * static functions are exercised directly. The VFE is backed by plain
 * memory; reset acknowledges, DMA frame allocation, the vb2 copy and vb2
 * completion are faked.
 */
#include <kunit/device.h>
#include <kunit/static_stub.h>
#include <kunit/test.h>
#include <linux/delay.h>
#include <linux/workqueue.h>

#define PIX_TEST_W	2000
#define PIX_TEST_H	1128
#define PIX_TEST_BPL	2000
#define PIX_TEST_SIZE	(PIX_TEST_BPL * PIX_TEST_H * 3 / 2)
#define PIX_TEST_NBUF	4
#define PIX_TEST_DMA(n)	(0x80000000u + (n) * 0x01000000u)
/* vb2 buffers are CPU memory; any of these in a WM register is a bug */
#define PIX_TEST_VB2(n)	(0x10000000u + (n) * 0x01000000u)

struct pix_test {
	struct camss camss;
	struct csid_device csid;
	struct vfe_device vfe;
	struct vfe_subdev_resources vres;
	struct vfe_hw_ops ops;
	struct camss_buffer buf[PIX_TEST_NBUF];

	/* emulated RESET_ACK: set by an acknowledged reset, write-1-to-clear */
	bool ack_reset;
	bool ack_pending;
	bool ack_stuck;
	unsigned int resets;

	unsigned int allocs, frees, fail_alloc_at;
	unsigned int to_device;
	/* device handoffs seen while no WM was enabled nor address published */
	unsigned int handoff_unpublished;
	dma_addr_t handoff[VFE_PIX_STAGING_NUM + 1];
	unsigned int faults;

	struct camss_buffer *returned[PIX_TEST_NBUF * 4];
	enum vb2_buffer_state state[PIX_TEST_NBUF * 4];
	u32 seq[PIX_TEST_NBUF * 4];
	u64 ts[PIX_TEST_NBUF * 4];
	unsigned int nreturned;

	u32 syncs[32];
	unsigned int nsyncs;

	/* copies: staging frame dma per copy; optionally block the first */
	dma_addr_t copied[PIX_TEST_NBUF * 2];
	unsigned int ncopied;
	bool block_copy;
	struct completion copy_entered, copy_release;

	struct delayed_work release_work;
	struct kunit *test;
};

static const struct camss_resources pix_test_res = {
	.csid_num = 1,
};

static struct pix_test *pix_test_ctx(void)
{
	return kunit_get_current_test()->priv;
}

static void pix_test_global_reset(struct vfe_device *vfe)
{
	struct pix_test *t = container_of(vfe, struct pix_test, vfe);

	t->resets++;
	if (t->ack_reset)
		t->ack_pending = true;
}

static bool pix_test_ack_status(struct vfe_device *vfe)
{
	struct pix_test *t = pix_test_ctx();

	return t->ack_pending || t->ack_stuck;
}

static void pix_test_ack_clear(struct vfe_device *vfe)
{
	pix_test_ctx()->ack_pending = false;
}

static void pix_test_signal_fault(struct vfe_line *line)
{
	pix_test_ctx()->faults++;
}

static void *pix_test_frame_alloc(struct vfe_device *vfe, size_t size, dma_addr_t *dma)
{
	struct pix_test *t = pix_test_ctx();

	if (t->fail_alloc_at && t->allocs + 1 == t->fail_alloc_at)
		return NULL;
	*dma = PIX_TEST_DMA(t->allocs);
	t->allocs++;
	/* nothing reads it: copies are faked */
	return kunit_kzalloc(kunit_get_current_test(), 16, GFP_KERNEL);
}

static void pix_test_frame_free(struct vfe_device *vfe, size_t size,
				struct vfe_pix_frame *f)
{
	pix_test_ctx()->frees++;
}

static int pix_test_copy(struct vfe_device *vfe, struct vfe_pix_staging *st,
			 struct vfe_pix_frame *f, struct camss_buffer *buf)
{
	struct pix_test *t = pix_test_ctx();

	if (t->ncopied < ARRAY_SIZE(t->copied))
		t->copied[t->ncopied] = f->dma;
	t->ncopied++;
	if (t->block_copy) {
		t->block_copy = false;
		complete(&t->copy_entered);
		wait_for_completion(&t->copy_release);
	}
	return 0;
}

static u32 pix_reg(struct pix_test *t, u32 reg);

static void pix_test_to_device(struct vfe_device *vfe, size_t size,
			       struct vfe_pix_frame *f)
{
	struct pix_test *t = pix_test_ctx();

	if (!pix_reg(t, VFE_BUS_WM_CFG(VFE_PIX_WM_Y)) &&
	    !pix_reg(t, VFE_BUS_WM_CFG(VFE_PIX_WM_C)) &&
	    !pix_reg(t, VFE_BUS_WM_IMAGE_ADDR(VFE_PIX_WM_Y)) &&
	    !pix_reg(t, VFE_BUS_WM_IMAGE_ADDR(VFE_PIX_WM_C)) &&
	    t->handoff_unpublished < ARRAY_SIZE(t->handoff))
		t->handoff[t->handoff_unpublished++] = f->dma;
	t->to_device++;
}

static void pix_test_return_buf(struct camss_buffer *buf, enum vb2_buffer_state state)
{
	struct pix_test *t = pix_test_ctx();

	if (t->nreturned < ARRAY_SIZE(t->returned)) {
		t->returned[t->nreturned] = buf;
		t->state[t->nreturned] = state;
		t->seq[t->nreturned] = buf->vb.sequence;
		t->ts[t->nreturned] = buf->vb.vb2_buf.timestamp;
	}
	t->nreturned++;
}

static void pix_test_frame_sync(struct vfe_line *line, u32 sequence)
{
	struct pix_test *t = pix_test_ctx();

	if (t->nsyncs < ARRAY_SIZE(t->syncs))
		t->syncs[t->nsyncs] = sequence;
	t->nsyncs++;
}

static u32 pix_reg(struct pix_test *t, u32 reg)
{
	return readl_relaxed(t->vfe.base + reg);
}

static int pix_test_init(struct kunit *test)
{
	struct pix_test *t;
	struct device *dev;
	unsigned int i;

	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t);
	test->priv = t;
	t->test = test;

	dev = kunit_device_register(test, "camss-pix-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	t->camss.res = &pix_test_res;
	t->camss.dev = dev;
	t->camss.csid = &t->csid;
	t->csid.camss = &t->camss;

	t->ops = vfe_ops_170;
	t->ops.subdev_init(dev, &t->vfe);
	t->ops.global_reset = pix_test_global_reset;
	t->vres.has_pix = true;
	t->vres.line_num = 4;
	t->vres.hw_ops = &t->ops;

	t->vfe.camss = &t->camss;
	t->vfe.res = &t->vres;
	t->vfe.base = kunit_kzalloc(test, 0x3000, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t->vfe.base);
	mutex_init(&t->vfe.stream_lock);
	spin_lock_init(&t->vfe.output_lock);
	init_completion(&t->vfe.reset_complete);
	for (i = 0; i < ARRAY_SIZE(t->vfe.wm_output_map); i++)
		t->vfe.wm_output_map[i] = VFE_LINE_NONE;

	for (i = 0; i < t->vres.line_num; i++) {
		struct vfe_line *l = &t->vfe.line[i];
		struct v4l2_pix_format_mplane *pix = &l->video_out.active_fmt.fmt.pix_mp;

		l->id = i;
		l->output.state = VFE_OUTPUT_OFF;
		INIT_LIST_HEAD(&l->output.pending_bufs);
		init_completion(&l->output.reg_update);
		l->fmt[MSM_VFE_PAD_SINK].code = MEDIA_BUS_FMT_SGRBG10_1X10;
		l->fmt[MSM_VFE_PAD_SINK].width = PIX_TEST_W;
		l->fmt[MSM_VFE_PAD_SINK].height = PIX_TEST_H;
		l->fmt[MSM_VFE_PAD_SRC] = l->fmt[MSM_VFE_PAD_SINK];
		l->fmt[MSM_VFE_PAD_SRC].code = MEDIA_BUS_FMT_YUYV8_1_5X8;
		pix->pixelformat = V4L2_PIX_FMT_NV12;
		pix->num_planes = 1;
		pix->width = PIX_TEST_W;
		pix->height = PIX_TEST_H;
		pix->plane_fmt[0].bytesperline = PIX_TEST_BPL;
		pix->plane_fmt[0].sizeimage = PIX_TEST_SIZE;
	}

	for (i = 0; i < PIX_TEST_NBUF; i++)
		t->buf[i].addr[0] = PIX_TEST_VB2(i);

	init_completion(&t->copy_entered);
	init_completion(&t->copy_release);

	camss_titan170_pix = true;
	kunit_activate_static_stub(test, vfe_pix_frame_alloc, pix_test_frame_alloc);
	kunit_activate_static_stub(test, vfe_pix_frame_free, pix_test_frame_free);
	kunit_activate_static_stub(test, vfe_pix_copy, pix_test_copy);
	kunit_activate_static_stub(test, vfe_pix_frame_to_device, pix_test_to_device);
	kunit_activate_static_stub(test, vfe_pix_return_buf, pix_test_return_buf);
	kunit_activate_static_stub(test, vfe_reset_ack_status, pix_test_ack_status);
	kunit_activate_static_stub(test, vfe_reset_ack_clear, pix_test_ack_clear);
	kunit_activate_static_stub(test, vfe_pix_signal_fault, pix_test_signal_fault);
	kunit_activate_static_stub(test, vfe_pix_frame_sync, pix_test_frame_sync);

	return 0;
}

/* Test-only: drop this test's retained entries, which production never frees */
static struct vfe_pix_quarantine *pix_test_quarantine(struct pix_test *t)
{
	struct vfe_pix_quarantine *q, *found = NULL;

	mutex_lock(&vfe_pix_quarantine_lock);
	list_for_each_entry(q, &vfe_pix_quarantine_list, list)
		if (q->dev == t->camss.dev)
			found = q;
	mutex_unlock(&vfe_pix_quarantine_lock);

	return found;
}

static void pix_test_exit(struct kunit *test)
{
	struct pix_test *t = test->priv;
	struct vfe_pix_quarantine *q;

	while ((q = pix_test_quarantine(t))) {
		mutex_lock(&vfe_pix_quarantine_lock);
		list_del(&q->list);
		mutex_unlock(&vfe_pix_quarantine_lock);
		put_device(q->dev);
		kfree(q);
	}
	camss_titan170_pix = false;
}

static struct vfe_line *pix_line(struct pix_test *t)
{
	return &t->vfe.line[VFE_LINE_PIX];
}

static struct vfe_pix_staging *pix_st(struct pix_test *t)
{
	return &pix_line(t)->output.pix;
}

static void pix_queue(struct pix_test *t, unsigned int i)
{
	unsigned long flags;

	spin_lock_irqsave(&t->vfe.output_lock, flags);
	vfe_buf_add_pending(&pix_line(t)->output, &t->buf[i]);
	spin_unlock_irqrestore(&t->vfe.output_lock, flags);
}

/*
 * The copy worker runs in a kworker, where static stubs would not find the
 * test; run the production worker with the test as current context.
 */
static void pix_test_copy_work(struct work_struct *work)
{
	struct vfe_pix_staging *st = container_of(work, struct vfe_pix_staging, copy_work);
	struct vfe_output *output = container_of(st, struct vfe_output, pix);
	struct vfe_line *line = container_of(output, struct vfe_line, output);
	struct pix_test *t = container_of(to_vfe(line), struct pix_test, vfe);
	struct kunit *prev = current->kunit_test;

	current->kunit_test = t->test;
	vfe_pix_copy_work(work);
	current->kunit_test = prev;
}

/* What vfe_set_stream() does before calling the backend */
static int pix_start(struct pix_test *t, struct vfe_line *line)
{
	int ret;

	line->output.state = VFE_OUTPUT_RESERVED;
	ret = t->vfe.res->hw_ops->vfe_enable(line);
	if (!ret && vfe_line_is_pix(line))
		line->output.pix.copy_work.func = pix_test_copy_work;

	return ret;
}

static int pix_stop(struct pix_test *t, struct vfe_line *line)
{
	return t->vfe.res->hw_ops->vfe_disable(line);
}

/* One IRQ snapshot with PIX SOF and/or image composite done */
static void pix_snap(struct pix_test *t, bool sof, bool done, u64 ts)
{
	vfe_pix_frame_irq(&t->vfe, sof, done, ts);
}

static void pix_sof(struct pix_test *t)
{
	pix_snap(t, true, false, 0);
}

static void pix_done(struct pix_test *t)
{
	pix_snap(t, false, true, 0);
}

/* A frame: SOF, then its composite done; then let the copy worker run */
static void pix_frame(struct pix_test *t)
{
	pix_sof(t);
	pix_done(t);
	flush_work(&pix_st(t)->copy_work);
}

/* The WM address registers only ever hold driver DMA frames */
static void pix_expect_driver_targets(struct kunit *test, struct pix_test *t)
{
	u32 y = pix_reg(t, VFE_BUS_WM_IMAGE_ADDR(VFE_PIX_WM_Y));
	u32 c = pix_reg(t, VFE_BUS_WM_IMAGE_ADDR(VFE_PIX_WM_C));

	KUNIT_EXPECT_GE(test, y, PIX_TEST_DMA(0));
	KUNIT_EXPECT_LT(test, y, PIX_TEST_DMA(VFE_PIX_STAGING_NUM + 1));
	KUNIT_EXPECT_EQ(test, c, y + PIX_TEST_BPL * PIX_TEST_H);
}

static void pix_assert_released(struct kunit *test, struct pix_test *t)
{
	struct vfe_output *output = &pix_line(t)->output;
	unsigned int i;

	KUNIT_EXPECT_EQ(test, t->vfe.stream_count, 0);
	KUNIT_EXPECT_FALSE(test, t->vfe.pix_active);
	KUNIT_EXPECT_EQ(test, t->vfe.wm_output_map[VFE_PIX_WM_Y], VFE_LINE_NONE);
	KUNIT_EXPECT_EQ(test, t->vfe.wm_output_map[VFE_PIX_WM_C], VFE_LINE_NONE);
	KUNIT_EXPECT_EQ(test, output->state, VFE_OUTPUT_OFF);
	for (i = 0; i <= VFE_PIX_STAGING_NUM; i++)
		KUNIT_EXPECT_NULL(test, output->pix.frame[i].cpu);
}

/* PIX first: an RDI start on the same VFE is refused, nothing is touched */
static void pix_test_pix_then_rdi(struct kunit *test)
{
	struct pix_test *t = test->priv;
	struct vfe_line *rdi = &t->vfe.line[VFE_LINE_RDI0];

	t->ack_reset = true;
	KUNIT_ASSERT_EQ(test, pix_start(t, pix_line(t)), 0);
	KUNIT_EXPECT_TRUE(test, t->vfe.pix_active);
	KUNIT_EXPECT_EQ(test, t->vfe.stream_count, 1);

	KUNIT_EXPECT_EQ(test, pix_start(t, rdi), -EBUSY);
	KUNIT_EXPECT_EQ(test, t->vfe.stream_count, 1);
	KUNIT_EXPECT_EQ(test, t->vfe.wm_output_map[VFE_LINE_RDI0], VFE_LINE_NONE);

	KUNIT_EXPECT_EQ(test, pix_stop(t, pix_line(t)), 0);
	pix_assert_released(test, t);
	KUNIT_EXPECT_EQ(test, t->frees, t->allocs);
}

/* One line at a time: a second RDI is refused too */
static void pix_test_rdi_then_rdi(struct kunit *test)
{
	struct pix_test *t = test->priv;

	t->vfe.stream_count = 1;	/* RDI0 streaming, as left by vfe_enable() */
	KUNIT_EXPECT_EQ(test, pix_start(t, &t->vfe.line[VFE_LINE_RDI1]), -EBUSY);
	KUNIT_EXPECT_EQ(test, t->vfe.stream_count, 1);
	KUNIT_EXPECT_EQ(test, t->vfe.wm_output_map[VFE_LINE_RDI1], VFE_LINE_NONE);
}

/* RDI first (stream_count as left by vfe_enable()): PIX is refused */
static void pix_test_rdi_then_pix(struct kunit *test)
{
	struct pix_test *t = test->priv;

	t->vfe.stream_count = 1;
	KUNIT_EXPECT_EQ(test, pix_start(t, pix_line(t)), -EBUSY);
	KUNIT_EXPECT_EQ(test, t->vfe.stream_count, 1);
	KUNIT_EXPECT_FALSE(test, t->vfe.pix_active);
	KUNIT_EXPECT_EQ(test, t->allocs, 0);
	KUNIT_EXPECT_EQ(test, t->vfe.wm_output_map[VFE_PIX_WM_Y], VFE_LINE_NONE);
}

/*
 * Arming: queued vb2 buffers are not programmed; both slots of both planes
 * hold staging frames when the write masters are enabled.
 */
static void pix_test_arm_driver_frames(struct kunit *test)
{
	struct pix_test *t = test->priv;

	t->ack_reset = true;
	pix_queue(t, 0);
	pix_queue(t, 1);
	KUNIT_ASSERT_EQ(test, pix_start(t, pix_line(t)), 0);

	KUNIT_EXPECT_EQ(test, t->allocs, VFE_PIX_STAGING_NUM + 1);
	/* every staging frame and the drop frame went to the device first */
	KUNIT_ASSERT_EQ(test, t->handoff_unpublished, VFE_PIX_STAGING_NUM + 1);
	for (unsigned int i = 0; i <= VFE_PIX_STAGING_NUM; i++)
		KUNIT_EXPECT_EQ(test, t->handoff[i], PIX_TEST_DMA(i));
	KUNIT_EXPECT_EQ(test, pix_st(t)->frame[VFE_PIX_DROP].dma,
			PIX_TEST_DMA(VFE_PIX_DROP));
	KUNIT_EXPECT_EQ(test, pix_st(t)->hw[0], 0);
	KUNIT_EXPECT_EQ(test, pix_st(t)->hw[1], 1);
	/* the last programmed slot: staging frame 1 */
	KUNIT_EXPECT_EQ(test, pix_reg(t, VFE_BUS_WM_IMAGE_ADDR(VFE_PIX_WM_Y)),
			PIX_TEST_DMA(1));
	pix_expect_driver_targets(test, t);
	KUNIT_EXPECT_EQ(test, pix_reg(t, VFE_BUS_WM_CFG(VFE_PIX_WM_Y)), WM_CFG_LINE_MODE_EN);
	KUNIT_EXPECT_EQ(test, pix_reg(t, VFE_BUS_WM_CFG(VFE_PIX_WM_C)), WM_CFG_LINE_MODE_EN);
	KUNIT_EXPECT_EQ(test, pix_reg(t, VFE_BUS_COMP_MASK(VFE_PIX_COMP_GRP)),
			BIT(VFE_PIX_WM_Y) | BIT(VFE_PIX_WM_C));
	KUNIT_EXPECT_EQ(test, pix_reg(t, VFE_BUS_WM_STRIDE(VFE_PIX_WM_C)), PIX_TEST_BPL);
	KUNIT_EXPECT_EQ(test, pix_reg(t, VFE_BUS_WM_BUFFER_HEIGHT_CFG(VFE_PIX_WM_C)),
			PIX_TEST_H / 2);

	KUNIT_EXPECT_EQ(test, pix_stop(t, pix_line(t)), 0);
}

/*
 * Frames are copied in completion order into buffers in queue order; a
 * frame with no queued buffer is dropped, its sequence number is skipped,
 * and every copied frame goes back to the device before reuse.
 */
static void pix_test_copy_order_and_starvation(struct kunit *test)
{
	struct pix_test *t = test->priv;

	t->ack_reset = true;
	pix_queue(t, 0);
	pix_queue(t, 1);
	KUNIT_ASSERT_EQ(test, pix_start(t, pix_line(t)), 0);

	pix_frame(t);	/* frame 0 in staging 0 -> buf 0 */
	pix_frame(t);	/* frame 1 in staging 1 -> buf 1 */
	pix_frame(t);	/* frame 2 in staging 2, no buffer: dropped */
	pix_queue(t, 2);
	pix_frame(t);	/* frame 3 in staging 0 (reused) -> buf 2 */

	KUNIT_ASSERT_EQ(test, t->nreturned, 3);
	KUNIT_EXPECT_PTR_EQ(test, t->returned[0], &t->buf[0]);
	KUNIT_EXPECT_PTR_EQ(test, t->returned[1], &t->buf[1]);
	KUNIT_EXPECT_PTR_EQ(test, t->returned[2], &t->buf[2]);
	KUNIT_EXPECT_EQ(test, t->seq[0], 0);
	KUNIT_EXPECT_EQ(test, t->seq[1], 1);
	KUNIT_EXPECT_EQ(test, t->seq[2], 3);
	KUNIT_EXPECT_EQ(test, t->state[2], VB2_BUF_STATE_DONE);
	KUNIT_EXPECT_EQ(test, t->copied[0], PIX_TEST_DMA(0));
	KUNIT_EXPECT_EQ(test, t->copied[1], PIX_TEST_DMA(1));
	KUNIT_EXPECT_EQ(test, t->copied[2], PIX_TEST_DMA(0));
	/* four initial handoffs, then one per copied or dropped frame */
	KUNIT_EXPECT_EQ(test, t->to_device, VFE_PIX_STAGING_NUM + 1 + 4);
	pix_expect_driver_targets(test, t);

	KUNIT_EXPECT_EQ(test, pix_stop(t, pix_line(t)), 0);
	pix_assert_released(test, t);
	KUNIT_EXPECT_EQ(test, t->frees, t->allocs);
}

/* A bus or CAMIF error during a frame fails exactly that frame's buffer */
static void pix_test_frame_error(struct kunit *test)
{
	struct pix_test *t = test->priv;

	t->ack_reset = true;
	pix_queue(t, 0);
	pix_queue(t, 1);
	KUNIT_ASSERT_EQ(test, pix_start(t, pix_line(t)), 0);

	vfe_pix_error(&t->vfe, STATUS_1_CAMIF_ERROR, 0);
	pix_frame(t);
	pix_frame(t);
	KUNIT_ASSERT_EQ(test, t->nreturned, 2);
	KUNIT_EXPECT_EQ(test, t->state[0], VB2_BUF_STATE_ERROR);
	KUNIT_EXPECT_EQ(test, t->state[1], VB2_BUF_STATE_DONE);

	KUNIT_EXPECT_EQ(test, pix_stop(t, pix_line(t)), 0);
}

/*
 * While a frame is being copied it is never handed to the hardware; with
 * every staging frame busy the hardware gets the drop frame.
 */
static void pix_test_no_reuse_during_copy(struct kunit *test)
{
	struct pix_test *t = test->priv;
	struct vfe_pix_staging *st = pix_st(t);
	unsigned int i;

	t->ack_reset = true;
	pix_queue(t, 0);
	KUNIT_ASSERT_EQ(test, pix_start(t, pix_line(t)), 0);

	t->block_copy = true;
	pix_sof(t);
	pix_done(t);			/* staging 0 -> copy, blocked */
	KUNIT_ASSERT_TRUE(test, wait_for_completion_timeout(&t->copy_entered, HZ));

	for (i = 0; i < 6; i++) {
		pix_sof(t);
		pix_done(t);
		KUNIT_EXPECT_NE(test, st->hw[0], 0);
		KUNIT_EXPECT_NE(test, st->hw[1], 0);
		KUNIT_EXPECT_NE(test, pix_reg(t, VFE_BUS_WM_IMAGE_ADDR(VFE_PIX_WM_Y)),
				PIX_TEST_DMA(0));
	}
	/* staging 1 and 2 completed and wait behind the blocked copy */
	KUNIT_EXPECT_EQ(test, st->hw[0], VFE_PIX_DROP);
	KUNIT_EXPECT_EQ(test, st->hw[1], VFE_PIX_DROP);

	complete(&t->copy_release);
	flush_work(&st->copy_work);
	KUNIT_EXPECT_EQ(test, t->nreturned, 1);
	KUNIT_EXPECT_EQ(test, st->free, GENMASK(VFE_PIX_STAGING_NUM - 1, 0));

	KUNIT_EXPECT_EQ(test, pix_stop(t, pix_line(t)), 0);
	pix_assert_released(test, t);
}

static void pix_test_release_copy(struct work_struct *work)
{
	struct pix_test *t = container_of(to_delayed_work(work), struct pix_test,
					  release_work);

	complete(&t->copy_release);
}

/*
 * Stop with a copy in flight: the stop waits for it, the buffer is
 * completed before the stop returns, IRQs after STOPPING queue nothing.
 */
static void pix_test_stop_during_copy(struct kunit *test)
{
	struct pix_test *t = test->priv;

	t->ack_reset = true;
	pix_queue(t, 0);
	pix_queue(t, 1);
	KUNIT_ASSERT_EQ(test, pix_start(t, pix_line(t)), 0);

	t->block_copy = true;
	pix_sof(t);
	pix_done(t);
	KUNIT_ASSERT_TRUE(test, wait_for_completion_timeout(&t->copy_entered, HZ));
	pix_sof(t);
	pix_done(t);			/* second frame ready, queued behind */

	INIT_DELAYED_WORK(&t->release_work, pix_test_release_copy);
	schedule_delayed_work(&t->release_work, msecs_to_jiffies(50));
	KUNIT_EXPECT_EQ(test, pix_stop(t, pix_line(t)), 0);
	flush_delayed_work(&t->release_work);

	/* the copy in flight finished; the ready one was not started */
	KUNIT_EXPECT_EQ(test, t->nreturned, 1);
	KUNIT_EXPECT_EQ(test, t->ncopied, 1);
	pix_snap(t, true, true, 0);	/* late IRQ */
	KUNIT_EXPECT_FALSE(test, work_pending(&pix_st(t)->copy_work));
	pix_assert_released(test, t);
	/* buf 1 never left the pending list: the caller's flush returns it */
	KUNIT_EXPECT_PTR_EQ(test, vfe_buf_get_pending(&pix_line(t)->output), &t->buf[1]);
}

/*
 * Downstream start failure: the video node unwinds by stopping the VFE.
 * Ownership is released and a retry restores the IRQ masks the stop
 * reset cleared.
 */
static void pix_test_start_unwind_and_retry(struct kunit *test)
{
	struct pix_test *t = test->priv;

	t->ack_reset = true;
	pix_queue(t, 0);
	KUNIT_ASSERT_EQ(test, pix_start(t, pix_line(t)), 0);
	KUNIT_EXPECT_EQ(test, pix_stop(t, pix_line(t)), 0);
	pix_assert_released(test, t);
	KUNIT_EXPECT_EQ(test, t->nreturned, 0);

	writel_relaxed(MASK_0_RESET_ACK, t->vfe.base + VFE_IRQ_MASK_0);
	KUNIT_ASSERT_EQ(test, pix_start(t, pix_line(t)), 0);
	KUNIT_EXPECT_EQ(test, pix_reg(t, VFE_IRQ_MASK_0), ~0u);
	KUNIT_EXPECT_EQ(test, t->vfe.stream_count, 1);
	KUNIT_EXPECT_EQ(test, pix_stop(t, pix_line(t)), 0);
	pix_assert_released(test, t);
	KUNIT_EXPECT_EQ(test, t->frees, t->allocs);
}

/* Failed starts leave no allocation, count or WM ownership behind */
static void pix_test_start_failures(struct kunit *test)
{
	struct pix_test *t = test->priv;
	struct v4l2_pix_format_mplane *pix =
		&pix_line(t)->video_out.active_fmt.fmt.pix_mp;

	t->vfe.wm_output_map[VFE_PIX_WM_C] = VFE_LINE_RDI0;
	KUNIT_EXPECT_EQ(test, pix_start(t, pix_line(t)), -EBUSY);
	KUNIT_EXPECT_EQ(test, t->frees, t->allocs);
	t->vfe.wm_output_map[VFE_PIX_WM_C] = VFE_LINE_NONE;

	t->fail_alloc_at = t->allocs + 3;	/* the third frame of this start */
	KUNIT_EXPECT_EQ(test, pix_start(t, pix_line(t)), -ENOMEM);
	KUNIT_EXPECT_EQ(test, t->frees, t->allocs);
	t->fail_alloc_at = 0;

	pix->plane_fmt[0].bytesperline = 4096;	/* user stride, size kept */
	KUNIT_EXPECT_EQ(test, pix_start(t, pix_line(t)), -EINVAL);

	KUNIT_EXPECT_EQ(test, t->vfe.stream_count, 0);
	KUNIT_EXPECT_FALSE(test, t->vfe.pix_active);
	KUNIT_EXPECT_EQ(test, pix_reg(t, VFE_BUS_WM_CFG(VFE_PIX_WM_Y)), 0);
}

/*
 * The reset is confirmed from STATUS_0 alone, with the ISR off the line:
 * the IRQ completion plays no part.
 */
static void pix_test_reset_polled(struct kunit *test)
{
	struct pix_test *t = test->priv;

	t->ack_reset = true;
	KUNIT_ASSERT_EQ(test, pix_start(t, pix_line(t)), 0);
	KUNIT_EXPECT_EQ(test, pix_stop(t, pix_line(t)), 0);
	KUNIT_EXPECT_EQ(test, t->resets, 1);
	KUNIT_EXPECT_FALSE(test, completion_done(&t->vfe.reset_complete));
	KUNIT_EXPECT_FALSE(test, t->ack_pending);
	KUNIT_EXPECT_FALSE(test, t->vfe.wedged);
	KUNIT_EXPECT_EQ(test, t->frees, t->allocs);
}

/*
 * A stale acknowledge is drained before the reset; one that does not clear
 * fails closed without issuing a reset it could stand in for.
 */
static void pix_test_reset_stale_ack(struct kunit *test)
{
	struct pix_test *t = test->priv;

	/* stale but clearable: drained, then the fresh one is required */
	t->ack_pending = true;
	t->ack_reset = false;
	KUNIT_EXPECT_LT(test, vfe_reset(&t->vfe), 0);
	KUNIT_EXPECT_EQ(test, t->resets, 1);
	KUNIT_EXPECT_TRUE(test, t->vfe.wedged);

	/* after a missing acknowledge nothing is retried, a late one is ignored */
	t->ack_pending = true;
	t->ack_reset = true;
	KUNIT_EXPECT_EQ(test, vfe_reset(&t->vfe), -EIO);
	KUNIT_EXPECT_EQ(test, t->resets, 1);
	KUNIT_EXPECT_EQ(test, pix_start(t, pix_line(t)), -EIO);
	KUNIT_EXPECT_EQ(test, pix_start(t, &t->vfe.line[VFE_LINE_RDI0]), -EIO);
}

static void pix_test_reset_stuck_ack(struct kunit *test)
{
	struct pix_test *t = test->priv;

	t->ack_stuck = true;
	t->ack_reset = true;
	KUNIT_EXPECT_EQ(test, vfe_reset(&t->vfe), -EIO);
	KUNIT_EXPECT_EQ(test, t->resets, 0);
	KUNIT_EXPECT_TRUE(test, t->vfe.wedged);
}

/*
 * A bus fault reported with a completion in the same interrupt: the real
 * ISR handles the error first, nothing is copied from then on, the queue
 * is failed once, the hardware only gets the drop frame.
 */
static void pix_test_bus_fault_fails_closed(struct kunit *test)
{
	struct pix_test *t = test->priv;
	struct vfe_pix_staging *st = pix_st(t);
	unsigned int i;

	t->ack_reset = true;
	pix_queue(t, 0);
	pix_queue(t, 1);
	pix_queue(t, 2);
	KUNIT_ASSERT_EQ(test, pix_start(t, pix_line(t)), 0);
	pix_frame(t);			/* frame 0 delivered */
	KUNIT_ASSERT_EQ(test, t->nreturned, 1);

	writel_relaxed(BIT(9), t->vfe.base + VFE_IRQ_STATUS_0);
	writel_relaxed(STATUS0_OVERFLOW | STATUS0_COMP_BUF_DONE(VFE_PIX_COMP_GRP),
		       t->vfe.base + VFE_BUS_IRQ_STATUS(0));
	vfe_isr(0, &t->vfe);
	writel_relaxed(0, t->vfe.base + VFE_BUS_IRQ_STATUS(0));
	flush_work(&st->copy_work);

	KUNIT_EXPECT_EQ(test, t->faults, 1);
	KUNIT_EXPECT_TRUE(test, st->faulted);
	KUNIT_EXPECT_EQ(test, st->nready, 0);
	for (i = 0; i < 4; i++)
		pix_frame(t);
	KUNIT_EXPECT_EQ(test, t->nreturned, 1);
	KUNIT_EXPECT_EQ(test, t->ncopied, 1);
	KUNIT_EXPECT_EQ(test, t->faults, 1);
	KUNIT_EXPECT_EQ(test, st->hw[0], VFE_PIX_DROP);
	KUNIT_EXPECT_EQ(test, st->hw[1], VFE_PIX_DROP);

	KUNIT_EXPECT_EQ(test, pix_stop(t, pix_line(t)), 0);
	pix_assert_released(test, t);
	KUNIT_EXPECT_EQ(test, t->frees, t->allocs);
}

/*
 * Unconfirmed reset: no DMA frame is freed or reused, the VFE refuses to
 * stream, queued vb2 buffers (never DMA targets) remain returnable.
 */
static void pix_test_reset_unconfirmed(struct kunit *test)
{
	struct pix_test *t = test->priv;
	struct vfe_pix_quarantine *q;

	t->ack_reset = true;
	pix_queue(t, 0);
	pix_queue(t, 1);
	KUNIT_ASSERT_EQ(test, pix_start(t, pix_line(t)), 0);
	pix_frame(t);

	t->ack_reset = false;
	KUNIT_EXPECT_LT(test, pix_stop(t, pix_line(t)), 0);
	KUNIT_EXPECT_EQ(test, t->frees, 0);
	KUNIT_EXPECT_EQ(test, t->vfe.quarantined,
			(size_t)(VFE_PIX_STAGING_NUM + 1) * PIX_TEST_SIZE);
	/* the handles stay owned: every frame, with its device reference */
	q = pix_test_quarantine(t);
	KUNIT_ASSERT_NOT_NULL(test, q);
	KUNIT_EXPECT_PTR_EQ(test, q->dev, t->camss.dev);
	KUNIT_EXPECT_EQ(test, q->size, PIX_TEST_SIZE);
	for (unsigned int i = 0; i <= VFE_PIX_STAGING_NUM; i++) {
		KUNIT_EXPECT_NOT_NULL(test, q->frame[i].cpu);
		KUNIT_EXPECT_EQ(test, q->frame[i].dma, PIX_TEST_DMA(i));
	}
	KUNIT_EXPECT_TRUE(test, t->vfe.wedged);
	pix_assert_released(test, t);
	pix_expect_driver_targets(test, t);
	KUNIT_EXPECT_PTR_EQ(test, vfe_buf_get_pending(&pix_line(t)->output), &t->buf[1]);

	t->ack_reset = true;
	KUNIT_EXPECT_EQ(test, pix_start(t, pix_line(t)), -EIO);
	KUNIT_EXPECT_EQ(test, t->allocs, VFE_PIX_STAGING_NUM + 1);
	KUNIT_EXPECT_EQ(test, t->vfe.stream_count, 0);
}

/*
 * Each delivered buffer carries its own SOF sequence and the done time;
 * the token keeps the SOF time. Each restart is a new generation counting
 * from 0.
 */
static void pix_test_token_and_generation(struct kunit *test)
{
	struct pix_test *t = test->priv;
	unsigned int run, n;

	t->ack_reset = true;
	for (run = 0; run < 3; run++) {
		t->nreturned = 0;
		for (n = 0; n < 3; n++)
			pix_queue(t, n);
		KUNIT_ASSERT_EQ(test, pix_start(t, pix_line(t)), 0);
		KUNIT_EXPECT_EQ(test, pix_st(t)->generation, run + 1);

		for (n = 0; n < 3; n++) {
			pix_snap(t, true, false, 1000 + 100 * n);	/* SOF */
			pix_snap(t, false, true, 1050 + 100 * n);	/* done */
			flush_work(&pix_st(t)->copy_work);
			KUNIT_EXPECT_EQ(test, pix_st(t)->frame[n].token.timestamp,
					1000 + 100 * n);
			KUNIT_EXPECT_EQ(test, pix_st(t)->frame[n].token.generation,
					run + 1);
		}

		KUNIT_ASSERT_EQ(test, t->nreturned, 3);
		for (n = 0; n < 3; n++) {
			KUNIT_EXPECT_EQ(test, t->seq[n], n);
			KUNIT_EXPECT_EQ(test, t->ts[n], 1050 + 100 * n);
			KUNIT_EXPECT_EQ(test, t->state[n], VB2_BUF_STATE_DONE);
		}
		KUNIT_EXPECT_EQ(test, t->faults, 0);
		KUNIT_EXPECT_EQ(test, pix_stop(t, pix_line(t)), 0);
	}
}

/*
 * SOF and done in one snapshot: done then SOF would be legal, SOF then
 * done not, and the status cannot tell. Fail closed, deliver nothing.
 */
static void pix_test_same_snapshot_fails_closed(struct kunit *test)
{
	struct pix_test *t = test->priv;

	t->ack_reset = true;
	pix_queue(t, 0);
	pix_queue(t, 1);
	KUNIT_ASSERT_EQ(test, pix_start(t, pix_line(t)), 0);

	pix_sof(t);
	pix_snap(t, true, true, 0);
	flush_work(&pix_st(t)->copy_work);
	KUNIT_EXPECT_EQ(test, t->faults, 1);
	KUNIT_EXPECT_EQ(test, t->nreturned, 0);
	KUNIT_EXPECT_EQ(test, t->nsyncs, 2);
	KUNIT_EXPECT_EQ(test, pix_stop(t, pix_line(t)), 0);
}

/* Every SOF is a FRAME_SYNC with its sequence, with or without a buffer */
static void pix_test_frame_sync_events(struct kunit *test)
{
	struct pix_test *t = test->priv;
	unsigned int i;

	t->ack_reset = true;
	pix_queue(t, 0);
	KUNIT_ASSERT_EQ(test, pix_start(t, pix_line(t)), 0);
	for (i = 0; i < 4; i++)
		pix_frame(t);
	KUNIT_ASSERT_EQ(test, t->nsyncs, 4);
	for (i = 0; i < 4; i++)
		KUNIT_EXPECT_EQ(test, t->syncs[i], i);
	/* one buffer: frame 0 delivered, 1..3 dropped but sequenced */
	KUNIT_EXPECT_EQ(test, t->nreturned, 1);
	KUNIT_EXPECT_EQ(test, pix_stop(t, pix_line(t)), 0);
}

/* A completion with no started frame: fail closed */
static void pix_test_done_without_sof(struct kunit *test)
{
	struct pix_test *t = test->priv;

	t->ack_reset = true;
	pix_queue(t, 0);
	pix_queue(t, 1);
	KUNIT_ASSERT_EQ(test, pix_start(t, pix_line(t)), 0);

	pix_done(t);
	flush_work(&pix_st(t)->copy_work);
	KUNIT_EXPECT_EQ(test, t->faults, 1);
	KUNIT_EXPECT_TRUE(test, pix_st(t)->faulted);
	pix_frame(t);
	KUNIT_EXPECT_EQ(test, t->nreturned, 0);
	KUNIT_EXPECT_EQ(test, pix_st(t)->hw[1], VFE_PIX_DROP);
	KUNIT_EXPECT_EQ(test, pix_stop(t, pix_line(t)), 0);
}

/*
 * The next frame starts while the previous one is unretired: its done is
 * late, lost, or will arrive merged with a later one. Fail closed at that
 * SOF, before the slot in doubt can be read; a done arriving afterwards
 * delivers nothing.
 */
static void pix_test_unretired_at_sof_fails_closed(struct kunit *test)
{
	struct pix_test *t = test->priv;
	unsigned int i;

	t->ack_reset = true;
	for (i = 0; i < 3; i++)
		pix_queue(t, i);
	KUNIT_ASSERT_EQ(test, pix_start(t, pix_line(t)), 0);

	pix_frame(t);			/* frame 0 delivered */
	pix_sof(t);			/* frame 1 */
	KUNIT_EXPECT_EQ(test, t->faults, 0);
	pix_sof(t);			/* frame 2, frame 1 not done */
	KUNIT_EXPECT_EQ(test, t->faults, 1);
	pix_done(t);			/* late, or frames 1+2 merged */
	pix_frame(t);
	flush_work(&pix_st(t)->copy_work);
	KUNIT_EXPECT_EQ(test, t->nreturned, 1);
	KUNIT_EXPECT_EQ(test, t->seq[0], 0);
	KUNIT_EXPECT_EQ(test, t->faults, 1);
	KUNIT_EXPECT_EQ(test, pix_stop(t, pix_line(t)), 0);
}

/* A lost SOF shows as a completion without a token: fail closed */
static void pix_test_missed_sof(struct kunit *test)
{
	struct pix_test *t = test->priv;

	t->ack_reset = true;
	pix_queue(t, 0);
	pix_queue(t, 1);
	KUNIT_ASSERT_EQ(test, pix_start(t, pix_line(t)), 0);

	pix_frame(t);		/* frame 0 delivered */
	pix_done(t);		/* frame 1 completes, its SOF was never seen */
	flush_work(&pix_st(t)->copy_work);
	KUNIT_EXPECT_EQ(test, t->faults, 1);
	KUNIT_ASSERT_EQ(test, t->nreturned, 1);
	KUNIT_EXPECT_EQ(test, t->seq[0], 0);
	KUNIT_EXPECT_EQ(test, pix_stop(t, pix_line(t)), 0);
}

/*
 * Known undetected case, recorded rather than claimed safe: a lost done
 * followed by the next frame's lost SOF looks like one normal frame. The
 * model delivers without a fault; physically the slot retired at the
 * second done held the lost frame's data, so that delivery can carry a
 * neighbouring frame's data under its token. This pins the model only.
 */
static void pix_test_lost_done_and_sof_undetected(struct kunit *test)
{
	struct pix_test *t = test->priv;
	unsigned int i;

	t->ack_reset = true;
	for (i = 0; i < 3; i++)
		pix_queue(t, i);
	KUNIT_ASSERT_EQ(test, pix_start(t, pix_line(t)), 0);

	pix_sof(t);			/* frame 0; its done is lost */
					/* frame 1's SOF is lost */
	pix_done(t);			/* frame 1 done, taken for frame 0 */
	pix_frame(t);			/* frame 2 */
	flush_work(&pix_st(t)->copy_work);

	KUNIT_EXPECT_EQ(test, t->faults, 0);
	KUNIT_ASSERT_EQ(test, t->nreturned, 2);
	KUNIT_EXPECT_EQ(test, t->seq[0], 0);
	KUNIT_EXPECT_EQ(test, t->seq[1], 1);
	KUNIT_EXPECT_EQ(test, pix_stop(t, pix_line(t)), 0);
}

/* Fake MMIO proves wiring/preservation only, never counter progression. */
static void pix_test_diag_counter_observation(struct kunit *test)
{
	struct pix_test *t = test->priv;
	/* SENSOR_SEL=RDI1 (2), RDI0/1/2 counters enabled; bits 0/4 clear. */
	u32 cfg = (2u << 1) | BIT(5) | BIT(6) | BIT(7);
	u32 status = 0xa53ce781;

	t->ack_reset = true;
	writel_relaxed(cfg, t->vfe.base + VFE_DIAG_CFG);
	writel_relaxed(status, t->vfe.base + VFE_DIAG_SENSOR_FRM_CNT_STATUS);
	KUNIT_ASSERT_EQ(test, pix_start(t, pix_line(t)), 0);
	KUNIT_EXPECT_EQ(test, pix_reg(t, VFE_DIAG_CFG),
			cfg | DIAG_CFG_DIAG_EN | DIAG_CFG_CAMIF_FRM_CNT_EN);
	KUNIT_EXPECT_EQ(test, vfe_pix_diag_frame_count(&t->vfe), status);
	KUNIT_EXPECT_EQ(test, pix_reg(t, VFE_DIAG_SENSOR_FRM_CNT_STATUS), status);
	KUNIT_EXPECT_EQ(test, pix_stop(t, pix_line(t)), 0);
}

static struct kunit_case pix_test_cases[] = {
	KUNIT_CASE(pix_test_diag_counter_observation),
	KUNIT_CASE(pix_test_pix_then_rdi),
	KUNIT_CASE(pix_test_rdi_then_pix),
	KUNIT_CASE(pix_test_rdi_then_rdi),
	KUNIT_CASE(pix_test_arm_driver_frames),
	KUNIT_CASE(pix_test_copy_order_and_starvation),
	KUNIT_CASE(pix_test_frame_error),
	KUNIT_CASE(pix_test_no_reuse_during_copy),
	KUNIT_CASE(pix_test_stop_during_copy),
	KUNIT_CASE(pix_test_start_unwind_and_retry),
	KUNIT_CASE(pix_test_start_failures),
	KUNIT_CASE(pix_test_reset_polled),
	KUNIT_CASE(pix_test_reset_stale_ack),
	KUNIT_CASE(pix_test_reset_stuck_ack),
	KUNIT_CASE(pix_test_bus_fault_fails_closed),
	KUNIT_CASE(pix_test_reset_unconfirmed),
	KUNIT_CASE(pix_test_token_and_generation),
	KUNIT_CASE(pix_test_same_snapshot_fails_closed),
	KUNIT_CASE(pix_test_frame_sync_events),
	KUNIT_CASE(pix_test_done_without_sof),
	KUNIT_CASE(pix_test_unretired_at_sof_fails_closed),
	KUNIT_CASE(pix_test_missed_sof),
	KUNIT_CASE(pix_test_lost_done_and_sof_undetected),
	{}
};

static struct kunit_suite pix_test_suite = {
	.name = "camss-vfe170-pix",
	.init = pix_test_init,
	.exit = pix_test_exit,
	.test_cases = pix_test_cases,
};

kunit_test_suite(pix_test_suite);
