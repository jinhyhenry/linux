#include <linux/device.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <media/media-device.h>
#include <media/v4l2-device.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-dma-contig.h>

#define ISP_NAME_PREFIX "virt_isp"

#define SIM_PTG

struct ptg_dev
{
	struct device           *dev;
	struct media_device		media_dev;
	struct v4l2_device		v4l2_dev;
	struct video_device	    vdev;
	struct vb2_queue	    vb_queue;

	struct mutex			lock;

	struct list_head        pending_bufs;
	struct virt_isp_buffer *cur_buffer;

	struct v4l2_pix_format  out_pix;
};

struct virt_isp_buffer {
	struct vb2_v4l2_buffer vbuf;
	dma_addr_t hw_addr;
	struct list_head head;
};

static int ptg_fh_open(struct file *file)
{
	return v4l2_fh_open(file);
}

static int ptg_fh_close(struct file *file)
{
	return v4l2_fh_release(file);
}

static __poll_t ptg_fh_poll(struct file *file,
				   struct poll_table_struct *wait)
{
	struct ptg_dev *ptg_dev = video_drvdata(file);

	return vb2_poll(&ptg_dev->vb_queue, file, wait);
}

static int ptg_fh_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct ptg_dev *ptg_dev = video_drvdata(file);

	return vb2_mmap(&ptg_dev->vb_queue, vma);;
}

static const struct v4l2_file_operations ptg_fh_fops = {
	.owner		= THIS_MODULE,
	.open		= ptg_fh_open,
	.release	= ptg_fh_close,
	.poll		= ptg_fh_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= ptg_fh_mmap,
};

static int ptg_streamon(struct file *file, void *priv,
				  enum v4l2_buf_type type)
{
	struct ptg_dev *ptg_dev = video_drvdata(file);
	return vb2_streamon(&ptg_dev->vb_queue, type);
}

static int ptg_streamoff(struct file *file, void *priv,
				  enum v4l2_buf_type type)
{
	struct ptg_dev *ptg_dev = video_drvdata(file);
	return vb2_streamoff(&ptg_dev->vb_queue, type);
}

static int ptg_reqbufs(struct file *file, void *priv,
				 struct v4l2_requestbuffers *rb)
{
	struct ptg_dev *ptg_dev = video_drvdata(file);
	return vb2_reqbufs(&ptg_dev->vb_queue, rb);
}

static int ptg_querybuf(struct file *file, void *priv,
				  struct v4l2_buffer *buf)
{
	struct ptg_dev *ptg_dev = video_drvdata(file);
	return vb2_querybuf(&ptg_dev->vb_queue, buf);
}

static int ptg_create_bufs(struct file *file, void *priv,
				 struct v4l2_create_buffers *create)
{
	struct ptg_dev *ptg_dev = video_drvdata(file);
	return vb2_create_bufs(&ptg_dev->vb_queue, create);
}

static int ptg_prepare_buf(struct file *file, void *priv,
				 struct v4l2_buffer *b)
{
	struct ptg_dev *ptg_dev = video_drvdata(file);
	return vb2_prepare_buf(&ptg_dev->vb_queue, &ptg_dev->media_dev, b);
}

static int ptg_qbuf(struct file *file, void *priv,
			  struct v4l2_buffer *buf)
{
	struct ptg_dev *ptg_dev = video_drvdata(file);
	return vb2_qbuf(&ptg_dev->vb_queue, &ptg_dev->media_dev, buf);
}

static int ptg_dqbuf(struct file *file, void *priv,
			  struct v4l2_buffer *buf)
{
	struct ptg_dev *ptg_dev = video_drvdata(file);
	return vb2_dqbuf(&ptg_dev->vb_queue, buf, file->f_flags & O_NONBLOCK);
}

static int ptg_s_fmt_mplane(struct file *file, void *fh,
					struct v4l2_format *f)
{
	struct ptg_dev *ptg_dev = video_drvdata(file);

	ptg_dev->out_pix = f->fmt.pix;

	return 0;
}

static int ptg_g_fmt_mplane(struct file *file, void *fh,
					struct v4l2_format *f)
{
	struct ptg_dev *ptg_dev = video_drvdata(file);

	f->fmt.pix = ptg_dev->out_pix;

	return 0;
}

