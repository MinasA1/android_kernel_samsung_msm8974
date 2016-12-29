/* Copyright (c) 2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/mutex.h>
#include <linux/io.h>
#include <media/v4l2-subdev.h>
#include <linux/ratelimit.h>

#include "msm.h"
#include "msm_isp_util.h"
#include "msm_isp_axi_util.h"
#include "msm_isp_stats_util.h"
#include "msm_camera_io_util.h"

#define MAX_ISP_V4l2_EVENTS 100
static DEFINE_MUTEX(bandwidth_mgr_mutex);
static struct msm_isp_bandwidth_mgr isp_bandwidth_mgr;

#define MSM_ISP_MIN_AB 300000000ULL  * 3 / 2
#define MSM_ISP_MIN_IB 450000000ULL  * 3 / 2

#define MSM_ISP_MIN_AB_RECORD 300000000ULL
#define MSM_ISP_MIN_IB_RECORD 450000000ULL

extern int32_t isp_recording_hint;

static struct msm_bus_vectors msm_isp_init_vectors[] = {
	{
		.src = MSM_BUS_MASTER_VFE,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = 0,
		.ib  = 0,
	},
};

static struct msm_bus_vectors msm_isp_ping_vectors[] = {
	{
		.src = MSM_BUS_MASTER_VFE,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = MSM_ISP_MIN_AB,
		.ib  = (uint64_t)MSM_ISP_MIN_IB,
	},
};

static struct msm_bus_vectors msm_isp_pong_vectors[] = {
	{
		.src = MSM_BUS_MASTER_VFE,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = MSM_ISP_MIN_AB,
		.ib  = (uint64_t)MSM_ISP_MIN_IB,
	},
};

static struct msm_bus_paths msm_isp_bus_client_config[] = {
	{
		ARRAY_SIZE(msm_isp_init_vectors),
		msm_isp_init_vectors,
	},
	{
		ARRAY_SIZE(msm_isp_ping_vectors),
		msm_isp_ping_vectors,
	},
	{
		ARRAY_SIZE(msm_isp_pong_vectors),
		msm_isp_pong_vectors,
	},
};

static struct msm_bus_scale_pdata msm_isp_bus_client_pdata = {
	msm_isp_bus_client_config,
	ARRAY_SIZE(msm_isp_bus_client_config),
	.name = "msm_camera_isp",
};

int msm_isp_init_bandwidth_mgr(enum msm_isp_hw_client client)
{
	int rc = 0;

	mutex_lock(&bandwidth_mgr_mutex);
	isp_bandwidth_mgr.client_info[client].active = 1;
	if (isp_bandwidth_mgr.use_count++) {
		mutex_unlock(&bandwidth_mgr_mutex);
		return rc;
	}
	isp_bandwidth_mgr.bus_client =
		msm_bus_scale_register_client(&msm_isp_bus_client_pdata);
	if (!isp_bandwidth_mgr.bus_client) {
		pr_err("%s: client register failed\n", __func__);
		mutex_unlock(&bandwidth_mgr_mutex);
		return -EINVAL;
	}

	isp_bandwidth_mgr.bus_vector_active_idx = 1;
	msm_bus_scale_client_update_request(
		isp_bandwidth_mgr.bus_client,
		isp_bandwidth_mgr.bus_vector_active_idx);

	mutex_unlock(&bandwidth_mgr_mutex);
	return 0;
}

int msm_isp_update_bandwidth(enum msm_isp_hw_client client,
			     uint64_t ab, uint64_t ib)
{
	int i;
	struct msm_bus_paths *path;

	mutex_lock(&bandwidth_mgr_mutex);
	if (!isp_bandwidth_mgr.use_count ||
	    !isp_bandwidth_mgr.bus_client) {
		pr_err("%s:error bandwidth manager inactive use_cnt:%d bus_clnt:%d\n",
			__func__, isp_bandwidth_mgr.use_count,
			isp_bandwidth_mgr.bus_client);
		mutex_unlock(&bandwidth_mgr_mutex);
		return -EINVAL;
	}

	isp_bandwidth_mgr.client_info[client].ab = ab;
	isp_bandwidth_mgr.client_info[client].ib = ib;
	ALT_VECTOR_IDX(isp_bandwidth_mgr.bus_vector_active_idx);
	path =
		&(msm_isp_bus_client_pdata.usecase[
			  isp_bandwidth_mgr.bus_vector_active_idx]);
	if (isp_recording_hint == 1) {
		pr_err("%s: [syscamera] RECORD\n", __func__);
		path->vectors[0].ab = MSM_ISP_MIN_AB_RECORD;
		path->vectors[0].ib = MSM_ISP_MIN_IB_RECORD;
	} else {
		pr_err("%s: [syscamera] CAMERA\n", __func__);
		path->vectors[0].ab = MSM_ISP_MIN_AB;
		path->vectors[0].ib = MSM_ISP_MIN_IB;
	}
	for (i = 0; i < MAX_ISP_CLIENT; i++) {
		if (isp_bandwidth_mgr.client_info[i].active) {
			path->vectors[0].ab +=
				isp_bandwidth_mgr.client_info[i].ab;
			path->vectors[0].ib +=
				isp_bandwidth_mgr.client_info[i].ib;
		}
	}
	msm_bus_scale_client_update_request(isp_bandwidth_mgr.bus_client,
					    isp_bandwidth_mgr.bus_vector_active_idx);
	mutex_unlock(&bandwidth_mgr_mutex);
	return 0;
}

void msm_isp_deinit_bandwidth_mgr(enum msm_isp_hw_client client)
{
	mutex_lock(&bandwidth_mgr_mutex);
	memset(&isp_bandwidth_mgr.client_info[client], 0,
	       sizeof(struct msm_isp_bandwidth_info));
	if (--isp_bandwidth_mgr.use_count) {
		mutex_unlock(&bandwidth_mgr_mutex);
		return;
	}

	if (!isp_bandwidth_mgr.bus_client) {
		pr_err("%s:%d error: bus client invalid\n", __func__, __LINE__);
		mutex_unlock(&bandwidth_mgr_mutex);
		return;
	}

	msm_bus_scale_client_update_request(
		isp_bandwidth_mgr.bus_client, 0);
	msm_bus_scale_unregister_client(isp_bandwidth_mgr.bus_client);
	isp_bandwidth_mgr.bus_client = 0;
	mutex_unlock(&bandwidth_mgr_mutex);
}

uint32_t msm_isp_get_framedrop_period(
	enum msm_vfe_frame_skip_pattern frame_skip_pattern)
{
	switch (frame_skip_pattern) {
	case NO_SKIP:
	case EVERY_2FRAME:
	case EVERY_3FRAME:
	case EVERY_4FRAME:
	case EVERY_5FRAME:
	case EVERY_6FRAME:
	case EVERY_7FRAME:
	case EVERY_8FRAME:
	case EVERY_9FRAME:
	case EVERY_10FRAME:
	case EVERY_11FRAME:
	case EVERY_12FRAME:
	case EVERY_13FRAME:
	case EVERY_14FRAME:
	case EVERY_15FRAME:
		return frame_skip_pattern + 1;
	case EVERY_16FRAME:
		return 16;
		break;
	case EVERY_32FRAME:
		return 32;
		break;
	case SKIP_ALL:
		return 1;
	default:
		return 1;
	}
	return 1;
}

static inline void msm_isp_get_timestamp(struct msm_isp_timestamp *time_stamp)
{
	struct timespec ts;

	ktime_get_ts(&ts);
	time_stamp->buf_time.tv_sec = ts.tv_sec;
	time_stamp->buf_time.tv_usec = ts.tv_nsec / 1000;
	do_gettimeofday(&(time_stamp->event_time));
}

int msm_isp_subscribe_event(struct v4l2_subdev *sd, struct v4l2_fh *fh,
			    struct v4l2_event_subscription *sub)
{
	struct vfe_device *vfe_dev = v4l2_get_subdevdata(sd);
	int rc = 0;

	rc = v4l2_event_subscribe(fh, sub, MAX_ISP_V4l2_EVENTS);
	if (rc == 0) {
		if (sub->type == V4L2_EVENT_ALL) {
			int i;

			vfe_dev->axi_data.event_mask = 0;
			for (i = 0; i < ISP_EVENT_MAX; i++)
				vfe_dev->axi_data.event_mask |= (1 << i);
		} else {
			int event_idx = sub->type - ISP_EVENT_BASE;

			vfe_dev->axi_data.event_mask |= (1 << event_idx);
		}
	}
	return rc;
}

int msm_isp_unsubscribe_event(struct v4l2_subdev *sd, struct v4l2_fh *fh,
			      struct v4l2_event_subscription *sub)
{
	struct vfe_device *vfe_dev = v4l2_get_subdevdata(sd);
	int rc = 0;

	rc = v4l2_event_unsubscribe(fh, sub);
	if (sub->type == V4L2_EVENT_ALL) {
		vfe_dev->axi_data.event_mask = 0;
	} else {
		int event_idx = sub->type - ISP_EVENT_BASE;

		vfe_dev->axi_data.event_mask &= ~(1 << event_idx);
	}
	return rc;
}

static int msm_isp_set_clk_rate(struct vfe_device *vfe_dev, long *rate)
{
	int rc = 0;
	int clk_idx = vfe_dev->hw_info->vfe_clk_idx;
	long round_rate =
		clk_round_rate(vfe_dev->vfe_clk[clk_idx], *rate);

	if (round_rate < 0) {
		pr_err("%s: Invalid vfe clock rate\n", __func__);
		return round_rate;
	}

	rc = clk_set_rate(vfe_dev->vfe_clk[clk_idx], round_rate);
	if (rc < 0) {
		pr_err("%s: Vfe set rate error\n", __func__);
		return rc;
	}
	*rate = round_rate;
	return 0;
}

int msm_isp_cfg_pix(struct vfe_device *vfe_dev,
		    struct msm_vfe_input_cfg *input_cfg)
{
	int rc = 0;

	if (vfe_dev->axi_data.src_info[VFE_PIX_0].active) {
		pr_err("%s: pixel path is active\n", __func__);
		return -EINVAL;
	}

	vfe_dev->axi_data.src_info[VFE_PIX_0].pixel_clock =
		input_cfg->input_pix_clk;
	vfe_dev->axi_data.src_info[VFE_PIX_0].input_mux =
		input_cfg->d.pix_cfg.input_mux;
	vfe_dev->axi_data.src_info[VFE_PIX_0].width =
		input_cfg->d.pix_cfg.camif_cfg.pixels_per_line;

	rc = msm_isp_set_clk_rate(vfe_dev,
				  &vfe_dev->axi_data.src_info[VFE_PIX_0].pixel_clock);
	if (rc < 0) {
		pr_err("%s: clock set rate failed\n", __func__);
		return rc;
	}

	vfe_dev->hw_info->vfe_ops.core_ops.cfg_camif(
		vfe_dev, &input_cfg->d.pix_cfg);
	return rc;
}

int msm_isp_cfg_rdi(struct vfe_device *vfe_dev,
		    struct msm_vfe_input_cfg *input_cfg)
{
	int rc = 0;

	if (vfe_dev->axi_data.src_info[input_cfg->input_src].active) {
		pr_err("%s: RAW%d path is active\n", __func__,
		       input_cfg->input_src - VFE_RAW_0);
		return -EINVAL;
	}

	vfe_dev->axi_data.src_info[input_cfg->input_src].pixel_clock =
		input_cfg->input_pix_clk;
	vfe_dev->hw_info->vfe_ops.core_ops.cfg_rdi_reg(
		vfe_dev, &input_cfg->d.rdi_cfg, input_cfg->input_src);

	rc = msm_isp_set_clk_rate(vfe_dev,
				  &vfe_dev->axi_data.src_info[input_cfg->input_src].pixel_clock);
	if (rc < 0) {
		pr_err("%s: clock set rate failed\n", __func__);
		return rc;
	}
	return rc;
}

int msm_isp_cfg_input(struct vfe_device *vfe_dev, void *arg)
{
	int rc = 0;
	struct msm_vfe_input_cfg *input_cfg = arg;

	switch (input_cfg->input_src) {
	case VFE_PIX_0:
		rc = msm_isp_cfg_pix(vfe_dev, input_cfg);
		break;
	case VFE_RAW_0:
	case VFE_RAW_1:
	case VFE_RAW_2:
		rc = msm_isp_cfg_rdi(vfe_dev, input_cfg);
		break;
	default:
		pr_err("%s: Invalid input source\n", __func__);
		rc = -EINVAL;
	}
	return rc;
}

long msm_isp_ioctl(struct v4l2_subdev *sd,
		   unsigned int cmd, void *arg)
{
	long rc = 0;
	struct vfe_device *vfe_dev = v4l2_get_subdevdata(sd);

	/* Use real time mutex for hard real-time ioctls such as
	 * buffer operations and register updates.
	 * Use core mutex for other ioctls that could take
	 * longer time to complete such as start/stop ISP streams
	 * which blocks until the hardware start/stop streaming
	 */
	ISP_DBG("%s cmd: %d\n", __func__, _IOC_TYPE(cmd));
	switch (cmd) {
	case VIDIOC_MSM_VFE_REG_CFG: {
		mutex_lock(&vfe_dev->realtime_mutex);
		rc = msm_isp_proc_cmd(vfe_dev, arg);
		mutex_unlock(&vfe_dev->realtime_mutex);
		break;
	}
	case VIDIOC_MSM_ISP_REQUEST_BUF:
	case VIDIOC_MSM_ISP_ENQUEUE_BUF:
	case VIDIOC_MSM_ISP_RELEASE_BUF: {
		mutex_lock(&vfe_dev->realtime_mutex);
		rc = msm_isp_proc_buf_cmd(vfe_dev->buf_mgr, cmd, arg);
		mutex_unlock(&vfe_dev->realtime_mutex);
		break;
	}
	case VIDIOC_MSM_ISP_REQUEST_STREAM:
		mutex_lock(&vfe_dev->core_mutex);
		rc = msm_isp_request_axi_stream(vfe_dev, arg);
		mutex_unlock(&vfe_dev->core_mutex);
		break;
	case VIDIOC_MSM_ISP_RELEASE_STREAM:
		mutex_lock(&vfe_dev->core_mutex);
		rc = msm_isp_release_axi_stream(vfe_dev, arg);
		mutex_unlock(&vfe_dev->core_mutex);
		break;
	case VIDIOC_MSM_ISP_CFG_STREAM:
		mutex_lock(&vfe_dev->core_mutex);
		rc = msm_isp_cfg_axi_stream(vfe_dev, arg);
		mutex_unlock(&vfe_dev->core_mutex);
		break;
	case VIDIOC_MSM_ISP_AXI_HALT:
		mutex_lock(&vfe_dev->core_mutex);
		rc = msm_isp_axi_halt(vfe_dev, arg);
		mutex_unlock(&vfe_dev->core_mutex);
		break;
	case VIDIOC_MSM_ISP_AXI_RESET:
		mutex_lock(&vfe_dev->core_mutex);
		rc = msm_isp_axi_reset(vfe_dev, arg);
		mutex_unlock(&vfe_dev->core_mutex);
		break;
	case VIDIOC_MSM_ISP_AXI_RESTART:
		mutex_lock(&vfe_dev->core_mutex);
		rc = msm_isp_axi_restart(vfe_dev, arg);
		mutex_unlock(&vfe_dev->core_mutex);
		break;
	case VIDIOC_MSM_ISP_INPUT_CFG:
		mutex_lock(&vfe_dev->core_mutex);
		rc = msm_isp_cfg_input(vfe_dev, arg);
		mutex_unlock(&vfe_dev->core_mutex);
		break;
	case VIDIOC_MSM_ISP_SET_SRC_STATE:
		mutex_lock(&vfe_dev->core_mutex);
		rc = msm_isp_set_src_state(vfe_dev, arg);
		mutex_unlock(&vfe_dev->core_mutex);
		break;
	case VIDIOC_MSM_ISP_REQUEST_STATS_STREAM:
		mutex_lock(&vfe_dev->core_mutex);
		rc = msm_isp_request_stats_stream(vfe_dev, arg);
		mutex_unlock(&vfe_dev->core_mutex);
		break;
	case VIDIOC_MSM_ISP_RELEASE_STATS_STREAM:
		mutex_lock(&vfe_dev->core_mutex);
		rc = msm_isp_release_stats_stream(vfe_dev, arg);
		mutex_unlock(&vfe_dev->core_mutex);
		break;
	case VIDIOC_MSM_ISP_CFG_STATS_STREAM:
		mutex_lock(&vfe_dev->core_mutex);
		rc = msm_isp_cfg_stats_stream(vfe_dev, arg);
		mutex_unlock(&vfe_dev->core_mutex);
		break;
	case VIDIOC_MSM_ISP_UPDATE_STREAM:
		mutex_lock(&vfe_dev->core_mutex);
		rc = msm_isp_update_axi_stream(vfe_dev, arg);
		mutex_unlock(&vfe_dev->core_mutex);
		break;
	case MSM_SD_SHUTDOWN:
		while (vfe_dev->vfe_open_cnt != 0)
			msm_isp_close_node(sd, NULL);
		rc = 0;
		break;

	default:
		pr_err("%s: Invalid ISP command\n", __func__);
		rc = -EINVAL;
	}
	return rc;
}

