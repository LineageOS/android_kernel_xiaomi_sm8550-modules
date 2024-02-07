/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2020 XiaoMi, Inc. All rights reserved.
 */

#define pr_fmt(fmt)	"mi-dsi-display:[%s] " fmt, __func__

#include "msm_kms.h"
#include "sde_trace.h"
#include "sde_connector.h"
#include "dsi_display.h"
#include "dsi_panel.h"
#include "mi_disp_print.h"
#include "mi_sde_encoder.h"
#include "mi_dsi_display.h"
#include "mi_dsi_panel.h"
#include "mi_disp_feature.h"
#include "mi_panel_id.h"
#include "mi_disp_flatmode.h"

static char oled_wp_info_str[32] = {0};
static char sec_oled_wp_info_str[32] = {0};
static char cell_id_info_str[32] = {0};
static struct panel_manufaturer_info g_panel_manufaturer_info[MI_DISP_MAX];

#define MAX_DEBUG_POLICY_CMDLINE_LEN 64
static char display_debug_policy[MAX_DEBUG_POLICY_CMDLINE_LEN] = {0};

static struct dsi_read_info g_dsi_read_info;

char *get_display_power_mode_name(int power_mode)
{
	switch (power_mode) {
	case SDE_MODE_DPMS_ON:
		return "On";
	case SDE_MODE_DPMS_LP1:
		return "Doze";
	case SDE_MODE_DPMS_LP2:
		return "Doze_Suspend";
	case SDE_MODE_DPMS_STANDBY:
		return "Standby";
	case SDE_MODE_DPMS_SUSPEND:
		return "Suspend";
	case SDE_MODE_DPMS_OFF:
		return "Off";
	default:
		return "Unknown";
	}
}

int mi_get_disp_id(const char *display_type)
{
	if (!strncmp(display_type, "primary", 7))
		return MI_DISP_PRIMARY;
	else
		return MI_DISP_SECONDARY;
}

struct dsi_display * mi_get_primary_dsi_display(void)
{
	struct disp_feature *df = mi_get_disp_feature();
	struct disp_display *dd_ptr = NULL;
	struct dsi_display *dsi_display = NULL;

	if (df) {
		dd_ptr = &df->d_display[MI_DISP_PRIMARY];
		if (dd_ptr->display && dd_ptr->intf_type == MI_INTF_DSI) {
			dsi_display = (struct dsi_display *)dd_ptr->display;
			return dsi_display;
		} else {
			return NULL;
		}
	} else {
		return NULL;
	}
}

struct dsi_display * mi_get_secondary_dsi_display(void)
{
	struct disp_feature *df = mi_get_disp_feature();
	struct disp_display *dd_ptr = NULL;
	struct dsi_display *dsi_display = NULL;

	if (df) {
		dd_ptr = &df->d_display[MI_DISP_SECONDARY];
		if (dd_ptr->display && dd_ptr->intf_type == MI_INTF_DSI) {
			dsi_display = (struct dsi_display *)dd_ptr->display;
			return dsi_display;
		} else {
			return NULL;
		}
	} else {
		return NULL;
	}
}

int mi_dsi_display_set_disp_param(void *display,
			struct disp_feature_ctl *ctl)
{
	struct dsi_display *dsi_display = (struct dsi_display *)display;
	char trace_buf[64];
	int ret = 0;

	if (!dsi_display || !ctl) {
		DISP_ERROR("Invalid display or ctl ptr\n");
		return -EINVAL;
	}

	if (sde_kms_is_suspend_blocked(dsi_display->drm_dev) &&
		mi_dsi_panel_is_need_tx_cmd(ctl->feature_id)) {
		DISP_ERROR("sde_kms is suspended, skip to set disp_param\n");
		return -EBUSY;
	}

	mi_dsi_acquire_wakelock(dsi_display->panel);
	snprintf(trace_buf, sizeof(trace_buf), "set_disp_param:%s",
			get_disp_feature_id_name(ctl->feature_id));
	SDE_ATRACE_BEGIN(trace_buf);
	ret = mi_dsi_panel_set_disp_param(dsi_display->panel, ctl);
	SDE_ATRACE_END(trace_buf);
	mi_dsi_release_wakelock(dsi_display->panel);

	return ret;
}

int mi_dsi_display_get_disp_param(void *display,
			struct disp_feature_ctl *ctl)
{
	struct dsi_display *dsi_display = (struct dsi_display *)display;

	if (!dsi_display) {
		DISP_ERROR("Invalid display ptr\n");
		return -EINVAL;
	}

	return mi_dsi_panel_get_disp_param(dsi_display->panel, ctl);
}

ssize_t mi_dsi_display_show_disp_param(void *display,
			char *buf, size_t size)
{
	struct dsi_display *dsi_display = (struct dsi_display *)display;

	if (!dsi_display) {
		DISP_ERROR("Invalid display ptr\n");
		return -EINVAL;
	}

	return mi_dsi_panel_show_disp_param(dsi_display->panel, buf, size);
}

static int mi_dsi_display_set_vdo_timing(struct dsi_mode_info *timing,
          const char *type, u32 value)
{
	int ret = 0;

	if (strncmp(type, "HPW", 3) == 0)
		timing->h_sync_width = value;
	else if (strncmp(type, "HFP", 3) == 0)
		timing->h_front_porch = value;
	else if (strncmp(type, "HBP", 3) == 0)
		timing->h_back_porch = value;
	else if (strncmp(type, "VPW", 3) == 0)
		timing->v_sync_width = value;
	else if (strncmp(type, "VFP", 3) == 0)
		timing->v_front_porch = value;
	else if (strncmp(type, "VBP", 3) == 0)
		timing->v_back_porch = value;
	else {
		DISP_ERROR("MI_dsi set timing :no such type!!\n");
		ret = -EINVAL;
	}
	return ret;
}

static u32 mi_dsi_display_get_cmd_timing_val(struct dsi_mode_info *timing,
          const char *type)
{
	u32 ret = 0;

	if (strncmp(type, "HPW", 3) == 0)
		ret = timing->h_sync_width;
	else if (strncmp(type, "HFP", 3) == 0)
		ret = timing->h_front_porch;
	else if (strncmp(type, "HBP", 3) == 0)
		ret = timing->h_back_porch;
	else if (strncmp(type, "HACT", 4) == 0)
		ret = timing->h_active;
	else if (strncmp(type, "VPW", 3) == 0)
		ret = timing->v_sync_width;
	else if (strncmp(type, "VFP", 3) == 0)
		ret = timing->v_front_porch;
	else if (strncmp(type, "VBP", 3) == 0)
		ret = timing->v_back_porch;
	else if (strncmp(type, "VACT", 4) == 0)
		ret = timing->v_active;
	else if (strncmp(type, "FPS", 3) == 0)
		ret = timing->refresh_rate;
	else {
		DISP_ERROR("MI_dsi set timing :no such type!!\n");
		ret = -EINVAL;
	}
	return ret;
}

