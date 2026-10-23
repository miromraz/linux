// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the CAMSS video node: line based plane sizing and the
 * pipeline start unwind. Included by camss-video.c.
 */
#include <kunit/test.h>

static const struct camss_format_info video_test_formats[] = {
	{ MEDIA_BUS_FMT_YUYV8_1_5X8, 8, V4L2_PIX_FMT_NV12, 1,
	  PER_PLANE_DATA(0, 1, 1, 2, 3, 8) },
	{ MEDIA_BUS_FMT_YUYV8_1X16, 8, V4L2_PIX_FMT_YUYV, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 16) },
	{ MEDIA_BUS_FMT_SGRBG10_1X10, 10, V4L2_PIX_FMT_SGRBG10P, 1,
	  PER_PLANE_DATA(0, 1, 1, 1, 1, 10) },
};

static struct v4l2_pix_format_mplane *
video_test_try(struct camss_video *video, u32 fourcc, u32 w, u32 h, u32 bpl,
	       u32 size, struct v4l2_format *f)
{
	memset(f, 0, sizeof(*f));
	f->fmt.pix_mp.pixelformat = fourcc;
	f->fmt.pix_mp.width = w;
	f->fmt.pix_mp.height = h;
	f->fmt.pix_mp.num_planes = 1;
	f->fmt.pix_mp.plane_fmt[0].bytesperline = bpl;
	f->fmt.pix_mp.plane_fmt[0].sizeimage = size;
	__video_try_fmt(video, f);

	return &f->fmt.pix_mp;
}

static struct camss_video *video_test_video(struct kunit *test, bool line_based)
{
	struct camss_video *video = kunit_kzalloc(test, sizeof(*video), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, video);
	video->formats = video_test_formats;
	video->nformats = ARRAY_SIZE(video_test_formats);
	video->bpl_alignment = 16;
	video->line_based = line_based;

	return video;
}

/* NV12 at the 4096 line limit gets the whole 3/2 plane */
static void video_test_nv12_max_height(struct kunit *test)
{
	struct v4l2_pix_format_mplane *pix;
	struct camss_video *video;
	struct v4l2_format f;

	video = video_test_video(test, true);
	pix = video_test_try(video, V4L2_PIX_FMT_NV12, 2000, 4096, 0, 0, &f);
	KUNIT_EXPECT_EQ(test, pix->height, 4096);
	KUNIT_EXPECT_EQ(test, pix->plane_fmt[0].bytesperline, 2000);
	KUNIT_EXPECT_EQ(test, pix->plane_fmt[0].sizeimage, 12288000);

	/* above the line limit the height is clamped, not the plane */
	pix = video_test_try(video, V4L2_PIX_FMT_NV12, 2000, 6000, 0, 0, &f);
	KUNIT_EXPECT_EQ(test, pix->height, 4096);
	KUNIT_EXPECT_EQ(test, pix->plane_fmt[0].sizeimage, 12288000);

	/* a requested larger plane is honoured up to the NV12 maximum */
	pix = video_test_try(video, V4L2_PIX_FMT_NV12, 2000, 1128, 2000, ~0u, &f);
	KUNIT_EXPECT_EQ(test, pix->plane_fmt[0].sizeimage, 2000 * 4096 * 3 / 2);
}

/* A user stride grows the plane for every line, chroma included */
static void video_test_nv12_user_stride(struct kunit *test)
{
	struct v4l2_pix_format_mplane *pix;
	struct camss_video *video;
	struct v4l2_format f;

	video = video_test_video(test, true);
	pix = video_test_try(video, V4L2_PIX_FMT_NV12, 2000, 1128, 4096, 0, &f);
	KUNIT_EXPECT_EQ(test, pix->plane_fmt[0].bytesperline, 4096);
	KUNIT_EXPECT_GE(test, pix->plane_fmt[0].sizeimage, 4096 * 1128 * 3 / 2);
}