static int msm_isp_send_hw_cmd(struct vfe_device *vfe_dev,
			       struct msm_vfe_reg_cfg_cmd *reg_cfg_cmd,
			       uint32_t *cfg_data, uint32_t cmd_len)
{
	if (!vfe_dev || !reg_cfg_cmd) {
		pr_err("%s:%d failed: vfe_dev %p reg_cfg_cmd %p\n", __func__,
			__LINE__, vfe_dev, reg_cfg_cmd);
		return -EINVAL;
	}
	if ((reg_cfg_cmd->cmd_type != VFE_CFG_MASK) &&
		(!cfg_data || !cmd_len)) {
		pr_err("%s:%d failed: cmd type %d cfg_data %p cmd_len %d\n",
			__func__, __LINE__, reg_cfg_cmd->cmd_type, cfg_data,
			cmd_len);
		return -EINVAL;
	}

	/* Validate input parameters */
	switch (reg_cfg_cmd->cmd_type) {
	case VFE_WRITE:
	case VFE_READ:
	case VFE_WRITE_MB: {
		if ((reg_cfg_cmd->u.rw_info.reg_offset >
			(UINT_MAX - reg_cfg_cmd->u.rw_info.len)) ||
			((reg_cfg_cmd->u.rw_info.reg_offset +
			reg_cfg_cmd->u.rw_info.len) >
			resource_size(vfe_dev->vfe_mem))) {
			pr_err("%s:%d reg_offset %d len %d res %d\n",
				__func__, __LINE__,
				reg_cfg_cmd->u.rw_info.reg_offset,
				reg_cfg_cmd->u.rw_info.len,
				(uint32_t)resource_size(vfe_dev->vfe_mem));
			return -EINVAL;
		}

		if ((reg_cfg_cmd->u.rw_info.cmd_data_offset >
			(UINT_MAX - reg_cfg_cmd->u.rw_info.len)) ||
			((reg_cfg_cmd->u.rw_info.cmd_data_offset +
			reg_cfg_cmd->u.rw_info.len) > cmd_len)) {
			pr_err("%s:%d cmd_data_offset %d len %d cmd_len %d\n",
				__func__, __LINE__,
				reg_cfg_cmd->u.rw_info.cmd_data_offset,
				reg_cfg_cmd->u.rw_info.len, cmd_len);
			return -EINVAL;
		}
		break;
	}

	case VFE_WRITE_DMI_16BIT:
	case VFE_WRITE_DMI_32BIT:
	case VFE_WRITE_DMI_64BIT:
	case VFE_READ_DMI_16BIT:
	case VFE_READ_DMI_32BIT:
	case VFE_READ_DMI_64BIT: {
			if (reg_cfg_cmd->cmd_type == VFE_WRITE_DMI_64BIT ||
	                    reg_cfg_cmd->cmd_type == VFE_READ_DMI_64BIT) {
			if ((reg_cfg_cmd->u.dmi_info.hi_tbl_offset <=
				reg_cfg_cmd->u.dmi_info.lo_tbl_offset) ||
				(reg_cfg_cmd->u.dmi_info.hi_tbl_offset -
				reg_cfg_cmd->u.dmi_info.lo_tbl_offset !=
				(sizeof(uint32_t)))) {
				pr_err("%s:%d hi %d lo %d\n",
					__func__, __LINE__,
					reg_cfg_cmd->u.dmi_info.hi_tbl_offset,
					reg_cfg_cmd->u.dmi_info.hi_tbl_offset);
				return -EINVAL;
			}
			if (reg_cfg_cmd->u.dmi_info.len <= sizeof(uint32_t)) {
				pr_err("%s:%d len %d\n",
					__func__, __LINE__,
					reg_cfg_cmd->u.dmi_info.len);
				return -EINVAL;
			}
			if (((UINT_MAX -
				reg_cfg_cmd->u.dmi_info.hi_tbl_offset) <
				(reg_cfg_cmd->u.dmi_info.len -
				sizeof(uint32_t))) ||
				((reg_cfg_cmd->u.dmi_info.hi_tbl_offset +
				reg_cfg_cmd->u.dmi_info.len -
				sizeof(uint32_t)) > cmd_len)) {
				pr_err("%s:%d hi_tbl_offset %d len %d cmd %d\n",
					__func__, __LINE__,
					reg_cfg_cmd->u.dmi_info.hi_tbl_offset,
					reg_cfg_cmd->u.dmi_info.len, cmd_len);
				return -EINVAL;
			}
		}
		if ((reg_cfg_cmd->u.dmi_info.lo_tbl_offset >
			(UINT_MAX - reg_cfg_cmd->u.dmi_info.len)) ||
			((reg_cfg_cmd->u.dmi_info.lo_tbl_offset +
			reg_cfg_cmd->u.dmi_info.len) > cmd_len)) {
			pr_err("%s:%d lo_tbl_offset %d len %d cmd_len %d\n",
				__func__, __LINE__,
				reg_cfg_cmd->u.dmi_info.lo_tbl_offset,
				reg_cfg_cmd->u.dmi_info.len, cmd_len);
			return -EINVAL;
		}
		break;
	}

	default:
		break;
	}

	switch (reg_cfg_cmd->cmd_type) {
	case VFE_WRITE: {
		msm_camera_io_memcpy(vfe_dev->vfe_base +
				     reg_cfg_cmd->u.rw_info.reg_offset,
				     cfg_data + reg_cfg_cmd->u.rw_info.cmd_data_offset / 4,
				     reg_cfg_cmd->u.rw_info.len);
		break;
	}
	case VFE_WRITE_MB: {
		msm_camera_io_memcpy_mb(vfe_dev->vfe_base +
			reg_cfg_cmd->u.rw_info.reg_offset,
			cfg_data + reg_cfg_cmd->u.rw_info.cmd_data_offset/4,
			reg_cfg_cmd->u.rw_info.len);
		break;
	}
	case VFE_CFG_MASK: {
		uint32_t temp;
		if ((UINT_MAX - sizeof(temp) <
			reg_cfg_cmd->u.mask_info.reg_offset) ||
			(resource_size(vfe_dev->vfe_mem) <
			reg_cfg_cmd->u.mask_info.reg_offset +
			sizeof(temp))) {
			pr_err("%s: VFE_CFG_MASK: Invalid length\n", __func__);
			return -EINVAL;
		}
		temp = msm_camera_io_r(vfe_dev->vfe_base +
				       reg_cfg_cmd->u.mask_info.reg_offset);
		temp &= ~reg_cfg_cmd->u.mask_info.mask;
		temp |= reg_cfg_cmd->u.mask_info.val;
		msm_camera_io_w(temp, vfe_dev->vfe_base +
				reg_cfg_cmd->u.mask_info.reg_offset);
		break;
	}
	case VFE_WRITE_DMI_16BIT:
	case VFE_WRITE_DMI_32BIT:
	case VFE_WRITE_DMI_64BIT: {
		int i;
		uint32_t *hi_tbl_ptr = NULL, *lo_tbl_ptr = NULL;
		uint32_t hi_val, lo_val, lo_val1;
		if (reg_cfg_cmd->cmd_type == VFE_WRITE_DMI_64BIT) {
			hi_tbl_ptr = cfg_data +
				     reg_cfg_cmd->u.dmi_info.hi_tbl_offset / 4;
		}

		lo_tbl_ptr = cfg_data +
			     reg_cfg_cmd->u.dmi_info.lo_tbl_offset / 4;

		if (reg_cfg_cmd->cmd_type == VFE_WRITE_DMI_64BIT)
			reg_cfg_cmd->u.dmi_info.len =
				reg_cfg_cmd->u.dmi_info.len / 2;

		for (i = 0; i < reg_cfg_cmd->u.dmi_info.len / 4; i++) {
			lo_val = *lo_tbl_ptr++;
			if (reg_cfg_cmd->cmd_type == VFE_WRITE_DMI_16BIT) {
				lo_val1 = lo_val & 0x0000FFFF;
				lo_val = (lo_val & 0xFFFF0000) >> 16;
				msm_camera_io_w(lo_val1, vfe_dev->vfe_base +
						vfe_dev->hw_info->dmi_reg_offset + 0x4);
			} else if (reg_cfg_cmd->cmd_type ==
				   VFE_WRITE_DMI_64BIT) {
				lo_tbl_ptr++;
				hi_val = *hi_tbl_ptr;
				hi_tbl_ptr = hi_tbl_ptr + 2;
				msm_camera_io_w(hi_val, vfe_dev->vfe_base +
						vfe_dev->hw_info->dmi_reg_offset);
			}
			msm_camera_io_w(lo_val, vfe_dev->vfe_base +
					vfe_dev->hw_info->dmi_reg_offset + 0x4);
		}
		break;
	}
	case VFE_READ_DMI_16BIT:
	case VFE_READ_DMI_32BIT:
	case VFE_READ_DMI_64BIT: {
		int i;
		uint32_t *hi_tbl_ptr = NULL, *lo_tbl_ptr = NULL;
		uint32_t hi_val, lo_val, lo_val1;
		if (reg_cfg_cmd->cmd_type == VFE_READ_DMI_64BIT) {
			hi_tbl_ptr = cfg_data +
				     reg_cfg_cmd->u.dmi_info.hi_tbl_offset / 4;
		}

		lo_tbl_ptr = cfg_data +
			     reg_cfg_cmd->u.dmi_info.lo_tbl_offset / 4;

		if (reg_cfg_cmd->cmd_type == VFE_READ_DMI_64BIT)
			reg_cfg_cmd->u.dmi_info.len =
				reg_cfg_cmd->u.dmi_info.len / 2;

		for (i = 0; i < reg_cfg_cmd->u.dmi_info.len / 4; i++) {
			lo_val = msm_camera_io_r(vfe_dev->vfe_base +
						 vfe_dev->hw_info->dmi_reg_offset + 0x4);

			if (reg_cfg_cmd->cmd_type == VFE_READ_DMI_16BIT) {
				lo_val1 = msm_camera_io_r(vfe_dev->vfe_base +
							  vfe_dev->hw_info->dmi_reg_offset + 0x4);
				lo_val |= lo_val1 << 16;
			}
			*lo_tbl_ptr++ = lo_val;
			if (reg_cfg_cmd->cmd_type == VFE_READ_DMI_64BIT) {
				hi_val = msm_camera_io_r(vfe_dev->vfe_base +
					vfe_dev->hw_info->dmi_reg_offset);
				*hi_tbl_ptr = hi_val;
				hi_tbl_ptr += 2;
				lo_tbl_ptr++;
			}
		}
		break;
	}
	case VFE_READ: {
		int i;
		uint32_t *data_ptr = cfg_data +
				     reg_cfg_cmd->u.rw_info.cmd_data_offset / 4;
		for (i = 0; i < reg_cfg_cmd->u.rw_info.len / 4; i++) {
			*data_ptr++ = msm_camera_io_r(vfe_dev->vfe_base +
						      reg_cfg_cmd->u.rw_info.reg_offset);
			reg_cfg_cmd->u.rw_info.reg_offset += 4;
		}
		break;
	}
	}
	return 0;
}