static ssize_t mi_dsi_display_set_dsi_phy_rw_real(void *display,
			u32 *buffer, u32 buf_size)
{
	struct dsi_display *dsi_display = (struct dsi_display *)display;
	struct dsi_display_mode *cur_mode = NULL;
	int ret = 0;

	if (!dsi_display || !dsi_display->panel) {
		DISP_ERROR("Invalid display/panel ptr\n");
		return -EINVAL;
	}

	mutex_lock(&dsi_display->display_lock);

	cur_mode = dsi_display->panel->cur_mode;
	if (cur_mode) {
		struct dsi_display_mode_priv_info *priv_info;

		priv_info = cur_mode->priv_info;
		priv_info->phy_timing_val = buffer;
		if (priv_info->phy_timing_len && priv_info->phy_timing_len == buf_size) {
			int i = 0;

			display_for_each_ctrl(i, dsi_display) {
				struct dsi_display_ctrl *ctrl;

				ctrl = &dsi_display->ctrl[i];
				ret = dsi_phy_set_timing_params(ctrl->phy,
						priv_info->phy_timing_val,
						priv_info->phy_timing_len,
						true);
				if (ret)
					DISP_ERROR("Fail to add timing params\n");
			}
		}
	} else {
		ret = -EINVAL;
	}
	mutex_unlock(&dsi_display->display_lock);

	return ret;
}

static ssize_t mi_dsi_display_set_dsi_porch_rw_real(void *display,
			const char *type, u32 value)
{
	struct dsi_display *dsi_display = (struct dsi_display *)display;
	struct dsi_mode_info *host_mode;
	struct dsi_display_ctrl *ctrl;
	struct dsi_ctrl *dsi_ctrl;
	int ret = 0, i = 0;

	if (!display) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}

	display_for_each_ctrl(i, dsi_display) {
		ctrl = &dsi_display->ctrl[i];
		if (!ctrl->ctrl)
			continue;

		dsi_ctrl = ctrl->ctrl;
		break;
	}

	if (!dsi_display || !dsi_display->panel) {
		DISP_ERROR("Invalid display/panel ptr\n");
		return -EINVAL;
	}

	mutex_lock(&dsi_ctrl->ctrl_lock);
	host_mode = &dsi_ctrl->host_config.video_timing;
	mi_dsi_display_set_vdo_timing(host_mode, type, value);
	if (dsi_ctrl->host_config.panel_mode == DSI_OP_CMD_MODE) {
		dsi_ctrl->hw.ops.setup_cmd_stream(&dsi_ctrl->hw,
				&dsi_ctrl->host_config.video_timing,
				&dsi_ctrl->host_config.common_config,
				0x0,
				&dsi_ctrl->roi);
	} else {
		dsi_ctrl->hw.ops.set_video_timing(&dsi_ctrl->hw, host_mode);
	}
	mutex_unlock(&dsi_ctrl->ctrl_lock);

	return ret;

}

static ssize_t mi_dsi_display_get_dsi_porch_rw(void *display,
			const char *type)
{
	struct dsi_display *dsi_display = (struct dsi_display *)display;
	struct dsi_mode_info *host_mode;
	struct dsi_display_ctrl *ctrl;
	struct dsi_ctrl *dsi_ctrl;
	int ret = 0, i = 0;

	if (!display) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}

	display_for_each_ctrl(i, dsi_display) {
		ctrl = &dsi_display->ctrl[i];
		if (!ctrl->ctrl)
			continue;

		dsi_ctrl = ctrl->ctrl;
		break;
	}

	if (!dsi_display || !dsi_display->panel) {
		DISP_ERROR("Invalid display/panel ptr\n");
		return -EINVAL;
	}

	mutex_lock(&dsi_ctrl->ctrl_lock);
	host_mode = &dsi_ctrl->host_config.video_timing;
	if (dsi_ctrl->host_config.panel_mode != DSI_OP_CMD_MODE) {
		ret = dsi_ctrl->hw.ops.get_video_timing(&dsi_ctrl->hw, type);
	} else {
		ret = mi_dsi_display_get_cmd_timing_val(host_mode, type);
	}
	mutex_unlock(&dsi_ctrl->ctrl_lock);

	return ret;

}

void mi_dsi_display_set_dsi_phy_rw(struct disp_display *dd_ptr, const char *opt)
{
	char *buf;
	char *token, *input_copy, *input_dup = NULL;
	const char *delim = " ";
	u32 *buffer = NULL;
	int ret = 0;
	u32 buf_size = 0;
	u32 tmp_data = 0;

	buf = (char *)(opt);
	DISP_TIME_INFO("dsi_phy display_debug buf {%s}\n", buf);
	input_copy = kstrdup(buf, GFP_KERNEL);
	if (!input_copy) {
		goto exit;
	}
	input_dup = input_copy;
	/* removes leading and trailing whitespace from input_copy */
	input_copy = strim(input_copy);

	/* Split a string into token */
	token = strsep(&input_copy, delim);
	if (token) {
		ret = kstrtoint(token, 10, &tmp_data);
		if (ret) {
			DISP_ERROR("dsi_phy input buffer conversion failed\n");
			goto exit_free0;
		}
	}
	/* Removes leading whitespace from input_copy */
	if (input_copy)
		input_copy = skip_spaces(input_copy);
	else
		goto exit_free0;
	buffer = kzalloc(strlen(input_copy), GFP_KERNEL);
	if (!buffer) {
		goto exit_free1;
	}

	token = strsep(&input_copy, delim);
	while (token) {
		ret = kstrtoint(token, 16, &tmp_data);
		if (ret) {
			DISP_ERROR("dsi_phy input buffer conversion failed\n");
			goto exit_free1;
		}
		DISP_TIME_INFO("dsi_phy -buffer[%d] = 0x%02x\n", buf_size, tmp_data);
		buffer[buf_size++] = (tmp_data & 0xff);
		/* Removes leading whitespace from input_copy */
		if (input_copy) {
			input_copy = skip_spaces(input_copy);
			token = strsep(&input_copy, delim);
		} else {
			token = NULL;
		}
	}

	mi_dsi_display_set_dsi_phy_rw_real(dd_ptr->display, buffer, buf_size);

exit_free1:
	kfree(buffer);
exit_free0:
	kfree(input_dup);
exit:
	return;
}

void mi_dsi_display_set_dsi_porch_rw(struct disp_display *dd_ptr, const char *opt)
{
	unsigned int phy_timming_value;

	if (strncmp(opt, "set", 3) == 0) {
		char option[100] = "";
		char *tmp;
		int i, ret;

		tmp = (char *)(opt + 4);
		for (i = 0; i < 100; i++) {
			if (tmp[i] != ',' && tmp[i] != ' ')
				option[i] = tmp[i];
			else
				break;
		}
		tmp += i + 1;
		ret = sscanf(tmp, "%u\n", &phy_timming_value);
		if (ret != 1) {
			DISP_ERROR("error to parse cmd %s: %s %s ret=%d\n", opt,
				  option, tmp, ret);
			return;
		}
		ret = mi_dsi_display_set_dsi_porch_rw_real(dd_ptr->display, option, phy_timming_value);
	}
	return;
}

static ssize_t mi_dsi_display_get_dsi_phy_rw(struct dsi_display  *dsi_display,
		u32 *phy_timming, char *buf, size_t size, u8 phy_len)
{
	struct dsi_display_ctrl *ctrl;
	struct msm_dsi_phy *phy = NULL;
	int i = 0;
	int rc = 0;
	ssize_t count = 0;

	if (!phy_timming || !dsi_display || !dsi_display->panel ||
		dsi_display->panel->power_mode != SDE_MODE_DPMS_ON) {
		DISP_ERROR("Invalid display/panel ptr or power off\n");
		return -EAGAIN;
	}
	if (dsi_display->config.panel_mode == DSI_OP_CMD_MODE) {
		rc = dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
			DSI_CORE_CLK, DSI_CLK_ON);
		if (rc) {
			DISP_ERROR("[%s] failed to enable DSI core clocks, rc=%d\n",
		       dsi_display->name, rc);
			return -EINVAL;
		}
	}

	display_for_each_ctrl(i, dsi_display) {
		ctrl = &dsi_display->ctrl[i];
		if (!ctrl->ctrl)
			continue;

		phy = ctrl->phy;
		break;
	}

	if (phy && phy->hw.ops.get_phy_timing) {
		phy->hw.ops.get_phy_timing(&phy->hw,
				phy_timming, phy_len);
	}

	if (!phy_timming) {
		DISP_ERROR("invalid params\n");
		rc = -EINVAL;
		goto exit;
	}
	count += snprintf(buf + count, size - count, "\n");
	for (i = 0; i < phy_len; i++) {
		if (i == phy_len - 1) {
			count += snprintf(buf + count, size - count, "0x%02X\n",
				    phy_timming[i]);
		} else {
			count += snprintf(buf + count, size - count, "0x%02X,",
			     phy_timming[i]);
		}
	}
	rc = count;