/* Formats without vertical subsampling keep their previous limits */
static void video_test_baseline_limits(struct kunit *test)
{
	struct v4l2_pix_format_mplane *pix;
	struct camss_video *video;
	struct v4l2_format f;

	video = video_test_video(test, true);
	pix = video_test_try(video, V4L2_PIX_FMT_YUYV, 1920, 1080, 3840, ~0u, &f);
	KUNIT_EXPECT_EQ(test, pix->plane_fmt[0].sizeimage, 3840 * 4096);

	video = video_test_video(test, false);
	pix = video_test_try(video, V4L2_PIX_FMT_SGRBG10P, 4624, 8000, 0, 0, &f);
	KUNIT_EXPECT_EQ(test, pix->height, 8000);
	KUNIT_EXPECT_EQ(test, pix->plane_fmt[0].sizeimage,
			roundup(4624 * 10 / 8, 16) * 8000);
}

struct video_test_sd {
	struct v4l2_subdev sd;
	int fail;
};

static char video_test_log[32];
static unsigned int video_test_nlog;

static int video_test_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct video_test_sd *t = container_of(sd, struct video_test_sd, sd);
	char id = '0' + (t - (struct video_test_sd *)v4l2_get_subdevdata(sd));

	if (video_test_nlog < sizeof(video_test_log) - 2) {
		video_test_log[video_test_nlog++] = enable ? '+' : '-';
		video_test_log[video_test_nlog++] = id;
	}

	return enable ? t->fail : 0;
}

static const struct v4l2_subdev_video_ops video_test_video_ops = {
	.s_stream = video_test_s_stream,
};

static const struct v4l2_subdev_ops video_test_ops = {
	.video = &video_test_video_ops,
};

static const struct v4l2_subdev_ops video_test_no_ops = {};

static void video_test_subdevs(struct video_test_sd *sds, struct v4l2_subdev **sd,
			       unsigned int n)
{
	unsigned int i;

	memset(video_test_log, 0, sizeof(video_test_log));
	video_test_nlog = 0;
	for (i = 0; i < n; i++) {
		memset(&sds[i], 0, sizeof(sds[i]));
		v4l2_subdev_init(&sds[i].sd, &video_test_ops);
		v4l2_set_subdevdata(&sds[i].sd, sds);
		sd[i] = &sds[i].sd;
	}
}

/* VFE, CSID, CSIPHY, sensor: the sensor fails, the rest stop upstream first */
static void video_test_start_unwind(struct kunit *test)
{
	struct video_test_sd sds[4];
	struct v4l2_subdev *sd[4];

	video_test_subdevs(sds, sd, 4);
	sds[3].fail = -EIO;
	KUNIT_EXPECT_EQ(test, camss_video_start_subdevs(sd, 4), -EIO);
	KUNIT_EXPECT_STREQ(test, video_test_log, "+0+1+2+3-2-1-0");

	video_test_subdevs(sds, sd, 4);
	sds[1].fail = -EPIPE;
	KUNIT_EXPECT_EQ(test, camss_video_start_subdevs(sd, 4), -EPIPE);
	KUNIT_EXPECT_STREQ(test, video_test_log, "+0+1-0");

	video_test_subdevs(sds, sd, 4);
	KUNIT_EXPECT_EQ(test, camss_video_start_subdevs(sd, 4), 0);
	KUNIT_EXPECT_STREQ(test, video_test_log, "+0+1+2+3");
}

/* A subdev without s_stream counts as started */
static void video_test_start_no_stream_op(struct kunit *test)
{
	struct video_test_sd sds[3];
	struct v4l2_subdev *sd[3];

	video_test_subdevs(sds, sd, 3);
	sds[1].sd.ops = &video_test_no_ops;
	sds[2].fail = -EIO;
	KUNIT_EXPECT_EQ(test, camss_video_start_subdevs(sd, 3), -EIO);
	KUNIT_EXPECT_STREQ(test, video_test_log, "+0+2-0");
}

static struct kunit_case video_test_cases[] = {
	KUNIT_CASE(video_test_nv12_max_height),
	KUNIT_CASE(video_test_nv12_user_stride),
	KUNIT_CASE(video_test_baseline_limits),
	KUNIT_CASE(video_test_start_unwind),
	KUNIT_CASE(video_test_start_no_stream_op),
	{}
};

static struct kunit_suite video_test_suite = {
	.name = "camss-video",
	.test_cases = video_test_cases,
};

kunit_test_suite(video_test_suite);
