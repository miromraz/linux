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

	/* reset acknowledged by IRQ, or only visible in STATUS_0 */
	bool ack_reset;
	bool ack_status_only;
	unsigned int resets;

	unsigned int allocs, frees, fail_alloc_at;
	unsigned int to_device;

	struct camss_buffer *returned[PIX_TEST_NBUF * 2];
	enum vb2_buffer_state state[PIX_TEST_NBUF * 2];
	u32 seq[PIX_TEST_NBUF * 2];
	unsigned int nreturned;

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
		complete(&vfe->reset_complete);
	else if (t->ack_status_only)
		writel_relaxed(STATUS_0_RESET_ACK, vfe->base + VFE_IRQ_STATUS_0);
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

static void pix_test_to_device(struct vfe_device *vfe, struct vfe_pix_staging *st,
			       struct vfe_pix_frame *f)
{
	pix_test_ctx()->to_device++;
}

static void pix_test_return_buf(struct camss_buffer *buf, enum vb2_buffer_state state)
{
	struct pix_test *t = pix_test_ctx();

	if (t->nreturned < ARRAY_SIZE(t->returned)) {
		t->returned[t->nreturned] = buf;
		t->state[t->nreturned] = state;
		t->seq[t->nreturned] = buf->vb.sequence;
	}
	t->nreturned++;
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

	return 0;
}

static void pix_test_exit(struct kunit *test)
{
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

/* A composite done IRQ, then let the copy worker run */
static void pix_frame(struct pix_test *t)
{
	vfe_pix_buf_done(&t->vfe);
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
	KUNIT_EXPECT_EQ(test, t->to_device, 4);
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
	vfe_pix_buf_done(&t->vfe);	/* staging 0 -> copy, blocked */
	KUNIT_ASSERT_TRUE(test, wait_for_completion_timeout(&t->copy_entered, HZ));

	for (i = 0; i < 6; i++) {
		vfe_pix_buf_done(&t->vfe);
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
	vfe_pix_buf_done(&t->vfe);
	KUNIT_ASSERT_TRUE(test, wait_for_completion_timeout(&t->copy_entered, HZ));
	vfe_pix_buf_done(&t->vfe);	/* second frame ready, queued behind */

	INIT_DELAYED_WORK(&t->release_work, pix_test_release_copy);
	schedule_delayed_work(&t->release_work, msecs_to_jiffies(50));
	KUNIT_EXPECT_EQ(test, pix_stop(t, pix_line(t)), 0);
	flush_delayed_work(&t->release_work);

	/* the copy in flight finished; the ready one was not started */
	KUNIT_EXPECT_EQ(test, t->nreturned, 1);
	KUNIT_EXPECT_EQ(test, t->ncopied, 1);
	vfe_pix_buf_done(&t->vfe);	/* late IRQ */
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

/* The acknowledge is in STATUS_0 but its IRQ was lost: a confirmed reset */
static void pix_test_reset_ack_without_irq(struct kunit *test)
{
	struct pix_test *t = test->priv;

	t->ack_reset = true;
	KUNIT_ASSERT_EQ(test, pix_start(t, pix_line(t)), 0);

	t->ack_reset = false;
	t->ack_status_only = true;
	KUNIT_EXPECT_EQ(test, pix_stop(t, pix_line(t)), 0);
	KUNIT_EXPECT_FALSE(test, t->vfe.wedged);
	KUNIT_EXPECT_EQ(test, t->frees, t->allocs);
	pix_assert_released(test, t);
}

/*
 * Unconfirmed reset: no DMA frame is freed or reused, the VFE refuses to
 * stream, queued vb2 buffers (never DMA targets) remain returnable.
 */
static void pix_test_reset_unconfirmed(struct kunit *test)
{
	struct pix_test *t = test->priv;

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
	KUNIT_EXPECT_TRUE(test, t->vfe.wedged);
	pix_assert_released(test, t);
	pix_expect_driver_targets(test, t);
	KUNIT_EXPECT_PTR_EQ(test, vfe_buf_get_pending(&pix_line(t)->output), &t->buf[1]);

	t->ack_reset = true;
	KUNIT_EXPECT_EQ(test, pix_start(t, pix_line(t)), -EIO);
	KUNIT_EXPECT_EQ(test, t->allocs, VFE_PIX_STAGING_NUM + 1);
	KUNIT_EXPECT_EQ(test, t->vfe.stream_count, 0);
}

static struct kunit_case pix_test_cases[] = {
	KUNIT_CASE(pix_test_pix_then_rdi),
	KUNIT_CASE(pix_test_rdi_then_pix),
	KUNIT_CASE(pix_test_arm_driver_frames),
	KUNIT_CASE(pix_test_copy_order_and_starvation),
	KUNIT_CASE(pix_test_frame_error),
	KUNIT_CASE(pix_test_no_reuse_during_copy),
	KUNIT_CASE(pix_test_stop_during_copy),
	KUNIT_CASE(pix_test_start_unwind_and_retry),
	KUNIT_CASE(pix_test_start_failures),
	KUNIT_CASE(pix_test_reset_ack_without_irq),
	KUNIT_CASE(pix_test_reset_unconfirmed),
	{}
};

static struct kunit_suite pix_test_suite = {
	.name = "camss-vfe170-pix",
	.init = pix_test_init,
	.exit = pix_test_exit,
	.test_cases = pix_test_cases,
};

kunit_test_suite(pix_test_suite);