int msm_isp_proc_cmd(struct vfe_device *vfe_dev, void *arg)
{
	int rc = 0, i;
	struct msm_vfe_cfg_cmd2 *proc_cmd = arg;
	struct msm_vfe_reg_cfg_cmd *reg_cfg_cmd;
	uint32_t *cfg_data;
	
	if (!proc_cmd->num_cfg) {
		pr_err("%s: Passed num_cfg as 0\n", __func__);
		return -EINVAL;
	}

	reg_cfg_cmd = kzalloc(sizeof(struct msm_vfe_reg_cfg_cmd) *
			      proc_cmd->num_cfg, GFP_KERNEL);
	if (!reg_cfg_cmd) {
		pr_err("%s: reg_cfg alloc failed\n", __func__);
		rc = -ENOMEM;
		goto reg_cfg_failed;
	}

	cfg_data = kzalloc(proc_cmd->cmd_len, GFP_KERNEL);
	if (!cfg_data) {
		pr_err("%s: cfg_data alloc failed\n", __func__);
		rc = -ENOMEM;
		goto cfg_data_failed;
	}

	if (copy_from_user(reg_cfg_cmd,
			   (void __user*)(proc_cmd->cfg_cmd),
			   sizeof(struct msm_vfe_reg_cfg_cmd) * proc_cmd->num_cfg)) {
		rc = -EFAULT;
		goto copy_cmd_failed;
	}

	if (copy_from_user(cfg_data,
			   (void __user*)(proc_cmd->cfg_data),
			   proc_cmd->cmd_len)) {
		rc = -EFAULT;
		goto copy_cmd_failed;
	}

	//pr_err("%s: platform_id=%u, kernel_id=%u\n", __func__,proc_cmd->frame_id, vfe_dev->frame_id);
	if( (vfe_dev->frame_id == proc_cmd->frame_id && vfe_dev->eof_event_occur != 1)
		|| proc_cmd->frame_id == 0) {
		for (i = 0; i < proc_cmd->num_cfg; i++)
			msm_isp_send_hw_cmd(vfe_dev, &reg_cfg_cmd[i],
				cfg_data, proc_cmd->cmd_len);
	}
	else{
		rc = MSM_VFE_REG_CFG_FRAME_ID_NOT_MATCH_ERROR;
		pr_err("%s: skip hw update, platform_id=%u, kernel_id=%u, eof_event_occur=%u\n",
			__func__,proc_cmd->frame_id, vfe_dev->frame_id, vfe_dev->eof_event_occur);
	}

	if (copy_to_user(proc_cmd->cfg_data,
			 cfg_data, proc_cmd->cmd_len)) {
		rc = -EFAULT;
		goto copy_cmd_failed;
	}

 copy_cmd_failed:
	kfree(cfg_data);
 cfg_data_failed:
	kfree(reg_cfg_cmd);
 reg_cfg_failed:
	return rc;
}