exit:
	if (dsi_display->config.panel_mode == DSI_OP_CMD_MODE) {
		dsi_display_clk_ctrl(dsi_display->dsi_clk_handle,
			DSI_CORE_CLK, DSI_CLK_OFF);
	}

	return rc;

}

ssize_t mi_dsi_display_show_dsi_phy_rw(void *display,
			char *buf, size_t size)
{
	struct dsi_display *dsi_display = (struct dsi_display *)display;
	struct dsi_display_mode *cur_mode = NULL;
	int ret = 0;
	u8 phy_timing_len = 0;
	u32 *phy_timing = NULL;

	if (!dsi_display || !dsi_display->panel) {
		DISP_ERROR("Invalid display/panel ptr\n");
		return -EINVAL;
	}
	mutex_lock(&dsi_display->display_lock);
	cur_mode = dsi_display->panel->cur_mode;

	if (cur_mode) {
		phy_timing_len = cur_mode->priv_info->phy_timing_len;
		phy_timing = kzalloc((sizeof(u32) * phy_timing_len), GFP_KERNEL);
		if (!phy_timing) {
			mutex_unlock(&dsi_display->display_lock);
			return -ENOMEM;
		}
		ret = mi_dsi_display_get_dsi_phy_rw(dsi_display, phy_timing,
				buf, size, phy_timing_len);
	} else {
		ret = -EINVAL;
	}

	mutex_unlock(&dsi_display->display_lock);
	kfree(phy_timing);
	return ret;
}

ssize_t mi_dsi_display_show_dsi_porch_rw(void *display,
			char *buf, size_t size)
{
	char vdo_type[] = {"HPW HFP HBP VPW VFP VBP"};
	char *p,*token;
	const char *delim = " ";
	ssize_t count = 0;

	p = vdo_type;
	token = strsep(&p, delim);
	count += snprintf(buf + count, size - count, "\n");
	while (token) {
		unsigned int dsi_porch_ret;

		dsi_porch_ret = mi_dsi_display_get_dsi_porch_rw(display, token);
		count += snprintf(buf + count, size - count, "%s:%u\n", token, dsi_porch_ret);
		/* Removes leading whitespace from input_copy */
		if (p) {
			p = skip_spaces(p);
			token = strsep(&p, delim);
		} else {
			token = NULL;
		}
	}

	return count;
}

ssize_t mi_dsi_display_show_pps_rw(void *display,
			char *buf, size_t size)
{
	struct dsi_display *dsi_display = (struct dsi_display *)display;
	u32 i = 0;
	ssize_t count = 0;

	if (!dsi_display || !dsi_display->panel) {
		DISP_ERROR("Invalid display/panel ptr\n");
		return -EINVAL;
	}
	count += snprintf(buf + count, size - count, "\n");
	mutex_lock(&dsi_display->display_lock);
	for (i = 7; i < DSI_CMD_PPS_SIZE; i++) {
		if (i == size - 1 || (i -6) % 16 == 0) {
			count += snprintf(buf + count, size - count, "0x%02X\n",
				    dsi_display->panel->dce_pps_cmd[i]);
		} else {
			count += snprintf(buf + count, size - count, "0x%02X,",
			     dsi_display->panel->dce_pps_cmd[i]);
		}
	}
	mutex_unlock(&dsi_display->display_lock);

	return count;
}

int mi_dsi_display_write_dsi_cmd(void *display,
			struct dsi_cmd_rw_ctl *ctl)
{
	struct dsi_display *dsi_display = (struct dsi_display *)display;
	int ret = 0;

	if (!dsi_display) {
		DISP_ERROR("Invalid display ptr\n");
		return -EINVAL;
	}

	if (sde_kms_is_suspend_blocked(dsi_display->drm_dev)) {
		DISP_ERROR("sde_kms is suspended, skip to write dsi cmd\n");
		return -EBUSY;
	}

	mi_dsi_acquire_wakelock(dsi_display->panel);
	ret = mi_dsi_panel_write_dsi_cmd(dsi_display->panel, ctl);
	mi_dsi_release_wakelock(dsi_display->panel);

	return ret;
}

int mi_dsi_display_read_dsi_cmd(void *display,
			struct dsi_cmd_rw_ctl *ctl)
{
	struct dsi_display *dsi_display = (struct dsi_display *)display;
	int ret = 0;

	if (!dsi_display) {
		DISP_ERROR("Invalid display ptr\n");
		return -EINVAL;
	}

	if (sde_kms_is_suspend_blocked(dsi_display->drm_dev)) {
		DISP_ERROR("sde_kms is suspended, skip to read dsi cmd\n");
		return -EBUSY;
	}

	mi_dsi_acquire_wakelock(dsi_display->panel);
	ret = dsi_display_cmd_receive(dsi_display,
			ctl->tx_ptr, ctl->tx_len, ctl->rx_ptr, ctl->rx_len);
	mi_dsi_release_wakelock(dsi_display->panel);

	return ret;
}