static const struct v4l2_ioctl_ops ptg_ioctl_ops = {
	.vidioc_querycap	  = NULL,
	.vidioc_enum_input	  = NULL,
	.vidioc_g_input		  = NULL,
	.vidioc_s_input		  = NULL,
	.vidioc_enum_fmt_vid_cap  = NULL,
	.vidioc_try_fmt_vid_cap	  = NULL,
	.vidioc_s_fmt_vid_cap	  = ptg_s_fmt_mplane,
	.vidioc_g_fmt_vid_cap	  = ptg_g_fmt_mplane,
	.vidioc_g_selection	  = NULL,
	.vidioc_s_selection	  = NULL,

	.vidioc_reqbufs		  = ptg_reqbufs,
	.vidioc_querybuf	  = ptg_querybuf,
	.vidioc_prepare_buf	  = ptg_prepare_buf,
	.vidioc_create_bufs	  = ptg_create_bufs,
	.vidioc_qbuf		  = ptg_qbuf,
	.vidioc_dqbuf		  = ptg_dqbuf,
	.vidioc_streamon	  = ptg_streamon,
	.vidioc_streamoff	  = ptg_streamoff,

	.vidioc_subscribe_event	  = NULL,
	.vidioc_unsubscribe_event = NULL,
	.vidioc_log_status	  = NULL,
};

static int ptg_queue_setup(struct vb2_queue *vq,
				unsigned int *nbuffers, unsigned int *nplanes,
				unsigned int sizes[], struct device *alloc_devs[])
{
	struct ptg_dev *ptg_dev = vb2_get_drv_priv(vq);
	WARN_ON(!ptg_dev);

	*nplanes = 1;
	sizes[0] = ptg_dev->out_pix.width * ptg_dev->out_pix.height;

	INIT_LIST_HEAD(&ptg_dev->pending_bufs);

	return 0;
}

static void ptg_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct ptg_dev *ptg_dev = vb2_get_drv_priv(vb->vb2_queue);
	struct virt_isp_buffer *buffer = container_of(vbuf, struct virt_isp_buffer,
						   vbuf);

	list_add_tail(&buffer->head, &ptg_dev->pending_bufs);
}

static int ptg_buf_init(struct vb2_buffer *vb)
{
	dma_addr_t hw_addr = 0;

	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct ptg_dev *ptg_dev = vb2_get_drv_priv(vb->vb2_queue);
	struct virt_isp_buffer *buffer = container_of(vbuf, struct virt_isp_buffer,
						   vbuf);

	hw_addr = vb2_dma_contig_plane_dma_addr(vb, 0);
	if (!hw_addr)
	{
		return -EFAULT;
	}
	buffer->hw_addr = hw_addr;

	// list_add_tail(&buffer->head, &ptg_dev->pending_bufs);

	return 0;
}

static int ptg_start_streaming(struct vb2_queue *vq,
						unsigned int count)
{
	int ret;
	struct ptg_dev *ptg_dev = vb2_get_drv_priv(vq);
	WARN_ON(!ptg_dev);

	struct virt_isp_buffer *buffer = list_first_entry(&ptg_dev->pending_bufs,
					  struct virt_isp_buffer,
					  head);
	list_del(&buffer->head);

	ptg_dev->cur_buffer = buffer;

	sim_ptg_cfg_dma(buffer->hw_addr);

	ret = sim_ptg_stream_on();
	if (ret)
		return ret;

	return 0;
}

static void ptg_stop_streaming(struct vb2_queue *vq)
{
	int ret;
	struct ptg_dev *ptg_dev = vb2_get_drv_priv(vq);
	WARN_ON(!ptg_dev);

	ret = sim_ptg_stream_off();
	if (ret)
		pr_err("failed to streamoff sim_ptg\n");
}

static void _on_ptg_frame_done(void* data)
{
	struct ptg_dev *ptg_dev = (struct ptg_dev *)data;
	struct virt_isp_buffer *buffer = NULL;

	BUG_ON(!ptg_dev);
	BUG_ON(!ptg_dev->cur_buffer);

	vb2_buffer_done(&ptg_dev->cur_buffer->vbuf.vb2_buf, VB2_BUF_STATE_DONE);

	BUG_ON(list_empty(&ptg_dev->pending_bufs));

	buffer = list_first_entry(&ptg_dev->pending_bufs,
					  struct virt_isp_buffer,
					  head);
	if (!buffer)
	{
		pr_err("no frame to shot\n");
		return;
	}
	list_del(&buffer->head);

	ptg_dev->cur_buffer = buffer;

	sim_ptg_cfg_dma(buffer->hw_addr);
}