int msm_isp_send_event(struct vfe_device *vfe_dev,
		       uint32_t event_type,
		       struct msm_isp_event_data *event_data)
{
	struct v4l2_event isp_event;

	memset(&isp_event, 0, sizeof(struct v4l2_event));
	isp_event.id = 0;
	isp_event.type = event_type;
	memcpy(&isp_event.u.data[0], event_data,
	       sizeof(struct msm_isp_event_data));
	v4l2_event_queue(vfe_dev->subdev.sd.devnode, &isp_event);
	return 0;
}

#define CAL_WORD(width, M, N) ((width * M + N - 1) / N)

int msm_isp_cal_word_per_line(uint32_t output_format,
			      uint32_t pixel_per_line)
{
	int val = -1;

	switch (output_format) {
	case V4L2_PIX_FMT_SBGGR8:
	case V4L2_PIX_FMT_SGBRG8:
	case V4L2_PIX_FMT_SGRBG8:
	case V4L2_PIX_FMT_SRGGB8:
	case V4L2_PIX_FMT_QBGGR8:
	case V4L2_PIX_FMT_QGBRG8:
	case V4L2_PIX_FMT_QGRBG8:
	case V4L2_PIX_FMT_QRGGB8:
	case V4L2_PIX_FMT_JPEG:
	case V4L2_PIX_FMT_META:
		val = CAL_WORD(pixel_per_line, 1, 8);
		break;
	case V4L2_PIX_FMT_SBGGR10:
	case V4L2_PIX_FMT_SGBRG10:
	case V4L2_PIX_FMT_SGRBG10:
	case V4L2_PIX_FMT_SRGGB10:
		val = CAL_WORD(pixel_per_line, 5, 32);
		break;
	case V4L2_PIX_FMT_SBGGR12:
	case V4L2_PIX_FMT_SGBRG12:
	case V4L2_PIX_FMT_SGRBG12:
	case V4L2_PIX_FMT_SRGGB12:
		val = CAL_WORD(pixel_per_line, 3, 16);
		break;
	case V4L2_PIX_FMT_QBGGR10:
	case V4L2_PIX_FMT_QGBRG10:
	case V4L2_PIX_FMT_QGRBG10:
	case V4L2_PIX_FMT_QRGGB10:
		val = CAL_WORD(pixel_per_line, 1, 6);
		break;
	case V4L2_PIX_FMT_QBGGR12:
	case V4L2_PIX_FMT_QGBRG12:
	case V4L2_PIX_FMT_QGRBG12:
	case V4L2_PIX_FMT_QRGGB12:
		val = CAL_WORD(pixel_per_line, 1, 5);
		break;
	case V4L2_PIX_FMT_NV12:
	case V4L2_PIX_FMT_NV21:
	case V4L2_PIX_FMT_NV14:
	case V4L2_PIX_FMT_NV41:
	case V4L2_PIX_FMT_NV16:
	case V4L2_PIX_FMT_NV61:
	case V4L2_PIX_FMT_NV46:
	case V4L2_PIX_FMT_NV64:
		val = CAL_WORD(pixel_per_line, 1, 8);
		break;
	/*TD: Add more image format*/
	default:
		pr_err("%s: Invalid output format\n", __func__);
		break;
	}
	return val;
}