int mi_dsi_display_set_mipi_rw(void *display, char *buf)
{
	struct dsi_display *dsi_display = (struct dsi_display *)display;
	struct dsi_cmd_rw_ctl ctl;
	int ret = 0;
	char *token, *input_copy, *input_dup = NULL;
	const char *delim = " ";
	bool is_read = false;
	char *buffer = NULL;
	u32 buf_size = 0;
	u32 tmp_data = 0;
	u32 recv_len = 0;

	if (!dsi_display) {
		DISP_ERROR("Invalid display ptr\n");
		return -EINVAL;
	}

	memset(&ctl, 0, sizeof(struct dsi_cmd_rw_ctl));
	memset(&g_dsi_read_info, 0, sizeof(struct dsi_read_info));

	DISP_TIME_INFO("input buffer:{%s}\n", buf);

	input_copy = kstrdup(buf, GFP_KERNEL);
	if (!input_copy) {
		ret = -ENOMEM;
		goto exit;
	}

	input_dup = input_copy;
	/* removes leading and trailing whitespace from input_copy */
	input_copy = strim(input_copy);

	/* Split a string into token */
	token = strsep(&input_copy, delim);
	if (token) {
		ret = kstrtoint(token, 10, &tmp_data);
		if (ret) {
			DISP_ERROR("input buffer conversion failed\n");
			goto exit_free0;
		}
		is_read = !!tmp_data;
	}

	/* Removes leading whitespace from input_copy */
	if (input_copy)
		input_copy = skip_spaces(input_copy);
	else
		goto exit_free0;

	token = strsep(&input_copy, delim);
	if (token) {
		ret = kstrtoint(token, 10, &tmp_data);
		if (ret) {
			DISP_ERROR("input buffer conversion failed\n");
			goto exit_free0;
		}
		if (tmp_data > sizeof(g_dsi_read_info.rx_buf)) {
			DISP_ERROR("read size exceeding the limit %d\n",
					sizeof(g_dsi_read_info.rx_buf));
			goto exit_free0;
		}
		ctl.rx_len = tmp_data;
		ctl.rx_ptr = g_dsi_read_info.rx_buf;
	}

	/* Removes leading whitespace from input_copy */
	if (input_copy)
		input_copy = skip_spaces(input_copy);
	else
		goto exit_free0;

	buffer = kzalloc(strlen(input_copy), GFP_KERNEL);
	if (!buffer) {
		ret = -ENOMEM;
		goto exit_free0;
	}

	token = strsep(&input_copy, delim);
	while (token) {
		ret = kstrtoint(token, 16, &tmp_data);
		if (ret) {
			DISP_ERROR("input buffer conversion failed\n");
			goto exit_free1;
		}
		DISP_DEBUG("buffer[%d] = 0x%02x\n", buf_size, tmp_data);
		buffer[buf_size++] = (tmp_data & 0xff);
		/* Removes leading whitespace from input_copy */
		if (input_copy) {
			input_copy = skip_spaces(input_copy);
			token = strsep(&input_copy, delim);
		} else {
			token = NULL;
		}
	}
	ctl.tx_len = buf_size;
	ctl.tx_ptr = buffer;

	if (is_read) {
		recv_len = mi_dsi_display_read_dsi_cmd(dsi_display, &ctl);
		if (recv_len <= 0 || recv_len != ctl.rx_len) {
			DISP_ERROR("read dsi cmd transfer failed rc = %d\n", ret);
			ret = -EAGAIN;
		} else {
			g_dsi_read_info.is_read_sucess = true;
			g_dsi_read_info.rx_len = recv_len;
			ret = 0;
		}
	} else {
		ret = mi_dsi_display_write_dsi_cmd(dsi_display, &ctl);
	}

exit_free1:
	kfree(buffer);
exit_free0:
	kfree(input_dup);
exit:
	return ret;
}

ssize_t mi_dsi_display_show_mipi_rw(void *display,
			char *buf, size_t size)
{
	struct dsi_display *dsi_display = (struct dsi_display *)display;
	ssize_t count = 0;
	int i = 0;

	if (!dsi_display) {
		DISP_ERROR("Invalid display ptr\n");
		return -EINVAL;
	}

	if (g_dsi_read_info.is_read_sucess) {
		for (i = 0; i < g_dsi_read_info.rx_len; i++) {
			if (i == g_dsi_read_info.rx_len - 1) {
				count += snprintf(buf + count, PAGE_SIZE - count, "0x%02X\n",
					 g_dsi_read_info.rx_buf[i]);
			} else {
				count += snprintf(buf + count, PAGE_SIZE - count, "0x%02X,",
					 g_dsi_read_info.rx_buf[i]);
			}
		}
	}

	return count;
}

ssize_t mi_dsi_display_read_panel_info(void *display,
			char *buf, size_t size)
{
	struct dsi_display *dsi_display = (struct dsi_display *)display;
	char *pname = NULL;
	int ret = 0;

	if (!dsi_display) {
		DISP_ERROR("Invalid display ptr\n");
		return -EINVAL;
	}

	pname = mi_dsi_display_get_cmdline_panel_info(dsi_display);
	if (pname) {
		ret = snprintf(buf, size, "panel_name=%s\n", pname);
		kfree(pname);
	} else {
		if (dsi_display->name) {
			/* find the last occurrence of a character in a string */
			pname = strrchr(dsi_display->name, ',');
			if (pname && *pname)
				ret = snprintf(buf, size, "panel_name=%s\n", ++pname);
			else
				ret = snprintf(buf, size, "panel_name=%s\n", dsi_display->name);
		} else {
			ret = snprintf(buf, size, "panel_name=%s\n", "null");
		}
	}

	return ret;
}

static int mi_dsi_display_read_panel_build_id_sub(struct dsi_display *display)
{
	int rc = 0;
	struct drm_panel_build_id_config *config;
	struct dsi_cmd_desc cmd;
	struct dsi_panel *panel;
	struct mi_dsi_panel_cfg *mi_cfg;

	if (!display || !display->panel)
		return -EINVAL;

	panel = display->panel;
	mi_cfg = &panel->mi_cfg;
	if (!mi_cfg->panel_build_id_read_needed) {
		DISP_INFO("panel build id do not have to read\n");
		return 0;
	}

	config = &(panel->id_config);
	cmd = config->sub_id_cmd.cmds[0];

	mutex_lock(&panel->panel_lock);
	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_PANEL_BUILD_ID_SUB_WRITE_1);
	if (rc) {
		DISP_ERROR("[%s] failed to send DSI_CMD_SET_MI_PANEL_BUILD_ID_SUB_WRITE_1 cmds, rc=%d\n",
		       panel->name, rc);
		mutex_unlock(&panel->panel_lock);
		return rc;
	} else
		mutex_unlock(&panel->panel_lock);

	rc = mi_dsi_display_cmd_read_locked(display, cmd, &config->build_id, config->id_cmds_rlen);
	if (rc <= 0)
		DISP_ERROR("[DSI] Display command receive failed, rc=%d\n", rc);

	mutex_lock(&panel->panel_lock);
	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_PANEL_BUILD_ID_SUB_WRITE_2);
	if (rc) {
		DISP_ERROR("[%s] failed to send DSI_CMD_SET_MI_PANEL_BUILD_ID_SUB_WRITE_2 cmds, rc=%d\n",
			panel->name, rc);
		mutex_unlock(&panel->panel_lock);
		return rc;
	} else {
		mutex_unlock(&panel->panel_lock);
		rc = config->id_cmds_rlen;
	}
	DISP_DEBUG("[DSI] readed build id %x, rc=%d\n", config->build_id, rc);
	return rc;
}

int mi_dsi_display_read_panel_build_id(struct dsi_display *display)
{
	int rc = 0;
	struct drm_panel_build_id_config *config;
	struct dsi_cmd_desc cmd;
	struct dsi_panel *panel;
	struct mi_dsi_panel_cfg *mi_cfg;

	if (!display || !display->panel)
		return -EINVAL;

	panel = display->panel;
	mi_cfg = &panel->mi_cfg;
	if (!mi_cfg->panel_build_id_read_needed) {
		DISP_INFO("panel build id do not have to read\n");
		return 0;
	}

	config = &(panel->id_config);

	cmd = config->id_cmd.cmds[0];
	rc = mi_dsi_display_cmd_read_locked(display, cmd, &config->build_id, config->id_cmds_rlen);

	if (rc <= 0)
		DISP_ERROR("[DSI] Display command receive failed, rc=%d\n", rc);
	else if ((config->build_id == 0x40) && (mi_get_panel_id_by_dsi_panel(panel) != M1_PANEL_PA)) {
		config->build_id = 0x00;
		rc = mi_dsi_display_read_panel_build_id_sub(display);
		if (rc <= 0)
			DISP_ERROR("[DSI] read sub panel id failed, rc=%d\n", rc);
		else {
			mi_cfg->panel_build_id_read_needed = false;
		}
	} else if ((mi_get_panel_id_by_dsi_panel(panel) == N11_PANEL_PA)) {
		DISP_INFO("[DSI] N11 readed build id %x\n", config->build_id);
	} else
		mi_cfg->panel_build_id_read_needed = false;

	return rc;
}