static const struct vb2_ops ptg_qops = {
	.queue_setup	 = ptg_queue_setup,
	.buf_init        = ptg_buf_init,
	.buf_queue       = ptg_buf_queue,
	.start_streaming = ptg_start_streaming,
	.stop_streaming	 = ptg_stop_streaming,
	.wait_prepare	 = vb2_ops_wait_prepare,
	.wait_finish	 = vb2_ops_wait_finish,
};

static int ptg_v_dev_register(struct ptg_dev *ptg_dev)
{
	int ret = 0;
	struct vb2_queue *vb_q = &ptg_dev->vb_queue;
	struct video_device *vfd = &ptg_dev->vdev;

	memset(vb_q, 0, sizeof(*vb_q));
	vb_q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	vb_q->io_modes = VB2_MMAP | VB2_USERPTR;
	vb_q->ops = &ptg_qops;
	vb_q->mem_ops = &vb2_dma_contig_memops;
	vb_q->drv_priv = ptg_dev;
	vb_q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	vb_q->lock = &ptg_dev->lock;
	vb_q->dev = ptg_dev->v4l2_dev.dev;
	vb_q->buf_struct_size = sizeof(struct virt_isp_buffer);

	ret = vb2_queue_init(vb_q);
	if (ret)
	{
		v4l2_err(&ptg_dev->v4l2_dev, "register q failed\n");
		return -1;
	}

	memset(vfd, 0, sizeof(*vfd));
	strscpy(vfd->name, ISP_NAME_PREFIX, sizeof(ptg_dev->v4l2_dev.name));

	vfd->fops = &ptg_fh_fops;
	vfd->ioctl_ops = &ptg_ioctl_ops;
	vfd->v4l2_dev = &ptg_dev->v4l2_dev;
	//vfd->minor = -1;
	vfd->release = video_device_release_empty;
	vfd->lock = &ptg_dev->lock;
	vfd->device_caps = V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_STREAMING
			  | V4L2_CAP_READWRITE | V4L2_CAP_IO_MC;

	video_set_drvdata(vfd, ptg_dev);

	ret = video_register_device(vfd, VFL_TYPE_VIDEO, -1);
	if (ret)
	{
		v4l2_err(&ptg_dev->v4l2_dev, "register v_dev failed\n");
		return -1;
	}

	v4l2_info(&ptg_dev->v4l2_dev, "register v_dev successed %s\n", video_device_node_name(vfd));

	return 0;
}

static int ptg_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct device *dev = &pdev->dev;
	struct ptg_dev *ptg_dev = NULL;

	ptg_dev = devm_kzalloc(dev, sizeof(struct ptg_dev), GFP_KERNEL);
	if (NULL == ptg_dev)
	{
		return -ENOMEM;
	}

	ptg_dev->dev = dev;

	platform_set_drvdata(pdev, ptg_dev);

	ptg_dev->media_dev.dev = dev;

	strscpy(ptg_dev->v4l2_dev.name, ISP_NAME_PREFIX, sizeof(ptg_dev->v4l2_dev.name));
	ptg_dev->v4l2_dev.mdev = &ptg_dev->media_dev;

	media_device_init(ptg_dev->v4l2_dev.mdev);

	ret = v4l2_device_register(ptg_dev->dev, &ptg_dev->v4l2_dev);
	if (ret < 0)
		return ret;

	mutex_init(&ptg_dev->lock);

#ifdef SIM_PTG

	ret = sim_ptg_init(30, _on_ptg_frame_done, ptg_dev);
	if (ret < 0)
		return ret;

#endif

	return ptg_v_dev_register(ptg_dev);
}

static int ptg_remove(struct platform_device *pdev)
{
	struct ptg_dev *ptg_dev = platform_get_drvdata(pdev);

	(void *)sim_ptg_deinit();

	video_unregister_device(&ptg_dev->vdev);

	v4l2_device_unregister(&ptg_dev->v4l2_dev);
	media_device_cleanup(&ptg_dev->media_dev);

	devm_kfree(&pdev->dev, ptg_dev);

	return 0;
}

static const u32 _res;

static const struct of_device_id virt_isp_dt_match[] = {
	{ .compatible = "virt_isp,ptg", .data = &_res, },
	{ }
};
MODULE_DEVICE_TABLE(of, virt_isp_dt_match);

static struct platform_driver ptg_driver = {
	.probe		= ptg_probe,
	.remove		= ptg_remove,
	.driver = {
		.name	= "ptg",
		.of_match_table = virt_isp_dt_match,
	}
};

module_platform_driver(ptg_driver);

MODULE_DESCRIPTION("pattern gen isp driver");
MODULE_LICENSE("GPL");