int msm_isp_get_bit_per_pixel(uint32_t output_format)
{
	switch (output_format) {
	case V4L2_PIX_FMT_SBGGR8:
	case V4L2_PIX_FMT_SGBRG8:
	case V4L2_PIX_FMT_SGRBG8:
	case V4L2_PIX_FMT_SRGGB8:
	case V4L2_PIX_FMT_QBGGR8:
	case V4L2_PIX_FMT_QGBRG8:
	case V4L2_PIX_FMT_QGRBG8:
	case V4L2_PIX_FMT_QRGGB8:
	case V4L2_PIX_FMT_JPEG:
	case V4L2_PIX_FMT_META:
		return 8;
	case V4L2_PIX_FMT_SBGGR10:
	case V4L2_PIX_FMT_SGBRG10:
	case V4L2_PIX_FMT_SGRBG10:
	case V4L2_PIX_FMT_SRGGB10:
	case V4L2_PIX_FMT_QBGGR10:
	case V4L2_PIX_FMT_QGBRG10:
	case V4L2_PIX_FMT_QGRBG10:
	case V4L2_PIX_FMT_QRGGB10:
		return 10;
	case V4L2_PIX_FMT_SBGGR12:
	case V4L2_PIX_FMT_SGBRG12:
	case V4L2_PIX_FMT_SGRBG12:
	case V4L2_PIX_FMT_SRGGB12:
	case V4L2_PIX_FMT_QBGGR12:
	case V4L2_PIX_FMT_QGBRG12:
	case V4L2_PIX_FMT_QGRBG12:
	case V4L2_PIX_FMT_QRGGB12:
		return 12;
	case V4L2_PIX_FMT_NV12:
	case V4L2_PIX_FMT_NV21:
	case V4L2_PIX_FMT_NV14:
	case V4L2_PIX_FMT_NV41:
	case V4L2_PIX_FMT_NV64:
	case V4L2_PIX_FMT_NV46:
		return 8;
	case V4L2_PIX_FMT_NV16:
	case V4L2_PIX_FMT_NV61:
		return 16;
	/*TD: Add more image format*/
	default:
		pr_err("%s: Invalid output format\n", __func__);
		return 10;
	}
}