ssize_t mi_dsi_display_read_panel_build_id_info(void *display,
                        char *buf, size_t size)
{
	struct dsi_display *dsi_display = (struct dsi_display *)display;
	struct dsi_panel *panel;
	int ret = 0;

	if (!dsi_display || !dsi_display->panel) {
		DISP_ERROR("Invalid display ptr\n");
		return -EINVAL;
	}

	panel = dsi_display->panel;
	if (panel->id_config.build_id) {
		ret = snprintf(buf, size, "0x%02X\n", panel->id_config.build_id);
	} else {
		ret = snprintf(buf, size, "%s\n", "Unsupported");
	}
	return ret;
}

static int mi_dsi_display_re_read_wp_info(struct dsi_display *display)
{
	int rc = 0;
	struct drm_panel_wp_config *config;
	struct dsi_cmd_desc cmd;
	struct dsi_panel *panel;

	if (!display || !display->panel)
		return -EINVAL;

	config = &(display->panel->wp_config);
	if (config->wp_cmd.count == 0 || config->wp_cmds_rlen == 0) {
		return -EINVAL;
	}

	if (config->offset_cmd.count != 0) {
		struct dsi_panel_cmd_set *cmd_sets = &config->offset_cmd;
		panel = display->panel;

		mutex_lock(&panel->panel_lock);
		rc = mi_dsi_panel_write_cmd_set(panel, cmd_sets);
		if (rc) {
			DISP_ERROR("[%s] failed to send DSI_CMD_SET_PANEL_WP_READ_OFFSET, rc=%d\n",
               panel->name, rc);
			mutex_unlock(&panel->panel_lock);
			return rc;
		} else {
			mutex_unlock(&panel->panel_lock);
		}
	}

	cmd = config->wp_cmd.cmds[0];
	if (!config->return_buf) {
		DISP_ERROR("[%s] wp_info return buffer is null, rc=%d\n",
			display->name, rc);
		return -ENOMEM;
	}

	memset(config->return_buf, 0x0, sizeof(*config->return_buf));

	rc = mi_dsi_display_cmd_read(display, cmd, config->return_buf, config->wp_cmds_rlen);
	if (rc <= 0)
		DISP_ERROR("[DSI] Display command receive failed, rc=%d\n", rc);
	return rc;
}

ssize_t mi_dsi_display_read_wp_info(void *display,
			char *buf, size_t size)
{
	struct dsi_display *dsi_display = (struct dsi_display *)display;
	struct dsi_panel *panel;
	int display_id = 0;
	int ret = 0, i = 0;
	char* wp_info_str;

	if (!dsi_display || !dsi_display->panel) {
		DISP_ERROR("Invalid display ptr\n");
		return -EINVAL;
	}

	display_id = mi_get_disp_id(dsi_display->display_type);
	panel = dsi_display->panel;

	if (display_id == MI_DISP_PRIMARY)
		wp_info_str = oled_wp_info_str;
	else if (display_id == MI_DISP_SECONDARY)
		wp_info_str = sec_oled_wp_info_str;
	else {
		ret = snprintf(buf, size, "%s\n", "Unsupported display");
		return ret;
	}

	if (!strlen(wp_info_str)) {
		DISP_WARN("[%s-%d]read oled_wp_info_str() failed from cmdline\n",
				dsi_display->name, display_id);
		ret = mi_dsi_display_re_read_wp_info(display);
		if (ret <= 0) {
			wp_info_str = NULL;
			DISP_ERROR("[%s-%d] read wp_info failed, rc=%d\n",
					dsi_display->name, display_id, ret);
			return ret;
		}

		for (i = 0; i < panel->wp_config.wp_cmds_rlen-panel->wp_config.wp_read_info_index; i++) {
			snprintf(wp_info_str + i * 2, size - (i * 2), "%02x", panel->wp_config.return_buf[i+panel->wp_config.wp_read_info_index]);
		}
	}

	DISP_TIME_INFO("%s  display wp info is %s,index is %d\n", dsi_display->display_type, wp_info_str,panel->wp_config.wp_read_info_index);
	ret = snprintf(buf, size, "%s\n", wp_info_str);
	return ret;
}

int mi_dsi_display_get_fps(void *display, u32 *fps)
{
	struct dsi_display *dsi_display = (struct dsi_display *)display;
	struct dsi_display_mode *cur_mode = NULL;
	u32 ddic_mode, ddic_fps, ddic_min_fps;
	int ret = 0;

	if (!dsi_display || !dsi_display->panel) {
		DISP_ERROR("Invalid display/panel ptr\n");
		return -EINVAL;
	}

	mutex_lock(&dsi_display->display_lock);
	cur_mode = dsi_display->panel->cur_mode;
	if (cur_mode) {
		if (cur_mode->timing.h_skew) {
			ddic_mode = (cur_mode->timing.h_skew >> 14);
			ddic_fps = ((cur_mode->timing.h_skew >> 7) & 0x7F);
			ddic_min_fps = cur_mode->timing.h_skew & 0x7F;
			*fps = (ddic_mode * 100000) + (ddic_fps * 100) + ddic_min_fps;
		}
		else
			*fps =  cur_mode->timing.refresh_rate;
	} else {
		ret = -EINVAL;
	}
	mutex_unlock(&dsi_display->display_lock);

	return ret;
}

int mi_dsi_display_set_doze_brightness(void *display,
			u32 doze_brightness)
{
	struct dsi_display *dsi_display = (struct dsi_display *)display;
	int disp_id = MI_DISP_PRIMARY;
	int ret = 0;

	if (!dsi_display || !dsi_display->panel) {
		DISP_ERROR("invalid display/panel\n");
		return -EINVAL;
	}

	if (sde_kms_is_suspend_blocked(dsi_display->drm_dev)) {
		DISP_ERROR("sde_kms is suspended, skip to set doze brightness\n");
		return -EBUSY;
	}

	mi_dsi_acquire_wakelock(dsi_display->panel);
	mutex_lock(&dsi_display->panel->mi_cfg.doze_lock);
	SDE_ATRACE_BEGIN("set_doze_brightness");
	ret = mi_dsi_panel_set_doze_brightness(dsi_display->panel,
				doze_brightness);
	SDE_ATRACE_END("set_doze_brightness");
	mutex_unlock(&dsi_display->panel->mi_cfg.doze_lock);
	mi_dsi_release_wakelock(dsi_display->panel);

	disp_id = mi_get_disp_id(dsi_display->display_type);
	mi_disp_feature_event_notify_by_type(disp_id, MI_DISP_EVENT_DOZE,
			sizeof(doze_brightness), doze_brightness);

	return ret;

}

int mi_dsi_display_get_doze_brightness(void *display,
			u32 *doze_brightness)
{
	struct dsi_display *dsi_display = (struct dsi_display *)display;

	if (!dsi_display) {
		DISP_ERROR("Invalid display ptr\n");
		return -EINVAL;
	}

	return mi_dsi_panel_get_doze_brightness(dsi_display->panel,
				doze_brightness);
}

int mi_dsi_display_get_brightness(void *display,
			u32 *brightness)
{
	struct dsi_display *dsi_display = (struct dsi_display *)display;

	if (!dsi_display) {
		DISP_ERROR("Invalid display ptr\n");
		return -EINVAL;
	}

	return mi_dsi_panel_get_brightness(dsi_display->panel,
				brightness);
}

int mi_dsi_display_write_dsi_cmd_set(void *display,
			int type)
{
	struct dsi_display *dsi_display = (struct dsi_display *)display;
	int ret = 0;

	if (!dsi_display) {
		DISP_ERROR("Invalid display ptr\n");
		return -EINVAL;
	}

	if (sde_kms_is_suspend_blocked(dsi_display->drm_dev)) {
		DISP_ERROR("sde_kms is suspended, skip to write dsi cmd_set\n");
		return -EBUSY;
	}

	mi_dsi_acquire_wakelock(dsi_display->panel);
	ret = mi_dsi_panel_write_dsi_cmd_set(dsi_display->panel, type);
	mi_dsi_release_wakelock(dsi_display->panel);

	return ret;
}