void msm_isp_update_error_frame_count(struct vfe_device *vfe_dev)
{
	struct msm_vfe_error_info *error_info = &vfe_dev->error_info;
	error_info->info_dump_frame_count++;
}

void msm_isp_process_error_info(struct vfe_device *vfe_dev)
{
	int i;
	uint8_t num_stats_type =
		vfe_dev->hw_info->stats_hw_info->num_stats_type;	
	struct msm_vfe_error_info *error_info = &vfe_dev->error_info;

	static DEFINE_RATELIMIT_STATE(rs,
				      DEFAULT_RATELIMIT_INTERVAL, DEFAULT_RATELIMIT_BURST);
	static DEFINE_RATELIMIT_STATE(rs_stats,
				      DEFAULT_RATELIMIT_INTERVAL, DEFAULT_RATELIMIT_BURST);

//	if (error_info->error_count == 1 ||
//		!(error_info->info_dump_frame_count % 100)) {
	if (1) {
		vfe_dev->hw_info->vfe_ops.core_ops.
		process_error_status(vfe_dev);
		error_info->error_mask0 = 0;
		error_info->error_mask1 = 0;
		error_info->camif_status = 0;
		error_info->violation_status = 0;
		for (i = 0; i < num_stats_type; i++) {
			if (error_info->stream_framedrop_count[i] != 0 &&
			    __ratelimit(&rs)) {
				pr_err("%s: No buffers! VFE%d, Frame Stream[%d]: dropped %d frames\n",
				       __func__, vfe_dev->pdev->id, i,
				       error_info->stream_framedrop_count[i]);
				error_info->stream_framedrop_count[i] = 0;
			}
		}
		for (i = 0; i < MSM_ISP_STATS_MAX; i++) {
			if (error_info->stats_framedrop_count[i] != 0 &&
			    __ratelimit(&rs_stats)) {
				pr_err("%s: No buffers! VFE%d, Stats stream[%d]: dropped %d frames\n",
				       __func__, vfe_dev->pdev->id, i,
				       error_info->stats_framedrop_count[i]);
				error_info->stats_framedrop_count[i] = 0;
			}
		}
	}
}

static inline void msm_isp_update_error_info(struct vfe_device *vfe_dev,
					     uint32_t error_mask0, uint32_t error_mask1)
{
	vfe_dev->error_info.error_mask0 |= error_mask0;
	vfe_dev->error_info.error_mask1 |= error_mask1;
	vfe_dev->error_info.error_count++;
}

static void msm_isp_process_overflow_irq(
	struct vfe_device *vfe_dev,
	uint32_t *irq_status0, uint32_t *irq_status1)
{
	uint32_t overflow_mask; //, rdi_wm_mask;

	if (vfe_dev->axi_data.src_info[VFE_PIX_0].frame_id == 0 &&
	    vfe_dev->axi_data.src_info[VFE_RAW_0].frame_id == 0) {
//		pr_err("%s first frame. Skip \n", __func__);
//		pr_err("%s: First frame irq_status0 0x%X irq_status1 0x%X\n",
//			__func__, *irq_status0, *irq_status1);
	}

	/*Mask out all other irqs if recovery is started*/
	if (atomic_read(&vfe_dev->error_info.overflow_state) != NO_OVERFLOW) {
		uint32_t halt_restart_mask0, halt_restart_mask1;
		vfe_dev->hw_info->vfe_ops.core_ops.
		get_halt_restart_mask(&halt_restart_mask0,
				      &halt_restart_mask1);
		*irq_status0 &= halt_restart_mask0;
		*irq_status1 &= halt_restart_mask1;

		return;
	}

	/*Check if any overflow bit is set*/
	vfe_dev->hw_info->vfe_ops.core_ops.
	get_overflow_mask(&overflow_mask);
	overflow_mask &= *irq_status1;
//  vfe_dev->hw_info->vfe_ops.core_ops.
//    get_rdi_wm_mask(vfe_dev, &rdi_wm_mask);
	/*
	if (((overflow_mask & 0xFE00) >> 9) & rdi_wm_mask) {

	}
	 */
	if (overflow_mask) {
		struct msm_isp_event_data error_event;
		pr_err("%s: Bus overflow detected: 0x%x\n",
		       __func__, overflow_mask);
		atomic_set(&vfe_dev->error_info.overflow_state,
			   OVERFLOW_DETECTED);
		pr_err("%s: Start bus overflow recovery\n", __func__);
		/*Store current IRQ mask*/
		vfe_dev->hw_info->vfe_ops.core_ops.get_irq_mask(vfe_dev,
								&vfe_dev->error_info.overflow_recover_irq_mask0,
								&vfe_dev->error_info.overflow_recover_irq_mask1);
		/*Halt the hardware & Clear all other IRQ mask*/
		vfe_dev->hw_info->vfe_ops.axi_ops.halt(vfe_dev, 0);
		pr_err("%s HALT vfe_dev %p \n", __func__, vfe_dev);
		/*Stop CAMIF Immediately*/
		vfe_dev->hw_info->vfe_ops.core_ops.
		update_camif_state(vfe_dev, DISABLE_CAMIF_IMMEDIATELY);

		/*Update overflow state*/
//		atomic_set(&vfe_dev->error_info.overflow_state, HALT_REQUESTED);
		*irq_status0 = 0;
		*irq_status1 = 0;

//  overflow_mask &= ~(rdi_wm_mask << 9);
		pr_err("%s: Error! RDI overflow detected. Notify ISPIF to reset overflow_mask 0x%x\n",
		       __func__, overflow_mask);
		/* frame id should be of src_info[overflow RDI WM]. For now take RAW_0 since RAW_0 *
		 * will be the first to be allocated anyway */
		error_event.frame_id = vfe_dev->axi_data.src_info[VFE_PIX_0].frame_id;
//    error_event.input_src = VFE_RAW_0; /* Only support single RDI usecase currently */
		error_event.u.error_info.error_mask = (1 << ISP_WM_BUS_OVERFLOW);
		msm_isp_send_event(vfe_dev, ISP_EVENT_ERROR, &error_event);
	}
}

#if 1
void msm_isp_reset_burst_count_and_frame_drop(
	struct vfe_device *vfe_dev, struct msm_vfe_axi_stream *stream_info)
{
	struct msm_vfe_axi_stream_request_cmd stream_cfg_cmd;

	if (stream_info->state != ACTIVE ||
	    stream_info->stream_type != BURST_STREAM) {
		return;
	}
	if (stream_info->stream_type == BURST_STREAM &&
	    stream_info->num_burst_capture != 0) {
		stream_cfg_cmd.axi_stream_handle =
			stream_info->stream_handle;
		stream_cfg_cmd.burst_count =
			stream_info->num_burst_capture;
		stream_cfg_cmd.frame_skip_pattern =
			stream_info->frame_skip_pattern;
		stream_cfg_cmd.init_frame_drop =
			stream_info->init_frame_drop;
		msm_isp_calculate_framedrop(&vfe_dev->axi_data,
					    &stream_cfg_cmd);
		msm_isp_reset_framedrop(vfe_dev, stream_info);
	}
}
#endif