ssize_t mi_dsi_display_show_dsi_cmd_set_type(void *display,
			char *buf, size_t size)
{
	struct dsi_display *dsi_display = (struct dsi_display *)display;

	if (!dsi_display) {
		DISP_ERROR("Invalid display ptr\n");
		return -EINVAL;
	}

	return mi_dsi_panel_show_dsi_cmd_set_type(dsi_display->panel, buf, size);
}

int mi_dsi_display_set_brightness_clone(void *display,
			u32 brightness_clone)
{
	struct dsi_display *dsi_display = (struct dsi_display *)display;
	int ret = 0;

	if (!dsi_display) {
		DISP_ERROR("Invalid display ptr\n");
		return -EINVAL;
	}

	ret = mi_dsi_panel_set_brightness_clone(dsi_display->panel,
				brightness_clone);

	return ret;
}

int mi_dsi_display_get_brightness_clone(void *display,
			u32 *brightness_clone)
{
	struct dsi_display *dsi_display = (struct dsi_display *)display;

	if (!dsi_display) {
		DISP_ERROR("Invalid display ptr\n");
		return -EINVAL;
	}

	return mi_dsi_panel_get_brightness_clone(dsi_display->panel,
				brightness_clone);
}

int mi_dsi_display_get_max_brightness_clone(void *display,
			u32 *max_brightness_clone)
{
	struct dsi_display *dsi_display = (struct dsi_display *)display;

	if (!dsi_display) {
		DISP_ERROR("Invalid display ptr\n");
		return -EINVAL;
	}

	return mi_dsi_panel_get_max_brightness_clone(dsi_display->panel,
				max_brightness_clone);
}

ssize_t mi_dsi_display_get_hw_vsync_info(void *display,
			char *buf, size_t size)
{
	struct dsi_display *dsi_display = (struct dsi_display *)display;

	if (!dsi_display) {
		DISP_ERROR("Invalid display ptr\n");
		return -EINVAL;
	}

	return mi_sde_encoder_calc_hw_vsync_info(dsi_display, buf, size);
}

static ssize_t compose_ddic_cell_id(char *outbuf, u32 outbuf_len,
		const char *inbuf, u32 inbuf_len)
{
	int i = 0;
	int idx = 0;
	ssize_t count =0;
	const char ddic_cell_id_dictionary[] =
	{
		'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B',
		'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N',
		'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
	};

	if (!outbuf || !inbuf) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	for (i = 0; i < inbuf_len; i++) {
		idx = inbuf[i];
		if (i == inbuf_len - 1)
			count += snprintf(outbuf + count, outbuf_len - count, "%c\n",
				ddic_cell_id_dictionary[idx]);
		else
			count += snprintf(outbuf + count, outbuf_len - count, "%c",
				ddic_cell_id_dictionary[idx]);
		DISP_DEBUG("cell_id[%d] = 0x%02X, ch%c\n", i, idx,
				ddic_cell_id_dictionary[idx]);
	}

	return count;
}

static ssize_t mi_dsi_display_read_ddic_cell_id(struct dsi_display *display,
		char *buf, size_t size)
{
	int rc = 0;
	u8 rdbuf[20] = {0};
	u32 rdlen = 0;
	u8 cmd[] = {0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xAC};
	ssize_t count = 0;

	if (!display || !display->panel) {
		DISP_ERROR("Invalid display/panel ptr\n");
		return -EINVAL;
	}

	if (mi_get_panel_id_by_dsi_panel(display->panel) == PANEL_ID_MAX) {
		rdlen = 13;
		rc = dsi_display_cmd_receive(display, cmd, sizeof(cmd), rdbuf, rdlen);
		if (rc == rdlen) {
			count = compose_ddic_cell_id(buf, size, rdbuf, rdlen);
			DISP_INFO("cell_id = %s\n", buf);
			return count;
		} else {
			DISP_ERROR("failed to read panel cell id, rc = %d\n", rc);
			return -EAGAIN;
		}
	} else {
		DISP_INFO("TODO\n");
		snprintf(buf, size, "%s\n", "Unsupported");
		return -EINVAL;
	}
}

ssize_t mi_dsi_display_read_cell_id(void *display,
			char *buf, size_t size)
{
	struct dsi_display *dsi_display = (struct dsi_display *)display;
	int ret = 0;

	if (!dsi_display) {
		DISP_ERROR("Invalid display ptr\n");
		return -EINVAL;
	}

	if (!strlen(cell_id_info_str)) {
		mi_dsi_acquire_wakelock(dsi_display->panel);
		ret = mi_dsi_display_read_ddic_cell_id(dsi_display, buf, size);
		mi_dsi_release_wakelock(dsi_display->panel);
	} else {
		ret = snprintf(buf, size, "%s\n", cell_id_info_str);
	}

	return ret;
}

int mi_dsi_display_esd_irq_ctrl(struct dsi_display *display,
			bool enable)
{
	int ret = 0;

	if (!display) {
		DISP_ERROR("Invalid display ptr\n");
		return -EINVAL;
	}

	mutex_lock(&display->display_lock);

	ret = mi_dsi_panel_esd_irq_ctrl(display->panel, enable);
	if (ret)
		DISP_ERROR("[%s] failed to set esd irq, rc=%d\n",
				display->name, ret);

	mutex_unlock(&display->display_lock);

	return ret;
}

void mi_dsi_display_wakeup_pending_doze_work(struct dsi_display *display)
{
	int disp_id = 0;
	struct disp_display *dd_ptr;
	struct disp_feature *df = mi_get_disp_feature();

	if (!display) {
		DISP_ERROR("Invalid display ptr\n");
		return;
	}

	disp_id = mi_get_disp_id(display->display_type);
	dd_ptr = &df->d_display[disp_id];
	DISP_DEBUG("%s pending_doze_cnt = %d\n",
			display->display_type, atomic_read(&dd_ptr->pending_doze_cnt));
	if (atomic_read(&dd_ptr->pending_doze_cnt)) {
		DISP_INFO("%s display wake up pending doze work, pending_doze_cnt = %d\n",
			display->display_type, atomic_read(&dd_ptr->pending_doze_cnt));
		wake_up_interruptible_all(&dd_ptr->pending_wq);
	}
}

ssize_t mi_dsi_display_parse_manufacturer_info(char *outbuf, u32 outbuf_len,
		const char *inbuf,u32 offset,u32 len){
	int i = 0;
	ssize_t count =0;
	if (!outbuf || !inbuf) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}
	memset(outbuf, 0, outbuf_len);
	for (i = offset; i < offset + len; i++) {
		count += snprintf(outbuf + count, outbuf_len - count, "%02X",inbuf[i]);
		DISP_DEBUG("inbuf[%d] = 0x%02X\n", i,
				inbuf[i]);
	}
	return count;
}
ssize_t mi_dsi_display_read_manufacturer_info(struct dsi_display *display,
		struct panel_manufaturer_info *info)
{
	int rc = 0;
	u8 rdbuf[64] = {0};
	u32 rdlen = 0;
	char *wp_buf;
	char *maxbrightness_buf;
	char *manufacturertime_buf;
	u32 info_buf_size;
	u32 manufacturer_info_addr;
	struct dsi_parser_utils *utils;
	u32 wp_offset;
	u32 wp_len;
	u32 maxbrightness_offset;
	u32 maxbrightness_len;
	u32 manufacturertime_offset;
	u32 manufacturertime_len;
	if (!display||!display->panel||!info) {
		DISP_ERROR("Invalid display/panel/info ptr\n");
		return -EINVAL;
	}
	utils = &display->panel->utils;
	rc = utils->read_u32(utils->data, "mi,panel-manufacturer-info-addr", &manufacturer_info_addr);
	if (rc) {
		wp_offset = -1;
		DISP_INFO("mi,panel-manufacturer-info-addr not specified\n");
		return -EAGAIN;
	} else {
		DISP_INFO("mi,panel-manufacturer-info-addr 0x%x\n",manufacturer_info_addr);
	}
	rc = utils->read_u32(utils->data, "mi,panel-wp-info-offset", &wp_offset);
	if (rc) {
		wp_offset = -1;
		DISP_INFO("mi,panel-wp-info-offset  not specified\n");
	} else {
		DISP_INFO("mi,panel-wp-info-offset %d\n",wp_offset);
	}
	rc = utils->read_u32(utils->data, "mi,panel-wp-info-len", &wp_len);
	if (rc) {
		wp_len = -1;
		DISP_INFO("mi,panel-wp-info-len  not specified\n");
	} else {
		DISP_INFO("mi,panel-wp-info-len %d\n",wp_len);
	}
	if(wp_offset != -1 && wp_len != -1){
		rdlen = (rdlen > wp_offset+wp_len) ? rdlen : wp_offset+wp_len;
	}
	rc = utils->read_u32(utils->data, "mi,panel-max-brightness-offset", &maxbrightness_offset);
	if (rc) {
		maxbrightness_offset = -1;
		DISP_INFO("mi,panel-max-brightness-offset  not specified\n");
	} else {
		DISP_INFO("mi,panel-max-brightness-offset %d\n",maxbrightness_offset);
	}
	rc = utils->read_u32(utils->data, "mi,panel-max-brightness-len", &maxbrightness_len);
	if (rc) {
		maxbrightness_len = -1;
		DISP_INFO("mi,panel-max-brightness-len  not specified\n");
	} else {
		DISP_INFO("mi,panel-max-brightness-len %d\n",maxbrightness_len);
	}
	if(maxbrightness_offset != -1 && maxbrightness_len != -1){
		rdlen = (rdlen > maxbrightness_offset+maxbrightness_len) ? rdlen : maxbrightness_offset+maxbrightness_len;
	}
	rc = utils->read_u32(utils->data, "mi,panel-manufacturer-time-offset", &manufacturertime_offset);
	if (rc) {
		manufacturertime_offset = -1;
		DISP_INFO("mi,panel-manufacturer-time-offset  not specified\n");
	} else {
		DISP_INFO("mi,panel-manufacturer-time-offset %d\n",manufacturertime_offset);
	}
	rc = utils->read_u32(utils->data, "mi,panel-manufacturer-time-len", &manufacturertime_len);
	if (rc) {
		manufacturertime_len = -1;
		DISP_INFO("mi,panel-manufacturer-time-len  not specified\n");
	} else {
		DISP_INFO("mi,panel-manufacturer-time-len %d\n",manufacturertime_len);
	}
	if(manufacturertime_offset != -1 && manufacturertime_len != -1){
		rdlen = (rdlen > manufacturertime_offset+manufacturertime_len) ? rdlen : manufacturertime_offset+manufacturertime_len;
	}
	if(rdlen == 0){
		DISP_INFO("no manufacruer info need read  \n");
		return -EAGAIN;
	}
	rc = mi_dsi_panel_read_manufacturer_info(display->panel,manufacturer_info_addr, rdbuf, rdlen);
	if (rc < 0) {
			DISP_ERROR("failed to read panel manufacturer info, rc = %d\n", rc);
			return -EAGAIN;
	} else {
		wp_buf = info->wp_info;
		maxbrightness_buf = info->maxbrightness;
		manufacturertime_buf = info->manufacturer_time;
		info_buf_size = 16;
		//get info from read buf
		if(wp_offset != -1 && wp_len != -1){
			info->wp_info_len = mi_dsi_display_parse_manufacturer_info(wp_buf, info_buf_size, rdbuf, wp_offset, wp_len);
		}else{
			info->wp_info_len=0;
		}
		if(maxbrightness_offset != -1&&maxbrightness_len != -1){
			info->max_brightness_len = mi_dsi_display_parse_manufacturer_info(maxbrightness_buf, info_buf_size,
					rdbuf, maxbrightness_offset, maxbrightness_len);
		}else{
			info->max_brightness_len = 0;
		}
		if(manufacturertime_offset != -1&&manufacturertime_len != -1){
			info->manufacturer_time_len = mi_dsi_display_parse_manufacturer_info(manufacturertime_buf, info_buf_size,
					rdbuf, manufacturertime_offset, manufacturertime_len);
		}else{
			info->manufacturer_time_len = 0;
		}
		return rc;
	}
}
ssize_t mi_dsi_display_manufacturer_info_init(void *display)
{
	struct panel_manufaturer_info *info;
	int rc = 0;
	struct dsi_display *dsi_display = (struct dsi_display *)display;
	if (!dsi_display) {
		DISP_ERROR("Invalid display ptr\n");
		return -EINVAL;
	}
	info = &g_panel_manufaturer_info[mi_get_disp_id(dsi_display->display_type)];
	if(!info->wp_info_len && !info->manufacturer_time_len && !info->manufacturer_time_len){
		rc = mi_dsi_display_read_manufacturer_info(dsi_display, info);
		if(rc < 0){
			DISP_ERROR("read_manufacturer_info error \n");
			return -EINVAL;
		}
	}
	return 0;
}
ssize_t mi_dsi_display_read_manufacturer_struct_by_globleparam(void *display,
			struct panel_manufaturer_info *manufaturer_info)
{
	struct dsi_display *dsi_display = (struct dsi_display *)display;
	struct panel_manufaturer_info * info;
	int rc = 0;
	if (!dsi_display&&!manufaturer_info) {
		DISP_ERROR("Invalid display/manufaturer_info ptr\n");
		return -EINVAL;
	}
	info = &g_panel_manufaturer_info[mi_get_disp_id(dsi_display->display_type)];
	if(!info->wp_info_len && !info->manufacturer_time_len && !info->manufacturer_time_len){
		DISP_ERROR("read_manufacturer_info error \n");
			return -EINVAL;
	}
	strcpy(manufaturer_info->wp_info,info->wp_info);
	strcpy(manufaturer_info->maxbrightness,info->maxbrightness);
	strcpy(manufaturer_info->manufacturer_time,info->manufacturer_time);
	manufaturer_info->wp_info_len = info->wp_info_len;
	manufaturer_info->max_brightness_len = info->max_brightness_len;
	manufaturer_info->manufacturer_time_len = info->manufacturer_time_len;
	return rc;
}
ssize_t mi_dsi_display_read_manufacturer_info_by_globleparam(void *display,
			char *buf,size_t size)
{
	struct dsi_display *dsi_display = (struct dsi_display *)display;
	struct panel_manufaturer_info * info;
	ssize_t count = 0;
	if (!dsi_display && !buf) {
		DISP_ERROR("Invalid display/buf ptr\n");
		return -EINVAL;
	}
	info = &g_panel_manufaturer_info[mi_get_disp_id(dsi_display->display_type)];
	memset(buf,0,size);
	count += snprintf(buf + count, size - count, "%036s: %s\n",
				"wp_info",info->wp_info);
	count += snprintf(buf + count, size - count, "%036s: %s\n",
				"max_brightness",info->maxbrightness);
	count += snprintf(buf + count, size - count, "%036s: %s\n",
				"manufacturer_time",info->manufacturer_time);
	return count;
}