#if 0
static inline void msm_isp_process_overflow_recovery(
	struct vfe_device *vfe_dev,
	uint32_t irq_status0, uint32_t irq_status1)
{
	uint32_t halt_restart_mask0, halt_restart_mask1;

	vfe_dev->hw_info->vfe_ops.core_ops.
	get_halt_restart_mask(&halt_restart_mask0,
			      &halt_restart_mask1);
	irq_status0 &= halt_restart_mask0;
	irq_status1 &= halt_restart_mask1;
	if (irq_status0 == 0 && irq_status1 == 0)
		return;

	switch (atomic_read(&vfe_dev->error_info.overflow_state)) {
	case HALT_REQUESTED: {
		pr_err("%s: Halt done, Restart Pending\n", __func__);
		/*Reset the hardware*/
		vfe_dev->hw_info->vfe_ops.core_ops.reset_hw(vfe_dev, 0);
		/*Update overflow state*/
		atomic_set(&vfe_dev->error_info.overflow_state,
			   RESTART_REQUESTED);
	}
	break;
	case RESTART_REQUESTED: {
		pr_err("%s: Restart done, Resuming\n", __func__);
		/*Reset the burst stream frame drop pattern, in the
		 * case where bus overflow happens during the burstshot,
		 * the framedrop pattern might be updated after reg update
		 * to skip all the frames after the burst shot. The burst shot
		 * might not be completed due to the overflow, so the framedrop
		 * pattern need to change back to the original settings in order
		 * to recovr from overflow.
		 */
		msm_isp_reset_burst_count(vfe_dev);
		vfe_dev->hw_info->vfe_ops.axi_ops.
		reload_wm(vfe_dev, 0xFFFFFFFF);
		vfe_dev->hw_info->vfe_ops.core_ops.restore_irq_mask(vfe_dev);
		vfe_dev->hw_info->vfe_ops.core_ops.reg_update(vfe_dev);
		memset(&vfe_dev->error_info, 0, sizeof(vfe_dev->error_info));
		atomic_set(&vfe_dev->error_info.overflow_state, NO_OVERFLOW);
		vfe_dev->hw_info->vfe_ops.core_ops.
		update_camif_state(vfe_dev, ENABLE_CAMIF);
	}
	break;
	case NO_OVERFLOW:
	case OVERFLOW_DETECTED:
	default:
		break;
	}
}
#endif
irqreturn_t msm_isp_process_irq(int irq_num, void *data)
{
	unsigned long flags;
	struct msm_vfe_tasklet_queue_cmd *queue_cmd;
	struct msm_vfe_tasklet_queue_cmd *regupdate_q_cmd;
	struct vfe_device *vfe_dev = (struct vfe_device *)data;
	uint32_t irq_status0, irq_status1;
	uint32_t error_mask0, error_mask1;
	struct msm_isp_timestamp ts;

	vfe_dev->hw_info->vfe_ops.irq_ops.
	read_irq_status(vfe_dev, &irq_status0, &irq_status1);
	msm_isp_process_overflow_irq(vfe_dev,
				     &irq_status0, &irq_status1);
	vfe_dev->hw_info->vfe_ops.core_ops.
	get_error_mask(&error_mask0, &error_mask1);
	error_mask0 &= irq_status0;
	error_mask1 &= irq_status1;
	irq_status0 &= ~error_mask0;
	irq_status1 &= ~error_mask1;
	if ((error_mask0 != 0) || (error_mask1 != 0))
		msm_isp_update_error_info(vfe_dev, error_mask0, error_mask1);

	if ((irq_status0 == 0) && (irq_status1 == 0) &&
		(!(((error_mask0 != 0) || (error_mask1 != 0)) &&
		 vfe_dev->error_info.error_count == 1))) {
		ISP_DBG("%s: irq status 0 and 1 = 0, also error irq hadnled!\n",
			__func__);
		return IRQ_HANDLED;
	}
/*
        if (atomic_read(&vfe_dev->error_info.overflow_state) != NO_OVERFLOW) {
                pr_err("%s HW is in overflow state. Don't process IRQ until recovery\n",
                        __func__);
                atomic_set(&vfe_dev->irq_cnt, 0);
                return IRQ_HANDLED;
        }
 */
	msm_isp_get_timestamp(&ts);
	spin_lock_irqsave(&vfe_dev->tasklet_lock, flags);
	queue_cmd = &vfe_dev->tasklet_queue_cmd[vfe_dev->taskletq_idx];
	if (queue_cmd->cmd_used) {
		pr_err_ratelimited("%s: Tasklet queue overflow: %d\n",
				   __func__, vfe_dev->pdev->id);
		list_del(&queue_cmd->list);
	} else {
		atomic_add(1, &vfe_dev->irq_cnt);
	}
	queue_cmd->vfeInterruptStatus0 = irq_status0;
	queue_cmd->vfeInterruptStatus1 = irq_status1;
	queue_cmd->ts = ts;
	queue_cmd->cmd_used = 1;
	vfe_dev->taskletq_idx =
		(vfe_dev->taskletq_idx + 1) % MSM_VFE_TASKLETQ_SIZE;
	list_add_tail(&queue_cmd->list, &vfe_dev->tasklet_q);
	if (vfe_dev->hw_info->vfe_ops.
		core_ops.get_regupdate_status(irq_status0, irq_status1)) {
		regupdate_q_cmd = &vfe_dev->
			tasklet_regupdate_queue_cmd[vfe_dev->
			taskletq_reg_update_idx];
		if (regupdate_q_cmd->cmd_used) {
			pr_err_ratelimited("%s: Tasklet Overflow", __func__);
			list_del(&regupdate_q_cmd->list);
		} else {
			atomic_add(1, &vfe_dev->reg_update_cnt);
		}
		regupdate_q_cmd->vfeInterruptStatus0 = irq_status0;
		regupdate_q_cmd->vfeInterruptStatus1 = irq_status1;
		regupdate_q_cmd->ts = ts;
		regupdate_q_cmd->cmd_used = 1;
		vfe_dev->taskletq_reg_update_idx =
			(vfe_dev->taskletq_reg_update_idx + 1) %
			MSM_VFE_TASKLETQ_SIZE;
		list_add_tail(&regupdate_q_cmd->list,
			&vfe_dev->tasklet_regupdate_q);
	}
	spin_unlock_irqrestore(&vfe_dev->tasklet_lock, flags);
	tasklet_schedule(&vfe_dev->vfe_tasklet);
	return IRQ_HANDLED;
}