int mi_dsi_display_cmd_read_locked(struct dsi_display *display,
			      struct dsi_cmd_desc cmd, u8 *rx_buf, u32 rx_len)
{
	int rc = 0;
	bool state = false;
	cmd.msg.flags |= MIPI_DSI_MSG_UNICAST_COMMAND;
	cmd.msg.rx_buf = rx_buf;
	cmd.msg.rx_len = rx_len;
	cmd.ctrl_flags = DSI_CTRL_CMD_READ;

	rc = dsi_display_ctrl_get_host_init_state(display, &state);

	if (!rc && !state) {
		DISP_ERROR("Command xfer attempted while device is in suspend state\n");
		rc = -EPERM;
		goto end;
	}
	if (rc || !state) {
		DISP_ERROR("[DSI] Invalid host state %d rc %d\n",
				state, rc);
		rc = -EPERM;
		goto end;
	}

	rc = dsi_display_cmd_rx(display, &cmd);
	if (rc <= 0)
		DISP_ERROR("[DSI] Display command receive failed, rc=%d\n", rc);
end:
	return rc;
}

int mi_dsi_display_cmd_read(struct dsi_display *display,
			      struct dsi_cmd_desc cmd, u8 *rx_buf, u32 rx_len)
{
	int rc = 0;

	if (!display) {
		DISP_ERROR("Invalid display ptr\n");
		return -EINVAL;
	}

	mutex_lock(&display->display_lock);
	rc = mi_dsi_display_cmd_read_locked(display, cmd, rx_buf, rx_len);
	mutex_unlock(&display->display_lock);
	return rc;
}

ssize_t mi_dsi_display_reset_qsync_on_cmd(struct dsi_display *display, u32 qsync_min_fps_index)
{
	int rc = 0;
	rc = mi_dsi_panel_update_mipi_qsync_on_cmd(display->panel, qsync_min_fps_index);
	return rc;
}

int mi_dsi_display_check_flatmode_status(void *display, bool *status)
{
	struct dsi_display *dsi_display = (struct dsi_display *)display;
	int rc = 0;

	if (!display || !status) {
		DISP_ERROR("Invalid display/status ptr\n");
		return -EINVAL;
	}

	*status = false;

	if (sde_kms_is_suspend_blocked(dsi_display->drm_dev)) {
		DISP_ERROR("sde_kms is suspended, skip to write dsi cmd\n");
		return -EBUSY;
	}

	mi_dsi_acquire_wakelock(dsi_display->panel);
	rc = mi_dsi_panel_flatmode_validate_status(dsi_display, status);
	mi_dsi_release_wakelock(dsi_display->panel);

	return rc;
}

bool mi_dsi_display_ramdump_support(void)
{
	/* when debug policy is 0x0 or 0x20, full dump not supported */
	if (strcmp(display_debug_policy, "0x0") != 0 && strcmp(display_debug_policy, "0x20") != 0)
		return true;
	return false;
}

static void mi_display_pm_suspend_delayed_work_handler(struct kthread_work *work)
{
	struct disp_delayed_work *delayed_work = container_of(work,
					struct disp_delayed_work, delayed_work.work);
	struct dsi_panel *dsi_panel = (struct dsi_panel *)(delayed_work->data);
	struct mi_dsi_panel_cfg *mi_cfg = &dsi_panel->mi_cfg;
	unsigned long mode_flags_backup = 0;
	int pmic_pwrkey_status = mi_cfg->pmic_pwrkey_status;
	int rc = 0;

	mi_dsi_acquire_wakelock(dsi_panel);

	dsi_panel_acquire_panel_lock(dsi_panel);
	if (dsi_panel->panel_initialized && pmic_pwrkey_status == PMIC_PWRKEY_BARK_TRIGGER) {
		mode_flags_backup = dsi_panel->mipi_device.mode_flags;
		dsi_panel->mipi_device.mode_flags |= MIPI_DSI_MODE_LPM;
		rc = mipi_dsi_dcs_set_display_off(&dsi_panel->mipi_device);
		dsi_panel->mipi_device.mode_flags = mode_flags_backup;
		if (rc < 0){
			DISP_ERROR("failed to send MIPI_DCS_SET_DISPLAY_OFF\n");
		} else {
			DISP_INFO("panel send MIPI_DCS_SET_DISPLAY_OFF\n");
		}
	} else {
		DISP_INFO("panel does not need to be off in advance\n");
	}
	dsi_panel_release_panel_lock(dsi_panel);

	mi_dsi_release_wakelock(dsi_panel);

	kfree(delayed_work);
}

int mi_display_pm_suspend_delayed_work(struct dsi_display *display)
{
	int disp_id = 0;
	struct disp_feature *df = mi_get_disp_feature();
	struct dsi_panel *panel = display->panel;
	struct disp_display *dd_ptr;
	struct disp_delayed_work *suspend_delayed_work;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	suspend_delayed_work = kzalloc(sizeof(*suspend_delayed_work), GFP_KERNEL);
	if (!suspend_delayed_work) {
		DISP_ERROR("failed to allocate delayed_work buffer\n");
		return -ENOMEM;
	}

	disp_id = mi_get_disp_id(panel->type);
	dd_ptr = &df->d_display[disp_id];

	kthread_init_delayed_work(&suspend_delayed_work->delayed_work,
			mi_display_pm_suspend_delayed_work_handler);
	suspend_delayed_work->dd_ptr = dd_ptr;
	suspend_delayed_work->wq = &dd_ptr->pending_wq;
	suspend_delayed_work->data = panel;
	return kthread_queue_delayed_work(dd_ptr->worker, &suspend_delayed_work->delayed_work,
				msecs_to_jiffies(DISPLAY_DELAY_SHUTDOWN_TIME_MS));
}

int mi_display_powerkey_callback(int status)
{
	struct dsi_display *dsi_display = mi_get_primary_dsi_display();
	struct dsi_panel *panel;
	struct mi_dsi_panel_cfg *mi_cfg;

	if (!dsi_display || !dsi_display->panel){
		DISP_ERROR("invalid dsi_display or dsi_panel ptr\n");
		return -EINVAL;
	}

	panel = dsi_display->panel;
	mi_cfg = &panel->mi_cfg;
	mi_cfg->pmic_pwrkey_status = status;

	if(status == PMIC_PWRKEY_BARK_TRIGGER){
		return mi_display_pm_suspend_delayed_work(dsi_display);
	}
	return 0;
}

module_param_string(oled_wp, oled_wp_info_str, MAX_CMDLINE_PARAM_LEN, 0600);
MODULE_PARM_DESC(oled_wp, "msm_drm.oled_wp=<wp info> while <wp info> is 'white point info' ");

module_param_string(sec_oled_wp, sec_oled_wp_info_str, MAX_CMDLINE_PARAM_LEN, 0600);
MODULE_PARM_DESC(sec_oled_wp, "msm_drm.sec_oled_wp=<wp info> while <wp info> is 'white point info' ");

module_param_string(cell_id, cell_id_info_str, MAX_CMDLINE_PARAM_LEN, 0600);
MODULE_PARM_DESC(cell_id, "msm_drm.cell_id=<cell id> while <cell id> is 'cell id info' ");

module_param_string(debugpolicy, display_debug_policy, MAX_DEBUG_POLICY_CMDLINE_LEN, 0600);
MODULE_PARM_DESC(debugpolicy, "msm_drm.debugpolicy=<debug policy> to indicate supporting ramdump or not ");