void msm_isp_do_tasklet(unsigned long data)
{
	unsigned long flags;
	struct vfe_device *vfe_dev = (struct vfe_device *)data;
	struct msm_vfe_irq_ops *irq_ops = &vfe_dev->hw_info->vfe_ops.irq_ops;
	struct msm_vfe_tasklet_queue_cmd *queue_cmd;
	struct msm_vfe_tasklet_queue_cmd *reg_update_q_cmd;
	struct msm_isp_timestamp ts;
	uint32_t irq_status0, irq_status1;

	while (atomic_read(&vfe_dev->irq_cnt) ||
		(atomic_read(&vfe_dev->reg_update_cnt))) {
		if (atomic_read(&vfe_dev->irq_cnt)) {
			spin_lock_irqsave(&vfe_dev->tasklet_lock, flags);
			queue_cmd = list_first_entry(&vfe_dev->tasklet_q,
			struct msm_vfe_tasklet_queue_cmd, list);
			if (!queue_cmd) {
				atomic_set(&vfe_dev->irq_cnt, 0);
				spin_unlock_irqrestore(&vfe_dev->tasklet_lock, flags);
				continue;
			}
			atomic_sub(1, &vfe_dev->irq_cnt);
			list_del(&queue_cmd->list);
			queue_cmd->cmd_used = 0;
			irq_status0 = queue_cmd->vfeInterruptStatus0;
			irq_status1 = queue_cmd->vfeInterruptStatus1;
			ts = queue_cmd->ts;
			spin_unlock_irqrestore(&vfe_dev->tasklet_lock, flags);

			if (atomic_read(&vfe_dev->error_info.overflow_state) != NO_OVERFLOW) {
				pr_err("Azam: There is Overflow, Ignore IRQs!!!");
				//			msm_isp_process_overflow_recovery(vfe_dev,
				//				irq_status0, irq_status1);
				continue;
			}
			ISP_DBG("%s: status0: 0x%x status1: 0x%x\n",
				__func__, irq_status0, irq_status1);
			irq_ops->process_reset_irq(vfe_dev,
							irq_status0, irq_status1);
			irq_ops->process_halt_irq(vfe_dev,
							irq_status0, irq_status1);
			irq_ops->process_camif_irq(vfe_dev,
							irq_status0, irq_status1, &ts);
			irq_ops->process_axi_irq(vfe_dev,
							irq_status0, irq_status1, &ts);
			irq_ops->process_stats_irq(vfe_dev,
							irq_status0, irq_status1, &ts);
			msm_isp_process_error_info(vfe_dev);
		}
		if (atomic_read(&vfe_dev->reg_update_cnt)) {
			spin_lock_irqsave(&vfe_dev->tasklet_lock, flags);
			reg_update_q_cmd = list_first_entry(
				&vfe_dev->tasklet_regupdate_q,
				struct msm_vfe_tasklet_queue_cmd, list);
			if (!reg_update_q_cmd) {
				atomic_set(&vfe_dev->reg_update_cnt, 0);
				spin_unlock_irqrestore(&vfe_dev->tasklet_lock,
								flags);
				continue;
			}
			atomic_sub(1, &vfe_dev->reg_update_cnt);
			list_del(&reg_update_q_cmd->list);
			reg_update_q_cmd->cmd_used = 0;
			irq_status0 = reg_update_q_cmd->vfeInterruptStatus0;
			irq_status1 = reg_update_q_cmd->vfeInterruptStatus1;
			ts = reg_update_q_cmd->ts;
			spin_unlock_irqrestore(&vfe_dev->tasklet_lock, flags);
			if (atomic_read(&vfe_dev->error_info.overflow_state) !=
				NO_OVERFLOW) {
				continue;
			}
			irq_ops->process_reg_update(vfe_dev,
							irq_status0, irq_status1, &ts);
		}
	}
}

int msm_isp_set_src_state(struct vfe_device *vfe_dev, void *arg)
{
	struct msm_vfe_axi_src_state *src_state = arg;
	if (src_state->input_src >= VFE_SRC_MAX)
		return -EINVAL;
	vfe_dev->axi_data.src_info[src_state->input_src].active =
	src_state->src_active;
	return 0;
}

int msm_isp_open_node(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct vfe_device *vfe_dev = v4l2_get_subdevdata(sd);
	long rc;

	ISP_DBG("%s\n", __func__);

	mutex_lock(&vfe_dev->realtime_mutex);
	mutex_lock(&vfe_dev->core_mutex);
	if (vfe_dev->vfe_open_cnt == 1) {
		pr_err("VFE already open\n");
		mutex_unlock(&vfe_dev->core_mutex);
		mutex_unlock(&vfe_dev->realtime_mutex);
		return -ENODEV;
	}

	if (vfe_dev->hw_info->vfe_ops.core_ops.init_hw(vfe_dev) < 0) {
		pr_err("%s: init hardware failed\n", __func__);
		mutex_unlock(&vfe_dev->core_mutex);
		mutex_unlock(&vfe_dev->realtime_mutex);
		return -EBUSY;
	}

	memset(&vfe_dev->error_info, 0, sizeof(vfe_dev->error_info));
	atomic_set(&vfe_dev->error_info.overflow_state, NO_OVERFLOW);

	rc = vfe_dev->hw_info->vfe_ops.core_ops.reset_hw(vfe_dev, 1, 1);
	if (rc <= 0) {
		pr_err("%s: reset timeout\n", __func__);
		mutex_unlock(&vfe_dev->core_mutex);
		mutex_unlock(&vfe_dev->realtime_mutex);
		return -EINVAL;
	}
	vfe_dev->vfe_hw_version = msm_camera_io_r(vfe_dev->vfe_base);
	ISP_DBG("%s: HW Version: 0x%x\n", __func__, vfe_dev->vfe_hw_version);

	vfe_dev->hw_info->vfe_ops.core_ops.init_hw_reg(vfe_dev);

	vfe_dev->buf_mgr->ops->buf_mgr_init(vfe_dev->buf_mgr, "msm_isp", 28);

	memset(&vfe_dev->axi_data, 0, sizeof(struct msm_vfe_axi_shared_data));
	memset(&vfe_dev->stats_data, 0,
	       sizeof(struct msm_vfe_stats_shared_data));
	vfe_dev->axi_data.hw_info = vfe_dev->hw_info->axi_hw_info;
	vfe_dev->vfe_open_cnt++;
	vfe_dev->taskletq_idx = 0;
	mutex_unlock(&vfe_dev->core_mutex);
	mutex_unlock(&vfe_dev->realtime_mutex);
	return 0;
}

int msm_isp_close_node(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	long rc;
	struct vfe_device *vfe_dev = v4l2_get_subdevdata(sd);

	ISP_DBG("%s\n", __func__);
	mutex_lock(&vfe_dev->realtime_mutex);
	mutex_lock(&vfe_dev->core_mutex);
	if (vfe_dev->vfe_open_cnt == 0) {
		pr_err("%s: Invalid close\n", __func__);
		mutex_unlock(&vfe_dev->core_mutex);
		mutex_unlock(&vfe_dev->realtime_mutex);
		return -ENODEV;
	}

	rc = vfe_dev->hw_info->vfe_ops.axi_ops.halt(vfe_dev, 1);
	if (rc <= 0)
		pr_err("%s: halt timeout rc=%ld\n", __func__, rc);

	vfe_dev->buf_mgr->ops->buf_mgr_deinit(vfe_dev->buf_mgr);
	vfe_dev->hw_info->vfe_ops.core_ops.release_hw(vfe_dev);
	vfe_dev->vfe_open_cnt--;
	mutex_unlock(&vfe_dev->core_mutex);
	mutex_unlock(&vfe_dev->realtime_mutex);
	return 0;
}
