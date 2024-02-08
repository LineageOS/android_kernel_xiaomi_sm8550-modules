/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2020 XiaoMi, Inc. All rights reserved.
 */

#define pr_fmt(fmt)	"mi-dsi-panel:[%s:%d] " fmt, __func__, __LINE__
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/rtc.h>
#include <linux/pm_wakeup.h>
#include <video/mipi_display.h>

#include "dsi_panel.h"
#include "dsi_display.h"
#include "dsi_ctrl_hw.h"
#include "dsi_parser.h"
#include "../../../../kernel/irq/internals.h"
#include "sde_connector.h"
#include "sde_trace.h"
#include "mi_dsi_panel.h"
#include "mi_disp_feature.h"
#include "mi_disp_print.h"
#include "mi_disp_parser.h"
#include "mi_dsi_display.h"
#include "mi_disp_lhbm.h"
#include "mi_panel_id.h"
#include "mi_disp_nvt_alpha_data.h"

#define to_dsi_display(x) container_of(x, struct dsi_display, host)

static int mi_dsi_update_lhbm_cmd_87reg(struct dsi_panel *panel,
			enum dsi_cmd_set_type type, int bl_lvl);
void mi_dsi_update_backlight_in_aod(struct dsi_panel *panel,
			bool restore_backlight);

static u64 g_panel_id[MI_DISP_MAX];

typedef int (*mi_display_pwrkey_callback)(int);
static void mi_display_pwrkey_callback_set(mi_display_pwrkey_callback callback) {}

static int mi_panel_id_init(struct dsi_panel *panel)
{
	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	g_panel_id[mi_get_disp_id(panel->type)] = panel->mi_cfg.mi_panel_id;

	return 0;
}

enum mi_project_panel_id mi_get_panel_id_by_dsi_panel(struct dsi_panel *panel)
{
	if (!panel) {
		DISP_ERROR("invalid params\n");
		return PANEL_ID_INVALID;
	}

	return mi_get_panel_id(panel->mi_cfg.mi_panel_id);
}

enum mi_project_panel_id mi_get_panel_id_by_disp_id(int disp_id)
{
	if (!is_support_disp_id(disp_id)) {
		DISP_ERROR("Unsupported display id\n");
		return PANEL_ID_INVALID;
	}

	return mi_get_panel_id(g_panel_id[disp_id]);
}

int mi_dsi_panel_init(struct dsi_panel *panel)
{
	struct mi_dsi_panel_cfg *mi_cfg = NULL;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mi_cfg = &panel->mi_cfg;

	mi_cfg->dsi_panel = panel;
	mutex_init(&mi_cfg->doze_lock);

	mi_cfg->disp_wakelock = wakeup_source_register(NULL, "disp_wakelock");
	if (!mi_cfg->disp_wakelock) {
		DISP_ERROR("doze_wakelock wake_source register failed");
		return -ENOMEM;
	}

	mi_dsi_panel_parse_config(panel);
	mi_panel_id_init(panel);
	atomic_set(&mi_cfg->real_brightness_clone, 0);
	if (mi_get_panel_id(panel->mi_cfg.mi_panel_id) == M1_PANEL_PA){
		mi_display_pwrkey_callback_set(mi_display_powerkey_callback);
	}
	return 0;
}

int mi_dsi_panel_deinit(struct dsi_panel *panel)
{
	struct mi_dsi_panel_cfg *mi_cfg = NULL;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mi_cfg = &panel->mi_cfg;

	if (mi_cfg->disp_wakelock) {
		wakeup_source_unregister(mi_cfg->disp_wakelock);
	}
	return 0;
}

int mi_dsi_acquire_wakelock(struct dsi_panel *panel)
{
	struct mi_dsi_panel_cfg *mi_cfg = NULL;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mi_cfg = &panel->mi_cfg;

	if (mi_cfg->disp_wakelock) {
		__pm_stay_awake(mi_cfg->disp_wakelock);
	}

	return 0;
}

int mi_dsi_release_wakelock(struct dsi_panel *panel)
{
	struct mi_dsi_panel_cfg *mi_cfg = NULL;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mi_cfg = &panel->mi_cfg;

	if (mi_cfg->disp_wakelock) {
		__pm_relax(mi_cfg->disp_wakelock);
	}

	return 0;

}

bool is_aod_and_panel_initialized(struct dsi_panel *panel)
{
	if ((panel->power_mode == SDE_MODE_DPMS_LP1 ||
		panel->power_mode == SDE_MODE_DPMS_LP2) &&
		dsi_panel_initialized(panel)){
		return true;
	} else {
		return false;
	}
}

int mi_dsi_panel_esd_irq_ctrl(struct dsi_panel *panel,
				bool enable)
{
	int ret  = 0;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);
	ret = mi_dsi_panel_esd_irq_ctrl_locked(panel, enable);
	mutex_unlock(&panel->panel_lock);

	return ret;
}

int mi_dsi_panel_esd_irq_ctrl_locked(struct dsi_panel *panel,
				bool enable)
{
	struct mi_dsi_panel_cfg *mi_cfg;
	struct irq_desc *desc;

	if (!panel || !panel->panel_initialized) {
		DISP_ERROR("Panel not ready!\n");
		return -EINVAL;
	}

	mi_cfg = &panel->mi_cfg;
	if (gpio_is_valid(mi_cfg->esd_err_irq_gpio)) {
		if (mi_cfg->esd_err_irq) {
			if (enable) {
				if (!mi_cfg->esd_err_enabled) {
					desc = irq_to_desc(mi_cfg->esd_err_irq);
					if (!irq_settings_is_level(desc))
						desc->istate &= ~IRQS_PENDING;
					enable_irq_wake(mi_cfg->esd_err_irq);
					enable_irq(mi_cfg->esd_err_irq);
					mi_cfg->esd_err_enabled = true;
					DISP_INFO("[%s] esd irq is enable\n", panel->type);
				}
			} else {
				if (mi_cfg->esd_err_enabled) {
					disable_irq_wake(mi_cfg->esd_err_irq);
					disable_irq_nosync(mi_cfg->esd_err_irq);
					mi_cfg->esd_err_enabled = false;
					DISP_INFO("[%s] esd irq is disable\n", panel->type);
				}
			}
		}
	} else {
		DISP_INFO("[%s] esd irq gpio invalid\n", panel->type);
	}

	return 0;
}

static void mi_disp_set_dimming_delayed_work_handler(struct kthread_work *work)
{
	struct disp_delayed_work *delayed_work = container_of(work,
					struct disp_delayed_work, delayed_work.work);
	struct dsi_panel *panel = (struct dsi_panel *)(delayed_work->data);
	struct disp_feature_ctl ctl;

	memset(&ctl, 0, sizeof(struct disp_feature_ctl));
	ctl.feature_id = DISP_FEATURE_DIMMING;
	ctl.feature_val = FEATURE_ON;

	DISP_INFO("[%s] panel set backlight dimming on\n", panel->type);
	mi_dsi_acquire_wakelock(panel);
	mi_dsi_panel_set_disp_param(panel, &ctl);
	mi_dsi_release_wakelock(panel);

	kfree(delayed_work);
}

int mi_dsi_panel_tigger_dimming_delayed_work(struct dsi_panel *panel)
{
	int disp_id = 0;
	struct disp_feature *df = mi_get_disp_feature();
	struct disp_display *dd_ptr;
	struct disp_delayed_work *delayed_work;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	delayed_work = kzalloc(sizeof(*delayed_work), GFP_KERNEL);
	if (!delayed_work) {
		DISP_ERROR("failed to allocate delayed_work buffer\n");
		return -ENOMEM;
	}

	disp_id = mi_get_disp_id(panel->type);
	dd_ptr = &df->d_display[disp_id];

	kthread_init_delayed_work(&delayed_work->delayed_work,
			mi_disp_set_dimming_delayed_work_handler);
	delayed_work->dd_ptr = dd_ptr;
	delayed_work->wq = &dd_ptr->pending_wq;
	delayed_work->data = panel;

	return kthread_queue_delayed_work(dd_ptr->worker, &delayed_work->delayed_work,
				msecs_to_jiffies(panel->mi_cfg.panel_on_dimming_delay));
}


static void mi_disp_timming_switch_delayed_work_handler(struct kthread_work *work)
{
	struct disp_delayed_work *delayed_work = container_of(work,
					struct disp_delayed_work, delayed_work.work);
	struct dsi_panel *dsi_panel = (struct dsi_panel *)(delayed_work->data);

	mi_dsi_acquire_wakelock(dsi_panel);

	dsi_panel_acquire_panel_lock(dsi_panel);
	if (dsi_panel->panel_initialized) {
		dsi_panel_tx_cmd_set(dsi_panel, DSI_CMD_SET_MI_FRAME_SWITCH_MODE_SEC);
		DISP_INFO("DSI_CMD_SET_MI_FRAME_SWITCH_MODE_SEC\n");
	} else {
		DISP_ERROR("Panel not initialized, don't send DSI_CMD_SET_MI_FRAME_SWITCH_MODE_SEC\n");
	}
	dsi_panel_release_panel_lock(dsi_panel);

	mi_dsi_release_wakelock(dsi_panel);

	kfree(delayed_work);
}

int mi_dsi_panel_tigger_timming_switch_delayed_work(struct dsi_panel *panel)
{
	int disp_id = 0;
	struct disp_feature *df = mi_get_disp_feature();
	struct disp_display *dd_ptr;
	struct disp_delayed_work *delayed_work;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	delayed_work = kzalloc(sizeof(*delayed_work), GFP_KERNEL);
	if (!delayed_work) {
		DISP_ERROR("failed to allocate delayed_work buffer\n");
		return -ENOMEM;
	}

	disp_id = mi_get_disp_id(panel->type);
	dd_ptr = &df->d_display[disp_id];

	kthread_init_delayed_work(&delayed_work->delayed_work,
			mi_disp_timming_switch_delayed_work_handler);
	delayed_work->dd_ptr = dd_ptr;
	delayed_work->wq = &dd_ptr->pending_wq;
	delayed_work->data = panel;
	return kthread_queue_delayed_work(dd_ptr->worker, &delayed_work->delayed_work,
				msecs_to_jiffies(20));
}

int mi_dsi_update_switch_cmd(struct dsi_panel *panel)
{
	struct dsi_cmd_update_info *info = NULL;
	struct dsi_display_mode *cur_mode = NULL;
	u8 gamma_cfg = 0;
	u8 lhbm_gamma_cfg = 0;
	u32 cmd_update_index = 0;

	if (!panel || !panel->cur_mode || !panel->cur_mode->priv_info) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	cur_mode = panel->cur_mode;

	cmd_update_index = DSI_CMD_SET_TIMING_SWITCH_UPDATA;
	info = cur_mode->priv_info->cmd_update[cmd_update_index];

	if (panel->mi_cfg.feature_val[DISP_FEATURE_FLAT_MODE] == FEATURE_ON) {
		gamma_cfg = cur_mode->priv_info->flat_on_gamma;
		lhbm_gamma_cfg = cur_mode->priv_info->flat_on_lhbm_gamma;
	} else {
		gamma_cfg = cur_mode->priv_info->flat_off_gamma;
		lhbm_gamma_cfg = cur_mode->priv_info->flat_off_lhbm_gamma;
	}

	if (gamma_cfg) {
		mi_dsi_panel_update_cmd_set(panel, cur_mode,
			DSI_CMD_SET_TIMING_SWITCH, info,
			&gamma_cfg, sizeof(gamma_cfg));
	} else {
		DISP_INFO("[%s] gamma cfg is 0, can not update switch cmd", panel->type);
	}

	if (lhbm_gamma_cfg) {
		info++;
		mi_dsi_panel_update_cmd_set(panel, cur_mode,
			DSI_CMD_SET_TIMING_SWITCH, info,
			&lhbm_gamma_cfg, sizeof(lhbm_gamma_cfg));
	} else {
		DISP_INFO("[%s] lhbm gamma cfg is 0, can not update switch cmd", panel->type);
	}

	return 0;
}

int mi_dsi_update_switch_cmd_N11(struct dsi_panel *panel)
{
	struct dsi_cmd_update_info *info = NULL;
	struct dsi_display_mode *cur_mode = NULL;
	int i = 0;
	u8 gamma_26_cfg = 0;
	u8 gamma_c0_cfg = 0;
	u8 gamma_df_cfg = 0;
	u32 cmd_update_index = 0;
	u8  cmd_update_count = 0;

	if (!panel || !panel->cur_mode || !panel->cur_mode->priv_info) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	cur_mode = panel->cur_mode;

	if (panel->mi_cfg.feature_val[DISP_FEATURE_FLAT_MODE] == FEATURE_ON) {
		gamma_26_cfg = cur_mode->priv_info->flat_on_26reg_gamma;
		gamma_c0_cfg = cur_mode->priv_info->flat_on_C0reg_gamma;
		gamma_df_cfg = cur_mode->priv_info->flat_on_DFreg_gamma;
	} else {
		gamma_26_cfg = cur_mode->priv_info->flat_off_26reg_gamma;
		gamma_c0_cfg = cur_mode->priv_info->flat_off_C0reg_gamma;
		gamma_df_cfg = cur_mode->priv_info->flat_off_DFreg_gamma;
	}

	cmd_update_index = DSI_CMD_SET_TIMING_SWITCH_UPDATA;
	info = cur_mode->priv_info->cmd_update[cmd_update_index];
	cmd_update_count = cur_mode->priv_info->cmd_update_count[cmd_update_index];

	for (i = 0; i < cmd_update_count; i++ ) {
		DISP_DEBUG("[%s] update [%s] mipi_address(0x%02X) index(%d) lenght(%d)\n",
			panel->type, cmd_set_prop_map[info->type],
			info->mipi_address, info->index, info->length);
		if (info && info->mipi_address == 0x26) {
			mi_dsi_panel_update_cmd_set(panel, cur_mode,
				DSI_CMD_SET_TIMING_SWITCH, info, &gamma_26_cfg, sizeof(gamma_26_cfg));
		} else if (info && info->mipi_address == 0xC0) {
			mi_dsi_panel_update_cmd_set(panel, cur_mode,
				DSI_CMD_SET_TIMING_SWITCH, info, &gamma_c0_cfg, sizeof(gamma_c0_cfg));
		} else if (info && info->mipi_address == 0xDF) {
			mi_dsi_panel_update_cmd_set(panel, cur_mode,
				DSI_CMD_SET_TIMING_SWITCH, info, &gamma_df_cfg, sizeof(gamma_df_cfg));
		}
		info++;
	}

	return 0;
}

static inline bool mi_dsi_update_BA_reg(struct dsi_panel *panel, u8 *val, u8 size)
{
	u32 last_framerate = 0;
	u32 cur_framerate = 0;
	u32 fps_mode = 0;
	struct mi_dsi_panel_cfg *mi_cfg  = NULL;
	bool ret = false;
	u8 *framerate_val_ptr = NULL;

	u8 switch_framerate_40HZ_cfg[2][9] = {
		/*120HZ switch to 40HZ*/
		{0x91,0x01,0x01,0x00,0x01,0x02,0x02,0x00,0x00},
		/*60/30/24/1HZ switch to 40HZ*/
		{0x91,0x02,0x02,0x00,0x01,0x02,0x02,0x00,0x00}
	};
	u8 switch_framerate_30HZ_cfg[2][9] = {
		/*120HZ switch to 30HZ*/
		{0x91,0x01,0x01,0x00,0x01,0x03,0x03,0x00,0x00},
		/*60/30/24/1HZ switch to 30HZ*/
		{0x91,0x03,0x03,0x00,0x01,0x03,0x03,0x00,0x00}
	};
	u8 switch_framerate_24HZ_cfg[3][9] = {
		/*120HZ switch to 24HZ*/
		{0x91,0x03,0x01,0x00,0x11,0x04,0x04,0x00,0x05},
		/*60HZ switch to 24HZ*/
		{0x91,0x03,0x03,0x00,0x11,0x04,0x04,0x00,0x05},
		/*40/30/1HZ switch to 24HZ*/
		{0x91,0x04,0x04,0x00,0x01,0x04,0x04,0x00,0x00}
	};

	if (!panel || !panel->cur_mode || !panel->cur_mode->priv_info) {
		DISP_ERROR("invalid params\n");
		ret = false;
		goto exit;
	}

	mi_cfg = &panel->mi_cfg;
	last_framerate = mi_cfg->last_refresh_rate;
	fps_mode = (panel->cur_mode->timing.h_skew >> FPS_MODE_OFFSET) & FPS_MODE_VALUE_MASK;

	if(panel->cur_mode->timing.h_skew == FPS_NORMAL)
		cur_framerate = panel->cur_mode->timing.refresh_rate;
	else if (fps_mode & FPS_MODE_IDLE)
		cur_framerate = panel->cur_mode->timing.h_skew & FPS_VALUE_MASK;
	else if ((fps_mode & FPS_MODE_AUTO) || (fps_mode & FPS_MODE_QSYNC))
		cur_framerate = (panel->cur_mode->timing.h_skew >> FPS_SF_FPS_OFFSET) & FPS_VALUE_MASK;

	DISP_DEBUG("fps mode:%d, last fps:%d, cur_fps:%d\n", fps_mode, last_framerate, cur_framerate);

	switch (cur_framerate) {
		case 120:
		case 90:
		case 60:
			framerate_val_ptr = NULL;
			ret = false;
			break;
		case 40:
			if(last_framerate == 120 || last_framerate == 90)
				framerate_val_ptr = switch_framerate_40HZ_cfg[0];
			else
				framerate_val_ptr = switch_framerate_40HZ_cfg[1];
			break;
		case 30:
			if(last_framerate == 120 || last_framerate == 90)
				framerate_val_ptr = switch_framerate_30HZ_cfg[0];
			else
				framerate_val_ptr = switch_framerate_30HZ_cfg[1];
			break;
		case 24:
			if(last_framerate == 120 || last_framerate == 90)
				framerate_val_ptr = switch_framerate_24HZ_cfg[0];
			else if (last_framerate == 60)
				framerate_val_ptr = switch_framerate_24HZ_cfg[1];
			else
				framerate_val_ptr = switch_framerate_24HZ_cfg[2];
			break;
		default:
			framerate_val_ptr = NULL;
			break;
	}

	if(framerate_val_ptr != NULL) {
		memcpy(val, framerate_val_ptr, size);
		ret = true;
	}

exit:
	return ret;
}

int mi_dsi_update_timing_switch_and_flat_mode_cmd(struct dsi_panel *panel,
		enum dsi_cmd_set_type type)
{
	struct dsi_cmd_update_info *info = NULL;
	struct dsi_display_mode *cur_mode = NULL;
	u8 gamma_2F_cfg = 0;
	u8 gamma_26_cfg = 0;
	u32 cmd_update_index = 0;
	u8  cmd_update_count = 0;
	int i = 0;
	bool is_update_BA_reg = false;
	u8 update_BA_reg_val[9] = {0};
	bool flat_mode_enable = false;
	bool peak_hdr_mode_enable = false;

	if (!panel || !panel->cur_mode || !panel->cur_mode->priv_info) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	cur_mode = panel->cur_mode;

	peak_hdr_mode_enable = panel->mi_cfg.feature_val[DISP_FEATURE_PEAK_HDR_MODE];

	switch (type) {
		case DSI_CMD_SET_TIMING_SWITCH:
			cmd_update_index = DSI_CMD_SET_TIMING_SWITCH_UPDATA;
			flat_mode_enable = panel->mi_cfg.feature_val[DISP_FEATURE_FLAT_MODE];
			is_update_BA_reg = mi_dsi_update_BA_reg(panel, update_BA_reg_val, sizeof(update_BA_reg_val));
			break;
		case DSI_CMD_SET_POST_TIMING_SWITCH:
			cmd_update_index = DSI_CMD_SET_POST_TIMING_SWITCH_UPDATE;
			flat_mode_enable = panel->mi_cfg.feature_val[DISP_FEATURE_FLAT_MODE];
			break;
		case DSI_CMD_SET_MI_FLAT_MODE_ON:
			cmd_update_index = DSI_CMD_SET_MI_FLAT_MODE_ON_UPDATE;
			flat_mode_enable = true;
			break;
		case DSI_CMD_SET_MI_FLAT_MODE_OFF:
			cmd_update_index = DSI_CMD_SET_MI_FLAT_MODE_OFF_UPDATE;
			flat_mode_enable = false;
			break;
		case DSI_CMD_SET_MI_DOZE_HBM:
			cmd_update_index = DSI_CMD_SET_MI_DOZE_HBM_UPDATE;
			flat_mode_enable = false;
			break;
		case DSI_CMD_SET_MI_DOZE_LBM:
			cmd_update_index = DSI_CMD_SET_MI_DOZE_LBM_UPDATE;
			flat_mode_enable = false;
			break;
		default :
			DISP_ERROR("[%s] unsupport cmd %s\n",
					panel->type, cmd_set_prop_map[type]);
			return -EINVAL;
	}

	/*Before P2. Invert gir on and gir off*/
	if ((panel->id_config.build_id == M1_PANEL_PA_P11) || (panel->id_config.build_id == M1_PANEL_PA_P10)) {
		if (flat_mode_enable) {
			gamma_2F_cfg = cur_mode->priv_info->flat_off_2Freg_gamma;
			gamma_26_cfg = cur_mode->priv_info->flat_off_26reg_gamma;
		} else {
			gamma_2F_cfg = cur_mode->priv_info->flat_on_2Freg_gamma;
			gamma_26_cfg = cur_mode->priv_info->flat_on_26reg_gamma;
		}
		/* Because AOD from 120HZ switch to 30HZ, gir off state
		 * Default command code is for P2,
		 * P1.1 need update to zero
		*/
		if (type == DSI_CMD_SET_MI_DOZE_HBM || type == DSI_CMD_SET_MI_DOZE_LBM) {
			gamma_2F_cfg = 0x00;
			gamma_26_cfg = 0x00;
		}
	} else {/*Since P2*/
		if (flat_mode_enable) {
			gamma_2F_cfg = cur_mode->priv_info->flat_on_2Freg_gamma;
			gamma_26_cfg = cur_mode->priv_info->flat_on_26reg_gamma;
		} else {
			gamma_2F_cfg = cur_mode->priv_info->flat_off_2Freg_gamma;
			gamma_26_cfg = cur_mode->priv_info->flat_off_26reg_gamma;
        	}
	}

	if (peak_hdr_mode_enable && (cur_mode->timing.refresh_rate != 90)) {
		gamma_2F_cfg = 0x00;
		gamma_26_cfg = 0x07;
	}

	DISP_DEBUG("panel id is 0x%x, gamma_2Freg_cfg is 0x%x, gamma_26reg_cfg is 0x%x",
			panel->id_config.build_id, gamma_2F_cfg, gamma_26_cfg);

	info = cur_mode->priv_info->cmd_update[cmd_update_index];
	cmd_update_count = cur_mode->priv_info->cmd_update_count[cmd_update_index];
	for (i = 0; i < cmd_update_count; i++ ) {
		if (info && info->mipi_address == 0x2F) {
			mi_dsi_panel_update_cmd_set(panel, cur_mode,
				type, info,
				&gamma_2F_cfg, sizeof(gamma_2F_cfg));
		}

		if (info && info->mipi_address == 0x26) {
			mi_dsi_panel_update_cmd_set(panel, cur_mode,
				type, info,
				&gamma_26_cfg, sizeof(gamma_26_cfg));
		}

		if (info && info->mipi_address == 0xBA && is_update_BA_reg) {
			mi_dsi_panel_update_cmd_set(panel, cur_mode,
				type, info,
				update_BA_reg_val, sizeof(update_BA_reg_val));
		}

		info++;
	}

	return 0;
}

int mi_dsi_panel_set_gamma_update_reg(struct dsi_panel *panel, u32 bl_lvl)
{
	int rc = 0;
	struct mi_dsi_panel_cfg *mi_cfg  = NULL;
	u32 last_refresh_rate;
	u32 vsync_period_us;
	ktime_t cur_time;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mi_cfg = &panel->mi_cfg;

	if (mi_cfg->panel_state != PANEL_STATE_ON) {
		DISP_INFO("%s exit when panel state(%d)\n", __func__, mi_cfg->panel_state);
		return 0;
	}

	last_refresh_rate = mi_cfg->last_refresh_rate;
	vsync_period_us = 1000000 / last_refresh_rate;

	if (mi_cfg->nedd_auto_update_gamma) {
		cur_time = ktime_get();
		if (last_refresh_rate && (ktime_to_us(cur_time - mi_cfg->last_mode_switch_time)
				<= vsync_period_us)) {
			DISP_INFO("sleep before update gamma, vsync_period = %d, diff_time = %llu",
					vsync_period_us,
					ktime_to_us(cur_time - mi_cfg->last_mode_switch_time));
			usleep_range(vsync_period_us, vsync_period_us + 10);
		}
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_AUTO_UPDATE_GAMMA);
		DISP_INFO("send auto update gamma cmd\n");
		if (rc)
			DISP_ERROR("failed to set DSI_CMD_SET_MI_AUTO_UPDATE_GAMMA: %d\n");
		else
			mi_cfg->nedd_auto_update_gamma = false;
	}

	return rc;
}

int mi_dsi_panel_set_gamma_update_state(struct dsi_panel *panel)
{
	int rc = 0;
	struct mi_dsi_panel_cfg *mi_cfg  = NULL;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mi_cfg = &panel->mi_cfg;

	if (mi_cfg->panel_state != PANEL_STATE_ON) {
		DISP_INFO("%s exit when panel state(%d)\n", __func__, mi_cfg->panel_state);
		return 0;
	}

	mutex_lock(&panel->panel_lock);

	mi_cfg->nedd_auto_update_gamma = true;
	DISP_DEBUG("set auto update gamma state to true\n");

	mutex_unlock(&panel->panel_lock);

	return rc;
}

int mi_dsi_first_timing_switch(struct dsi_panel *panel)
{
	int rc = 0;
	struct mi_dsi_panel_cfg *mi_cfg  = NULL;
	u32 last_refresh_rate;
	u32 vsync_period_us;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mi_cfg = &panel->mi_cfg;

	if (mi_cfg->panel_state != PANEL_STATE_ON) {
		DISP_INFO("%s exit when panel state(%d)\n", __func__, mi_cfg->panel_state);
		return 0;
	}

	last_refresh_rate = mi_cfg->last_refresh_rate;
	vsync_period_us = 1000000 / last_refresh_rate;

	if (mi_cfg->first_timing_switch) {
		DISP_INFO("sleep after timing switch, vsync_period = %d", vsync_period_us);
		usleep_range(vsync_period_us, vsync_period_us + 10);

		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_AUTO_UPDATE_GAMMA);
		DISP_INFO("send auto update gamma cmd\n");
		if (rc)
			DISP_ERROR("failed to set DSI_CMD_SET_MI_AUTO_UPDATE_GAMMA: %d\n");
		else
			mi_cfg->first_timing_switch = false;
	}

	return rc;
}


bool is_hbm_fod_on(struct dsi_panel *panel)
{
	struct mi_dsi_panel_cfg *mi_cfg = &panel->mi_cfg;
	int feature_val;
	bool is_fod_on = false;

	feature_val = mi_cfg->feature_val[DISP_FEATURE_LOCAL_HBM];
	switch (feature_val) {
	case LOCAL_HBM_NORMAL_WHITE_1000NIT:
	case LOCAL_HBM_NORMAL_WHITE_750NIT:
	case LOCAL_HBM_NORMAL_WHITE_500NIT:
	case LOCAL_HBM_NORMAL_WHITE_110NIT:
	case LOCAL_HBM_NORMAL_GREEN_500NIT:
	case LOCAL_HBM_HLPM_WHITE_1000NIT:
	case LOCAL_HBM_HLPM_WHITE_110NIT:
		is_fod_on = true;
		break;
	default:
		break;
	}

	if (mi_cfg->feature_val[DISP_FEATURE_HBM_FOD] == FEATURE_ON) {
		is_fod_on = true;
	}

	return is_fod_on;
}

bool is_dc_on_skip_backlight(struct dsi_panel *panel, u32 bl_lvl)
{
	struct mi_dsi_panel_cfg *mi_cfg = &panel->mi_cfg;

	if (!mi_cfg->dc_feature_enable)
		return false;
	if (mi_cfg->feature_val[DISP_FEATURE_DC] == FEATURE_OFF)
		return false;
	if (mi_cfg->dc_type == TYPE_CRC_SKIP_BL && bl_lvl < mi_cfg->dc_threshold)
		return true;
	else
		return false;
}

void mi_dsi_panel_sync_lhbm_alpha(struct dsi_panel *panel, u32 bl_lvl) {
	struct mi_dsi_panel_cfg *mi_cfg = &panel->mi_cfg;
	struct disp_feature_ctl ctl;

	if (!is_hbm_fod_on(panel))
		return;

	memset(&ctl, 0, sizeof(struct disp_feature_ctl));
	ctl.feature_id = DISP_FEATURE_LOCAL_HBM;
	ctl.feature_val = mi_cfg->feature_val[DISP_FEATURE_LOCAL_HBM];

	mi_dsi_panel_update_last_bl_level(panel, bl_lvl);
	mi_dsi_panel_set_lhbm_fod_locked(panel, &ctl);
}

bool is_backlight_set_skip(struct dsi_panel *panel, u32 bl_lvl)
{
	struct mi_dsi_panel_cfg *mi_cfg = &panel->mi_cfg;

	if (mi_cfg->in_fod_calibration || is_hbm_fod_on(panel)) {
		if (bl_lvl != 0) {
			if ((mi_get_panel_id(panel->mi_cfg.mi_panel_id) == M11_PANEL_PA) ||
				(mi_get_panel_id(panel->mi_cfg.mi_panel_id) == N11_PANEL_PA)) {
				DISP_INFO("[%s] skip set backlight %d due to LHBM is on,"
					"but update alpha\n", panel->type, bl_lvl);

				mi_dsi_panel_sync_lhbm_alpha(panel, bl_lvl);
				return true;
			} else if (mi_get_panel_id(panel->mi_cfg.mi_panel_id) == M2_PANEL_PA && is_hbm_fod_on(panel)) {
				if ( bl_lvl > mi_cfg->panel_1920pwm_dbv_threshold)
					mi_cfg->is_em_cycle_16_pulse = false;
				else
					mi_cfg->is_em_cycle_16_pulse = true;
				DISP_INFO("[%s] skip set backlight %d due to LHBM is on", panel->type, bl_lvl);
				return true;
			}
			DISP_INFO("[%s] update 51 reg to %d even LHBM is on,",
				panel->type, bl_lvl);
			return false;
		} else {
			DSI_INFO("[%s] skip set backlight %d due to fod hbm "
					"or fod calibration\n", panel->type, bl_lvl);
		}
		return true;
	} else if (bl_lvl != 0 && is_dc_on_skip_backlight(panel, bl_lvl)) {
		DISP_DEBUG("[%s] skip set backlight %d due to DC on\n", panel->type, bl_lvl);
		return true;
	} else if (panel->power_mode == SDE_MODE_DPMS_LP1) {
		if (bl_lvl == 0) {
			DSI_INFO("[%s] skip set backlight 0 due to LP1 on\n", panel->type);
			return true;
		}
		if (mi_get_panel_id(panel->mi_cfg.mi_panel_id) == M18_PANEL_PA ||
			mi_get_panel_id(panel->mi_cfg.mi_panel_id) == M18_PANEL_SA) {
			DSI_INFO("[%s] skip set backlight due to LP1 on\n", panel->type);
			return true;
		}
		return false;
	} else if (mi_get_panel_id(panel->mi_cfg.mi_panel_id) == M1_PANEL_PA &&
		mi_cfg->feature_val[DISP_FEATURE_PEAK_HDR_MODE]) {
		if (bl_lvl == 0){
			mi_cfg->feature_val[DISP_FEATURE_PEAK_HDR_MODE] = false;
			mi_dsi_update_timing_switch_and_flat_mode_cmd(panel, DSI_CMD_SET_MI_FLAT_MODE_OFF);
			dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_FLAT_MODE_OFF);
			return false;
		}
		else{
			DSI_INFO("[%s] skip set backlight due to Peak Hdr Mode on\n", panel->type);
			return true;
		}
	} else {
		return false;
	}
}

void mi_dsi_panel_update_last_bl_level(struct dsi_panel *panel, int brightness)
{
	struct mi_dsi_panel_cfg *mi_cfg;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return;
	}

	mi_cfg = &panel->mi_cfg;

	if ((mi_cfg->last_bl_level == 0 || mi_cfg->dimming_state == STATE_DIM_RESTORE) &&
		brightness > 0 && !is_hbm_fod_on(panel) && !mi_cfg->in_fod_calibration) {
		if (!panel->mi_cfg.dimming_need_update_speed)
			mi_dsi_panel_tigger_dimming_delayed_work(panel);
		else {
			mi_cfg->feature_val[DISP_FEATURE_DIMMING] = FEATURE_ON;
			DISP_INFO("[%s] enable dimming setting.\n", panel->type);
		}

		if (mi_cfg->dimming_state == STATE_DIM_RESTORE)
			mi_cfg->dimming_state = STATE_NONE;
	}

	mi_cfg->last_bl_level = brightness;
	if (brightness != 0)
		mi_cfg->last_no_zero_bl_level = brightness;

	return;
}

void mi_dsi_panel_update_dimming_frame(struct dsi_panel *panel, u8 dimming_switch, u8 frame)
{
	static u8 frame_state = DIMMING_SPEED_0FRAME;
	static u8 dimming_switch_state = DIMMING_SPEED_SWITCH_DEFAULT;

	if (frame_state == frame && dimming_switch_state == dimming_switch) {
		DISP_ERROR("don't need to update dimming state\n");
		return;
	}

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return;
	}

	if (dimming_switch_state != dimming_switch) {
		if (dimming_switch == DIMMING_SPEED_SWITCH_ON) {
			dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DIMMINGON);
			DISP_INFO("[%s] panel set dimming on.\n", panel->type);
		} else if (dimming_switch == DIMMING_SPEED_SWITCH_OFF) {
			dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DIMMINGOFF);
			DISP_INFO("[%s] panel set dimming off.\n", panel->type);
		}
		dimming_switch_state = dimming_switch;
	}

	if (dimming_switch_state == DIMMING_SPEED_SWITCH_ON) {
		if (frame == DIMMING_SPEED_8FRAME && frame_state != frame) {
			dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DIMMING_8FRAME);
			DISP_INFO("[%s] panel set dimming frame %d.\n", panel->type, frame);
		} else if (frame == DIMMING_SPEED_4FRAME && frame_state != frame) {
			dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DIMMING_4FRAME);
			DISP_INFO("[%s] panel set dimming frame %d.\n", panel->type, frame);
		}
	}
	frame_state = frame;

	return;
}

int mi_dsi_print_51_backlight_log(struct dsi_panel *panel,
		struct dsi_cmd_desc *cmd)
{
	u8 *buf = NULL;
	u32 bl_lvl = 0;
	int i = 0;
	struct mi_dsi_panel_cfg *mi_cfg;
	static int use_count = 20;

	if (!panel || !cmd) {
		DISP_ERROR("Invalid params\n");
		return -EINVAL;
	}

	mi_cfg = &panel->mi_cfg;
	buf = (u8 *)cmd->msg.tx_buf;
	if (buf && buf[0] == MIPI_DCS_SET_DISPLAY_BRIGHTNESS) {
		if (cmd->msg.tx_len >= 3) {
			if (panel->bl_config.bl_inverted_dbv)
				bl_lvl = (buf[1] << 8) | buf[2];
			else
				bl_lvl = buf[1] | (buf[2] << 8);

			if (use_count-- > 0)
				DISP_TIME_INFO("[%s] set 51 backlight %d\n", panel->type, bl_lvl);

			if (!mi_cfg->last_bl_level || !bl_lvl) {
					use_count = 20;
			}
		}

		if (mi_get_backlight_log_mask() & BACKLIGHT_LOG_ENABLE) {
			DISP_INFO("[%s] [0x51 backlight debug] tx_len = %d\n",
					panel->type, cmd->msg.tx_len);
			for (i = 0; i < cmd->msg.tx_len; i++) {
				DISP_INFO("[%s] [0x51 backlight debug] tx_buf[%d] = 0x%02X\n",
					panel->type, i, buf[i]);
			}

			if (mi_get_backlight_log_mask() & BACKLIGHT_LOG_DUMP_STACK)
				dump_stack();
		}
	}

	return 0;
}

int mi_dsi_panel_parse_cmd_sets_update(struct dsi_panel *panel,
		struct dsi_display_mode *mode)
{
	int rc = 0;
	int i = 0, j = 0, k = 0;
	u32 length = 0;
	u32 count = 0;
	u32 size = 0;
	u32 *arr_32 = NULL;
	const u32 *arr;
	struct dsi_parser_utils *utils = &panel->utils;
	struct dsi_cmd_update_info *info;
	int type = -1;

	if (!mode || !mode->priv_info) {
		DISP_ERROR("invalid arguments\n");
		return -EINVAL;
	}

	DISP_DEBUG("WxH: %dx%d, FPS: %d\n", mode->timing.h_active,
			mode->timing.v_active, mode->timing.refresh_rate);

	for (i = 0; i < DSI_CMD_UPDATE_MAX; i++) {
		arr = utils->get_property(utils->data, cmd_set_update_map[i],
			&length);

		if (!arr) {
			DISP_DEBUG("[%s] commands not defined\n", cmd_set_update_map[i]);
			continue;
		}

		if (length & 0x1) {
			DISP_ERROR("[%s] syntax error\n", cmd_set_update_map[i]);
			rc = -EINVAL;
			goto error;
		}

		DISP_DEBUG("%s length = %d\n", cmd_set_update_map[i], length);

		for (j = DSI_CMD_SET_PRE_ON; j < DSI_CMD_SET_MAX; j++) {
			if (strstr(cmd_set_update_map[i], cmd_set_prop_map[j])) {
				type = j;
				DISP_DEBUG("find type(%d) is [%s] commands\n", type,
					cmd_set_prop_map[type]);
				break;
			}
		}
		if (type < 0 || j == DSI_CMD_SET_MAX) {
			rc = -EINVAL;
			goto error;
		}

		length = length / sizeof(u32);
		size = length * sizeof(u32);

		arr_32 = kzalloc(size, GFP_KERNEL);
		if (!arr_32) {
			rc = -ENOMEM;
			goto error;
		}

		rc = utils->read_u32_array(utils->data, cmd_set_update_map[i],
						arr_32, length);
		if (rc) {
			DISP_ERROR("[%s] read failed\n", cmd_set_update_map[i]);
			goto error_free_arr_32;
		}

		count = length / 3;
		size = count * sizeof(*info);
		info = kzalloc(size, GFP_KERNEL);
		if (!info) {
			rc = -ENOMEM;
			goto error_free_arr_32;
		}

		mode->priv_info->cmd_update[i] = info;
		mode->priv_info->cmd_update_count[i] = count;

		for (k = 0; k < length; k += 3) {
			info->type = type;
			info->mipi_address= arr_32[k];
			info->index= arr_32[k + 1];
			info->length= arr_32[k + 2];
			DISP_DEBUG("update [%s] mipi_address(0x%02X) index(%d) lenght(%d)\n",
				cmd_set_update_map[i], info->mipi_address,
				info->index, info->length);
			info++;
		}
	}

error_free_arr_32:
	kfree(arr_32);
error:
	return rc;
}

int mi_dsi_panel_update_cmd_set(struct dsi_panel *panel,
			struct dsi_display_mode *mode, enum dsi_cmd_set_type type,
			struct dsi_cmd_update_info *info, u8 *payload, u32 size)
{
	int rc = 0;
	int i = 0;
	struct dsi_cmd_desc *cmds = NULL;
	u32 count;
	u8 *tx_buf = NULL;
	size_t tx_len;

	if (!panel || !mode || !mode->priv_info)
	{
		DISP_ERROR("invalid panel or cur_mode params\n");
		return -EINVAL;
	}

	if (!info || !payload) {
		DISP_ERROR("invalid info or payload params\n");
		return -EINVAL;
	}

	if (type != info->type || size != info->length) {
		DISP_ERROR("please check type(%d, %d) or update size(%d, %d)\n",
			type, info->type, info->length, size);
		return -EINVAL;
	}

	cmds = mode->priv_info->cmd_sets[type].cmds;
	count = mode->priv_info->cmd_sets[type].count;
	if (count == 0) {
		DISP_ERROR("[%s] No commands to be sent\n", cmd_set_prop_map[type]);
		return -EINVAL;
	}

	DISP_DEBUG("WxH: %dx%d, FPS: %d\n", mode->timing.h_active,
			mode->timing.v_active, mode->timing.refresh_rate);

	DISP_DEBUG("[%s] update [%s] mipi_address(0x%02X) index(%d) lenght(%d)\n",
			panel->type, cmd_set_prop_map[info->type],
			info->mipi_address, info->index, info->length);
	for (i = 0; i < size; i++) {
		DISP_DEBUG("[%s] payload[%d] = 0x%02X\n", panel->type, i, payload[i]);
	}

	if (cmds && count > info->index) {
		tx_buf = (u8 *)cmds[info->index].msg.tx_buf;
		tx_len = cmds[info->index].msg.tx_len;
		if (tx_buf && tx_buf[0] == info->mipi_address && tx_len >= info->length) {
			memcpy(&tx_buf[1], payload, info->length);
			for (i = 0; i < tx_len; i++) {
				DISP_DEBUG("[%s] tx_buf[%d] = 0x%02X\n",
					panel->type, i, tx_buf[i]);
			}
		} else {
			if (tx_buf) {
				DISP_ERROR("[%s] %s mipi address(0x%02X != 0x%02X)\n",
					panel->type, cmd_set_prop_map[type],
					tx_buf[0], info->mipi_address);
			} else {
				DISP_ERROR("[%s] panel tx_buf is NULL pointer\n",
					panel->type);
			}
			rc = -EINVAL;
		}
	} else {
		DISP_ERROR("[%s] panel cmd[%s] index error\n",
			panel->type, cmd_set_prop_map[type]);
		rc = -EINVAL;
	}

	return rc;
}

int mi_dsi_panel_parse_gamma_config(struct dsi_panel *panel,
		struct dsi_display_mode *mode)
{
	int rc;
	struct dsi_display_mode_priv_info *priv_info;
	struct dsi_parser_utils *utils = &panel->utils;
	u32 gamma_cfg[2] = {0, 0};
	u32 gamma_c0_cfg[2] = {0, 0};
	u32 gamma_df_cfg[2] = {0, 0};
	u32 lhbm_gamma_cfg[2] = {0, 0};

	priv_info = mode->priv_info;

	rc = utils->read_u32_array(utils->data, "mi,mdss-flat-status-control-gamma-cfg",
			gamma_cfg, 2);
	if (rc) {
		DISP_DEBUG("mi,mdss-flat-status-control-gamma-cfg not defined rc=%d\n", rc);
	} else {
		DISP_INFO("FPS: %d, gamma cfg: 0x%02X, 0x%02X\n",
			mode->timing.refresh_rate, gamma_cfg[0], gamma_cfg[1]);
		priv_info->flat_on_gamma = gamma_cfg[0];
		priv_info->flat_off_gamma = gamma_cfg[1];
	}
	rc = utils->read_u32_array(utils->data, "mi,mdss-flat-status-control-gamma-C0-cfg",
			gamma_c0_cfg, 2);
	if (rc) {
		DISP_DEBUG("mi,mdss-flat-status-control-gamma-C0-cfg not defined rc=%d\n", rc);
	} else {
		DISP_INFO("FPS: %d, gamma c0 cfg: 0x%02X, 0x%02X\n",
			mode->timing.refresh_rate, gamma_c0_cfg[0], gamma_c0_cfg[1]);
		priv_info->flat_on_C0reg_gamma = gamma_c0_cfg[0];
		priv_info->flat_off_C0reg_gamma = gamma_c0_cfg[1];
	}

	rc = utils->read_u32_array(utils->data, "mi,mdss-flat-status-control-gamma-DF-cfg",
			gamma_df_cfg, 2);
	if (rc) {
		DISP_DEBUG("mi,mdss-flat-status-control-gamma-DF-cfg not defined rc=%d\n", rc);
	} else {
		DISP_INFO("FPS: %d, gamma df cfg: 0x%02X, 0x%02X\n",
			mode->timing.refresh_rate, gamma_df_cfg[0], gamma_df_cfg[1]);
		priv_info->flat_on_DFreg_gamma = gamma_df_cfg[0];
		priv_info->flat_off_DFreg_gamma = gamma_df_cfg[1];
	}

	rc = utils->read_u32_array(utils->data, "mi,mdss-flat-status-control-lhbm-gamma-cfg",
			lhbm_gamma_cfg, 2);
	if (rc) {
		DISP_DEBUG("mi,mdss-flat-status-control-lhbm-gamma-cfg not defined rc=%d\n", rc);
	} else {
		DISP_INFO("FPS: %d, lhbm gamma cfg: 0x%02X, 0x%02X\n",
			mode->timing.refresh_rate, lhbm_gamma_cfg[0], lhbm_gamma_cfg[1]);
		priv_info->flat_on_lhbm_gamma = lhbm_gamma_cfg[0];
		priv_info->flat_off_lhbm_gamma = lhbm_gamma_cfg[1];
	}

	return 0;
}

int mi_dsi_panel_parse_2F26reg_gamma_config(struct dsi_panel *panel,
		struct dsi_display_mode *mode)
{
	int rc;
	struct dsi_display_mode_priv_info *priv_info;
	struct dsi_parser_utils *utils = &panel->utils;
	u32 gamma_2f_cfg[2] = {0, 0};
	u32 gamma_26_cfg[2] = {0, 0};

	priv_info = mode->priv_info;

	rc = utils->read_u32_array(utils->data, "mi,mdss-flat-status-control-gamma-2F-cfg",
			gamma_2f_cfg, 2);
	if (rc) {
		DISP_DEBUG("mi,mdss-flat-status-control-gamma-2F-cfg not defined rc=%d\n", rc);
	} else {
		DISP_INFO("FPS: %d, gamma 2F cfg: 0x%02X, 0x%02X\n",
			mode->timing.refresh_rate, gamma_2f_cfg[0], gamma_2f_cfg[1]);
		priv_info->flat_on_2Freg_gamma = gamma_2f_cfg[0];
		priv_info->flat_off_2Freg_gamma = gamma_2f_cfg[1];
	}

	rc = utils->read_u32_array(utils->data, "mi,mdss-flat-status-control-gamma-26-cfg",
			gamma_26_cfg, 2);
	if (rc) {
		DISP_DEBUG("mi,mdss-flat-status-control-gamma-26-cfg not defined rc=%d\n", rc);
	} else {
		DISP_INFO("FPS: %d, gamma 26 cfg: 0x%02X, 0x%02X\n",
			mode->timing.refresh_rate, gamma_26_cfg[0], gamma_26_cfg[1]);
		priv_info->flat_on_26reg_gamma = gamma_26_cfg[0];
		priv_info->flat_off_26reg_gamma = gamma_26_cfg[1];
	}

	priv_info->is_update_aod_gir = false;

	return 0;
}

int mi_dsi_panel_write_cmd_set(struct dsi_panel *panel,
				struct dsi_panel_cmd_set *cmd_sets)
{
	int rc = 0, i = 0;
	ssize_t len;
	struct dsi_cmd_desc *cmds;
	u32 count;
	enum dsi_cmd_set_state state;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	cmds = cmd_sets->cmds;
	count = cmd_sets->count;
	state = cmd_sets->state;

	if (count == 0) {
		DISP_DEBUG("[%s] No commands to be sent for state\n", panel->type);
		goto error;
	}

	for (i = 0; i < count; i++) {
		cmds->ctrl_flags = 0;

		if (state == DSI_CMD_SET_STATE_LP)
			cmds->msg.flags |= MIPI_DSI_MSG_USE_LPM;

		len = dsi_host_transfer_sub(panel->host, cmds);
		if (len < 0) {
			rc = len;
			DISP_ERROR("failed to set cmds, rc=%d\n", rc);
			goto error;
		}
		if (cmds->post_wait_ms)
			usleep_range(cmds->post_wait_ms * 1000,
					((cmds->post_wait_ms * 1000) + 10));
		cmds++;
	}
error:
	return rc;
}

int mi_dsi_panel_read_batch_number(struct dsi_panel *panel)
{
	int rc = 0;
	unsigned long mode_flags_backup = 0;
	u8 rdbuf[8];
	ssize_t read_len = 0;
	u8 read_batch_number = 0;

	int i = 0;
	struct panel_batch_info info[] = {
		{0x00, "P0.0"},
		{0x01, "P0.1"},
		{0x10, "P1.0"},
		{0x11, "P1.1"},
		{0x12, "P1.2"},
		{0x13, "P1.2"},
		{0x20, "P2.0"},
		{0x21, "P2.1"},
		{0x30, "MP"},
	};

	if (!panel || !panel->host) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}
	mutex_lock(&panel->panel_lock);

	mode_flags_backup = panel->mipi_device.mode_flags;
	panel->mipi_device.mode_flags |= MIPI_DSI_MODE_LPM;
	read_len = mipi_dsi_dcs_read(&panel->mipi_device, 0xDA, rdbuf, 1);
	panel->mipi_device.mode_flags = mode_flags_backup;
	if (read_len > 0) {
		read_batch_number = rdbuf[0];
		panel->mi_cfg.panel_batch_number = read_batch_number;
		for (i = 0; i < ARRAY_SIZE(info); i++) {
			if (read_batch_number == info[i].batch_number) {
				DISP_INFO("panel batch is %s\n", info[i].batch_name);
				break;
			}
		}
		rc = 0;
 		panel->mi_cfg.panel_batch_number_read_done = true;
	} else {
		DISP_ERROR("failed to read panel batch number\n");
		panel->mi_cfg.panel_batch_number = 0;
		rc = -EAGAIN;
		panel->mi_cfg.panel_batch_number_read_done = false;
	}

	mutex_unlock(&panel->panel_lock);
	return rc;
}

int mi_dsi_panel_write_dsi_cmd_set(struct dsi_panel *panel,
			int type)
{
	int rc = 0;
	int i = 0, j = 0;
	u8 *tx_buf = NULL;
	u8 *buffer = NULL;
	int buf_size = 1024;
	u32 cmd_count = 0;
	int buf_count = 1024;
	struct dsi_cmd_desc *cmds;
	enum dsi_cmd_set_state state;
	struct dsi_display_mode *mode;

	if (!panel || !panel->cur_mode || type < 0 || type >= DSI_CMD_SET_MAX) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	buffer = kzalloc(buf_size, GFP_KERNEL);
	if (!buffer) {
		return -ENOMEM;
	}

	mutex_lock(&panel->panel_lock);

	mode = panel->cur_mode;
	cmds = mode->priv_info->cmd_sets[type].cmds;
	cmd_count = mode->priv_info->cmd_sets[type].count;
	state = mode->priv_info->cmd_sets[type].state;

	if (cmd_count == 0) {
		DISP_ERROR("[%s] No commands to be sent\n", cmd_set_prop_map[type]);
		rc = -EAGAIN;
		goto error;
	}

	DISP_INFO("set cmds [%s], count (%d), state(%s)\n",
		cmd_set_prop_map[type], cmd_count,
		(state == DSI_CMD_SET_STATE_LP) ? "dsi_lp_mode" : "dsi_hs_mode");

	for (i = 0; i < cmd_count; i++) {
		memset(buffer, 0, buf_size);
		buf_count = snprintf(buffer, buf_size, "%02X", cmds->msg.tx_len);
		tx_buf = (u8 *)cmds->msg.tx_buf;
		for (j = 0; j < cmds->msg.tx_len ; j++) {
			buf_count += snprintf(buffer + buf_count,
					buf_size - buf_count, " %02X", tx_buf[j]);
		}
		DISP_DEBUG("[%d] %s\n", i, buffer);
		cmds++;
	}

	rc = dsi_panel_tx_cmd_set(panel, type);

error:
	mutex_unlock(&panel->panel_lock);
	kfree(buffer);
	return rc;
}

ssize_t mi_dsi_panel_show_dsi_cmd_set_type(struct dsi_panel *panel,
			char *buf, size_t size)
{
	ssize_t count = 0;
	int type = 0;

	if (!panel || !buf) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	count = snprintf(buf, size, "%s: dsi cmd_set name\n", "id");

	for (type = DSI_CMD_SET_PRE_ON; type < DSI_CMD_SET_MAX; type++) {
		count += snprintf(buf + count, size - count, "%02d: %s\n",
				     type, cmd_set_prop_map[type]);
	}

	return count;
}

int mi_dsi_panel_update_doze_cmd_locked(struct dsi_panel *panel, u8 value)
{
	int rc = 0;
	struct dsi_cmd_update_info *info = NULL;
	struct dsi_display_mode *cur_mode = NULL;
	u32 cmd_update_index = 0;
	u32 cmd_update_count = 0;
	int i = 0;

	if (!panel || !panel->cur_mode || !panel->cur_mode->priv_info) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	cur_mode = panel->cur_mode;

	cmd_update_index = DSI_CMD_SET_MI_DOZE_LBM_UPDATE;
	info = cur_mode->priv_info->cmd_update[cmd_update_index];
	cmd_update_count = cur_mode->priv_info->cmd_update_count[cmd_update_index];
	for (i = 0; i < cmd_update_count; i++){
		DISP_INFO("[%s] update [%s] mipi_address(0x%02X) index(%d) lenght(%d)\n",
			panel->type, cmd_set_prop_map[info->type],
			info->mipi_address, info->index, info->length);
		mi_dsi_panel_update_cmd_set(panel, cur_mode,
			DSI_CMD_SET_MI_DOZE_LBM, info,
			&value, sizeof(value));
		info++;
	}

	return rc;
}

int mi_dsi_panel_set_doze_brightness(struct dsi_panel *panel,
			u32 doze_brightness)
{
	int rc = 0;
	struct mi_dsi_panel_cfg *mi_cfg;

	mutex_lock(&panel->panel_lock);

	mi_cfg = &panel->mi_cfg;

	if (is_hbm_fod_on(panel)) {
		mi_cfg->last_doze_brightness = mi_cfg->doze_brightness;
		mi_cfg->doze_brightness = doze_brightness;
		DISP_INFO("Skip! [%s] set doze brightness %d due to FOD_HBM_ON\n",
			panel->type, doze_brightness);
	} else if (panel->mi_cfg.aod_to_normal_statue == true) {
		mi_cfg->last_doze_brightness = mi_cfg->doze_brightness;
		mi_cfg->doze_brightness = doze_brightness;
		mi_dsi_update_backlight_in_aod(panel, false);
	} else if (panel->mi_cfg.panel_state == PANEL_STATE_ON
		|| mi_cfg->doze_brightness != doze_brightness) {
		if (doze_brightness == DOZE_BRIGHTNESS_HBM) {
			panel->mi_cfg.panel_state = PANEL_STATE_DOZE_HIGH;
			if (mi_get_panel_id_by_dsi_panel(panel) == M2_PANEL_PA ||
				mi_get_panel_id_by_dsi_panel(panel) == M3_PANEL_PA) {
				mi_dsi_update_51_mipi_cmd(panel, DSI_CMD_SET_MI_DOZE_HBM, 0);
			}
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DOZE_HBM);
			if (rc) {
				DISP_ERROR("[%s] failed to send DOZE_HBM cmd, rc=%d\n",
					panel->type, rc);
			}
		} else if (doze_brightness == DOZE_BRIGHTNESS_LBM) {
			panel->mi_cfg.panel_state = PANEL_STATE_DOZE_LOW;
			if (mi_get_panel_id_by_dsi_panel(panel) == M2_PANEL_PA ||
				mi_get_panel_id_by_dsi_panel(panel) == M3_PANEL_PA) {
				mi_dsi_update_51_mipi_cmd(panel, DSI_CMD_SET_MI_DOZE_LBM, 0);
			}
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DOZE_LBM);
			if (rc) {
				DISP_ERROR("[%s] failed to send DOZE_LBM cmd, rc=%d\n",
					panel->type, rc);
			}
		}
		mi_cfg->last_doze_brightness = mi_cfg->doze_brightness;
		mi_cfg->doze_brightness = doze_brightness;
		DISP_TIME_INFO("[%s] set doze brightness to %s\n",
			panel->type, get_doze_brightness_name(doze_brightness));
	} else {
		DISP_INFO("[%s] %s has been set, skip\n", panel->type,
			get_doze_brightness_name(doze_brightness));
	}

	mutex_unlock(&panel->panel_lock);

	return rc;
}

int mi_dsi_panel_get_doze_brightness(struct dsi_panel *panel,
			u32 *doze_brightness)
{
	struct mi_dsi_panel_cfg *mi_cfg;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);

	mi_cfg = &panel->mi_cfg;
	*doze_brightness =  mi_cfg->doze_brightness;

	mutex_unlock(&panel->panel_lock);

	return 0;
}

int mi_dsi_panel_get_brightness(struct dsi_panel *panel,
			u32 *brightness)
{
	struct mi_dsi_panel_cfg *mi_cfg;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);

	mi_cfg = &panel->mi_cfg;
	*brightness =  mi_cfg->last_bl_level;

	mutex_unlock(&panel->panel_lock);

	return 0;
}

int mi_dsi_panel_write_dsi_cmd(struct dsi_panel *panel,
			struct dsi_cmd_rw_ctl *ctl)
{
	struct dsi_panel_cmd_set cmd_sets = {0};
	u32 packet_count = 0;
	u32 dlen = 0;
	int rc = 0;

	mutex_lock(&panel->panel_lock);

	if (!panel || !panel->panel_initialized) {
		DISP_ERROR("Panel not initialized!\n");
		rc = -EAGAIN;
		goto exit_unlock;
	}

	if (!ctl->tx_len || !ctl->tx_ptr) {
		DISP_ERROR("[%s] invalid params\n", panel->type);
		rc = -EINVAL;
		goto exit_unlock;
	}

	rc = dsi_panel_get_cmd_pkt_count(ctl->tx_ptr, ctl->tx_len, &packet_count);
	if (rc) {
		DISP_ERROR("[%s] write dsi commands failed, rc=%d\n",
			panel->type, rc);
		goto exit_unlock;
	}

	DISP_DEBUG("[%s] packet-count=%d\n", panel->type, packet_count);

	rc = dsi_panel_alloc_cmd_packets(&cmd_sets, packet_count);
	if (rc) {
		DISP_ERROR("[%s] failed to allocate cmd packets, rc=%d\n",
			panel->type, rc);
		goto exit_unlock;
	}

	rc = dsi_panel_create_cmd_packets(ctl->tx_ptr, dlen, packet_count,
				cmd_sets.cmds);
	if (rc) {
		DISP_ERROR("[%s] failed to create cmd packets, rc=%d\n",
			panel->type, rc);
		goto exit_free1;
	}

	if (ctl->tx_state == MI_DSI_CMD_LP_STATE) {
		cmd_sets.state = DSI_CMD_SET_STATE_LP;
	} else if (ctl->tx_state == MI_DSI_CMD_HS_STATE) {
		cmd_sets.state = DSI_CMD_SET_STATE_HS;
	} else {
		DISP_ERROR("[%s] command state unrecognized-%s\n",
			panel->type, cmd_sets.state);
		goto exit_free1;
	}

	rc = mi_dsi_panel_write_cmd_set(panel, &cmd_sets);
	if (rc) {
		DISP_ERROR("[%s] failed to send cmds, rc=%d\n", panel->type, rc);
		goto exit_free2;
	}

exit_free2:
	if (ctl->tx_len && ctl->tx_ptr)
		dsi_panel_destroy_cmd_packets(&cmd_sets);
exit_free1:
	if (ctl->tx_len && ctl->tx_ptr)
		dsi_panel_dealloc_cmd_packets(&cmd_sets);
exit_unlock:
	mutex_unlock(&panel->panel_lock);
	return rc;
}

int mi_dsi_panel_set_brightness_clone(struct dsi_panel *panel,
			u32 brightness_clone)
{
	int rc = 0;
	struct mi_dsi_panel_cfg *mi_cfg;
	int disp_id = MI_DISP_PRIMARY;
	static int log_count = 20;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);

	mi_cfg = &panel->mi_cfg;

	mi_cfg->user_brightness_clone = brightness_clone;

	if (!mi_cfg->thermal_dimming_enabled) {
		if (brightness_clone > mi_cfg->thermal_max_brightness_clone)
			brightness_clone = mi_cfg->thermal_max_brightness_clone;
	}

	if (brightness_clone > mi_cfg->max_brightness_clone)
		brightness_clone = mi_cfg->max_brightness_clone;

	if (!brightness_clone)
		log_count = 20;

	if (log_count-- > 0)
		DISP_TIME_INFO("[%s] set brightness clone to %d\n",
				panel->type, brightness_clone);
	atomic_set(&mi_cfg->real_brightness_clone, brightness_clone);

	disp_id = mi_get_disp_id(panel->type);
	mi_disp_feature_event_notify_by_type(disp_id,
			MI_DISP_EVENT_BRIGHTNESS_CLONE,
			sizeof(mi_cfg->real_brightness_clone),
			atomic_read(&mi_cfg->real_brightness_clone));

	mutex_unlock(&panel->panel_lock);

	return rc;
}

int mi_dsi_panel_get_brightness_clone(struct dsi_panel *panel,
			u32 *brightness_clone)
{
	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	*brightness_clone = atomic_read(&panel->mi_cfg.real_brightness_clone);

	return 0;
}

int mi_dsi_panel_get_max_brightness_clone(struct dsi_panel *panel,
			u32 *max_brightness_clone)
{
	struct mi_dsi_panel_cfg *mi_cfg;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mi_cfg = &panel->mi_cfg;
	*max_brightness_clone =  mi_cfg->max_brightness_clone;

	return 0;
}

int mi_dsi_panel_switch_page_locked(struct dsi_panel *panel, u8 page_index)
{
	int rc = 0;
	u8 tx_buf[5] = {0x55, 0xAA, 0x52, 0x08, page_index};
	struct dsi_cmd_update_info *info = NULL;
	struct dsi_display_mode *cur_mode = NULL;
	u32 cmd_update_index = 0;
	u32 cmd_update_count = 0;
	int i = 0;

	if (!panel || !panel->cur_mode || !panel->cur_mode->priv_info) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	if (!panel->panel_initialized) {
		DISP_ERROR("[%s] panel not initialized\n", panel->type);
		return -EINVAL;
	}

	cur_mode = panel->cur_mode;

	cmd_update_index = DSI_CMD_SET_MI_SWITCH_PAGE_UPDATE;
	info = cur_mode->priv_info->cmd_update[cmd_update_index];
	cmd_update_count = cur_mode->priv_info->cmd_update_count[cmd_update_index];
	for (i = 0; i < cmd_update_count; i++){
		DISP_DEBUG("[%s] update [%s] mipi_address(0x%02X) index(%d) lenght(%d)\n",
			panel->type, cmd_set_prop_map[info->type],
			info->mipi_address, info->index, info->length);
		mi_dsi_panel_update_cmd_set(panel, cur_mode,
			DSI_CMD_SET_MI_SWITCH_PAGE, info,
			tx_buf, sizeof(tx_buf));
		info++;
	}

	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_SWITCH_PAGE);
	if (rc) {
		DISP_ERROR("[%s] failed to send switch page(%d) cmd\n",
			panel->type, page_index);
	}

	return rc;
}

int mi_dsi_panel_switch_page(struct dsi_panel *panel, u8 page_index)
{
	int rc = 0;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);

	rc = mi_dsi_panel_switch_page_locked(panel, page_index);

	mutex_unlock(&panel->panel_lock);

	return rc;
}

int mi_dsi_panel_read_and_update_flat_param(struct dsi_panel *panel)
{
	int ret = 0;
	mutex_lock(&panel->panel_lock);
	ret = mi_dsi_panel_read_and_update_flat_param_locked(panel);
	mutex_unlock(&panel->panel_lock);
	return ret;
}


int mi_dsi_panel_read_and_update_flat_param_locked(struct dsi_panel *panel)
{

	int rc = 0;
	struct flat_mode_cfg *flat_cfg = NULL;
	struct dsi_display *display;
	u32 num_display_modes;
	struct dsi_display_mode *cur_mode = NULL;
	struct dsi_display_mode_priv_info *priv_info;
	struct dsi_cmd_update_info *info = NULL;
	u32 cmd_update_index = 0;
	u32 cmd_update_count = 0;
	u8 rdbuf[10];
	uint request_rx_len = 0;
	unsigned long mode_flags_backup = 0;
	ssize_t read_len = 0;
	int i = 0, j = 0;

	if (!panel || !panel->host) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	if (!panel->mi_cfg.flat_update_flag) {
		DISP_DEBUG("[%s] flat_update_flag is not configed\n", panel->type);
		return 0;
	}

	flat_cfg = &panel->mi_cfg.flat_cfg;
	if (flat_cfg->update_done) {
		DISP_DEBUG("flat param already updated\n");
		rc = 0;
		goto exit;
	}

	DISP_INFO("[%s] flat param update start\n", panel->type);

	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_PREPARE_READ_FLAT);
	if (rc) {
		DISP_ERROR("[%s] failed to send PREPARE_READ_FLAT cmd\n",
			panel->type);
		goto exit;
	}

	if (sizeof(rdbuf) >= sizeof(flat_cfg->flat_on_data) ) {
		request_rx_len = sizeof(flat_cfg->flat_on_data);
	} else {
		DISP_ERROR("please check 0xB8 read_buf size, must > or = %d\n",
			sizeof(flat_cfg->flat_on_data));
		rc = -EAGAIN;
		goto exit;
	}

	mode_flags_backup = panel->mipi_device.mode_flags;
	panel->mipi_device.mode_flags |= MIPI_DSI_MODE_LPM;
	read_len = mipi_dsi_dcs_read(&panel->mipi_device, 0xB8,
		rdbuf, request_rx_len);
	panel->mipi_device.mode_flags = mode_flags_backup;

	if (read_len == request_rx_len) {
		memcpy(flat_cfg->flat_on_data, rdbuf, read_len);
		for (i = 0; i < sizeof(flat_cfg->flat_on_data); i++)
			DISP_INFO("flat read 0xB8, flat_on_data[%d] = 0x%02X\n",
				i, flat_cfg->flat_on_data[i]);
	} else {
		DISP_INFO("read flat 0xB8 failed (%d)\n", read_len);
		rc = -EAGAIN;
		goto exit;
	}

	num_display_modes = panel->num_display_modes;
	display = to_dsi_display(panel->host);
	if (!display || !display->modes) {
		DISP_ERROR("invalid display or display->modes ptr\n");
		rc = -EINVAL;
		goto exit;
	}

	for (i = 0; i < num_display_modes; i++) {
		cur_mode = &display->modes[i];
		DISP_INFO("[%s] update %d fps flat cmd\n", panel->type,
				cur_mode->timing.refresh_rate);

		priv_info = cur_mode->priv_info;
		cmd_update_index = DSI_CMD_SET_MI_FLAT_MODE_ON_UPDATE;
		info = priv_info->cmd_update[cmd_update_index];
		cmd_update_count = priv_info->cmd_update_count[cmd_update_index];
		for (j = 0; j < cmd_update_count; j++){
			DISP_DEBUG("[%s] update [%s] mipi_address(0x%02X) index(%d) lenght(%d)\n",
				panel->type, cmd_set_prop_map[info->type],
				info->mipi_address, info->index, info->length);
			mi_dsi_panel_update_cmd_set(panel, cur_mode,
				DSI_CMD_SET_MI_FLAT_MODE_ON, info,
				flat_cfg->flat_on_data, sizeof(flat_cfg->flat_on_data));
			info++;
		}
	}

	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_PREPARE_READ_FLAT_OFF);
	if (rc) {
		DISP_ERROR("[%s] failed to send PREPARE_READ_FLAT_OFF cmd\n",
			panel->type);
		goto exit;
	}

	if (sizeof(rdbuf) >= sizeof(flat_cfg->flat_off_data) ) {
		request_rx_len = sizeof(flat_cfg->flat_off_data);
	} else {
		DISP_ERROR("please check 0xB8 read_buf size, must > or = %d\n",
			sizeof(flat_cfg->flat_off_data));
		rc = -EAGAIN;
		goto exit;
	}

	mode_flags_backup = panel->mipi_device.mode_flags;
	panel->mipi_device.mode_flags |= MIPI_DSI_MODE_LPM;
	read_len = mipi_dsi_dcs_read(&panel->mipi_device, 0xB8,
		rdbuf, request_rx_len);
	panel->mipi_device.mode_flags = mode_flags_backup;

	if (read_len == request_rx_len) {
		memcpy(flat_cfg->flat_off_data, rdbuf, read_len);
		for (i = 0; i < sizeof(flat_cfg->flat_off_data); i++)
			DISP_INFO("flat off read 0xB8, flat_off_data[%d] = 0x%02X\n",
				i, flat_cfg->flat_off_data[i]);
	} else {
		DISP_INFO("read flat off 0xB8 failed (%d)\n", read_len);
		rc = -EAGAIN;
		goto exit;
	}

	for (i = 0; i < num_display_modes; i++) {
		cur_mode = &display->modes[i];
		DISP_INFO("[%s] update %d fps flat off cmd\n", panel->type,
				cur_mode->timing.refresh_rate);

		priv_info = cur_mode->priv_info;
		cmd_update_index = DSI_CMD_SET_MI_FLAT_MODE_OFF_UPDATE;
		info = priv_info->cmd_update[cmd_update_index];
		cmd_update_count = priv_info->cmd_update_count[cmd_update_index];
		for (j = 0; j < cmd_update_count; j++){
			DISP_DEBUG("[%s] update [%s] mipi_address(0x%02X) index(%d) lenght(%d)\n",
				panel->type, cmd_set_prop_map[info->type],
				info->mipi_address, info->index, info->length);
			mi_dsi_panel_update_cmd_set(panel, cur_mode,
				DSI_CMD_SET_MI_FLAT_MODE_OFF, info,
				flat_cfg->flat_off_data, sizeof(flat_cfg->flat_off_data));
			info++;
		}
	}

exit:

	return rc;
}

int mi_dsi_panel_read_and_update_dc_param(struct dsi_panel *panel)
{
	int ret = 0;
	mutex_lock(&panel->panel_lock);
	ret = mi_dsi_panel_read_and_update_dc_param_locked(panel);
	mutex_unlock(&panel->panel_lock);
	return ret;
}


int mi_dsi_panel_read_and_update_dc_param_locked(struct dsi_panel *panel)
{
	int rc = 0;
	struct dc_lut_cfg *dc_cfg = NULL;
	struct dsi_display *display;
	struct dsi_display_mode *cur_mode = NULL;
	unsigned long mode_flags_backup = 0;
	u32 num_display_modes;
	struct dsi_cmd_update_info *info = NULL;
	u8 read_dc_reg = 0;
	u8 read_count = 0;
	ssize_t read_len = 0;
	uint request_rx_len = 0;
	u8 read_buf[80] = {0};
	int i = 0, j = 0;
	u32 cmd_update_index = 0;
	u32 cmd_update_count = 0;

	if (!panel || !panel->host) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	if (!panel->mi_cfg.dc_update_flag) {
		DISP_DEBUG("[%s] dc_update_flag is not configed\n", panel->type);
		return 0;
	}

	dc_cfg = &panel->mi_cfg.dc_cfg;
	if (dc_cfg->update_done) {
		DISP_DEBUG("DC param already updated\n");
		rc = 0;
		goto exit;
	}

	if (!panel->panel_initialized) {
		DISP_ERROR("[%s] Panel not initialized\n", panel->type);
		rc = -EINVAL;
		goto exit;
	}

	DISP_INFO("[%s] DC param update start\n", panel->type);

	/* switch page 4 */
	rc = mi_dsi_panel_switch_page_locked(panel, 4);
	if (rc) {
		DISP_ERROR("[%s] failed to send SWITCH_PAGE cmd\n",
			panel->type);
		goto exit;
	}

	for (read_count = DC_LUT_60HZ; read_count < DC_LUT_MAX; read_count++) {
		if (read_count == DC_LUT_60HZ) {
			read_dc_reg = 0xD2;
		} else {
			read_dc_reg = 0xD4;
		}

		if (sizeof(read_buf) >= sizeof(dc_cfg->exit_dc_lut[read_count])) {
			request_rx_len = sizeof(dc_cfg->exit_dc_lut[read_count]);
		} else {
			DISP_ERROR("please check 0x%02X read_buf size, must > or = %d\n",
				read_dc_reg, sizeof(dc_cfg->exit_dc_lut[read_count]));
			rc = -EAGAIN;
			goto exit;
		}
		DISP_INFO("[%s]read DC 0x%02X paramter length is %d\n",
			panel->type, read_dc_reg, request_rx_len);

		mode_flags_backup = panel->mipi_device.mode_flags;
		panel->mipi_device.mode_flags |= MIPI_DSI_MODE_LPM;
		read_len = mipi_dsi_dcs_read(&panel->mipi_device, read_dc_reg,
			read_buf, request_rx_len);
		panel->mipi_device.mode_flags = mode_flags_backup;
		if (read_len == request_rx_len) {
			memcpy(dc_cfg->exit_dc_lut[read_count], read_buf, read_len);
			for (i = 0; i < sizeof(dc_cfg->exit_dc_lut[read_count]); i++) {
				DISP_DEBUG("DC 0x%02X exit_dc_lut[%d] = 0x%02X\n",
					read_dc_reg, i, dc_cfg->exit_dc_lut[read_count][i]);
			}
		} else {
			DISP_INFO("read DC 0x%02X failed (%d)\n", read_dc_reg, read_len);
			rc = -EAGAIN;
			goto exit;
		}

		memcpy(dc_cfg->enter_dc_lut[read_count], dc_cfg->exit_dc_lut[read_count],
				sizeof(dc_cfg->exit_dc_lut[read_count]));
		for (i = 0; i < sizeof(dc_cfg->enter_dc_lut[read_count])/5; i++) {
			for (j = i * 5; j < i  * 5 + 3 ; j++) {
				dc_cfg->enter_dc_lut[read_count][j] =
					dc_cfg->exit_dc_lut[read_count][i * 5 + 2];
			}
		}
		for (i = 0; i < sizeof(dc_cfg->enter_dc_lut[read_count]); i++) {
			DISP_DEBUG("DC 0x%02X enter_dc_lut[%d] = 0x%02X\n",
				read_dc_reg, i, dc_cfg->enter_dc_lut[read_count][i]);
		}
		DISP_INFO("[%s] DC 0x%02X parameter read done\n",
			panel->type, read_dc_reg);

	}

	num_display_modes = panel->num_display_modes;
	display = to_dsi_display(panel->host);
	if (!display || !display->modes) {
		DISP_ERROR("invalid display or display->modes ptr\n");
		rc = -EINVAL;
		goto exit;
	}

	for (i = 0; i < num_display_modes; i++) {
		cur_mode = &display->modes[i];

		DISP_INFO("[%s] update %d fps DC cmd\n", panel->type,
			cur_mode->timing.refresh_rate);

		cmd_update_index = DSI_CMD_SET_MI_DC_ON_UPDATE;
		info = cur_mode->priv_info->cmd_update[cmd_update_index];
		cmd_update_count = cur_mode->priv_info->cmd_update_count[cmd_update_index];
		for (j = 0; j < cmd_update_count; j++){
			DISP_INFO("[%s] update [%s] mipi_address(0x%02X) index(%d) lenght(%d)\n",
				panel->type, cmd_set_prop_map[info->type],
				info->mipi_address, info->index, info->length);
			mi_dsi_panel_update_cmd_set(panel, cur_mode, DSI_CMD_SET_MI_DC_ON, info,
				dc_cfg->enter_dc_lut[j], sizeof(dc_cfg->enter_dc_lut[j]));
			info++;
		}

		cmd_update_index = DSI_CMD_SET_MI_DC_OFF_UPDATE;
		info = cur_mode->priv_info->cmd_update[cmd_update_index];
		cmd_update_count = cur_mode->priv_info->cmd_update_count[cmd_update_index];
		for (j = 0; j < cmd_update_count; j++){
			DISP_INFO("[%s] update [%s] mipi_address(0x%02X) index(%d) lenght(%d)\n",
				panel->type, cmd_set_prop_map[info->type],
				info->mipi_address, info->index, info->length);
			mi_dsi_panel_update_cmd_set(panel, cur_mode, DSI_CMD_SET_MI_DC_OFF,
				info, dc_cfg->exit_dc_lut[j], sizeof(dc_cfg->exit_dc_lut[j]));
			info++;
		}
	}
	dc_cfg->update_done = true;
	DISP_INFO("[%s] DC param update end\n", panel->type);

exit:
	return rc;
}

void mi_dsi_update_backlight_in_aod(struct dsi_panel *panel, bool restore_backlight)
{
	int bl_lvl = 0;
	struct mi_dsi_panel_cfg *mi_cfg = &panel->mi_cfg;
	struct mipi_dsi_device *dsi = &panel->mipi_device;

	if (restore_backlight) {
		bl_lvl = mi_cfg->last_bl_level;
	} else {
		switch (mi_cfg->doze_brightness) {
			case DOZE_BRIGHTNESS_HBM:
				bl_lvl = mi_cfg->doze_hbm_dbv_level;
				break;
			case DOZE_BRIGHTNESS_LBM:
				bl_lvl = mi_cfg->doze_lbm_dbv_level;
				break;
			default:
				return;
		}
	}
	DISP_INFO("[%s] mi_dsi_update_backlight_in_aod bl_lvl=%d\n",
			panel->type, bl_lvl);
	if (panel->bl_config.bl_inverted_dbv)
		bl_lvl = (((bl_lvl & 0xff) << 8) | (bl_lvl >> 8));
	mipi_dsi_dcs_set_display_brightness(dsi, bl_lvl);

	return;
}

int mi_dsi_update_51_mipi_cmd(struct dsi_panel *panel,
			enum dsi_cmd_set_type type, int bl_lvl)
{
	struct dsi_cmd_update_info *info = NULL;
	struct dsi_display_mode *cur_mode = NULL;
	u32 cmd_update_index = 0;
	u32 cmd_update_count = 0;
	u8 bl_buf[6] = {0};
	int j = 0;
	int size = 2;

	if (!panel || !panel->cur_mode || !panel->cur_mode->priv_info) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	cur_mode = panel->cur_mode;

	DISP_INFO("[%s] bl_lvl = %d\n", panel->type, bl_lvl);

	bl_buf[0] = (bl_lvl >> 8) & 0xff;
	bl_buf[1] = bl_lvl & 0xff;
	size = 2;

	switch (type) {
	case DSI_CMD_SET_NOLP:
		cmd_update_index = DSI_CMD_SET_NOLP_UPDATE;
		break;
	case DSI_CMD_SET_MI_HBM_ON:
		cmd_update_index = DSI_CMD_SET_MI_HBM_ON_UPDATA;
		break;
	case DSI_CMD_SET_MI_HBM_OFF:
		cmd_update_index = DSI_CMD_SET_MI_HBM_OFF_UPDATA;
		break;
	case DSI_CMD_SET_MI_HBM_FOD_ON:
		cmd_update_index = DSI_CMD_SET_MI_HBM_FOD_ON_UPDATA;
		break;
	case DSI_CMD_SET_MI_HBM_FOD_OFF:
		cmd_update_index = DSI_CMD_SET_MI_HBM_FOD_OFF_UPDATA;
		break;
	case DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT:
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT_UPDATE;
		break;
	case DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_1000NIT:
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_1000NIT_UPDATE;
		break;
	case DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_110NIT:
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_110NIT_UPDATE;
		break;
	case DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_HBM:
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_HBM_UPDATE;
		break;
	case DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL:
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL_UPDATE;
		break;
	case DSI_CMD_SET_MI_DOZE_HBM:
		cmd_update_index = DSI_CMD_SET_MI_DOZE_HBM_UPDATE;
		break;
	case DSI_CMD_SET_MI_DOZE_LBM:
		cmd_update_index = DSI_CMD_SET_MI_DOZE_LBM_UPDATE;
		break;
	case DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_HLPM:
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_HLPM_UPDATE;
		break;
	case DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_LLPM:
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_LLPM_UPDATE;
		break;
	default:
		DISP_ERROR("[%s] unsupport cmd %s\n",
				panel->type, cmd_set_prop_map[type]);
		return -EINVAL;
	}

	info = cur_mode->priv_info->cmd_update[cmd_update_index];
	cmd_update_count = cur_mode->priv_info->cmd_update_count[cmd_update_index];
	for (j = 0; j < cmd_update_count; j++) {
		DISP_DEBUG("[%s] update [%s] mipi_address(0x%02X) index(%d) lenght(%d)\n",
			panel->type, cmd_set_prop_map[info->type],
			info->mipi_address, info->index, info->length);
		if (info && info->mipi_address != 0x51) {
			DISP_DEBUG("[%s] error mipi address (0x%02X)\n", panel->type, info->mipi_address);
			info++;
			continue;
		} else {
			mi_dsi_panel_update_cmd_set(panel, cur_mode,
					type, info, bl_buf, size);
			break;
		}
	}

	return 0;
}

static int mi_dsi_update_lhbm_cmd_87reg(struct dsi_panel *panel,
			enum dsi_cmd_set_type type, int bl_lvl)
{
	struct dsi_cmd_update_info *info = NULL;
	struct dsi_display_mode *cur_mode = NULL;
	u32 cmd_update_index = 0;
	u32 cmd_update_count = 0;
	u8 alpha_buf[2] = {0};
	int j = 0;

	if (!panel || !panel->cur_mode || !panel->cur_mode->priv_info) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	cur_mode = panel->cur_mode;

	if (!panel->mi_cfg.lhbm_alpha_ctrlaa) {
		DISP_DEBUG("local hbm can't use alpha control AA area brightness\n");
		return 0;
	}

	if ((mi_get_panel_id(panel->mi_cfg.mi_panel_id) == M11_PANEL_PA) ||
		(mi_get_panel_id(panel->mi_cfg.mi_panel_id) == N11_PANEL_PA)) {
		if (bl_lvl > 327) {
			DISP_INFO("[%s] bl_lvl = %d, alpha = 0x%x\n",
					panel->type, bl_lvl, 0xFFF);
			alpha_buf[0] = 0x10 | 0x0F ;
			alpha_buf[1] = 0xFF;
		} else {
			DISP_INFO("[%s] bl_lvl = %d, alpha = 0x%x\n",
					panel->type, bl_lvl,  aa_alpha_M11_PANEL_PA[bl_lvl]);
			alpha_buf[0] = 0x10 | ((aa_alpha_M11_PANEL_PA[bl_lvl] >> 8) & 0xFF);
			alpha_buf[1] = aa_alpha_M11_PANEL_PA[bl_lvl] & 0xFF;
		}
	} else {
		DISP_INFO("[%s] bl_lvl = %d, alpha = %d\n",
				panel->type, bl_lvl,  aa_alpha_set[bl_lvl]);
		alpha_buf[0] = (aa_alpha_set[bl_lvl] >> 8) & 0x0f;
		alpha_buf[1] = aa_alpha_set[bl_lvl] & 0xff;
	}

	switch (type) {
	case DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_GREEN_500NIT:
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_GREEN_500NIT_UPDATE;
		break;
	case DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT:
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT_UPDATE;
		break;
	case DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_110NIT:
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_110NIT_UPDATE;
		break;
	case DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_1000NIT:
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_1000NIT_UPDATE;
		break;
	case DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_110NIT:
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_110NIT_UPDATE;
		break;
	default :
		DISP_ERROR("[%s] unsupport cmd %s\n",
				panel->type, cmd_set_prop_map[type]);
		return -EINVAL;
	}

	info = cur_mode->priv_info->cmd_update[cmd_update_index];
	cmd_update_count = cur_mode->priv_info->cmd_update_count[cmd_update_index];
	for (j = 0; j < cmd_update_count; j++) {
		DISP_DEBUG("[%s] update [%s] mipi_address(0x%02X) index(%d) lenght(%d)\n",
			panel->type, cmd_set_prop_map[info->type],
			info->mipi_address, info->index, info->length);
		if (info && info->mipi_address != 0x87) {
			DISP_ERROR("[%s] error mipi address (0x%02X)\n", panel->type, info->mipi_address);
			info++;
			continue;
		} else {
			mi_dsi_panel_update_cmd_set(panel, cur_mode,
					type, info, alpha_buf, info->length);
			break;
		}
	}

	return 0;
}

int mi_dsi_update_hbm_cmd_53reg(struct dsi_panel *panel,
				enum dsi_cmd_set_type type)
{
	struct dsi_cmd_update_info *info = NULL;
	struct dsi_display_mode *cur_mode = NULL;
	u32 cmd_update_index = 0;
	u32 cmd_update_count = 0;
	u8 dimming_buf[1] = {0};
	int j = 0;

	if (!panel || !panel->cur_mode || !panel->cur_mode->priv_info) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	cur_mode = panel->cur_mode;

	switch (type) {
	case DSI_CMD_SET_MI_HBM_ON:
	{
		cmd_update_index = DSI_CMD_SET_MI_HBM_ON_UPDATA;
		if (mi_get_panel_id(panel->mi_cfg.mi_panel_id) == M2_PANEL_PA) {
			if (is_hbm_fod_on(panel)) {
				dimming_buf[0] = 0x38;
				DISP_INFO("because of fod is on,update 53 reg to 38\n");
			}
			else
				dimming_buf[0] = 0x28;
		}
	}
		break;
	case DSI_CMD_SET_MI_HBM_OFF:
	{
		cmd_update_index = DSI_CMD_SET_MI_HBM_OFF_UPDATA;
		if (mi_get_panel_id(panel->mi_cfg.mi_panel_id) == M2_PANEL_PA) {
			if (is_hbm_fod_on(panel)) {
				dimming_buf[0] = 0x30;
				DISP_INFO("because of fod is on,update 53 reg to 30\n");
			}
			else
				dimming_buf[0] = 0x20;
		}
	}
		break;
	default :
		DISP_ERROR("[%s] unsupport cmd %s\n",
			panel->type, cmd_set_prop_map[type]);
		return -EINVAL;
	}

	info = cur_mode->priv_info->cmd_update[cmd_update_index];
	cmd_update_count = cur_mode->priv_info->cmd_update_count[cmd_update_index];
	for (j = 0; j < cmd_update_count; j++) {
		DISP_DEBUG("[%s] update [%s] mipi_address(0x%02X) index(%d) lenght(%d)\n",
			panel->type, cmd_set_prop_map[info->type],
			info->mipi_address, info->index, info->length);
		if (info && info->mipi_address == 0x53) {
			mi_dsi_panel_update_cmd_set(panel, cur_mode,
					type, info, dimming_buf, info->length);
			break;
		} else {
			info++;
			continue;
		}
	}

	return 0;
}

static int mi_dsi_update_lhbm_cmd_DF_reg(struct dsi_panel *panel,
			enum dsi_cmd_set_type type, int bl_lvl)
{
	struct dsi_cmd_update_info *info = NULL;
	struct dsi_display_mode *cur_mode = NULL;
	u32 cmd_update_index = 0;
	u32 cmd_update_count = 0;
	u8 df_reg_buf[2] = {0};
	int j = 0;

	if (!panel || !panel->cur_mode || !panel->cur_mode->priv_info) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	cur_mode = panel->cur_mode;

	if (!panel->mi_cfg.lhbm_ctrl_df_reg) {
		DISP_DEBUG("local hbm can't use control DF reg\n");
		return 0;
	}

	if(bl_lvl > 327) {
		df_reg_buf[0] = ((bl_lvl * 4) >> 8) & 0xFF;
		df_reg_buf[1] = (bl_lvl * 4) & 0xFF;
	}else {
		df_reg_buf[0] = 0x05;
		df_reg_buf[1] = 0x1C;
	}
	DISP_INFO("[%s] bl_lvl = %d, df reg = 0x%x 0x%x\n",
			panel->type, bl_lvl, df_reg_buf[0], df_reg_buf[1]);

	switch (type) {
	case DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_GREEN_500NIT:
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_GREEN_500NIT_UPDATE;
		break;
	case DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT:
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT_UPDATE;
		break;
	case DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_1000NIT:
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_1000NIT_UPDATE;
		break;
	default :
		DISP_ERROR("[%s] unsupport cmd %s\n",
				panel->type, cmd_set_prop_map[type]);
		return -EINVAL;
	}

	info = cur_mode->priv_info->cmd_update[cmd_update_index];
	cmd_update_count = cur_mode->priv_info->cmd_update_count[cmd_update_index];
	for (j = 0; j < cmd_update_count; j++) {
		DISP_INFO("[%s] update [%s] mipi_address(0x%02X) index(%d) lenght(%d)\n",
			panel->type, cmd_set_prop_map[info->type],
			info->mipi_address, info->index, info->length);
		if (info && info->mipi_address != 0xDF) {
			DISP_ERROR("[%s] error mipi address (0x%02X)\n", panel->type, info->mipi_address);
			info++;
			continue;
		} else {
			mi_dsi_panel_update_cmd_set(panel, cur_mode,
					type, info, df_reg_buf, sizeof(df_reg_buf));
			break;
		}
	}

	return 0;
}


int mi_dsi_update_nolp_cmd_B2reg(struct dsi_panel *panel,
			enum dsi_cmd_set_type type)
{
	struct dsi_cmd_update_info *info = NULL;
	struct dsi_display_mode *cur_mode = NULL;
	u32 cmd_update_index = 0;
	u8 set_buf[1] = {0};

	if (!panel || !panel->cur_mode || !panel->cur_mode->priv_info) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	cur_mode = panel->cur_mode;

	panel->mi_cfg.panel_state = PANEL_STATE_ON;

	if (panel->mi_cfg.feature_val[DISP_FEATURE_DC] == FEATURE_OFF)
		set_buf[0] = 0x18;
	else
		set_buf[0] = 0x98;

	switch (type) {
	case DSI_CMD_SET_NOLP:
		cmd_update_index = DSI_CMD_SET_NOLP_UPDATE;
		break;
	case DSI_CMD_SET_MI_DOZE_HBM_NOLP:
		cmd_update_index = DSI_CMD_SET_MI_DOZE_HBM_NOLP_UPDATE;
		break;
	case DSI_CMD_SET_MI_DOZE_LBM_NOLP:
		cmd_update_index = DSI_CMD_SET_MI_DOZE_LBM_NOLP_UPDATE;
		break;
	case DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_1000NIT:
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_1000NIT_B2_UPDATE;
		break;
	case DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_110NIT:
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_110NIT_B2_UPDATE;
		break;
	default :
		DISP_ERROR("[%s] unsupport cmd %s\n",
				panel->type, cmd_set_prop_map[type]);
		return -EINVAL;
	}

	info = cur_mode->priv_info->cmd_update[cmd_update_index];
	if (info && info->mipi_address != 0xB2) {
		DISP_ERROR("error mipi address (0x%02X)\n", info->mipi_address);
		return -EINVAL;
	}
	mi_dsi_panel_update_cmd_set(panel, cur_mode,
			type, info, set_buf, sizeof(set_buf));

	return 0;
}

/* Note: Factory version need flat cmd send out immediately,
 * do not care it may lead panel flash.
 * Dev version need flat cmd send out send with te
 */
static int mi_dsi_send_flat_sync_with_te_locked(struct dsi_panel *panel,
			bool enable)
{
	int rc = 0;

#ifdef DISPLAY_FACTORY_BUILD
	if (enable) {
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_FLAT_MODE_ON);
		rc |= dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_FLAT_MODE_SEC_ON);
	} else {
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_FLAT_MODE_OFF);
		rc |= dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_FLAT_MODE_SEC_OFF);
	}
	DISP_INFO("send flat %s cmd immediately", enable ? "ON" : "OFF");
#else
	/*flat cmd will be send out at mi_sde_connector_flat_fence*/
	DISP_DEBUG("flat cmd should send out sync with te");
#endif
	return rc;
}


int mi_dsi_panel_read_and_update_doze_param(struct dsi_panel *panel)
{
	int ret = 0;
	mutex_lock(&panel->panel_lock);
	ret = mi_dsi_panel_read_and_update_doze_param_locked(panel);
	mutex_unlock(&panel->panel_lock);
	return ret;
}

int mi_dsi_panel_read_and_update_doze_param_locked(struct dsi_panel *panel)
{
	int rc = 0;
	int i = 0, j = 0;
	int rdlength = 10;
	u8 r_buf[10] = {0};
	u8 r_buf_doze_hbm[11] = {0};
	u8 r_buf_doze_lbm[11] = {0};
	u8 diff[11] = {0x08, 0x03, 0x09, 0x0E, 0x0B, 0x04, 0x0F, 0x08, 0x02, 0x15, 0x11};
	u32 num_display_modes;
	struct dsi_display *display;
	struct dsi_display_mode *cur_mode = NULL;
	struct dsi_display_mode_priv_info *priv_info;
	struct dsi_cmd_update_info *info = NULL;
	u32 cmd_update_index = 0;
	u32 cmd_update_count = 0;

	if (!panel || !panel->host) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	if (!panel->panel_initialized) {
		DISP_ERROR("[%s] Panel not initialized\n", panel->type);
		rc = -EINVAL;
		goto exit;
	}

	DISP_INFO("[%s] DOZE param read cmd start\n", panel->type);

	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DOZE_PARAM_READ);
	if (rc) {
		DISP_ERROR("[%s] failed to send DOZE_PARAM_READ cmds, rc=%d\n",
			panel->type, rc);
		rc = -EINVAL;
		goto exit;
	}
	rc = mipi_dsi_dcs_read(&panel->mipi_device, 0xBB, r_buf, rdlength);
	if (rc < 0) {
		DISP_ERROR("[%s] read back doze param failed, rc=%d\n", panel->type, rc);
		goto exit;
	}
	if (rc != rdlength) {
		DISP_INFO("read doze param BB failed (%d)\n", rc);
		rc = -EAGAIN;
		goto exit;
	}
	for(i = 0; i < 10; i++) {
		r_buf_doze_hbm[i] = r_buf[i];
	}

	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DOZE_PARAM_READ_END);
	if (rc) {
		DISP_ERROR("[%s] failed to send DOZE_PARAM_READ cmds, rc=%d\n",
			panel->type, rc);
		rc = -EINVAL;
		goto exit;
	}
	rc = mipi_dsi_dcs_read(&panel->mipi_device, 0xBB, r_buf, rdlength);
	if (rc < 0) {
		DISP_ERROR("[%s] read back doze param failed, rc=%d\n", panel->type, rc);
		goto exit;
	}
	if (rc != rdlength) {
		DISP_INFO("read doze param BB failed (%d)\n", rc);
		rc = -EAGAIN;
		goto exit;
	}
	r_buf_doze_hbm[10] = r_buf[9];


	for(i = 0; i < 11; i++) {
		if (i == 5) {
			r_buf_doze_lbm[i] = r_buf_doze_hbm[i] - 4;
			continue;
		}
		r_buf_doze_lbm[i] = r_buf_doze_hbm[i] + diff[i];
		DISP_INFO("r_buf_doze_hbm is 0x%02X, r_buf_doze_lbm is 0x%02X",r_buf_doze_hbm[i], r_buf_doze_lbm[i]);
	}

	num_display_modes = panel->num_display_modes;
	display = to_dsi_display(panel->host);
	if (!display || !display->modes) {
		DISP_ERROR("invalid display or display->modes ptr\n");
		rc = -EINVAL;
		goto exit;
	}

	for (i = 0; i < num_display_modes; i++) {
		cur_mode = &display->modes[i];
		DISP_INFO("[%s] update %d fps doze cmd\n", panel->type,
					cur_mode->timing.refresh_rate);

		priv_info = cur_mode->priv_info;
		cmd_update_index = DSI_CMD_SET_MI_DOZE_HBM_UPDATE;
		info = priv_info->cmd_update[cmd_update_index];
		cmd_update_count = priv_info->cmd_update_count[cmd_update_index];
		for (j = 0; j < cmd_update_count; j++){
			DISP_DEBUG("[%s] update [%s] mipi_address(0x%02X) index(%d) lenght(%d)\n",
				panel->type, cmd_set_prop_map[info->type],
					info->mipi_address, info->index, info->length);
			mi_dsi_panel_update_cmd_set(panel, cur_mode,
					DSI_CMD_SET_MI_DOZE_HBM, info,
					r_buf_doze_hbm, sizeof(r_buf_doze_hbm));
			info++;
		}

		cmd_update_index = DSI_CMD_SET_MI_DOZE_LBM_UPDATE;
		info = priv_info->cmd_update[cmd_update_index];
		cmd_update_count = priv_info->cmd_update_count[cmd_update_index];
		for (j = 0; j < cmd_update_count; j++){
			DISP_DEBUG("[%s] update [%s] mipi_address(0x%02X) index(%d) lenght(%d)\n",
				panel->type, cmd_set_prop_map[info->type],
					info->mipi_address, info->index, info->length);
			mi_dsi_panel_update_cmd_set(panel, cur_mode,
					DSI_CMD_SET_MI_DOZE_LBM, info,
					r_buf_doze_lbm, sizeof(r_buf_doze_lbm));
			info++;
		}
	}
	DISP_INFO("[%s] doze param read back and update end\n", panel->type);
exit:
	return rc;
}

int mi_dsi_panel_read_manufacturer_info(struct dsi_panel *panel,
		u32 manufacturer_info_addr, char *rdbuf, int rdlen)
{
	int ret = 0;
	if (!panel || !panel->host) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}
	mutex_lock(&panel->panel_lock);
	ret = mi_dsi_panel_read_manufacturer_info_locked(panel, manufacturer_info_addr, rdbuf, rdlen);
	mutex_unlock(&panel->panel_lock);
	return ret;
}

int mi_dsi_panel_read_manufacturer_info_locked(struct dsi_panel *panel,
		u32 manufacturer_info_addr, char *rdbuf, int rdlen)
{
	int rc = 0;

	if (!panel || !panel->host) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	rc = mipi_dsi_dcs_read(&panel->mipi_device, manufacturer_info_addr, rdbuf, rdlen);
	if (rc < 0) {
		DISP_ERROR("[%s] read manufacturer_info param failed, rc=%d\n", panel->type, rc);
		goto exit;
	}
	if (rc != rdlen) {
		DISP_INFO("read manufacturer_info failed (%d)\n", rc);
		goto exit;
	}

exit:
	return rc;
}

int mi_dsi_panel_read_lhbm_white_param(struct dsi_panel *panel)
{
	int ret = 0;
	mutex_lock(&panel->panel_lock);
	ret = mi_dsi_panel_read_lhbm_white_param_locked(panel);
	mutex_unlock(&panel->panel_lock);
	return ret;
}


int mi_dsi_panel_read_lhbm_white_param_locked(struct dsi_panel *panel)
{
	int rc = 0;
	u8 r_buf[20] = {0};
	u8 g_buf[20] = {0};
	u8 b_buf[20] = {0};
	int rdlength = 0;
	struct mi_dsi_panel_cfg *mi_cfg = &panel->mi_cfg;
	int j=0;

	if (!panel || !panel->host) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	if (!mi_cfg->lhbm_w1000_update_flag && !mi_cfg->lhbm_w110_update_flag) {
		DISP_DEBUG("[%s] don't need read back white rgb config\n", panel->type);
		return 0;
	}

	if (!panel->panel_initialized) {
		DISP_ERROR("[%s] Panel not initialized\n", panel->type);
		rc = -EINVAL;
		goto exit;
	}

	if (mi_cfg->uefi_read_lhbm_success ||
		(mi_cfg->lhbm_w1000_readbackdone && mi_cfg->lhbm_w110_readbackdone)) {
		DISP_DEBUG("lhbm white rgb read back done\n");
		rc = 0;
		goto exit;
	}

	DISP_INFO("[%s] LHBM white read cmd start\n", panel->type);

	if (mi_cfg->lhbm_w1000_update_flag) {
		rdlength = 10;
		rc = dsi_panel_tx_cmd_set(panel,
			DSI_CMD_SET_MI_LOCAL_HBM_WHITE_1000NIT_GIRON_PRE_READ);
		if (rc) {
			DISP_ERROR("[%s] failed to send HBM_WHITE_1000NIT_GIRON_PRE_READ cmds, rc=%d\n",
				panel->type, rc);
			rc = -EINVAL;
			goto exit;
		}

		rc = mipi_dsi_dcs_read(&panel->mipi_device, 0xB2, r_buf, rdlength);
		if (rc < 0) {
			DISP_ERROR("[%s] read back w1000_on red param failed, rc=%d\n", panel->type, rc);
			goto exit;
		}

		rc = mipi_dsi_dcs_read(&panel->mipi_device, 0xB5, g_buf, rdlength);
		if (rc < 0) {
			DISP_ERROR("[%s] read back w1000_on green param failed, rc=%d\n", panel->type, rc);
			goto exit;
		}

		rc = mipi_dsi_dcs_read(&panel->mipi_device, 0xB8, b_buf, rdlength);
		if (rc < 0) {
			DISP_ERROR("[%s] read back w1000_on blue param failed, rc=%d\n", panel->type, rc);
			goto exit;
		}

		mi_cfg->whitebuf_1000_gir_on[0] = r_buf[8];
		mi_cfg->whitebuf_1000_gir_on[1] = r_buf[9];
		mi_cfg->whitebuf_1000_gir_on[2] = g_buf[8];
		mi_cfg->whitebuf_1000_gir_on[3] = g_buf[9];
		mi_cfg->whitebuf_1000_gir_on[4] = b_buf[8];
		mi_cfg->whitebuf_1000_gir_on[5] = b_buf[9];

		memset(r_buf, 0, sizeof(r_buf));
		memset(g_buf, 0, sizeof(g_buf));
		memset(b_buf, 0, sizeof(b_buf));

		rc = dsi_panel_tx_cmd_set(panel,
			DSI_CMD_SET_MI_LOCAL_HBM_WHITE_1000NIT_GIROFF_PRE_READ);
		if (rc) {
			DISP_ERROR("[%s] failed to send HBM_WHITE_1000NIT_GIROFF_PRE_READ cmds, rc=%d\n",
				panel->type, rc);
			rc = -EINVAL;
			goto exit;
		}

		rc = mipi_dsi_dcs_read(&panel->mipi_device, 0xB2, r_buf, rdlength);
		if (rc < 0) {
			DISP_ERROR("[%s] read back w1000_off red param failed, rc=%d\n", panel->type, rc);
			goto exit;
		}

		rc = mipi_dsi_dcs_read(&panel->mipi_device, 0xB5, g_buf, rdlength);
		if (rc < 0) {
			DISP_ERROR("[%s] read back w1000_off green param failed, rc=%d\n", panel->type, rc);
			goto exit;
		}

		rc = mipi_dsi_dcs_read(&panel->mipi_device, 0xB8, b_buf, rdlength);
		if (rc < 0) {
			DISP_ERROR("[%s] read back w1000_off blue param failed, rc=%d\n", panel->type, rc);
			goto exit;
		}

		mi_cfg->whitebuf_1000_gir_off[0] =r_buf[8];
		mi_cfg->whitebuf_1000_gir_off[1] =r_buf[9];
		mi_cfg->whitebuf_1000_gir_off[2] =g_buf[8];
		mi_cfg->whitebuf_1000_gir_off[3] =g_buf[9];
		mi_cfg->whitebuf_1000_gir_off[4] =b_buf[8];
		mi_cfg->whitebuf_1000_gir_off[5] =b_buf[9];

		mi_cfg->lhbm_w1000_readbackdone = true;

		for (j = 0; j < sizeof(mi_cfg->whitebuf_1000_gir_on); j++)
			DISP_INFO("read from 0xB2,0xB5,0xB8 whitebuf_1000_gir_on[%d] = 0x%02X\n", j, mi_cfg->whitebuf_1000_gir_on[j]);

		for (j = 0; j < sizeof(mi_cfg->whitebuf_1000_gir_off); j++)
			DISP_INFO("read from 0xB2,0xB5, 0xB8,whitebuf_1000_gir_off[%d] = 0x%02X\n", j, mi_cfg->whitebuf_1000_gir_off[j]);
	} else {
		mi_cfg->lhbm_w1000_readbackdone = true;
	}

	if (mi_cfg->lhbm_w110_update_flag) {
		rdlength = 4;

		rc = dsi_panel_tx_cmd_set(panel,
			DSI_CMD_SET_MI_LOCAL_HBM_WHITE_110NIT_GIRON_PRE_READ);
		if (rc) {
			DISP_ERROR("[%s] failed to send HBM_WHITE_110NIT_GIRON_PRE_READ cmds, rc=%d\n",
				panel->type, rc);
			rc = -EINVAL;
			goto exit;
		}

		rc = mipi_dsi_dcs_read(&panel->mipi_device, 0xB2, r_buf, rdlength);
		if (rc < 0) {
			DISP_ERROR("[%s] read back w110_on red param failed, rc=%d\n", panel->type, rc);
			goto exit;
		}

		rc = mipi_dsi_dcs_read(&panel->mipi_device, 0xB5, g_buf, rdlength);
		if (rc < 0) {
			DISP_ERROR("[%s] read back w110_on green param failed, rc=%d\n", panel->type, rc);
			goto exit;
		}

		rc = mipi_dsi_dcs_read(&panel->mipi_device, 0xB8, b_buf, rdlength);
		if (rc < 0) {
			DISP_ERROR("[%s] read back w110_on blue param failed, rc=%d\n", panel->type, rc);
			goto exit;
		}

		mi_cfg->whitebuf_110_gir_on[0] = r_buf[2];
		mi_cfg->whitebuf_110_gir_on[1] = r_buf[3];
		mi_cfg->whitebuf_110_gir_on[2] = g_buf[2];
		mi_cfg->whitebuf_110_gir_on[3] = g_buf[3];
		mi_cfg->whitebuf_110_gir_on[4] = b_buf[2];
		mi_cfg->whitebuf_110_gir_on[5] = b_buf[3];

		memset(r_buf, 0, sizeof(r_buf));
		memset(g_buf, 0, sizeof(g_buf));
		memset(b_buf, 0, sizeof(b_buf));

		rc = dsi_panel_tx_cmd_set(panel,
			DSI_CMD_SET_MI_LOCAL_HBM_WHITE_110NIT_GIROFF_PRE_READ);
		if (rc) {
			DISP_ERROR("[%s] failed to send HBM_WHITE_110NIT_GIROFF_PRE_READ cmds, rc=%d\n",
				panel->type, rc);
			rc = -EINVAL;
			goto exit;
		}

		rc = mipi_dsi_dcs_read(&panel->mipi_device, 0xB2, r_buf, rdlength);
		if (rc < 0) {
			DISP_ERROR("[%s] read back w110_off red param failed, rc=%d\n", panel->type, rc);
			goto exit;
		}

		rc = mipi_dsi_dcs_read(&panel->mipi_device, 0xB5, g_buf, rdlength);
		if (rc < 0) {
			DISP_ERROR("[%s] read back w110_off green param failed, rc=%d\n", panel->type, rc);
			goto exit;
		}

		rc = mipi_dsi_dcs_read(&panel->mipi_device, 0xB8, b_buf, rdlength);
		if (rc < 0) {
			DISP_ERROR("[%s] read back w110_off blue param failed, rc=%d\n", panel->type, rc);
			goto exit;
		}

		mi_cfg->whitebuf_110_gir_off[0] = r_buf[2];
		mi_cfg->whitebuf_110_gir_off[1] = r_buf[3];
		mi_cfg->whitebuf_110_gir_off[2] = g_buf[2];
		mi_cfg->whitebuf_110_gir_off[3] = g_buf[3];
		mi_cfg->whitebuf_110_gir_off[4] = b_buf[2];
		mi_cfg->whitebuf_110_gir_off[5] = b_buf[3];

		mi_cfg->lhbm_w110_readbackdone = true;

		for (j = 0; j < sizeof(mi_cfg->whitebuf_110_gir_on); j++)
			DISP_INFO("read from 0xB2,0xB5, 0xB8,whitebuf_110_gir_on[%d] = 0x%02X\n", j, mi_cfg->whitebuf_110_gir_on[j]);

		for (j = 0; j < sizeof(mi_cfg->whitebuf_110_gir_off); j++)
			DISP_INFO("read from 0xB2,0xB5, 0xB8,whitebuf_110_gir_off[%d] = 0x%02X\n", j, mi_cfg->whitebuf_110_gir_off[j]);
	} else {
		mi_cfg->lhbm_w110_readbackdone = true;
	}
	DISP_INFO("[%s] LHBM white param read back end\n", panel->type);

exit:
	return rc;
}

static int mi_dsi_panel_update_lhbm_white_param(struct dsi_panel * panel,enum dsi_cmd_set_type type,int flat_mode)
{
	struct dsi_cmd_update_info *info = NULL;
	struct dsi_display_mode *cur_mode = NULL;
	u32 cmd_update_index = 0;
	u32 cmd_update_count = 0;
	int i= 0, j = 0, level = 1000;
	u8 *whitebuf = NULL;
	u32 r = 0, g = 0, b = 0;

	if (mi_get_panel_id_by_dsi_panel(panel) == N11_PANEL_PA) {
		DISP_DEBUG("this project don't need update white lhbm command\n");
		return 0;
	}

	if (!panel || !panel->cur_mode || !panel->cur_mode->priv_info) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	cur_mode = panel->cur_mode;

	if (!panel->mi_cfg.lhbm_w1000_update_flag && !panel->mi_cfg.lhbm_w110_update_flag) {
		DISP_DEBUG("[%s] don't need update white rgb config\n", panel->type);
		return 0;
	}

	if (!panel->panel_initialized) {
		DISP_ERROR("[%s] Panel not initialized\n", panel->type);
		return -EINVAL;
	}

	if (panel->mi_cfg.uefi_read_lhbm_success &&
		(!panel->mi_cfg.lhbm_w1000_readbackdone || !panel->mi_cfg.lhbm_w110_readbackdone)) {
		for (i = 0; i < sizeof(panel->mi_cfg.lhbm_rgb_param); i++) {
			if (i < 6)
				panel->mi_cfg.whitebuf_1000_gir_on[i] = panel->mi_cfg.lhbm_rgb_param[i];
			else if (i < 12)
					panel->mi_cfg.whitebuf_1000_gir_off[i-6] = panel->mi_cfg.lhbm_rgb_param[i];
			else if (i < 18)
				panel->mi_cfg.whitebuf_110_gir_on[i-12] = panel->mi_cfg.lhbm_rgb_param[i];
			else if (i < 24)
				panel->mi_cfg.whitebuf_110_gir_off[i-18] = panel->mi_cfg.lhbm_rgb_param[i];
		}

		if ((mi_get_panel_id(panel->mi_cfg.mi_panel_id) == M11_PANEL_PA)) {
			/*250nit gamma GAMMA R=R[11:0]+70; 250nit GAMMA G=G[11:0]+70; 250nit GAMMA B=B[11:0]*+70
			*/
			for (i = 0; i < 2; i++) {
				if (i == 0) {
					whitebuf = panel->mi_cfg.whitebuf_110_gir_on;
				} else if (i == 1) {
					whitebuf = panel->mi_cfg.whitebuf_110_gir_off;
				}

				r = (whitebuf[0] << 8) + whitebuf[1] + 70;
				g = (whitebuf[2] << 8) + whitebuf[3] + 70;
				b = (whitebuf[4] << 8) + whitebuf[5] + 70;

				whitebuf[0] = (r >> 8) & 0xFF;
				whitebuf[1] = r & 0xFF;
				whitebuf[2] =(g >> 8) & 0xFF;
				whitebuf[3] = g & 0xFF;
				whitebuf[4] = (b >> 8) & 0xFF;
				whitebuf[5] = b & 0xFF;
			}
		} else if (mi_get_panel_id(panel->mi_cfg.mi_panel_id) == N11_PANEL_PA){
			/*
			* gamma update, R,G,B for 1000nit & 110nit
			* 1000nit 5.3/4.2
			* 110nit 5/4.2
			*/
			for (i = 0; i < 4; i++) {
				if (i == 0) {
					whitebuf = panel->mi_cfg.whitebuf_1000_gir_on;
				} else if (i == 1) {
					whitebuf = panel->mi_cfg.whitebuf_1000_gir_off;
				} else if (i == 2) {
					whitebuf = panel->mi_cfg.whitebuf_110_gir_on;
				} else if (i == 3) {
					whitebuf = panel->mi_cfg.whitebuf_110_gir_off;
				}

				if (i == 0 || i == 1) {
					r = ((whitebuf[0] << 8) + whitebuf[1]) * 1052 / 1000;
					g = ((whitebuf[2] << 8) + whitebuf[3]) * 1052 / 1000;
					b = ((whitebuf[4] << 8) + whitebuf[5]) * 1052 / 1000;
					DISP_INFO("gamma update：r[%d] g[%d] b[%d] 1052 / 1000\n", r, g, b);
				} else if (i == 2 || i == 3) {
					r = ((whitebuf[0] << 8) + whitebuf[1]) * 1040 / 1000;
					g = ((whitebuf[2] << 8) + whitebuf[3]) * 1040 / 1000;
					b = ((whitebuf[4] << 8) + whitebuf[5]) * 1040 / 1000;
					DISP_INFO("gamma update：r[%d] g[%d] b[%d] 1040 / 1000\n", r, g, b);
				}

				whitebuf[0] = (r >> 8) & 0xFF;
				whitebuf[1] = r & 0xFF;
				whitebuf[2] =(g >> 8) & 0xFF;
				whitebuf[3] = g & 0xFF;
				whitebuf[4] = (b >> 8) & 0xFF;
				whitebuf[5] = b & 0xFF;
			}
		}

		panel->mi_cfg.lhbm_w1000_readbackdone = true;
		panel->mi_cfg.lhbm_w110_readbackdone = true;
	}

	switch (type) {
	case DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT:
		level = 1000;
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT_UPDATE;
		break;
	case DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_1000NIT:
		level = 1000;
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_1000NIT_UPDATE;
		break;
	case DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_110NIT:
		level = 110;
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_110NIT_UPDATE;
		break;
	case DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_110NIT:
		level = 110;
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_110NIT_UPDATE;
		break;
	default:
		DISP_ERROR("[%s] unsupport cmd %s\n", panel->type, cmd_set_prop_map[type]);
		return -EINVAL;
	}

	info = cur_mode->priv_info->cmd_update[cmd_update_index];
	cmd_update_count = cur_mode->priv_info->cmd_update_count[cmd_update_index];
	for (j = 0; j < cmd_update_count; j++) {
		DISP_DEBUG("[%s] update [%s] mipi_address(0x%02X) index(%d) lenght(%d)\n",
			panel->type, cmd_set_prop_map[info->type],
			info->mipi_address, info->index, info->length);
		if (info && info->mipi_address != 0xD0) {
			DISP_ERROR("error mipi address (0x%02X)\n", info->mipi_address);
			info++;
			continue;
		} else {
			if (level == 110) {
				if(flat_mode!= 0) {
					mi_dsi_panel_update_cmd_set(panel, cur_mode, type, info,
					panel->mi_cfg.whitebuf_110_gir_on, sizeof(panel->mi_cfg.whitebuf_110_gir_on));
				} else {
					mi_dsi_panel_update_cmd_set(panel, cur_mode, type, info,
					panel->mi_cfg.whitebuf_110_gir_off, sizeof(panel->mi_cfg.whitebuf_110_gir_off));
				}
			} else {
				if(flat_mode!= 0) {
					mi_dsi_panel_update_cmd_set(panel, cur_mode, type, info,
					panel->mi_cfg.whitebuf_1000_gir_on, sizeof(panel->mi_cfg.whitebuf_1000_gir_on));
				} else {
					mi_dsi_panel_update_cmd_set(panel, cur_mode, type, info,
					panel->mi_cfg.whitebuf_1000_gir_off, sizeof(panel->mi_cfg.whitebuf_1000_gir_off));
				}
			}
		}
	}

	return 0;
}

#define LHBM1000NITS 0
#define LHBM750NITS 6
#define LHBM500NITS 12
#define LHBM110NITS 18
#define LHBMGREEN500NITS 24
#define LHBMGIRONNITSOFFSET 26

static inline void mi_dsi_panel_cal_lhbm_param(enum dsi_cmd_set_type type, u8 *update_val, u8 size, bool flat_mode)
{
	int i = 0;
	u16 tmp;
	/*
	 * gamma update, R,G,B except 500nits and 110nits*
	 * flat mode on  4.75/4.2 
	 * flat mode off 4.8/4.2 
	*/
	for (i = 0; i < size / 2; i++) {
		tmp = (update_val[i*2] << 8) | update_val[i*2+1];
		if(flat_mode)
			tmp = tmp * 475 / 420;
		else
			tmp = tmp * 480 / 420;
		update_val[i*2] = (tmp >> 8) & 0xFF;
		update_val[i*2 + 1] = tmp & 0xFF;
	}
}

static int mi_dsi_panel_update_lhbm_param(struct dsi_panel * panel, enum dsi_cmd_set_type type, int flat_mode, int bl_lvl)
{
	struct dsi_cmd_update_info *info = NULL;
	struct dsi_display_mode *cur_mode = NULL;
	u32 cmd_update_index = 0;
	u32 cmd_update_count = 0;
	int i= 0;
	u8 DF_gir_reg_val[2] = {0x22, 0x20};
	u8 DF_bl_reg_val[2] = {0x05, 0x1C};
	u8 alpha_buf[2] = {0};
	u8 *update_D0_reg_ptr = NULL;
	u8 update_D0_reg[6] = {0};
	u32 tmp_bl;
	u8 length = 6;
	u8 lhbm_data_index = 0;
	bool is_cal_lhbm_param = false;

	if (mi_get_panel_id_by_dsi_panel(panel) != M1_PANEL_PA) {
		DISP_DEBUG("this project don't need update lhbm command\n");
		return 0;
	}

	if (!panel || !panel->cur_mode || !panel->cur_mode->priv_info) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	cur_mode = panel->cur_mode;

	if (!panel->panel_initialized) {
		DISP_ERROR("[%s] Panel not initialized\n", panel->type);
		return -EINVAL;
	}

	if(bl_lvl < 327) {
		alpha_buf[0] = (aa_alpha_M1_PANEL_PA[bl_lvl] >> 8) & 0xFF;
		alpha_buf[1] = aa_alpha_M1_PANEL_PA[bl_lvl] & 0xFF;
	} else {
		alpha_buf[0] = 0x0F;
		alpha_buf[1] = 0xFF;
		tmp_bl = bl_lvl * 4;
		DF_bl_reg_val[0] = (tmp_bl>> 8) & 0xff;
		DF_bl_reg_val[1] = tmp_bl & 0xff;
	}
	DISP_INFO("[%s] bl_lvl = %d, alpha = 0x%x%x\n", panel->type, bl_lvl, alpha_buf[0], alpha_buf[1]);

	switch (type) {
	case DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT:
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT_UPDATE;
		lhbm_data_index = LHBM1000NITS;
		is_cal_lhbm_param = true;
		break;
	case DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_750NIT:
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_750NIT_UPDATE;
		lhbm_data_index = LHBM750NITS;
		is_cal_lhbm_param = true;
		break;
	case DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_500NIT:
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_500NIT_UPDATE;
		lhbm_data_index = LHBM500NITS;
		break;
	case DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_110NIT:
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_110NIT_UPDATE;
		lhbm_data_index = LHBM110NITS;
		break;
	case DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_1000NIT:
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_1000NIT_UPDATE;
		lhbm_data_index = LHBM1000NITS;
		is_cal_lhbm_param = true;
		break;
	case DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_110NIT:
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_110NIT_UPDATE;
		lhbm_data_index = LHBM110NITS;
		break;
	case DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_GREEN_500NIT:
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_GREEN_500NIT_UPDATE;
		lhbm_data_index = LHBMGREEN500NITS;
		is_cal_lhbm_param = true;
		break;
	default:
		DISP_ERROR("[%s] unsupport cmd %s\n", panel->type, cmd_set_prop_map[type]);
		return -EINVAL;
	}

	if(panel->mi_cfg.read_lhbm_gamma_success) {
		if(lhbm_data_index == LHBMGREEN500NITS) {
			length = 2;
			update_D0_reg_ptr = &update_D0_reg[2];
		} else {
			length = 6;
			update_D0_reg_ptr = update_D0_reg;
		}
		if (flat_mode) {
			memcpy(update_D0_reg_ptr,
				&panel->mi_cfg.lhbm_param[lhbm_data_index+LHBMGIRONNITSOFFSET], length);
		} else {
			memcpy(update_D0_reg_ptr, &panel->mi_cfg.lhbm_param[lhbm_data_index], length);
		}

		if (is_cal_lhbm_param)
			mi_dsi_panel_cal_lhbm_param(type, update_D0_reg_ptr, length, flat_mode);
		DISP_INFO("update lbhm data index %d, calculate data %d, length %d",
				lhbm_data_index, is_cal_lhbm_param, length);
	}

	info = cur_mode->priv_info->cmd_update[cmd_update_index];
	cmd_update_count = cur_mode->priv_info->cmd_update_count[cmd_update_index];
	for (i = 0; i < cmd_update_count; i++) {
		if (info && info->mipi_address == 0xDF && info->length == 1) {
			/*Before P2. Invert gir on and gir off*/
			if ((panel->id_config.build_id == M1_PANEL_PA_P11) ||
				(panel->id_config.build_id == M1_PANEL_PA_P10))
				flat_mode = flat_mode ? 0 : 1;
			else
				flat_mode = flat_mode ? 1 : 0;

			mi_dsi_panel_update_cmd_set(panel, cur_mode, type, info,
				&DF_gir_reg_val[flat_mode], 1);
		} else if (info && info->mipi_address == 0xDF && info->length == 2) {
			mi_dsi_panel_update_cmd_set(panel, cur_mode, type, info,
					DF_bl_reg_val, sizeof(DF_bl_reg_val));
		}  else if (info && info->mipi_address == 0xD0 && info->length == length &&
			panel->mi_cfg.read_lhbm_gamma_success){
			mi_dsi_panel_update_cmd_set(panel, cur_mode, type, info,
				update_D0_reg, sizeof(update_D0_reg));
		} else if (info && info->mipi_address == 0x87) {
			mi_dsi_panel_update_cmd_set(panel, cur_mode, type, info,
				alpha_buf, sizeof(alpha_buf));
		}
		info++;
	}

	return 0;
}

static int mi_dsi_panel_update_lhbm_param_N11(struct dsi_panel * panel, enum dsi_cmd_set_type type, int flat_mode, int bl_lvl)
{
	struct dsi_cmd_update_info *info = NULL;
	struct dsi_display_mode *cur_mode = NULL;
	u32 cmd_update_index = 0;
	u32 cmd_update_count = 0;
	int i= 0, j = 0, level = 1000;
	int type_index = 0;
	int bl_index = 0;
	int coefficient_index = 0;
	u16 tmp = 0;
	u8 length = 6;
	int tmp_index = 0;
	u8 *update_D0_reg_ptr = NULL;
	u8 update_D0_reg[6] = {0};

	if (mi_get_panel_id_by_dsi_panel(panel) != N11_PANEL_PA) {
		DISP_DEBUG("this project don't need update white lhbm command\n");
		return 0;
	}

	if (!panel || !panel->cur_mode || !panel->cur_mode->priv_info) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	cur_mode = panel->cur_mode;

	if (!panel->mi_cfg.lhbm_w1000_update_flag && !panel->mi_cfg.lhbm_w110_update_flag) {
		DISP_DEBUG("[%s] don't need update white rgb config\n", panel->type);
		return 0;
	}

	if (!panel->panel_initialized) {
		DISP_ERROR("[%s] Panel not initialized\n", panel->type);
		return -EINVAL;
	}

	switch (type) {
	case DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT:
		level = 1000;
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT_UPDATE;
		type_index = 0;
		break;
	case DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_1000NIT:
		level = 1000;
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_1000NIT_UPDATE;
		type_index = 0;
		break;
	case DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_110NIT:
		level = 110;
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_110NIT_UPDATE;
		type_index = 1;
		break;
	case DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_110NIT:
		level = 110;
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_110NIT_UPDATE;
		type_index = 1;
		break;
	case DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_GREEN_500NIT:
		level = 500;
		cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_GREEN_500NIT_UPDATE;
		type_index = 2;
		break;
	default:
		DISP_ERROR("[%s] unsupport cmd %s\n", panel->type, cmd_set_prop_map[type]);
		return -EINVAL;
	}

	if (bl_lvl <= 2047) bl_index = 0;
	else if (2047 < bl_lvl && bl_lvl <= 2293) bl_index = 1;
	else if (2293 < bl_lvl && bl_lvl <= 2440) bl_index = 2;
	else if (2440 < bl_lvl && bl_lvl <= 2637) bl_index = 3;
	else if (2637 < bl_lvl && bl_lvl <= 2858) bl_index = 4;
	else if (2858 < bl_lvl && bl_lvl <= 2957) bl_index = 5;
	else if (2957 < bl_lvl && bl_lvl <= 3055) bl_index = 6;
	else if (3055 < bl_lvl && bl_lvl <= 3154) bl_index = 7;
	else if (3154 < bl_lvl && bl_lvl <= 3275) bl_index = 8;
	else if (3275 < bl_lvl && bl_lvl <= 3375) bl_index = 9;
	else if (3375 < bl_lvl && bl_lvl <= 3476) bl_index = 10;
	else if (3476 < bl_lvl && bl_lvl <= 3590) bl_index = 11;
	else if (3590 < bl_lvl && bl_lvl <= 3791) bl_index = 12;
	else if (3791 < bl_lvl && bl_lvl <= 3900) bl_index = 13;

	coefficient_index = type_index*14 + bl_index;
	DISP_DEBUG("type_index = %d,bl_index = %d,coefficient_index = %d\n",type_index,bl_index,coefficient_index);
	if (coefficient_index < 0 || coefficient_index > 41) {
		DISP_ERROR("invalid lhbm coefficient_index\n");
		return 0;
	}

	if (flat_mode) {
		tmp_index = type_index*2*6;
	} else {
		tmp_index = (type_index == 2) ? type_index*2*6+type_index : (type_index*2+1)*6;
	}

	if (panel->mi_cfg.uefi_read_lhbm_success) {
		if (type_index < 2) {
			length = 6;
			update_D0_reg_ptr = update_D0_reg;
		} else {
			length = 2;
			update_D0_reg_ptr = &update_D0_reg[2];
		}
		for (i = 0; i < length / 2; i++) {
			tmp = (panel->mi_cfg.lhbm_rgb_param[tmp_index+i*2] << 8) | panel->mi_cfg.lhbm_rgb_param[tmp_index+i*2+1];
			DISP_DEBUG("gamma update：tmp[%d] * lhbm_coefficient_N11_PANEL_PA[%d] / 10000\n",tmp,coefficient_index);
			tmp = tmp * lhbm_coefficient_N11_PANEL_PA[coefficient_index] / 10000;
			update_D0_reg_ptr[i*2] = (tmp >> 8) & 0xFF;
			update_D0_reg_ptr[i*2 + 1] = tmp & 0xFF;
		}
	}

	info = cur_mode->priv_info->cmd_update[cmd_update_index];
	cmd_update_count = cur_mode->priv_info->cmd_update_count[cmd_update_index];
	for (j = 0; j < cmd_update_count; j++) {
		DISP_DEBUG("[%s] update [%s] mipi_address(0x%02X) index(%d) lenght(%d)\n",
			panel->type, cmd_set_prop_map[info->type],
			info->mipi_address, info->index, info->length);
		if (info && info->mipi_address != 0xD0) {
			DISP_ERROR("error mipi address (0x%02X)\n", info->mipi_address);
			info++;
			continue;
		} else if (info && info->mipi_address == 0xD0 && panel->mi_cfg.uefi_read_lhbm_success) {
			mi_dsi_panel_update_cmd_set(panel, cur_mode, type, info,
				update_D0_reg, sizeof(update_D0_reg));
		}
	}

	return 0;
}

int mi_dsi_panel_set_round_corner_locked(struct dsi_panel *panel,
			bool enable)
{
	int rc = 0;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	if (panel->mi_cfg.ddic_round_corner_enabled) {
		if (enable)
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_ROUND_CORNER_ON);
		else
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_ROUND_CORNER_OFF);

		if (rc)
			DISP_ERROR("[%s] failed to send ROUND_CORNER(%s) cmds, rc=%d\n",
				panel->type, enable ? "On" : "Off", rc);
	} else {
		DISP_INFO("[%s] ddic round corner feature not enabled\n", panel->type);
	}

	return rc;
}

int mi_dsi_panel_set_round_corner(struct dsi_panel *panel, bool enable)
{
	int rc = 0;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);

	rc = mi_dsi_panel_set_round_corner_locked(panel, enable);

	mutex_unlock(&panel->panel_lock);

	return rc;
}

int mi_dsi_panel_set_dc_mode(struct dsi_panel *panel, bool enable)
{
	int rc = 0;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);

	rc = mi_dsi_panel_set_dc_mode_locked(panel, enable);

	mutex_unlock(&panel->panel_lock);

	return rc;
}


int mi_dsi_panel_set_dc_mode_locked(struct dsi_panel *panel, bool enable)
{
	int rc = 0;
	struct mi_dsi_panel_cfg *mi_cfg  = NULL;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mi_cfg = &panel->mi_cfg;

	if (mi_cfg->dc_feature_enable) {
		if (enable) {
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DC_ON);
			mi_cfg->real_dc_state = FEATURE_ON;
		} else {
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DC_OFF);
			mi_cfg->real_dc_state = FEATURE_OFF;
		}

		if (rc)
			DISP_ERROR("failed to set DC mode: %d\n", enable);
		else
			DISP_INFO("DC mode: %s\n", enable ? "On" : "Off");
	} else {
		DISP_INFO("DC mode: TODO\n");
	}

	return rc;
}

int mi_dsi_panel_set_lhbm_fod_locked(struct dsi_panel *panel,
		struct disp_feature_ctl *ctl)
{
	int rc = 0;
	int update_bl_lvl = 0;
	struct mi_dsi_panel_cfg *mi_cfg = NULL;

	if (!panel || !ctl || ctl->feature_id != DISP_FEATURE_LOCAL_HBM) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mi_cfg = &panel->mi_cfg;

	if ((mi_cfg->feature_val[DISP_FEATURE_FOD_CALIBRATION_BRIGHTNESS] > 0) && mi_get_panel_id_by_dsi_panel(panel) == M1_PANEL_PA) {
		DISP_TIME_INFO("fod cal brightness, update alpha,last bl (%d)-->fod cal bl(%d)", mi_cfg->last_bl_level,
				mi_cfg->feature_val[DISP_FEATURE_FOD_CALIBRATION_BRIGHTNESS]);
		mi_cfg->last_bl_level = mi_cfg->feature_val[DISP_FEATURE_FOD_CALIBRATION_BRIGHTNESS];
	}

	DISP_TIME_INFO("[%s] Local HBM: %s\n", panel->type,
			get_local_hbm_state_name(ctl->feature_val));

	switch (ctl->feature_val) {
	case LOCAL_HBM_OFF_TO_NORMAL:
		mi_dsi_update_backlight_in_aod(panel, true);
		if (mi_cfg->feature_val[DISP_FEATURE_HBM] == FEATURE_ON) {
			DISP_INFO("LOCAL_HBM_OFF_TO_HBM\n");
			mi_cfg->dimming_state = STATE_DIM_BLOCK;
			mi_dsi_update_51_mipi_cmd(panel, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_HBM,
					mi_cfg->last_bl_level);
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_HBM);
		} else {
			if (mi_get_panel_id(panel->mi_cfg.mi_panel_id) == M2_PANEL_PA) {
				if (mi_cfg->is_em_cycle_16_pulse && mi_cfg->lhbm_gxzw && mi_cfg->feature_val[DISP_FEATURE_FP_STATUS] != AUTH_STOP) {
					DISP_INFO("LOCAL_HBM_OFF_TO_0SIZE_LHBM\n");
					rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT_PRE);
					mi_cfg->lhbm_0size_on = true;
				} else if (mi_cfg->is_em_cycle_16_pulse) {
					DISP_INFO("LOCAL_HBM_OFF_TO_NORMAL CVC\n");
					mi_dsi_update_em_cycle_cmd(panel, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL_EM_CYCLE_16PULSE,
						mi_cfg->last_bl_level);
					rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL_EM_CYCLE_16PULSE);
					mi_cfg->lhbm_0size_on = false;
				} else {
					DISP_INFO("LOCAL_HBM_OFF_TO_NORMAL\n");
					mi_dsi_update_em_cycle_cmd(panel,DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL,mi_cfg->last_bl_level);
					rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL);
					mi_cfg->lhbm_0size_on = false;
				}
			} else {
				DISP_INFO("LOCAL_HBM_OFF_TO_NORMAL\n");
				if ( mi_get_panel_id_by_dsi_panel(panel) == N11_PANEL_PA) {
					if (mi_cfg->last_bl_level <= mi_cfg->panel_3840pwm_dbv_threshold) {
						rc |= dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_32PULSE_NO51);
						mi_cfg->is_em_cycle_32_pulse = true;
						DISP_INFO("LOCAL_HBM_OFF_TO_NORMAL set DSI_CMD_SET_MI_32PULSE_NO51\n");
					} else {
						rc |= dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_16PULSE_NO51);
						mi_cfg->is_em_cycle_32_pulse = false;
						DISP_INFO("LOCAL_HBM_OFF_TO_NORMAL set DSI_CMD_SET_MI_16PULSE_NO51\n");
					}
				}
				mi_dsi_update_51_mipi_cmd(panel, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL,
					mi_cfg->last_bl_level);
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL);
				mi_cfg->lhbm_0size_on = false;
			}
			mi_cfg->dimming_state = STATE_DIM_RESTORE;
			mi_dsi_panel_update_last_bl_level(panel, mi_cfg->last_bl_level);
		}
		/*normal fod, restore dbi by dbv*/
		if (mi_get_panel_id(mi_cfg->mi_panel_id) == M1_PANEL_PA && mi_cfg->last_bl_level < 0x84) {
			mi_cfg->dbi_bwg_type = DSI_CMD_SET_MAX;
			dsi_panel_update_backlight(panel, mi_cfg->last_bl_level);
		}
		break;
	case LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT:
		DISP_INFO("LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT\n");
		mi_dsi_update_backlight_in_aod(panel, false);
		/* display backlight value should equal AOD brightness */
		if (is_aod_and_panel_initialized(panel)) {
			switch (mi_cfg->doze_brightness) {
			case DOZE_BRIGHTNESS_HBM:
				if (mi_cfg->last_no_zero_bl_level < mi_cfg->doze_hbm_dbv_level
					&& mi_cfg->feature_val[DISP_FEATURE_FP_STATUS] == AUTH_STOP) {
					mi_dsi_update_51_mipi_cmd(panel, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL,
						mi_cfg->last_no_zero_bl_level);
				} else {
					mi_dsi_update_51_mipi_cmd(panel, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL,
						mi_cfg->doze_hbm_dbv_level);
				}
				DISP_INFO("LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT in doze_hbm_dbv_level\n");
				break;
			case DOZE_BRIGHTNESS_LBM:
				if (mi_cfg->last_no_zero_bl_level < mi_cfg->doze_lbm_dbv_level) {
					mi_dsi_update_51_mipi_cmd(panel, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL,
						mi_cfg->last_no_zero_bl_level);
				} else {
					mi_dsi_update_51_mipi_cmd(panel, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL,
						mi_cfg->doze_lbm_dbv_level);
				}
				DISP_INFO("LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT in doze_lbm_dbv_level\n");
				break;
			default:
				DISP_INFO("LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT defaults\n");
				break;
			}
		} else {
			DISP_INFO("LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT last_bl_level %d\n", mi_cfg->last_bl_level);
			mi_dsi_update_51_mipi_cmd(panel, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL, mi_cfg->last_bl_level);
		}
		if (mi_get_panel_id_by_dsi_panel(panel) == M2_PANEL_PA) {
			DISP_INFO("LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT CVC\n");
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL_EM_CYCLE_16PULSE);
			mi_cfg->lhbm_0size_on = false;
			mi_cfg->is_em_cycle_16_pulse = true;
			panel->mi_cfg.aod_to_normal_statue = true;
		} else {
			DISP_INFO("LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT\n");
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL);
			mi_cfg->lhbm_0size_on = false;
			if (mi_cfg->need_fod_animal_in_normal)
				panel->mi_cfg.aod_to_normal_statue = true;
		}
		mi_cfg->dimming_state = STATE_DIM_RESTORE;
		if (mi_get_panel_id_by_dsi_panel(panel) == N11_PANEL_PA)
			mi_cfg->is_em_cycle_32_pulse = true;
		break;
	case LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT_RESTORE:
		/* display backlight value should equal unlock brightness */
		DISP_INFO("LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT_RESTORE\n");
		mi_dsi_update_backlight_in_aod(panel, true);
		mi_dsi_update_51_mipi_cmd(panel, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL,
					mi_cfg->last_bl_level);

		/*mi_dsi_update_backlight_in_aod will change 51's value */
		if (mi_get_panel_id(panel->mi_cfg.mi_panel_id) == M2_PANEL_PA) {
			if (mi_cfg->last_bl_level > mi_cfg->panel_1920pwm_dbv_threshold && mi_cfg->is_em_cycle_16_pulse)
				mi_cfg->is_em_cycle_16_pulse = false;
		}

		if (mi_cfg->is_em_cycle_16_pulse) {
			DISP_INFO("LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT_RESTORE CVC\n");
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL_EM_CYCLE_16PULSE);
			mi_cfg->lhbm_0size_on = false;
		} else {
			DISP_INFO("LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT_RESTORE\n");
			if (mi_get_panel_id_by_dsi_panel(panel) == N11_PANEL_PA) {
				if (mi_cfg->last_bl_level > 0 && mi_cfg->last_bl_level <= mi_cfg->panel_3840pwm_dbv_threshold) {
					rc |= dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_32PULSE_NO51);
					mi_cfg->is_em_cycle_32_pulse = true;
					DISP_INFO("LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT_RESTORE set DSI_CMD_SET_MI_32PULSE_NO51\n");
				} else if (mi_cfg->last_bl_level > mi_cfg->panel_3840pwm_dbv_threshold) {
					rc |= dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_16PULSE_NO51);
					mi_cfg->is_em_cycle_32_pulse = false;
					DISP_INFO("LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT_RESTORE set DSI_CMD_SET_MI_16PULSE_NO51\n");
				}
			}
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL);
			mi_cfg->lhbm_0size_on = false;
		}

		/*mi_dsi_update_backlight_in_aod will change 51's value */
		if (mi_get_panel_id(panel->mi_cfg.mi_panel_id) == N11_PANEL_PA) {
			if (mi_cfg->last_bl_level > mi_cfg->panel_3840pwm_dbv_threshold && mi_cfg->is_em_cycle_32_pulse)
				mi_cfg->is_em_cycle_32_pulse = false;
		}

		mi_cfg->dimming_state = STATE_DIM_RESTORE;
		break;
	case LOCAL_HBM_NORMAL_WHITE_1000NIT:
		mi_cfg->dimming_state = STATE_DIM_BLOCK;
		if (mi_cfg->feature_val[DISP_FEATURE_HBM] == FEATURE_ON) {
			DISP_INFO("LOCAL_HBM_NORMAL_WHITE_1000NIT in HBM\n");
			if (mi_get_panel_id(mi_cfg->mi_panel_id) == M3_PANEL_PA)
				update_bl_lvl = mi_cfg->last_bl_level;
			else {
				update_bl_lvl = panel->bl_config.bl_max_level;
			}
		} else {
			DISP_INFO("LOCAL_HBM_NORMAL_WHITE_1000NIT\n");
			if (is_aod_and_panel_initialized(panel)) {
				switch (mi_cfg->doze_brightness) {
				case DOZE_BRIGHTNESS_HBM:
					update_bl_lvl = mi_cfg->doze_hbm_dbv_level;
					DISP_INFO("LOCAL_HBM_NORMAL_WHITE_1000NIT in doze_hbm_dbv_level\n");
					break;
				case DOZE_BRIGHTNESS_LBM:
					update_bl_lvl = mi_cfg->doze_lbm_dbv_level;
					DISP_INFO("LOCAL_HBM_NORMAL_WHITE_1000NIT in doze_lbm_dbv_level\n");
					break;
				default:
					update_bl_lvl = mi_cfg->doze_hbm_dbv_level;
					DISP_INFO("LOCAL_HBM_NORMAL_WHITE_1000NIT use doze_hbm_dbv_level as defaults\n");
					break;
				}
			} else {
				update_bl_lvl = mi_cfg->last_bl_level;
			}
		}
		mi_dsi_update_51_mipi_cmd(panel,
				DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT, update_bl_lvl);

		mi_dsi_update_lhbm_cmd_87reg(panel,
				DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT, update_bl_lvl);

		mi_dsi_update_lhbm_cmd_DF_reg(panel,
				DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT,
				mi_cfg->last_bl_level);

		mi_dsi_panel_update_lhbm_white_param(panel,
				DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT,
				mi_cfg->feature_val[DISP_FEATURE_FLAT_MODE]);

		mi_dsi_panel_update_lhbm_param(panel,
				DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT,
				mi_cfg->feature_val[DISP_FEATURE_FLAT_MODE], update_bl_lvl);

		if (mi_get_panel_id_by_dsi_panel(panel) == N11_PANEL_PA) {
			mi_dsi_panel_update_lhbm_param_N11(panel,
				DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT,
				mi_cfg->feature_val[DISP_FEATURE_FLAT_MODE], update_bl_lvl);
		}
		if (mi_cfg->is_em_cycle_16_pulse)
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT_EM_CYCLE_16PULSE);
		else
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT);
		break;
	case LOCAL_HBM_NORMAL_WHITE_750NIT:
		mi_dsi_panel_update_lhbm_param(panel,
				DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_750NIT,
				mi_cfg->feature_val[DISP_FEATURE_FLAT_MODE], mi_cfg->last_bl_level);

		if (mi_cfg->is_em_cycle_16_pulse) {
			DISP_INFO("LOCAL_HBM_NORMAL_WHITE_750NIT_CVC\n");
			mi_cfg->dimming_state = STATE_DIM_BLOCK;
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_750NIT_EM_CYCLE_16PULSE);
		} else {
			DISP_INFO("LOCAL_HBM_NORMAL_WHITE_750NIT\n");
			mi_cfg->dimming_state = STATE_DIM_BLOCK;
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_750NIT);
		}
		break;
	case LOCAL_HBM_NORMAL_WHITE_500NIT:
		mi_dsi_panel_update_lhbm_param(panel,
				DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_500NIT,
				mi_cfg->feature_val[DISP_FEATURE_FLAT_MODE], mi_cfg->last_bl_level);

		if (mi_cfg->is_em_cycle_16_pulse) {
			DISP_INFO("LOCAL_HBM_NORMAL_WHITE_500NIT_CVC\n");
			mi_cfg->dimming_state = STATE_DIM_BLOCK;
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_500NIT_EM_CYCLE_16PULSE);
		} else {
			DISP_INFO("LOCAL_HBM_NORMAL_WHITE_500NIT\n");
			mi_cfg->dimming_state = STATE_DIM_BLOCK;
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_500NIT);
		}
		break;
	case LOCAL_HBM_NORMAL_WHITE_110NIT:
		DISP_INFO("LOCAL_HBM_NORMAL_WHITE_110NIT\n");
		mi_cfg->dimming_state = STATE_DIM_BLOCK;

		update_bl_lvl = mi_cfg->last_bl_level;

		if (is_aod_and_panel_initialized(panel)) {
			switch (mi_cfg->doze_brightness) {
			case DOZE_BRIGHTNESS_HBM:
				update_bl_lvl = mi_cfg->doze_hbm_dbv_level;
				DISP_INFO("LOCAL_HBM_NORMAL_WHITE_110NIT in doze_hbm_dbv_level\n");
				break;
			case DOZE_BRIGHTNESS_LBM:
				update_bl_lvl = mi_cfg->doze_lbm_dbv_level;
				DISP_INFO("LOCAL_HBM_NORMAL_WHITE_110NIT in doze_lbm_dbv_level\n");
				break;
			default:
				update_bl_lvl = mi_cfg->doze_lbm_dbv_level;
				DISP_INFO("LOCAL_HBM_NORMAL_WHITE_110NIT use doze_lbm_dbv_level as defaults\n");
				break;
			}
		}
		mi_dsi_update_lhbm_cmd_87reg(panel, DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_110NIT,
				update_bl_lvl);

		mi_dsi_panel_update_lhbm_white_param(panel,
				DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_110NIT,
				mi_cfg->feature_val[DISP_FEATURE_FLAT_MODE]);

		mi_dsi_panel_update_lhbm_param(panel,
				DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_110NIT,
				mi_cfg->feature_val[DISP_FEATURE_FLAT_MODE], update_bl_lvl);

		if (mi_get_panel_id_by_dsi_panel(panel) == N11_PANEL_PA) {
			mi_dsi_panel_update_lhbm_param_N11(panel,
				DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_110NIT,
				mi_cfg->feature_val[DISP_FEATURE_FLAT_MODE], update_bl_lvl);
		}
		if (mi_cfg->is_em_cycle_16_pulse) {
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_110NIT_EM_CYCLE_16PULSE);
		} else {
			if (mi_get_panel_id_by_dsi_panel(panel) == N11_PANEL_PA) {
				if (update_bl_lvl > 0 && update_bl_lvl <= mi_cfg->panel_3840pwm_dbv_threshold) {
					rc |= dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_32PULSE_NO51);
					mi_cfg->is_em_cycle_32_pulse = true;
					DISP_INFO("LOCAL_HBM_NORMAL_WHITE_110NIT set DSI_CMD_SET_MI_32PULSE_NO51\n");
				} else if (update_bl_lvl > mi_cfg->panel_3840pwm_dbv_threshold) {
					rc |= dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_16PULSE_NO51);
					mi_cfg->is_em_cycle_32_pulse = false;
					DISP_INFO("LOCAL_HBM_NORMAL_WHITE_110NIT set DSI_CMD_SET_MI_16PULSE_NO51\n");
				}
			}
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_110NIT);
		}
		break;
	case LOCAL_HBM_NORMAL_GREEN_500NIT:
		DISP_INFO("LOCAL_HBM_NORMAL_GREEN_500NIT\n");
		mi_cfg->dimming_state = STATE_DIM_BLOCK;
		mi_dsi_update_lhbm_cmd_DF_reg(panel,
				DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_GREEN_500NIT, mi_cfg->last_bl_level);

		mi_dsi_update_lhbm_cmd_87reg(panel, DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_GREEN_500NIT,
				mi_cfg->last_bl_level);

		mi_dsi_panel_update_lhbm_param(panel, DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_GREEN_500NIT,
			mi_cfg->feature_val[DISP_FEATURE_FLAT_MODE], mi_cfg->last_bl_level);

		if (mi_get_panel_id_by_dsi_panel(panel) == N11_PANEL_PA) {
			mi_dsi_panel_update_lhbm_param_N11(panel, DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_GREEN_500NIT,
			mi_cfg->feature_val[DISP_FEATURE_FLAT_MODE], mi_cfg->last_bl_level);
		}
		if (mi_cfg->is_em_cycle_16_pulse)
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_GREEN_500NIT_EM_CYCLE_16PULSE);
		else
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_GREEN_500NIT);
		break;
	case LOCAL_HBM_HLPM_WHITE_1000NIT:
		DISP_INFO("LOCAL_HBM_HLPM_WHITE_1000NIT\n");
		mi_cfg->dimming_state = STATE_DIM_BLOCK;
		mi_dsi_update_nolp_cmd_B2reg(panel, DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_1000NIT);

		update_bl_lvl = mi_cfg->last_bl_level;

		if (is_aod_and_panel_initialized(panel)) {
			switch (mi_cfg->doze_brightness) {
			case DOZE_BRIGHTNESS_HBM:
				update_bl_lvl = mi_cfg->doze_hbm_dbv_level;
				DISP_INFO("LOCAL_HBM_HLPM_WHITE_1000NIT in doze_hbm_dbv_level\n");
				break;
			case DOZE_BRIGHTNESS_LBM:
				update_bl_lvl = mi_cfg->doze_lbm_dbv_level;
				DISP_INFO("LOCAL_HBM_HLPM_WHITE_1000NIT in doze_lbm_dbv_level\n");
				break;
			default:
				update_bl_lvl = mi_cfg->doze_hbm_dbv_level;
				DISP_INFO("LOCAL_HBM_HLPM_WHITE_1000NIT use doze_hbm_dbv_level as defaults\n");
				break;
			}
		}
		mi_dsi_update_51_mipi_cmd(panel,
			DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_1000NIT, update_bl_lvl);

		mi_dsi_update_lhbm_cmd_87reg(panel,
			DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_1000NIT, update_bl_lvl);

		if (mi_get_panel_id_by_dsi_panel(panel) == N11_PANEL_PA) {
			if (update_bl_lvl > 0 && update_bl_lvl <= mi_cfg->panel_3840pwm_dbv_threshold) {
				rc |= dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_32PULSE_NO51);
				mi_cfg->is_em_cycle_32_pulse = true;
				DISP_INFO("LOCAL_HBM_HLPM_WHITE_1000NIT set DSI_CMD_SET_MI_32PULSE_NO51\n");
			} else if (update_bl_lvl > mi_cfg->panel_3840pwm_dbv_threshold) {
				rc |= dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_16PULSE_NO51);
				mi_cfg->is_em_cycle_32_pulse = false;
				DISP_INFO("LOCAL_HBM_HLPM_WHITE_1000NIT set DSI_CMD_SET_MI_16PULSE_NO51\n");
			}
		}

		mi_dsi_update_backlight_in_aod(panel, false);

		mi_dsi_panel_update_lhbm_white_param(panel,
				DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_1000NIT,
				mi_cfg->feature_val[DISP_FEATURE_FLAT_MODE]);

		mi_dsi_panel_update_lhbm_param(panel,
				DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_1000NIT,
				mi_cfg->feature_val[DISP_FEATURE_FLAT_MODE], update_bl_lvl);

		if (mi_get_panel_id_by_dsi_panel(panel) == N11_PANEL_PA) {
			mi_dsi_panel_update_lhbm_param_N11(panel,
				DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_1000NIT,
				mi_cfg->feature_val[DISP_FEATURE_FLAT_MODE], update_bl_lvl);
		}
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_1000NIT);
		panel->mi_cfg.panel_state = PANEL_STATE_ON;
		break;
	case LOCAL_HBM_HLPM_WHITE_110NIT:
		DISP_INFO("LOCAL_HBM_HLPM_WHITE_110NIT\n");
		mi_cfg->dimming_state = STATE_DIM_BLOCK;
		mi_dsi_update_nolp_cmd_B2reg(panel, DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_110NIT);

		update_bl_lvl = mi_cfg->last_bl_level;

		if (is_aod_and_panel_initialized(panel)) {
			switch (mi_cfg->doze_brightness) {
			case DOZE_BRIGHTNESS_HBM:
				update_bl_lvl = mi_cfg->doze_hbm_dbv_level;
				DISP_INFO("LOCAL_HBM_HLPM_WHITE_110NIT in doze_hbm_dbv_level\n");
				break;
			case DOZE_BRIGHTNESS_LBM:
				update_bl_lvl = mi_cfg->doze_lbm_dbv_level;
				DISP_INFO("LOCAL_HBM_HLPM_WHITE_110NIT in doze_lbm_dbv_level\n");
				break;
			default:
				update_bl_lvl = mi_cfg->doze_lbm_dbv_level;
				DISP_INFO("LOCAL_HBM_HLPM_WHITE_1000NIT use doze_lbm_dbv_level as defaults\n");
				break;
			}
		}
		mi_dsi_update_51_mipi_cmd(panel,
			DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_110NIT, update_bl_lvl);

		mi_dsi_update_lhbm_cmd_87reg(panel,
			DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_110NIT, update_bl_lvl);

		mi_dsi_update_backlight_in_aod(panel, false);

		mi_dsi_panel_update_lhbm_white_param(panel,
				DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_110NIT,
				mi_cfg->feature_val[DISP_FEATURE_FLAT_MODE]);

		mi_dsi_panel_update_lhbm_param(panel,
				DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_110NIT,
				mi_cfg->feature_val[DISP_FEATURE_FLAT_MODE], update_bl_lvl);

		if (mi_get_panel_id_by_dsi_panel(panel) == N11_PANEL_PA) {
			mi_dsi_panel_update_lhbm_param_N11(panel,
				DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_110NIT,
				mi_cfg->feature_val[DISP_FEATURE_FLAT_MODE], update_bl_lvl);
		}
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_110NIT);
		panel->mi_cfg.panel_state = PANEL_STATE_ON;
		break;
	case LOCAL_HBM_OFF_TO_HLPM:
		DISP_INFO("LOCAL_HBM_OFF_TO_HLPM\n");
		mi_cfg->dimming_state = STATE_DIM_BLOCK;
		panel->mi_cfg.panel_state = PANEL_STATE_DOZE_HIGH;
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_HLPM);
		break;
	case LOCAL_HBM_OFF_TO_LLPM:
		DISP_INFO("LOCAL_HBM_OFF_TO_LLPM\n");
		mi_cfg->dimming_state = STATE_DIM_BLOCK;
		panel->mi_cfg.panel_state = PANEL_STATE_DOZE_LOW;
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_LLPM);
		break;
	default:
		DISP_ERROR("invalid local hbm value\n");
		break;
	}

	return rc;
}

static int mi_dsi_panel_aod_to_normal_optimize_locked(struct dsi_panel *panel,
		bool enable)
{
	int rc = 0;
	struct mi_dsi_panel_cfg *mi_cfg = NULL;

	if (!panel || !panel->panel_initialized) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mi_cfg = &panel->mi_cfg;

	if (!mi_cfg->need_fod_animal_in_normal)
		return 0;

	DISP_TIME_DEBUG("[%s] fod aod_to_normal: %d\n", panel->type, enable);

	if (is_hbm_fod_on(panel)) {
		DSI_INFO("fod hbm on, skip fod aod_to_normal: %d\n", enable);
		return 0;
	}

	if (enable && mi_cfg->panel_state != PANEL_STATE_ON) {
		switch (mi_cfg->doze_brightness) {
		case DOZE_BRIGHTNESS_HBM:
			DISP_INFO("enter DOZE HBM NOLP\n");
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DOZE_HBM_NOLP);
			if (rc) {
				DISP_ERROR("[%s] failed to send MI_DOZE_HBM_NOLP cmd, rc=%d\n",
					panel->type, rc);
			} else {
				panel->mi_cfg.panel_state = PANEL_STATE_ON;
				mi_cfg->aod_to_normal_statue = true;
				if (mi_get_panel_id_by_dsi_panel(panel) == M2_PANEL_PA)
					mi_cfg->is_em_cycle_16_pulse = true;
				if (mi_get_panel_id_by_dsi_panel(panel) == N11_PANEL_PA)
					mi_cfg->is_em_cycle_32_pulse = true;
			}
			break;
		case DOZE_BRIGHTNESS_LBM:
			DISP_INFO("enter DOZE LBM NOLP\n");
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DOZE_LBM_NOLP);
			if (rc) {
				DISP_ERROR("[%s] failed to send MI_DOZE_LBM_NOLP cmd, rc=%d\n",
					panel->type, rc);
			} else {
				panel->mi_cfg.panel_state = PANEL_STATE_ON;
				mi_cfg->aod_to_normal_statue = true;
				if (mi_get_panel_id_by_dsi_panel(panel) == M2_PANEL_PA)
					mi_cfg->is_em_cycle_16_pulse = true;
				if (mi_get_panel_id_by_dsi_panel(panel) == N11_PANEL_PA) {
					dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_32PULSE_NO51);
					mi_cfg->is_em_cycle_32_pulse = true;
					DSI_INFO("DSI_CMD_SET_MI_32PULSE_NO51\n");
				}
			}
			break;
		default:
			break;
		}
		/* If fp is enable and lock fps is not 120HZ.
		 * So enter nolp, need switch the right FR */
		if (panel->cur_mode->timing.refresh_rate != 120) {
			if (mi_get_panel_id_by_dsi_panel(panel) == M1_PANEL_PA ||
				mi_get_panel_id_by_dsi_panel(panel) == M2_PANEL_PA) {
				if (mi_cfg->doze_brightness == DOZE_BRIGHTNESS_HBM ||
					mi_cfg->doze_brightness == DOZE_BRIGHTNESS_LBM) {
					dsi_panel_switch_locked(panel);
				}
			}
		}
	} else if (!enable && mi_cfg->panel_state == PANEL_STATE_ON &&
		panel->power_mode != SDE_MODE_DPMS_ON &&
		mi_cfg->feature_val[DISP_FEATURE_FP_STATUS] != AUTH_STOP) {
		switch (mi_cfg->doze_brightness) {
		case DOZE_BRIGHTNESS_HBM:
			DISP_INFO("enter DOZE HBM\n");
			if (mi_get_panel_id_by_dsi_panel(panel) == M2_PANEL_PA ||
				mi_get_panel_id_by_dsi_panel(panel) == M3_PANEL_PA) {
				mi_dsi_update_51_mipi_cmd(panel, DSI_CMD_SET_MI_DOZE_HBM, mi_cfg->doze_51reg_update_value);
			} else {
				mi_dsi_update_51_mipi_cmd(panel, DSI_CMD_SET_MI_DOZE_HBM,
					mi_cfg->doze_hbm_dbv_level);
			}
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DOZE_HBM);
			if (rc) {
				DISP_ERROR("[%s] failed to send MI_DOZE_HBM_NOLP cmd, rc=%d\n",
					panel->type, rc);
			}
			panel->mi_cfg.panel_state = PANEL_STATE_DOZE_HIGH;
			mi_cfg->aod_to_normal_statue = false;
			break;
		case DOZE_BRIGHTNESS_LBM:
			DISP_INFO("enter DOZE LBM\n");
			if (mi_get_panel_id_by_dsi_panel(panel) == M2_PANEL_PA ||
				mi_get_panel_id_by_dsi_panel(panel) == M3_PANEL_PA) {
				mi_dsi_update_51_mipi_cmd(panel, DSI_CMD_SET_MI_DOZE_LBM, mi_cfg->doze_51reg_update_value);
			} else {
				mi_dsi_update_51_mipi_cmd(panel, DSI_CMD_SET_MI_DOZE_LBM,
					mi_cfg->doze_lbm_dbv_level);
			}
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DOZE_LBM);
			if (rc) {
				DISP_ERROR("[%s] failed to send MI_DOZE_LBM_NOLP cmd, rc=%d\n",
					panel->type, rc);
			}
			panel->mi_cfg.panel_state = PANEL_STATE_DOZE_LOW;
			mi_cfg->aod_to_normal_statue = false;
			break;
		default:
			break;
		}
	}

	return rc;
}

bool mi_dsi_panel_is_need_tx_cmd(u32 feature_id)
{
	switch (feature_id) {
	case DISP_FEATURE_SENSOR_LUX:
	case DISP_FEATURE_LOW_BRIGHTNESS_FOD:
	case DISP_FEATURE_FP_STATUS:
	case DISP_FEATURE_FOLD_STATUS:
	case DISP_FEATURE_POWERSTATUS:
	case DISP_FEATURE_SYSTEM_BUILD_VERSION:
	case DISP_FEATURE_FRAME_DROP_COUNT:
		return false;
	default:
		return true;
	}
}

int mi_dsi_panel_parse_peak_gamma_config(struct dsi_panel *panel,
		struct dsi_display_mode *mode)
{
	int rc;
	struct dsi_display_mode_priv_info *priv_info;
	struct dsi_parser_utils *utils = &panel->utils;
	int gamma_offset[75] = {0};

	priv_info = mode->priv_info;

	if (mi_get_panel_id_by_dsi_panel(panel) == N11_PANEL_PA) {
		DISP_INFO("N11 panel parse peak gamma config\n");
		/*read peak 4000nit 60hz gamma offset*/
		memset(gamma_offset, 0, sizeof(gamma_offset));
		rc = utils->read_u32_array(utils->data, "mi,mdss-dsi-panel-peak-hdr-gamma-value",
				gamma_offset, 75);
		if (rc)
			DISP_ERROR("mi,mdss-dsi-panel-peak-hdr-gamma-value not defined rc=%d\n", rc);
		else
			memcpy(priv_info->gamma_offset, gamma_offset, sizeof(gamma_offset));

		/*read peak 4000nit 120hz gamma offset*/
		memset(gamma_offset, 0, sizeof(gamma_offset));
		rc = utils->read_u32_array(utils->data, "mi,mdss-dsi-panel-peak4000-120hz-gamma-value",
				gamma_offset, 75);
		if (rc)
			DISP_ERROR("mi,mdss-dsi-panel-peak4000-120hz-gamma-value not defined rc=%d\n", rc);
		else
			memcpy(priv_info->gamma_offset_peak4000_120hz, gamma_offset, sizeof(gamma_offset));

		/*read peak 3600nit 60hz gamma offset*/
		memset(gamma_offset, 0, sizeof(gamma_offset));
		rc = utils->read_u32_array(utils->data, "mi,mdss-dsi-panel-peak3600-60hz-gamma-value",
				gamma_offset, 75);
		if (rc)
			DISP_ERROR("mi,mdss-dsi-panel-peak3600-60hz-gamma-value not defined rc=%d\n", rc);
		else
			memcpy(priv_info->gamma_offset_peak3600_60hz, gamma_offset, sizeof(gamma_offset));

		/*read peak 3600nit 120hz gamma offset*/
		memset(gamma_offset, 0, sizeof(gamma_offset));
		rc = utils->read_u32_array(utils->data, "mi,mdss-dsi-panel-peak3600-120hz-gamma-value",
				gamma_offset, 75);
		if (rc)
			DISP_ERROR("mi,mdss-dsi-panel-peak3600-120hz-gamma-value not defined rc=%d\n", rc);
		else
			memcpy(priv_info->gamma_offset_peak3600_120hz, gamma_offset, sizeof(gamma_offset));
	} else {
		rc = utils->read_u32_array(utils->data, "mi,mdss-dsi-panel-peak-hdr-gamma-value",
				gamma_offset, 75);
		if (rc)
			DISP_ERROR("mi,mdss-flat-status-control-gamma-cfg not defined rc=%d\n", rc);
		else
			memcpy(priv_info->gamma_offset, gamma_offset, sizeof(gamma_offset));
	}


	return 0;
}

int mi_dsi_panel_read_peak_hdr_gamma(struct dsi_panel *panel)
{
	struct gamma_cfg *gamma_cfg;
	struct dsi_display_mode_priv_info *priv_info = NULL;
	u8 gamma_reg_data[18] = {0};
	u8 gamma_data[150];
	u8 peak_gamma_reg[9] = {0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8};
	unsigned long mode_flags_backup = 0;
	int i = 0;
	int rc = 0;
	int rd_length = 0;
	int rd_length_num1 = 0;
	int rd_length_num2 = 0;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}
	mutex_lock(&panel->panel_lock);

	DISP_INFO("[%s] read peak hdr gamma start\n", panel->type);

	gamma_cfg = &panel->mi_cfg.gamma_cfg;
	mode_flags_backup = panel->mipi_device.mode_flags;
	panel->mipi_device.mode_flags |= MIPI_DSI_MODE_LPM;
	priv_info = panel->cur_mode->priv_info;

	memset(gamma_data, 0, sizeof(gamma_data));
	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_PEAK_GAMMA_READ_PRE);
	if (rc) {
		DISP_ERROR("[%s] pre read peak hdr gamma  send cmd failed, rc=%d\n", panel->type, rc);
		goto exit;
	}

	for(i = 0; i < 9; i++){
		if(i % 3 != 2)
			rd_length = 18;
		else
			rd_length = 14;
		memset(gamma_reg_data, 0, sizeof(gamma_reg_data));
		rc = mipi_dsi_dcs_read(&panel->mipi_device, peak_gamma_reg[i], gamma_reg_data, rd_length);

		memcpy(&gamma_data[18 * rd_length_num1 + rd_length_num2 * 14], gamma_reg_data, rd_length);

		if(rd_length == 18)
			rd_length_num1++;
		else
			rd_length_num2++;
		if (rc < 0 || rc != rd_length) {
			DISP_ERROR("[%s] read  reg  failed, rc=%d\n", panel->type, rc);
			goto exit;
		}
	}

	panel->mipi_device.mode_flags = mode_flags_backup;
	gamma_cfg->read_done = true;

	for(i = 0; i < 75; i++){
		int gamma_data_half = (int)(gamma_data[i*2] << 8 | gamma_data[i*2 + 1]) + priv_info->gamma_offset[i];
		if(i < 9){
			gamma_cfg->peak_reg.reg_B0[i * 2] = (gamma_data_half>> 8) & 0xFF;
			gamma_cfg->peak_reg.reg_B0[i * 2 + 1] = gamma_data_half & 0xFF;
		}
		else if(i >= 9 && i< 18){
			gamma_cfg->peak_reg.reg_B1[(i - 9) * 2] = (gamma_data_half>> 8) & 0xFF;
			gamma_cfg->peak_reg.reg_B1[(i - 9) * 2 + 1] = gamma_data_half & 0xFF;
		}
		else if(i >= 18 && i < 25){
			gamma_cfg->peak_reg.reg_B2[(i -18 ) * 2] = (gamma_data_half>> 8) & 0xFF;
			gamma_cfg->peak_reg.reg_B2[(i - 18) * 2 + 1] = gamma_data_half & 0xFF;
		}
		else if(i >= 25 && i < 34){
			gamma_cfg->peak_reg.reg_B3[(i - 25) *2 ] = (gamma_data_half>> 8) & 0xFF;
			gamma_cfg->peak_reg.reg_B3[(i - 25) *2 + 1] = gamma_data_half & 0xFF;
		}
		else if(i >= 34 && i< 43){
			gamma_cfg->peak_reg.reg_B4[(i - 34) * 2] = (gamma_data_half>> 8) & 0xFF;
			gamma_cfg->peak_reg.reg_B4[(i - 34) * 2 + 1] = gamma_data_half & 0xFF;
		}
		else if(i >= 43 && i < 50){
			gamma_cfg->peak_reg.reg_B5[(i - 43) * 2] = (gamma_data_half>> 8) & 0xFF;
			gamma_cfg->peak_reg.reg_B5[(i - 43) * 2 + 1] = gamma_data_half & 0xFF;
		}
		else if(i >= 50 && i < 59){
			gamma_cfg->peak_reg.reg_B6[(i - 50) * 2] = (gamma_data_half>> 8) & 0xFF;
			gamma_cfg->peak_reg.reg_B6[(i - 50) * 2 + 1] = gamma_data_half & 0xFF;
		}
		else if(i >= 59 && i < 68){
			gamma_cfg->peak_reg.reg_B7[(i - 59) * 2] = (gamma_data_half>> 8) & 0xFF;
			gamma_cfg->peak_reg.reg_B7[(i - 59) * 2+ 1] = gamma_data_half & 0xFF;
		}
		else if(i >= 68 && i < 75){
			gamma_cfg->peak_reg.reg_B8[(i - 68) * 2] = (gamma_data_half>> 8) & 0xFF;
			gamma_cfg->peak_reg.reg_B8[(i - 68) * 2 + 1] = gamma_data_half & 0xFF;
		}
	}
	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_PEAK_GAMMA_READ_PRE_DISABLE);
	if (rc) {
		DISP_ERROR("[%s] disable pre read peak hdr gamma  send cmd failed, rc=%d\n", panel->type, rc);
		goto exit;
	}
exit:
	mutex_unlock(&panel->panel_lock);
	return rc;
}

int mi_dsi_panel_read_peak_hdr_gamma_N11(struct dsi_panel *panel)
{
	struct gamma_cfg *gamma_cfg;
	struct dsi_display_mode_priv_info *priv_info = NULL;
	struct dsi_cmd_update_info *info = NULL;
	u8 gamma_reg_data[18] = {0};
	u8 gamma_data[150];  /*use for 120hz GIR ON APL GAMMA,read 0xBF 0x0B*/
	u8 gamma_data1[150]; /*use for  60hz GIR ON APL GAMMA,read 0xBF 0x1B*/
	u8 peak_gamma_reg[9] = {0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8};
	unsigned long mode_flags_backup = 0;
	int i = 0;
	int rc = 0;
	int rd_length = 0;
	int rd_length_num1 = 0;
	int rd_length_num2 = 0;
	/*0:no offset; 1: 3600nit offset; 2:4000nit offset*/
	int offset_flags = 2;
	/*60hz r,g,b 120hz r,g,b*/
	int reg_rgb[6] = {0};
	/*[0][6]4000nit 60/120hz rgb, [1][6]3600nit 60/120hz rgb*/
	int offset[2][6] = {0};
	int gamma_data_half = 0, gamma_data_half1 = 0;
	u32 cmd_update_index = 0;
	u8 reg_BF = 0x1B;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	if (mi_get_panel_id_by_dsi_panel(panel) != N11_PANEL_PA) {
		DISP_ERROR("only support N11 panel\n");
		return -EINVAL;
	}

	DISP_INFO("[%s] N11  read peak hdr gamma start\n", panel->type);

	mutex_lock(&panel->panel_lock);

	gamma_cfg = &panel->mi_cfg.gamma_cfg;
	mode_flags_backup = panel->mipi_device.mode_flags;
	panel->mipi_device.mode_flags |= MIPI_DSI_MODE_LPM;
	priv_info = panel->cur_mode->priv_info;

	memset(gamma_data, 0, sizeof(gamma_data));
	memset(gamma_data1, 0, sizeof(gamma_data1));
	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_PEAK_GAMMA_READ_PRE);
	if (rc) {
		DISP_ERROR("[%s] pre read peak hdr gamma  send cmd failed, rc=%d\n", panel->type, rc);
		goto exit;
	}

	for(i = 0; i < 9; i++){
		if(i % 3 != 2)
			rd_length = 18;
		else
			rd_length = 14;
		memset(gamma_reg_data, 0, sizeof(gamma_reg_data));
		rc = mipi_dsi_dcs_read(&panel->mipi_device, peak_gamma_reg[i], gamma_reg_data, rd_length);
		memcpy(&gamma_data[18 * rd_length_num1 + rd_length_num2 * 14], gamma_reg_data, rd_length);

/*
		for (j = 0; j < rd_length; j++) {
			DISP_INFO("GAMMAREG:0x%02x  j:%d value:%02x ",peak_gamma_reg[i],j,gamma_reg_data[j]);
		}
*/
		if(rd_length == 18)
			rd_length_num1++;
		else
			rd_length_num2++;
		if (rc < 0 || rc != rd_length) {
			DISP_ERROR("[%s] read  reg  failed, rc=%d\n", panel->type, rc);
			goto exit;
		}
	}

	/*read 0xB1*/
	cmd_update_index = DSI_CMD_SET_MI_PEAK_GAMMA_READ_PRE_COMMAND_UPDATE;
	info = priv_info->cmd_update[cmd_update_index];
	if (info && info->mipi_address == 0xBF) {
		DISP_INFO("update 0x1b\n");
		mi_dsi_panel_update_cmd_set(panel, panel->cur_mode,
			DSI_CMD_SET_MI_PEAK_GAMMA_READ_PRE, info, &reg_BF, sizeof(reg_BF));
	}

	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_PEAK_GAMMA_READ_PRE);
	if (rc) {
		DISP_ERROR("[%s] pre read peak hdr gamma  send cmd failed, rc=%d\n", panel->type, rc);
		goto exit;
	}

	rd_length_num1 = 0;
	rd_length_num2 = 0;
	for(i = 0; i < 9; i++){
		if(i % 3 != 2)
			rd_length = 18;
		else
			rd_length = 14;
		memset(gamma_reg_data, 0, sizeof(gamma_reg_data));
		rc = mipi_dsi_dcs_read(&panel->mipi_device, peak_gamma_reg[i], gamma_reg_data, rd_length);
		memcpy(&gamma_data1[18 * rd_length_num1 + rd_length_num2 * 14], gamma_reg_data, rd_length);

/*
		for (j =0; j < rd_length; j++) {
			DISP_INFO("GAMMAREG:0x%02x  j:%d value:%02x ",peak_gamma_reg[i],j,gamma_reg_data[j]);
		}
*/
		if(rd_length == 18)
			rd_length_num1++;
		else
			rd_length_num2++;
		if (rc < 0 || rc != rd_length) {
			DISP_ERROR("[%s] read  reg  failed, rc=%d\n", panel->type, rc);
			goto exit;
		}
	}

	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_PEAK_GAMMA_READ_PRE_DISABLE);
	if (rc) {
		DISP_ERROR("[%s] pre read peak hdr gamma  send cmd failed, rc=%d\n", panel->type, rc);
		goto exit;
	}
	panel->mipi_device.mode_flags = mode_flags_backup;
	gamma_cfg->read_done = true;

	/*determine 3600nit offset or 4000nit offset*/
	/*120hz gir on,read 0x0b*/
	reg_rgb[0] = (int)(gamma_data[48] << 8 | gamma_data[49]);
	reg_rgb[1] = (int)(gamma_data[98] << 8 | gamma_data[99]);
	reg_rgb[2] = (int)(gamma_data[148] << 8 | gamma_data[149]);
	/*60hz gir on, read 0x1b*/
	reg_rgb[3] = (int)(gamma_data1[48] << 8 | gamma_data1[49]);
	reg_rgb[4] = (int)(gamma_data1[98] << 8 | gamma_data1[99]);
	reg_rgb[5] = (int)(gamma_data1[148] << 8 | gamma_data1[149]);

	/*offset of peak4000nit 120hz*/
	offset[0][0] = priv_info->gamma_offset_peak4000_120hz[24];
	offset[0][1] = priv_info->gamma_offset_peak4000_120hz[49];
	offset[0][2] = priv_info->gamma_offset_peak4000_120hz[74];

	/*offset of peak4000nit 60hz*/
	offset[0][3] = priv_info->gamma_offset[24];
	offset[0][4] = priv_info->gamma_offset[49];
	offset[0][5] = priv_info->gamma_offset[74];

	/*offset of peak3600nit 120hz*/
	offset[1][0] = priv_info->gamma_offset_peak3600_120hz[24];
	offset[1][1] = priv_info->gamma_offset_peak3600_120hz[49];
	offset[1][2] = priv_info->gamma_offset_peak3600_120hz[74];

	/*offset of peak3600nit 60hz*/
	offset[1][3] = priv_info->gamma_offset_peak3600_60hz[24];
	offset[1][4] = priv_info->gamma_offset_peak3600_60hz[49];
	offset[1][5] = priv_info->gamma_offset_peak3600_60hz[74];

	/*if any reg + offset > = 4095, back to 3600nit*/
	for (i = 0; i < 6; i++) {
		DISP_INFO("4000nit index:%d reg_rgb:%d offset:%d\n",i,reg_rgb[i],offset[0][i]);
		if ((reg_rgb[i] + offset[0][i]) >= 4095) {
			offset_flags = 1;
			break;
		}
	}

	if (offset_flags == 1) {
		for (i = 0; i < 6; i++) {
			DISP_INFO("3600nit index:%d reg_rgb:%d offset1:%d\n",i,reg_rgb[i],offset[1][i]);
			if ((reg_rgb[i] + offset[1][i]) >= 4095) {
				offset_flags = 0;
				break;
			}
		}
	}

	DISP_INFO("offset_flags:%d\n",offset_flags);

	/*peak_reg:0x0B 120hz, peak_reg1:0x1b 60hz*/
	for(i = 0; i < 75; i++){
		if (offset_flags == 2) {
			gamma_data_half  = (int)(gamma_data[i*2] << 8 | gamma_data[i*2 + 1]) + priv_info->gamma_offset_peak4000_120hz[i];
			gamma_data_half1 = (int)(gamma_data1[i*2] << 8 | gamma_data1[i*2 + 1]) + priv_info->gamma_offset[i];
		} else if (offset_flags == 1) {
			gamma_data_half  = (int)(gamma_data[i*2] << 8 | gamma_data[i*2 + 1]) + priv_info->gamma_offset_peak3600_120hz[i];
			gamma_data_half1 = (int)(gamma_data1[i*2] << 8 | gamma_data1[i*2 + 1]) + priv_info->gamma_offset_peak3600_60hz[i];
		} else {
			gamma_data_half  = (int)(gamma_data[i*2] << 8 | gamma_data[i*2 + 1]);
			gamma_data_half1 = (int)(gamma_data1[i*2] << 8 | gamma_data1[i*2 + 1]);
		}
		DISP_DEBUG("gamma_data_half:%02x %02x gamma_data_half1:%02x %02x\n",
			(gamma_data_half >> 8) & 0xFF ,gamma_data_half & 0xFF, (gamma_data_half1 >> 8) & 0xFF ,gamma_data_half1 & 0xFF);

		if(i < 9){
			gamma_cfg->peak_reg.reg_B0[i * 2] = (gamma_data_half>> 8) & 0xFF;
			gamma_cfg->peak_reg.reg_B0[i * 2 + 1] = gamma_data_half & 0xFF;
			gamma_cfg->peak_reg1.reg_B0[i * 2] = (gamma_data_half1>> 8) & 0xFF;
			gamma_cfg->peak_reg1.reg_B0[i * 2 + 1] = gamma_data_half1 & 0xFF;
		}
		else if(i >= 9 && i < 18){
			gamma_cfg->peak_reg.reg_B1[(i - 9) * 2] = (gamma_data_half >> 8) & 0xFF;
			gamma_cfg->peak_reg.reg_B1[(i - 9) * 2 + 1] = gamma_data_half & 0xFF;
			gamma_cfg->peak_reg1.reg_B1[(i - 9) * 2] = (gamma_data_half1 >> 8) & 0xFF;
			gamma_cfg->peak_reg1.reg_B1[(i - 9) * 2 + 1] = gamma_data_half1 & 0xFF;
		}
		else if(i >= 18 && i < 25){
			gamma_cfg->peak_reg.reg_B2[(i -18 ) * 2] = (gamma_data_half >> 8) & 0xFF;
			gamma_cfg->peak_reg.reg_B2[(i - 18) * 2 + 1] = gamma_data_half & 0xFF;
			gamma_cfg->peak_reg1.reg_B2[(i -18 ) * 2] = (gamma_data_half1 >> 8) & 0xFF;
			gamma_cfg->peak_reg1.reg_B2[(i - 18) * 2 + 1] = gamma_data_half1 & 0xFF;
		}
		else if(i >= 25 && i < 34){
			gamma_cfg->peak_reg.reg_B3[(i - 25) *2 ] = (gamma_data_half >> 8) & 0xFF;
			gamma_cfg->peak_reg.reg_B3[(i - 25) *2 + 1] = gamma_data_half & 0xFF;
			gamma_cfg->peak_reg1.reg_B3[(i - 25) *2 ] = (gamma_data_half1 >> 8) & 0xFF;
			gamma_cfg->peak_reg1.reg_B3[(i - 25) *2 + 1] = gamma_data_half1 & 0xFF;
		}
		else if(i >= 34 && i < 43){
			gamma_cfg->peak_reg.reg_B4[(i - 34) * 2] = (gamma_data_half >> 8) & 0xFF;
			gamma_cfg->peak_reg.reg_B4[(i - 34) * 2 + 1] = gamma_data_half & 0xFF;
			gamma_cfg->peak_reg1.reg_B4[(i - 34) * 2] = (gamma_data_half1 >> 8) & 0xFF;
			gamma_cfg->peak_reg1.reg_B4[(i - 34) * 2 + 1] = gamma_data_half1 & 0xFF;
		}
		else if(i >= 43 && i < 50){
			gamma_cfg->peak_reg.reg_B5[(i - 43) * 2] = (gamma_data_half >> 8) & 0xFF;
			gamma_cfg->peak_reg.reg_B5[(i - 43) * 2 + 1] = gamma_data_half & 0xFF;
			gamma_cfg->peak_reg1.reg_B5[(i - 43) * 2] = (gamma_data_half1 >> 8) & 0xFF;
			gamma_cfg->peak_reg1.reg_B5[(i - 43) * 2 + 1] = gamma_data_half1 & 0xFF;
		}
		else if(i >= 50 && i < 59){
			gamma_cfg->peak_reg.reg_B6[(i - 50) * 2] = (gamma_data_half >> 8) & 0xFF;
			gamma_cfg->peak_reg.reg_B6[(i - 50) * 2 + 1] = gamma_data_half & 0xFF;
			gamma_cfg->peak_reg1.reg_B6[(i - 50) * 2] = (gamma_data_half1 >> 8) & 0xFF;
			gamma_cfg->peak_reg1.reg_B6[(i - 50) * 2 + 1] = gamma_data_half1 & 0xFF;
		}
		else if(i >= 59 && i < 68){
			gamma_cfg->peak_reg.reg_B7[(i - 59) * 2] = (gamma_data_half >> 8) & 0xFF;
			gamma_cfg->peak_reg.reg_B7[(i - 59) * 2+ 1] = gamma_data_half & 0xFF;
			gamma_cfg->peak_reg1.reg_B7[(i - 59) * 2] = (gamma_data_half1 >> 8) & 0xFF;
			gamma_cfg->peak_reg1.reg_B7[(i - 59) * 2+ 1] = gamma_data_half1 & 0xFF;
		}
		else if(i >= 68 && i < 75){
			gamma_cfg->peak_reg.reg_B8[(i - 68) * 2] = (gamma_data_half >> 8) & 0xFF;
			gamma_cfg->peak_reg.reg_B8[(i - 68) * 2 + 1] = gamma_data_half & 0xFF;
			gamma_cfg->peak_reg1.reg_B8[(i - 68) * 2] = (gamma_data_half1 >> 8) & 0xFF;
			gamma_cfg->peak_reg1.reg_B8[(i - 68) * 2 + 1] = gamma_data_half1 & 0xFF;
		}
	}

exit:
	mutex_unlock(&panel->panel_lock);
	return rc;
}

int mi_dsi_panel_cal_send_peak_hdr_gamma(struct dsi_panel *panel)
{

	struct dsi_cmd_update_info *info = NULL;
	struct dsi_display_mode *cur_mode = NULL;
	int rc = 0;
	int i = 0;
	struct gamma_cfg *gamma_cfg;
	u32 cmd_update_index = 0;
	u32 cmd_update_count = 0;

	if (!panel || !panel->cur_mode || !panel->cur_mode->priv_info) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	cur_mode = panel->cur_mode;

	gamma_cfg = &panel->mi_cfg.gamma_cfg;

	cmd_update_index = DSI_CMD_SET_MI_PEAK_GAMMA_COMMAND_UPDATE;
	cmd_update_count = cur_mode->priv_info->cmd_update_count[cmd_update_index];
	info = cur_mode->priv_info->cmd_update[cmd_update_index];
	for(i = 0; i < cmd_update_count; i++){
		if (info && info->mipi_address == 0xB0)
			mi_dsi_panel_update_cmd_set(panel, cur_mode,
					DSI_CMD_SET_MI_PEAK_GAMMA, info,
					gamma_cfg->peak_reg.reg_B0, sizeof(gamma_cfg->peak_reg.reg_B0));
		else if (info && info->mipi_address == 0xB1)
			mi_dsi_panel_update_cmd_set(panel, cur_mode,
					DSI_CMD_SET_MI_PEAK_GAMMA, info,
					gamma_cfg->peak_reg.reg_B1, sizeof(gamma_cfg->peak_reg.reg_B1));
		else if (info && info->mipi_address == 0xB2)
			mi_dsi_panel_update_cmd_set(panel, cur_mode,
					DSI_CMD_SET_MI_PEAK_GAMMA, info,
					gamma_cfg->peak_reg.reg_B2, sizeof(gamma_cfg->peak_reg.reg_B2));
		else if (info && info->mipi_address == 0xB3)
			mi_dsi_panel_update_cmd_set(panel, cur_mode,
					DSI_CMD_SET_MI_PEAK_GAMMA, info,
					gamma_cfg->peak_reg.reg_B3, sizeof(gamma_cfg->peak_reg.reg_B3));
		else if (info && info->mipi_address == 0xB4)
			mi_dsi_panel_update_cmd_set(panel, cur_mode,
					DSI_CMD_SET_MI_PEAK_GAMMA, info,
					gamma_cfg->peak_reg.reg_B4, sizeof(gamma_cfg->peak_reg.reg_B4));
		else if (info && info->mipi_address == 0xB5)
			mi_dsi_panel_update_cmd_set(panel, cur_mode,
					DSI_CMD_SET_MI_PEAK_GAMMA, info,
					gamma_cfg->peak_reg.reg_B5, sizeof(gamma_cfg->peak_reg.reg_B5));
		else if (info && info->mipi_address == 0xB6)
			mi_dsi_panel_update_cmd_set(panel, cur_mode,
					DSI_CMD_SET_MI_PEAK_GAMMA, info,
					gamma_cfg->peak_reg.reg_B6, sizeof(gamma_cfg->peak_reg.reg_B6));
		else if (info && info->mipi_address == 0xB7)
			mi_dsi_panel_update_cmd_set(panel, cur_mode,
					DSI_CMD_SET_MI_PEAK_GAMMA, info,
					gamma_cfg->peak_reg.reg_B7, sizeof(gamma_cfg->peak_reg.reg_B7));
		else if (info && info->mipi_address == 0xB8)
			mi_dsi_panel_update_cmd_set(panel, cur_mode,
					DSI_CMD_SET_MI_PEAK_GAMMA, info,
					gamma_cfg->peak_reg.reg_B8, sizeof(gamma_cfg->peak_reg.reg_B8));
		info++;
	}

	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_PEAK_GAMMA_READ_PRE);
	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_PEAK_GAMMA);
	if (rc < 0) {
		DISP_ERROR("[%s] wirte reg faild, rc=%d\n", panel->type, rc);
	}
	gamma_cfg->update_done = true;

	return rc;
}

int mi_dsi_panel_cal_update_peak_hdr_gamma_N11(struct dsi_panel *panel)
{

	struct dsi_cmd_update_info *info = NULL;
	struct dsi_display_mode *cur_mode = NULL;
	int i = 0;
	struct gamma_cfg *gamma_cfg;
	u32 cmd_update_index = 0;
	u32 cmd_update_count = 0;

	if (!panel || !panel->cur_mode || !panel->cur_mode->priv_info) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	if (mi_get_panel_id_by_dsi_panel(panel) != N11_PANEL_PA) {
		DISP_ERROR("only support N11 panel\n");
		return -EINVAL;
	}

	cur_mode = panel->cur_mode;
	gamma_cfg = &panel->mi_cfg.gamma_cfg;

	if (!gamma_cfg->read_done) {
		DISP_ERROR("gamma reg read error,not update cmds\n");
		return -EINVAL;
	}

	cmd_update_index = DSI_CMD_SET_MI_PEAK_GAMMA_COMMAND_UPDATE;
	cmd_update_count = cur_mode->priv_info->cmd_update_count[cmd_update_index];
	info = cur_mode->priv_info->cmd_update[cmd_update_index];

	DISP_INFO("%s begin ++++\n", __func__);

	for(i = 0; i < cmd_update_count; i++){
		if ((info->index >= 4 && info->index <= 12) ||(info->index>=24 && info->index <= 32)) {
			if (info && info->mipi_address == 0xB0)
				mi_dsi_panel_update_cmd_set(panel, cur_mode,
						DSI_CMD_SET_MI_PEAK_GAMMA, info, gamma_cfg->peak_reg.reg_B0, sizeof(gamma_cfg->peak_reg.reg_B0));
			else if (info && info->mipi_address == 0xB1)
				mi_dsi_panel_update_cmd_set(panel, cur_mode,
						DSI_CMD_SET_MI_PEAK_GAMMA, info, gamma_cfg->peak_reg.reg_B1, sizeof(gamma_cfg->peak_reg.reg_B1));
			else if (info && info->mipi_address == 0xB2)
				mi_dsi_panel_update_cmd_set(panel, cur_mode,
						DSI_CMD_SET_MI_PEAK_GAMMA, info, gamma_cfg->peak_reg.reg_B2, sizeof(gamma_cfg->peak_reg.reg_B2));
			else if (info && info->mipi_address == 0xB3)
				mi_dsi_panel_update_cmd_set(panel, cur_mode,
						DSI_CMD_SET_MI_PEAK_GAMMA, info, gamma_cfg->peak_reg.reg_B3, sizeof(gamma_cfg->peak_reg.reg_B3));
			else if (info && info->mipi_address == 0xB4)
				mi_dsi_panel_update_cmd_set(panel, cur_mode,
						DSI_CMD_SET_MI_PEAK_GAMMA, info, gamma_cfg->peak_reg.reg_B4, sizeof(gamma_cfg->peak_reg.reg_B4));
			else if (info && info->mipi_address == 0xB5)
				mi_dsi_panel_update_cmd_set(panel, cur_mode,
						DSI_CMD_SET_MI_PEAK_GAMMA, info, gamma_cfg->peak_reg.reg_B5, sizeof(gamma_cfg->peak_reg.reg_B5));
			else if (info && info->mipi_address == 0xB6)
				mi_dsi_panel_update_cmd_set(panel, cur_mode,
						DSI_CMD_SET_MI_PEAK_GAMMA, info, gamma_cfg->peak_reg.reg_B6, sizeof(gamma_cfg->peak_reg.reg_B6));
			else if (info && info->mipi_address == 0xB7)
				mi_dsi_panel_update_cmd_set(panel, cur_mode,
						DSI_CMD_SET_MI_PEAK_GAMMA, info, gamma_cfg->peak_reg.reg_B7, sizeof(gamma_cfg->peak_reg.reg_B7));
			else if (info && info->mipi_address == 0xB8)
				mi_dsi_panel_update_cmd_set(panel, cur_mode,
						DSI_CMD_SET_MI_PEAK_GAMMA, info, gamma_cfg->peak_reg.reg_B8, sizeof(gamma_cfg->peak_reg.reg_B8));
		} else if ((info->index >= 14 && info->index <= 22) || (info->index>=34 && info->index <= 42)) {
			if (info && info->mipi_address == 0xB0)
				mi_dsi_panel_update_cmd_set(panel, cur_mode,
						DSI_CMD_SET_MI_PEAK_GAMMA, info, gamma_cfg->peak_reg1.reg_B0, sizeof(gamma_cfg->peak_reg1.reg_B0));
			else if (info && info->mipi_address == 0xB1)
				mi_dsi_panel_update_cmd_set(panel, cur_mode,
						DSI_CMD_SET_MI_PEAK_GAMMA, info, gamma_cfg->peak_reg1.reg_B1, sizeof(gamma_cfg->peak_reg1.reg_B1));
			else if (info && info->mipi_address == 0xB2)
				mi_dsi_panel_update_cmd_set(panel, cur_mode,
						DSI_CMD_SET_MI_PEAK_GAMMA, info, gamma_cfg->peak_reg1.reg_B2, sizeof(gamma_cfg->peak_reg1.reg_B2));
			else if (info && info->mipi_address == 0xB3)
				mi_dsi_panel_update_cmd_set(panel, cur_mode,
						DSI_CMD_SET_MI_PEAK_GAMMA, info, gamma_cfg->peak_reg1.reg_B3, sizeof(gamma_cfg->peak_reg1.reg_B3));
			else if (info && info->mipi_address == 0xB4)
				mi_dsi_panel_update_cmd_set(panel, cur_mode,
						DSI_CMD_SET_MI_PEAK_GAMMA, info, gamma_cfg->peak_reg1.reg_B4, sizeof(gamma_cfg->peak_reg1.reg_B4));
			else if (info && info->mipi_address == 0xB5)
				mi_dsi_panel_update_cmd_set(panel, cur_mode,
						DSI_CMD_SET_MI_PEAK_GAMMA, info, gamma_cfg->peak_reg1.reg_B5, sizeof(gamma_cfg->peak_reg1.reg_B5));
			else if (info && info->mipi_address == 0xB6)
				mi_dsi_panel_update_cmd_set(panel, cur_mode,
						DSI_CMD_SET_MI_PEAK_GAMMA, info, gamma_cfg->peak_reg1.reg_B6, sizeof(gamma_cfg->peak_reg1.reg_B6));
			else if (info && info->mipi_address == 0xB7)
				mi_dsi_panel_update_cmd_set(panel, cur_mode,
						DSI_CMD_SET_MI_PEAK_GAMMA, info, gamma_cfg->peak_reg1.reg_B7, sizeof(gamma_cfg->peak_reg1.reg_B7));
			else if (info && info->mipi_address == 0xB8)
				mi_dsi_panel_update_cmd_set(panel, cur_mode,
						DSI_CMD_SET_MI_PEAK_GAMMA, info, gamma_cfg->peak_reg1.reg_B8, sizeof(gamma_cfg->peak_reg1.reg_B8));
		}
		info++;
	}

	gamma_cfg->update_done = true;
	DISP_INFO("%s end ---\n", __func__);
	return 0;
}

int mi_dsi_panel_set_disp_param(struct dsi_panel *panel, struct disp_feature_ctl *ctl)
{
	int rc = 0;
	struct mi_dsi_panel_cfg *mi_cfg = NULL;

	if (!panel || !ctl) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);

	DISP_TIME_INFO("[%s] feature: %s, value: %d\n", panel->type,
			get_disp_feature_id_name(ctl->feature_id), ctl->feature_val);


	mi_cfg = &panel->mi_cfg;

	if (!panel->panel_initialized &&
		mi_dsi_panel_is_need_tx_cmd(ctl->feature_id)) {
		if (ctl->feature_id == DISP_FEATURE_DC)
			mi_cfg->feature_val[DISP_FEATURE_DC] = ctl->feature_val;
		if (ctl->feature_id == DISP_FEATURE_DBI)
			mi_cfg->feature_val[DISP_FEATURE_DBI] = ctl->feature_val;
		DISP_WARN("[%s] panel not initialized!\n", panel->type);
		rc = -ENODEV;
		goto exit;
	}


	switch (ctl->feature_id) {
	case DISP_FEATURE_DIMMING:
		if (!mi_cfg->disable_ic_dimming){
			if(panel->power_mode == SDE_MODE_DPMS_LP1 ||
				panel->power_mode == SDE_MODE_DPMS_LP2) {
				if (mi_get_panel_id(panel->mi_cfg.mi_panel_id) == M18_PANEL_PA ||
					mi_get_panel_id(panel->mi_cfg.mi_panel_id) == M18_PANEL_SA) {
					DISP_INFO("doze skip dimming %s\n", ctl->feature_val ? "on" : "off");
					goto exit;
				}
			}

			if (mi_cfg->dimming_state != STATE_DIM_BLOCK) {
				if (ctl->feature_val == FEATURE_ON )
					rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DIMMINGON);
				else
					rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DIMMINGOFF);
				mi_cfg->feature_val[DISP_FEATURE_DIMMING] = ctl->feature_val;
			} else {
				DISP_INFO("skip dimming %s\n", ctl->feature_val ? "on" : "off");
			}
		} else{
			DISP_INFO("disable_ic_dimming is %d\n", mi_cfg->disable_ic_dimming);
		}
		break;
	case DISP_FEATURE_HBM:
		mi_cfg->feature_val[DISP_FEATURE_HBM] = ctl->feature_val;
#ifdef DISPLAY_FACTORY_BUILD
		if (ctl->feature_val == FEATURE_ON) {
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_HBM_FOD_ON);
			mi_cfg->dimming_state = STATE_DIM_BLOCK;
		} else {
			mi_dsi_update_51_mipi_cmd(panel, DSI_CMD_SET_MI_HBM_FOD_OFF,
					mi_cfg->last_bl_level);
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_HBM_FOD_OFF);
			mi_cfg->dimming_state = STATE_DIM_RESTORE;
			if (mi_cfg->disable_ic_dimming) {
				rc  |= dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DIMMINGOFF);
			}
		}
#else
		if (mi_get_panel_id_by_dsi_panel(panel) == M2_PANEL_PA ||
			mi_get_panel_id_by_dsi_panel(panel) == M3_PANEL_PA) {
			if (ctl->feature_val == FEATURE_ON) {
				dsi_panel_send_em_cycle_setting(panel,2047);
				mi_dsi_update_hbm_cmd_53reg(panel,DSI_CMD_SET_MI_HBM_ON);
				panel->mi_cfg.last_bl_level = 2047;
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_HBM_ON);
				mi_cfg->dimming_state = STATE_DIM_BLOCK;
			} else {
				panel->mi_cfg.last_bl_level = 2047;
				mi_dsi_update_51_mipi_cmd(panel, DSI_CMD_SET_MI_HBM_OFF, 2047);
				mi_dsi_update_hbm_cmd_53reg(panel,DSI_CMD_SET_MI_HBM_OFF);
				rc  = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_HBM_OFF);
				mi_cfg->dimming_state = STATE_DIM_RESTORE;
			}
		}
#endif
		mi_disp_feature_event_notify_by_type(mi_get_disp_id(panel->type),
				MI_DISP_EVENT_HBM, sizeof(ctl->feature_val), ctl->feature_val);
		break;
	case DISP_FEATURE_HBM_FOD:
		if (ctl->feature_val == FEATURE_ON) {
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_HBM_FOD_ON);
			mi_cfg->dimming_state = STATE_DIM_BLOCK;
		} else {
			mi_dsi_update_51_mipi_cmd(panel, DSI_CMD_SET_MI_HBM_FOD_OFF,
					mi_cfg->last_bl_level);
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_HBM_FOD_OFF);
			mi_cfg->dimming_state = STATE_DIM_RESTORE;
		}
		mi_cfg->feature_val[DISP_FEATURE_HBM_FOD] = ctl->feature_val;
		break;
	case DISP_FEATURE_DOZE_BRIGHTNESS:
#ifdef DISPLAY_FACTORY_BUILD
		if (dsi_panel_initialized(panel) &&
			is_aod_brightness(ctl->feature_val)) {
			if (ctl->feature_val == DOZE_BRIGHTNESS_HBM)
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DOZE_HBM);
			else
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DOZE_LBM);
		} else {
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_NOLP);
			dsi_panel_update_backlight(panel, mi_cfg->last_bl_level);
		}
#else
		if (is_aod_and_panel_initialized(panel) &&
			is_aod_brightness(ctl->feature_val)) {
			if (ctl->feature_val == DOZE_BRIGHTNESS_HBM) {
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DOZE_HBM);
			} else {
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DOZE_LBM);
			}
		} else {
			if (!mi_dsi_panel_new_aod(panel)) {
				mi_dsi_update_nolp_cmd_B2reg(panel, DSI_CMD_SET_NOLP);
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_NOLP);
			}
		}
#endif
		mi_cfg->feature_val[DISP_FEATURE_DOZE_BRIGHTNESS] = ctl->feature_val;
		break;
	case DISP_FEATURE_FOD_CALIBRATION_BRIGHTNESS:
		if (ctl->feature_val == -1) {
			DISP_INFO("FOD calibration brightness restore last_bl_level=%d\n",
				mi_cfg->last_bl_level);
			dsi_panel_update_backlight(panel, mi_cfg->last_bl_level);
			mi_cfg->in_fod_calibration = false;
		} else {
			if (ctl->feature_val >= 0 &&
				ctl->feature_val <= panel->bl_config.bl_max_level) {
				mi_cfg->in_fod_calibration = true;
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DIMMINGOFF);
				dsi_panel_update_backlight(panel, ctl->feature_val);
				mi_cfg->dimming_state = STATE_NONE;
			} else {
				mi_cfg->in_fod_calibration = false;
				DISP_ERROR("FOD calibration invalid brightness level:%d\n",
						ctl->feature_val);
			}
		}
		mi_cfg->feature_val[DISP_FEATURE_FOD_CALIBRATION_BRIGHTNESS] = ctl->feature_val;
		break;
	case DISP_FEATURE_FOD_CALIBRATION_HBM:
		if (ctl->feature_val == -1) {
			DISP_INFO("FOD calibration HBM restore last_bl_level=%d\n",
					mi_cfg->last_bl_level);
			mi_dsi_update_51_mipi_cmd(panel, DSI_CMD_SET_MI_HBM_FOD_OFF,
					mi_cfg->last_bl_level);
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_HBM_FOD_OFF);
			mi_cfg->dimming_state = STATE_DIM_RESTORE;
			mi_cfg->in_fod_calibration = false;
		} else {
			mi_cfg->in_fod_calibration = true;
			mi_cfg->dimming_state = STATE_DIM_BLOCK;
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_HBM_FOD_ON);
		}
		mi_cfg->feature_val[DISP_FEATURE_FOD_CALIBRATION_HBM] = ctl->feature_val;
		break;
	case DISP_FEATURE_FLAT_MODE:
		if (!mi_cfg->flat_sync_te) {
			if (ctl->feature_val == FEATURE_ON) {
				DISP_INFO("flat mode on\n");

				if (mi_get_panel_id_by_dsi_panel(panel) == M1_PANEL_PA)
					mi_dsi_update_timing_switch_and_flat_mode_cmd(panel, DSI_CMD_SET_MI_FLAT_MODE_ON);

				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_FLAT_MODE_ON);
				rc |= dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_FLAT_MODE_SEC_ON);
			}
			else {
				DISP_INFO("flat mode off\n");
				if (mi_get_panel_id_by_dsi_panel(panel) == M1_PANEL_PA)
					mi_dsi_update_timing_switch_and_flat_mode_cmd(panel, DSI_CMD_SET_MI_FLAT_MODE_OFF);

				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_FLAT_MODE_OFF);
				rc |= dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_FLAT_MODE_SEC_OFF);
			}
			mi_disp_feature_event_notify_by_type(mi_get_disp_id(panel->type),
				MI_DISP_EVENT_FLAT_MODE, sizeof(ctl->feature_val), ctl->feature_val);
		} else {
			rc = mi_dsi_send_flat_sync_with_te_locked(panel,
					ctl->feature_val == FEATURE_ON);
		}
		mi_cfg->feature_val[DISP_FEATURE_FLAT_MODE] = ctl->feature_val;
		break;
	case DISP_FEATURE_CRC:
		DISP_INFO("TODO\n");
		break;
	case DISP_FEATURE_DC:
		DISP_INFO("DC mode state:%d\n", ctl->feature_val);
		if (mi_cfg->dc_feature_enable) {
			rc = mi_dsi_panel_set_dc_mode_locked(panel, ctl->feature_val == FEATURE_ON);
		}
		mi_cfg->feature_val[DISP_FEATURE_DC] = ctl->feature_val;
		mi_disp_feature_event_notify_by_type(mi_get_disp_id(panel->type),
				MI_DISP_EVENT_DC, sizeof(ctl->feature_val), ctl->feature_val);
		break;
	case DISP_FEATURE_LOCAL_HBM:
		if (mi_get_panel_id(panel->mi_cfg.mi_panel_id) == M1_PANEL_PA &&
			mi_cfg->feature_val[DISP_FEATURE_PEAK_HDR_MODE]) {
			DSI_INFO("[%s] close peak mode because lhbm change backlight\n", panel->type);
			mi_cfg->feature_val[DISP_FEATURE_PEAK_HDR_MODE] = false;
			mi_dsi_update_timing_switch_and_flat_mode_cmd(panel, DSI_CMD_SET_MI_FLAT_MODE_OFF);
			dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_FLAT_MODE_OFF);
		}
		rc = mi_dsi_panel_set_lhbm_fod_locked(panel, ctl);
		mi_cfg->feature_val[DISP_FEATURE_LOCAL_HBM] = ctl->feature_val;
		break;
	case DISP_FEATURE_SENSOR_LUX:
		DISP_DEBUG("DISP_FEATURE_SENSOR_LUX=%d\n", ctl->feature_val);
		mi_cfg->feature_val[DISP_FEATURE_SENSOR_LUX] = ctl->feature_val;
		break;
	case DISP_FEATURE_FP_STATUS:
		mi_cfg->feature_val[DISP_FEATURE_FP_STATUS] = ctl->feature_val;
		if (ctl->feature_val == ENROLL_STOP ||
			ctl->feature_val == AUTH_STOP ||
			ctl->feature_val == HEART_RATE_STOP) {
			if (is_aod_and_panel_initialized(panel)) {
				mi_dsi_update_backlight_in_aod(panel, true);
			}
		}
		break;
	case DISP_FEATURE_FOLD_STATUS:
		DISP_INFO("DISP_FEATURE_FOLD_STATUS=%d\n", ctl->feature_val);
		mi_cfg->feature_val[DISP_FEATURE_FOLD_STATUS] = ctl->feature_val;
		break;
	case DISP_FEATURE_NATURE_FLAT_MODE:
		DISP_INFO("TODO\n");
		break;
	case DISP_FEATURE_SPR_RENDER:
		DISP_INFO("TODO\n");
		break;
	case DISP_FEATURE_AOD_TO_NORMAL:
		rc = mi_dsi_panel_aod_to_normal_optimize_locked(panel,
				ctl->feature_val == FEATURE_ON);
		mi_cfg->feature_val[DISP_FEATURE_AOD_TO_NORMAL] = ctl->feature_val;
		break;
	case DISP_FEATURE_COLOR_INVERT:
		DISP_INFO("TODO\n");
		break;
	case DISP_FEATURE_DC_BACKLIGHT:
		DISP_INFO("TODO\n");
		break;
	case DISP_FEATURE_GIR:
		DISP_INFO("TODO\n");
		break;
	case DISP_FEATURE_DBI:
		mi_cfg->feature_val[DISP_FEATURE_DBI] = ctl->feature_val;
		DISP_DEBUG("DISP_FEATURE_DBI  feature_val:%d\n", ctl->feature_val);

		if (panel->mi_cfg.dbi_3dlut_compensate_enable) {
			rc = mi_dsi_panel_set_3dlut_temperature_compensation(panel, ctl->feature_val,false);

		} else if (panel->mi_cfg.last_bl_level < 0x84 && panel->mi_cfg.last_bl_level
				&& !is_aod_and_panel_initialized(panel)) {
			dsi_panel_update_backlight(panel, mi_cfg->last_bl_level);
		}
		break;
	case DISP_FEATURE_DDIC_ROUND_CORNER:
		DISP_INFO("DDIC round corner state:%d\n", ctl->feature_val);
		rc = mi_dsi_panel_set_round_corner_locked(panel, ctl->feature_val == FEATURE_ON);
		mi_cfg->feature_val[DISP_FEATURE_DDIC_ROUND_CORNER] = ctl->feature_val;
		break;
	case DISP_FEATURE_HBM_BACKLIGHT:
		DISP_INFO("hbm backlight:%d\n", ctl->feature_val);
		panel->mi_cfg.last_bl_level = ctl->feature_val;
		dsi_panel_update_backlight(panel, panel->mi_cfg.last_bl_level);
		break;
	case DISP_FEATURE_BACKLIGHT:
		DISP_INFO("backlight:%d\n", ctl->feature_val);
		panel->mi_cfg.last_bl_level = ctl->feature_val;
		dsi_panel_update_backlight(panel, panel->mi_cfg.last_bl_level);
		break;
	case DISP_FEATURE_PEAK_HDR_MODE:
		if (ctl->feature_val){
			DISP_INFO("peak_hdr_mode must backlight 0xFFF\n");
			dsi_panel_update_backlight(panel, PEAK_HDR_MODE_BL);
		}
		/*Gir off in M1 PEAK2600, DF send the value*/
		if(!mi_cfg->gamma_cfg.update_done){
			rc = mi_dsi_panel_cal_send_peak_hdr_gamma(panel);
			if (rc)
				DSI_ERR("write peak gamma reg failed, rc=%d\n",rc);
		}
		DISP_INFO("peak_hdr_mode:%d\n", ctl->feature_val);
		mi_cfg->feature_val[DISP_FEATURE_PEAK_HDR_MODE] = ctl->feature_val;
		if (mi_get_panel_id_by_dsi_panel(panel) == M1_PANEL_PA)
			mi_dsi_update_timing_switch_and_flat_mode_cmd(panel, DSI_CMD_SET_MI_FLAT_MODE_OFF);
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_FLAT_MODE_OFF);
		break;
	case DISP_FEATURE_POWERSTATUS:
		DISP_DEBUG("power get:%d, last power mode = %d\n", ctl->feature_val, panel->power_mode);
		break;
	case DISP_FEATURE_SYSTEM_BUILD_VERSION:
		DISP_INFO("system build version:%s\n", ctl->tx_ptr);
		break;
	case DISP_FEATURE_FRAME_DROP_COUNT:
		DISP_INFO("frame drop count:%d\n", ctl->feature_val);
		break;
	default:
		DISP_ERROR("invalid feature argument: %d\n", ctl->feature_id);
		break;
	}
exit:
	mutex_unlock(&panel->panel_lock);
	return rc;
}

int mi_dsi_panel_get_disp_param(struct dsi_panel *panel,
			struct disp_feature_ctl *ctl)
{
	struct mi_dsi_panel_cfg *mi_cfg;
	int i = 0;

	if (!panel || !ctl) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	if (!is_support_disp_feature_id(ctl->feature_id)) {
		DISP_ERROR("unsupported disp feature id\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);
	mi_cfg = &panel->mi_cfg;
	for (i = DISP_FEATURE_DIMMING; i < DISP_FEATURE_MAX; i++) {
		if (i == ctl->feature_id) {
			ctl->feature_val =  mi_cfg->feature_val[i];
			DISP_INFO("%s: %d\n", get_disp_feature_id_name(ctl->feature_id),
				ctl->feature_val);
		}
	}
	mutex_unlock(&panel->panel_lock);

	return 0;
}

ssize_t mi_dsi_panel_show_disp_param(struct dsi_panel *panel,
			char *buf, size_t size)
{
	struct mi_dsi_panel_cfg *mi_cfg;
	ssize_t count = 0;
	int i = 0;

	if (!panel || !buf || !size) {
		DISP_ERROR("invalid params\n");
		return -EAGAIN;
	}

	count = snprintf(buf, size, "%040s: feature vaule\n", "feature name[feature id]");

	mutex_lock(&panel->panel_lock);
	mi_cfg = &panel->mi_cfg;
	for (i = DISP_FEATURE_DIMMING; i < DISP_FEATURE_MAX; i++) {
		count += snprintf(buf + count, size - count, "%036s[%02d]: %d\n",
				get_disp_feature_id_name(i), i, mi_cfg->feature_val[i]);

	}
	mutex_unlock(&panel->panel_lock);

	return count;
}

int dsi_panel_parse_build_id_read_config(struct dsi_panel *panel)
{
	struct drm_panel_build_id_config *id_config;
	struct dsi_parser_utils *utils = &panel->utils;
	int rc = 0;

	if (!panel) {
		DISP_ERROR("Invalid Params\n");
		return -EINVAL;
	}

	id_config = &panel->id_config;
	id_config->build_id = 0;
	if (!id_config)
		return -EINVAL;

	rc = utils->read_u32(utils->data, "mi,panel-build-id-read-length",
                                &id_config->id_cmds_rlen);
	if (rc) {
		id_config->id_cmds_rlen = 0;
		return -EINVAL;
	}

	dsi_panel_parse_cmd_sets_sub(&id_config->id_cmd,
			DSI_CMD_SET_MI_PANEL_BUILD_ID, utils);
	if (!(id_config->id_cmd.count)) {
		DISP_ERROR("panel build id read command parsing failed\n");
		return -EINVAL;
	}

	dsi_panel_parse_cmd_sets_sub(&id_config->sub_id_cmd,
		DSI_CMD_SET_MI_PANEL_BUILD_ID_SUB_READ, utils);
	if (!(id_config->sub_id_cmd.count)) {
		DISP_ERROR("panel sub build id read command parsing failed\n");
	}
	return 0;
}


int mi_dsi_update_em_cycle_cmd(struct dsi_panel *panel,
			enum dsi_cmd_set_type type, int bl_lvl)
{
	struct dsi_cmd_update_info *info = NULL;
	struct dsi_display_mode *cur_mode = NULL;
	struct drm_panel_build_id_config *config = NULL;
	struct mi_dsi_panel_cfg *mi_cfg;
	u32 cmd_update_index = 0;
	u32 cmd_update_count = 0;
	bool is_force_increasing_on = false;
	u8 refresh_rate;
	u8 em_cycle_cfg[2] = {0};
	u8 bl_buf[2] = {0};
	u8 *gamma_ptr;
	u32 gamma_length;
	int i = 0;

	if (mi_get_panel_id_by_dsi_panel(panel) != M2_PANEL_PA) {
		DISP_DEBUG("this project don't need update em cycle command\n");
		return 0;
	}

	if (!panel || !panel->cur_mode || !panel->cur_mode->priv_info) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	cur_mode = panel->cur_mode;

	mi_cfg = &panel->mi_cfg;
	config = &(panel->id_config);
	if (config->build_id == 0xFB || config->build_id == 0x50 || config->build_id == 0xB0) {
		DISP_ERROR("invalid panel_build_id\n");
		return -EINVAL;
	}

	if (bl_lvl == 0 && type != DSI_CMD_SET_QSYNC_OFF && !is_aod_and_panel_initialized(panel)) {
		DISP_INFO("[%s] skip to update BDreg during bl_lvl = %d\n", panel->type, bl_lvl);
		return 0;
	}

	refresh_rate = panel->cur_mode->timing.refresh_rate;

	bl_buf[0] = (bl_lvl >> 8) & 0xff;
	bl_buf[1] = bl_lvl & 0xff;

	if (refresh_rate == 120)
		is_force_increasing_on = true;
	else
		is_force_increasing_on = false;

	if (panel->qsync_enable) {
		em_cycle_cfg[0] = 0x21;
		em_cycle_cfg[1] = (bl_lvl > 328) ? 0x02 : 0x0F;
	} else {
		em_cycle_cfg[0] = 0x21;
		em_cycle_cfg[1] = (is_force_increasing_on ? 0x00 : 0x80) & 0xF0;
		em_cycle_cfg[1] |= (bl_lvl>328? 0x02 : 0x0F) & 0x0F;
	}

	if (refresh_rate == 120 || panel->qsync_enable) {
		gamma_ptr = mi_cfg->gamma_auto_param;
		gamma_length = sizeof(mi_cfg->gamma_auto_param);
	}else if (bl_lvl <= mi_cfg->panel_1920pwm_dbv_threshold) {
		gamma_ptr = mi_cfg->gamma_16pulse_param;
		gamma_length = sizeof(mi_cfg->gamma_16pulse_param);
	} else {
		gamma_ptr = mi_cfg->gamma_3pulse_param;
		gamma_length = sizeof(mi_cfg->gamma_3pulse_param);
	}

	/*enter doze nolp mode, 16 pluse on*/
	if (is_aod_and_panel_initialized(panel)) {
		if (refresh_rate != 120 && mi_cfg->aod_to_normal_statue) {
			em_cycle_cfg[0] = 0x21;
			em_cycle_cfg[1] = 0x8F;
			gamma_ptr = mi_cfg->gamma_16pulse_param;
			gamma_length = sizeof(mi_cfg->gamma_16pulse_param);
		} else {
			return -EINVAL;
		}
	}

	DISP_INFO("[%s] bl_lvl[%d],refresh_rate[%d], em_cycle_cfg[0x%02x 0x%02x], gamma[0x%02x 0x%02x]\n",
                  panel->type, bl_lvl, refresh_rate, em_cycle_cfg[0], em_cycle_cfg[1], gamma_ptr[0], gamma_ptr[1]);

	switch (type) {
		case DSI_CMD_SET_MI_PANEL_EM_CYCLE_16PULSE_OFF:
			cmd_update_index = DSI_CMD_SET_MI_PANEL_EM_CYCLE_16PULSE_OFF_UPDATA;
			break;
		case DSI_CMD_SET_MI_PANEL_EM_CYCLE_16PULSE_ON:
			cmd_update_index = DSI_CMD_SET_MI_PANEL_EM_CYCLE_16PULSE_ON_UPDATA;
			break;
		case DSI_CMD_SET_TIMING_SWITCH:
			cmd_update_index = DSI_CMD_SET_TIMING_SWITCH_UPDATA;
			break;
		case DSI_CMD_SET_QSYNC_OFF:
			cmd_update_index = DSI_CMD_SET_MI_QSYNC_OFF_COMMANDS_EM_CYCLE_UPDATE;
			break;
		case DSI_CMD_SET_QSYNC_ON:
			cmd_update_index = DSI_CMD_SET_MI_QSYNC_ON_COMMANDS_EM_CYCLE_UPDATE;
			break;
		case DSI_CMD_SET_MI_EXIT_90FPS_TIMING_SWITCH:
			cmd_update_index = DSI_CMD_SET_MI_EXIT_90FPS_TIMING_SWITCH_UPDATA;
			break;
		case DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL_EM_CYCLE_16PULSE:
			cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL_EM_CYCLE_16PULSE_ON_UPDATE;
			break;
		case DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL:
			cmd_update_index = DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL_UPDATE;
			break;
		default:
			DISP_ERROR("[%s] unsupport cmd %s\n",
				panel->type, cmd_set_prop_map[type]);
			return -EINVAL;
	}

	info = cur_mode->priv_info->cmd_update[cmd_update_index];
	cmd_update_count = cur_mode->priv_info->cmd_update_count[cmd_update_index];

	for (i = 0; i < cmd_update_count; i++) {
		DISP_DEBUG("[%s] update [%s] mipi_address(0x%02X) index(%d) lenght(%d)\n",
			panel->type, cmd_set_prop_map[info->type],
			info->mipi_address, info->index, info->length);
		if (info->mipi_address == 0xBD && info->length == gamma_length) {
			mi_dsi_panel_update_cmd_set(panel, cur_mode,
					type, info, gamma_ptr, gamma_length);
		} else if (info->mipi_address == 0xBD && info->length == sizeof(em_cycle_cfg)) {
			mi_dsi_panel_update_cmd_set(panel, cur_mode,
					type, info, em_cycle_cfg, sizeof(em_cycle_cfg));
		} else if (info->mipi_address == 0x51 && info->length == sizeof(bl_buf)) {
			mi_dsi_panel_update_cmd_set(panel, cur_mode,
					type, info, bl_buf, sizeof(bl_buf));
		}
		info++;
	}

	return 0;
}

int dsi_panel_send_em_cycle_setting(struct dsi_panel *panel, u32 bl_lvl)
{
	int rc = 0;
	struct drm_panel_build_id_config *config;
	struct mi_dsi_panel_cfg *mi_cfg;
	ktime_t start_ktime;
	s64 elapsed_us;

	if (!panel)
		return -EINVAL;

	if (mi_get_panel_id_by_dsi_panel(panel) != M2_PANEL_PA)
		return 0;

	start_ktime = ktime_get();

	mi_cfg = &panel->mi_cfg;
	config = &(panel->id_config);
	if (!config->build_id || bl_lvl == 0) {
		return 0;
	} else if (config->build_id != 0xFB && config->build_id != 0x50 && config->build_id != 0xB0) {
		//p0,p0.1,p1(0xb0) does not support 1920pwm
		if (mi_cfg->last_bl_level == 0 && bl_lvl != 0) {
			if (bl_lvl <= mi_cfg->panel_1920pwm_dbv_threshold) {
				mi_cfg->is_em_cycle_16_pulse = false;
			} else
				mi_cfg->is_em_cycle_16_pulse = true;
		}
		if (bl_lvl <= mi_cfg->panel_1920pwm_dbv_threshold && !mi_cfg->is_em_cycle_16_pulse) {
			if (mi_cfg->lhbm_0size_on) {
				mi_cfg->is_em_cycle_16_pulse = true;
				DISP_INFO("0size status is on, skip bl[%d] set 16 pulse\n",bl_lvl);
				return rc;
			}
			rc = mi_dsi_update_em_cycle_cmd(panel,DSI_CMD_SET_MI_PANEL_EM_CYCLE_16PULSE_ON,bl_lvl);
			rc |= dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_PANEL_EM_CYCLE_16PULSE_ON);
			mi_cfg->is_em_cycle_16_pulse = true;
			if (rc) {
				DISP_ERROR("[%s] failed to send EM_CYCLE_16PULSE_ON cmds, bl_lvl[%d],rc=%d\n",
						panel->name, bl_lvl, rc);
				return rc;
			} else {
				DSI_INFO("[%s] send EM_CYCLE_16PULSE_ON cmds, bl_lvl[%d],16_pulse: %d\n",
						panel->name, bl_lvl, mi_cfg->is_em_cycle_16_pulse);
			}
			if (panel->cur_mode->timing.refresh_rate == 120) {
				mi_dsi_update_em_cycle_cmd(panel,DSI_CMD_SET_TIMING_SWITCH,bl_lvl);
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_TIMING_SWITCH);
				if (rc) {
					DISP_ERROR("[%s] failed to send DSI_CMD_SET_TIMING_SWITCH cmds[120hz], bl_lvl[%d],rc=%d\n",
						panel->name, bl_lvl, rc);
					return rc;
				} else {
					DSI_INFO("[%s] send DSI_CMD_SET_TIMING_SWITCH cmds[120hz], bl_lvl[%d],16_pulse: %d\n",
						panel->name, bl_lvl, mi_cfg->is_em_cycle_16_pulse);
				}
			}
			mi_disp_update_0size_lhbm_info(panel);
		} else if (bl_lvl > mi_cfg->panel_1920pwm_dbv_threshold && mi_cfg->is_em_cycle_16_pulse) {
			mi_cfg->is_em_cycle_16_pulse = false;
			rc = mi_dsi_update_em_cycle_cmd(panel,DSI_CMD_SET_MI_PANEL_EM_CYCLE_16PULSE_OFF,bl_lvl);
			rc |= dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_PANEL_EM_CYCLE_16PULSE_OFF);
			if (rc) {
				DISP_ERROR("[%s] failed to send EM_CYCLE_16PULSE_OFF cmds, bl_lvl[%d], rc=%d\n",
                                                panel->name, bl_lvl, rc);
				return rc;
			} else {
				DSI_INFO("[%s] send EM_CYCLE_16PULSE_OFF cmds, bl_lvl[%d], 16_pulse: %d\n",
						panel->name, bl_lvl, mi_cfg->is_em_cycle_16_pulse);
			}
			if (panel->cur_mode->timing.refresh_rate == 120) {
				mi_dsi_update_em_cycle_cmd(panel,DSI_CMD_SET_TIMING_SWITCH,bl_lvl);
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_TIMING_SWITCH);
				if (rc) {
					DISP_ERROR("[%s] failed to send DSI_CMD_SET_TIMING_SWITCH cmds[120hz], bl_lvl[%d],rc=%d\n",
						panel->name, bl_lvl, rc);
					return rc;
				} else {
					DSI_INFO("[%s] send DSI_CMD_SET_TIMING_SWITCH cmds[120hz], bl_lvl[%d],16_pulse: %d\n",
						panel->name, bl_lvl, mi_cfg->is_em_cycle_16_pulse);
				}
			}
		}
	}

	elapsed_us = ktime_us_delta(ktime_get(), start_ktime);
	DISP_DEBUG("elapsed time - %d.%d(ms)\n", (int)(elapsed_us / 1000), (int)(elapsed_us % 1000));
	return rc;
}

int dsi_panel_send_em_cycle_setting_N11(struct dsi_panel *panel, u32 bl_lvl)
{
	int rc = 0;
	struct drm_panel_build_id_config *config;
	struct mi_dsi_panel_cfg *mi_cfg;
	ktime_t start_ktime;
	s64 elapsed_us;

	if (!panel)
		return -EINVAL;

	if (mi_get_panel_id_by_dsi_panel(panel) != N11_PANEL_PA)
		return 0;

	start_ktime = ktime_get();

	mi_cfg = &panel->mi_cfg;
	config = &(panel->id_config);

	if (!config->build_id || bl_lvl == 0) {
		return 0;
	} else {
		if (mi_cfg->last_bl_level == 0 && bl_lvl != 0) {
			if (bl_lvl <= mi_cfg->panel_3840pwm_dbv_threshold) {
				mi_cfg->is_em_cycle_32_pulse = false;
			} else {
				mi_cfg->is_em_cycle_32_pulse = true;
			}
		}

		if (bl_lvl <= mi_cfg->panel_3840pwm_dbv_threshold && !mi_cfg->is_em_cycle_32_pulse) {
			rc |= dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_32PULSE);
			mi_cfg->is_em_cycle_32_pulse = true;
			if (rc) {
				DISP_ERROR("[%s] DSI_CMD_SET_MI_32PULSE rc = %d, bl_lvl[%d]\n",
						panel->name, rc, bl_lvl);
				return rc;
			} else {
				DSI_INFO("[%s] DSI_CMD_SET_MI_32PULSE rc = %d, bl_lvl[%d],32_pulse: %d\n",
						panel->name, rc, bl_lvl, mi_cfg->is_em_cycle_32_pulse);
			}
		} else if (bl_lvl > mi_cfg->panel_3840pwm_dbv_threshold && mi_cfg->is_em_cycle_32_pulse) {
			mi_cfg->is_em_cycle_32_pulse = false;
			rc |= dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_16PULSE);
			if (rc) {
				DISP_ERROR("[%s] DSI_CMD_SET_MI_16PULSE rc = %d, bl_lvl[%d]\n",
						panel->name, rc, bl_lvl);
				return rc;
			} else {
				DSI_INFO("[%s] DSI_CMD_SET_MI_16PULSE rc = %d, bl_lvl[%d],16_pulse: %d\n",
						panel->name, rc, bl_lvl, mi_cfg->is_em_cycle_32_pulse);
			}
		}
	}

	elapsed_us = ktime_us_delta(ktime_get(), start_ktime);
	DISP_DEBUG("elapsed time - %d.%d(ms)\n", (int)(elapsed_us / 1000), (int)(elapsed_us % 1000));
	return rc;
}

int dsi_panel_parse_wp_reg_read_config(struct dsi_panel *panel)
{
	struct drm_panel_wp_config *wp_config;
	struct dsi_parser_utils *utils = &panel->utils;
	int rc = 0;

	if (!panel) {
		DISP_ERROR("Invalid Params\n");
		return -EINVAL;
	}

	wp_config = &panel->wp_config;
	if (!wp_config)
		return -EINVAL;

	dsi_panel_parse_cmd_sets_sub(&wp_config->wp_cmd,
			DSI_CMD_SET_PANEL_WP_READ, utils);
	if (!wp_config->wp_cmd.count) {
		DISP_ERROR("wp_info read command parsing failed\n");
		return -EINVAL;
	}

	dsi_panel_parse_cmd_sets_sub(&wp_config->offset_cmd,
			DSI_CMD_SET_PANEL_WP_READ_OFFSET, utils);
	if (!wp_config->offset_cmd.count)
		DISP_INFO("wp_info offset command parsing failed\n");

	rc = utils->read_u32(utils->data, "mi,mdss-dsi-panel-wp-read-length",
				&wp_config->wp_cmds_rlen);
	if (rc || !wp_config->wp_cmds_rlen) {
		wp_config->wp_cmds_rlen = 0;
		return -EINVAL;
	}

	rc = utils->read_u32(utils->data, "mi,mdss-dsi-panel-wp-read-index",
				&wp_config->wp_read_info_index);
	if (rc || !wp_config->wp_read_info_index) {
		wp_config->wp_read_info_index = 0;
	}

	wp_config->return_buf = kcalloc(wp_config->wp_cmds_rlen,
		sizeof(unsigned char), GFP_KERNEL);
	if (!wp_config->return_buf) {
		return -ENOMEM;
	}
	return 0;
}

static int mi_dsi_panel_cal_fps_val(struct dsi_panel *panel, u32 qsync_min_fps, u8 *buf)
{
	int rc = 0;
	struct mi_dsi_panel_cfg *mi_cfg = &panel->mi_cfg;
	u32 valid_fps = 0;

	/*SanSung panel cal*/
	if (mi_get_panel_id(mi_cfg->mi_panel_id) == M2_PANEL_PA && qsync_min_fps) {
		valid_fps = 120 % qsync_min_fps;
		if (valid_fps){
			DISP_ERROR("ERROR Qsync Min FPS: %d, Set default min fps:20!\n", qsync_min_fps);
			qsync_min_fps = 20;
		}
		mi_cfg->active_qsync_min_fps = qsync_min_fps;
		buf[0] = (120/qsync_min_fps) -1;
		buf[1] = 0;
		rc = 0;
	} else {
		mi_cfg->active_qsync_min_fps = panel->cur_mode->timing.qsync_min_fps;
		rc = -1;
	}

	return rc;
}

int mi_dsi_panel_update_mipi_qsync_on_cmd(struct dsi_panel *panel, u32 cmd_update_val)
{
	int rc = 0;
	struct dsi_cmd_update_info *info = NULL;
	struct dsi_display_mode *cur_mode = NULL;
	struct mi_dsi_panel_cfg *mi_cfg = &panel->mi_cfg;
	enum dsi_cmd_set_type type = DSI_CMD_SET_QSYNC_ON;
	u32 cmd_update_index = DSI_CMD_SET_MI_QSYNC_ON_COMMANDS_UPDATE;
	u8 cmd_val_buf[2] = {0};
	u8 cmd_val_size = 1;//undo,B nvt panel is 2

	if (!panel || !panel->cur_mode || !panel->cur_mode->priv_info) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	cur_mode = panel->cur_mode;

	if (mi_cfg->active_qsync_min_fps != cmd_update_val) {
		rc = mi_dsi_panel_cal_fps_val(panel, cmd_update_val, cmd_val_buf);
		if (rc != 0) {
			DISP_INFO("qsync min fps = %d, set error, no update cmd!", cmd_val_buf);
			goto exit;
		}

		DISP_INFO("[%s] qsync min fps update val = 0x%x,0x%x index=%d, size=%d\n", panel->type,
				cmd_update_val, cmd_val_buf[0], cmd_val_buf[1], cmd_val_size);

		info = cur_mode->priv_info->cmd_update[cmd_update_index];
		if (!info) {
			DISP_ERROR("Please check DTS %s was added!!!! ", cmd_set_update_map[cmd_update_index]);
			/*err set, no update cmd, init active_qsync_min_fps to default*/
			mi_cfg->active_qsync_min_fps = cur_mode->timing.qsync_min_fps;
			rc = -EINVAL;
			goto exit;
		}

		rc = mi_dsi_panel_update_cmd_set(panel, cur_mode, type, info,
					cmd_val_buf, cmd_val_size);
	} else {
		DISP_INFO("Same qsync config:%d, %d, %d\n",mi_cfg->active_qsync_min_fps,
					cmd_update_val, mi_cfg->active_qsync_min_fps);
		rc = 0;
	}
exit:
	return rc;
}

static inline void mi_dsi_panel_lgs_cie_by_temperature(enum dsi_cmd_set_type *dbi_bwg_type, int bl_lvl, int temp)
{
	if (temp >= 32100) {
		if (0x84 > bl_lvl && bl_lvl >= 0x7B) {
			*dbi_bwg_type = DSI_CMD_SET_MI_DBI_BWG_45_MODE;
		} else if (0x7B > bl_lvl && bl_lvl >= 0x4C) {
			*dbi_bwg_type = DSI_CMD_SET_MI_DBI_BWG_46_MODE;
		} else if (0x4C > bl_lvl && bl_lvl >= 0x28) {
			*dbi_bwg_type = DSI_CMD_SET_MI_DBI_BWG_47_MODE;
		} else if (0x28 > bl_lvl && bl_lvl >= 0x19) {
			*dbi_bwg_type = DSI_CMD_SET_MI_DBI_BWG_48_MODE;
		}
	} else if (32100 > temp && temp >= 28100) {
		if (0x84 > bl_lvl && bl_lvl >= 0x7B) {
			*dbi_bwg_type = DSI_CMD_SET_MI_DBI_BWG_41_MODE;
		} else if (0x7B > bl_lvl && bl_lvl >= 0x4C) {
			*dbi_bwg_type = DSI_CMD_SET_MI_DBI_BWG_42_MODE;
		} else if (0x4C > bl_lvl && bl_lvl >= 0x28) {
			*dbi_bwg_type = DSI_CMD_SET_MI_DBI_BWG_43_MODE;
		} else if (0x28 > bl_lvl && bl_lvl >= 0x19) {
			*dbi_bwg_type = DSI_CMD_SET_MI_DBI_BWG_44_MODE;
		}
	} else if (temp < 28100) {
		*dbi_bwg_type = DSI_CMD_SET_MI_DBI_BWG_0_30NIT_MODE;
	}
}

int mi_dsi_panel_vrr_set_by_dbv(struct dsi_panel *panel, u32 bl_lvl)
{
	int rc = 0;
	struct mi_dsi_panel_cfg *mi_cfg;
	enum dsi_cmd_set_type skip_source_type, dbi_bwg_type;
	int temp = 0;

	if (!panel)
		return -EINVAL;

	if (mi_get_panel_id_by_dsi_panel(panel) != M1_PANEL_PA)
		return 0;

	mi_cfg = &panel->mi_cfg;

	skip_source_type = dbi_bwg_type = DSI_CMD_SET_MAX;
	if (bl_lvl == 0) {
		mi_cfg->skip_source_type = DSI_CMD_SET_MAX;
		mi_cfg->dbi_bwg_type = DSI_CMD_SET_MAX;
		return -EINVAL;
	}

	if (bl_lvl  >= 0x7FF) {
		skip_source_type = DSI_CMD_SET_MI_SKIP_SOURCE_500_1300NIT_MODE;
		dbi_bwg_type = DSI_CMD_SET_MI_DBI_BWG_500_1300NIT_MODE;
	} else if (0x7FF > bl_lvl && bl_lvl >= 0x333) {
		skip_source_type = DSI_CMD_SET_MI_SKIP_SOURCE_200_500NIT_MODE;
		dbi_bwg_type = DSI_CMD_SET_MI_DBI_BWG_200_500NIT_MODE;
	} else if (0x333 > bl_lvl && bl_lvl >= 0x1C2) {
		skip_source_type = DSI_CMD_SET_MI_SKIP_SOURCE_110_200NIT_MODE;
		dbi_bwg_type = DSI_CMD_SET_MI_DBI_BWG_110_200NIT_MODE;
	} else if (0x1C2 > bl_lvl && bl_lvl >= 0x147) {
		skip_source_type = DSI_CMD_SET_MI_SKIP_SOURCE_80_110NIT_MODE;
		dbi_bwg_type = DSI_CMD_SET_MI_DBI_BWG_80_110NIT_MODE;
	} else if (0x147 > bl_lvl && bl_lvl >= 0x12B) {
		skip_source_type = DSI_CMD_SET_MI_SKIP_SOURCE_72_79NIT_MODE;
		dbi_bwg_type = DSI_CMD_SET_MI_DBI_BWG_72_79NIT_MODE;
	} else if (0x12B > bl_lvl && bl_lvl >= 0x110) {
		skip_source_type = DSI_CMD_SET_MI_SKIP_SOURCE_65_72NIT_MODE;
		dbi_bwg_type = DSI_CMD_SET_MI_DBI_BWG_65_72NIT_MODE;
	} else if (0x110 > bl_lvl && bl_lvl >= 0xF5) {
		skip_source_type = DSI_CMD_SET_MI_SKIP_SOURCE_60_65NIT_MODE;
		dbi_bwg_type = DSI_CMD_SET_MI_DBI_BWG_60_65NIT_MODE;
	} else if (0xF5 > bl_lvl && bl_lvl >= 0xDC) {
		skip_source_type = DSI_CMD_SET_MI_SKIP_SOURCE_53_60NIT_MODE;
		dbi_bwg_type = DSI_CMD_SET_MI_DBI_BWG_53_60NIT_MODE;
	} else if (0xDC > bl_lvl && bl_lvl >= 0x84) {
		skip_source_type = DSI_CMD_SET_MI_SKIP_SOURCE_30_53NIT_MODE;
		dbi_bwg_type = DSI_CMD_SET_MI_DBI_BWG_30_53NIT_MODE;
	} else if (0x84 > bl_lvl) {
		temp = mi_cfg->feature_val[DISP_FEATURE_DBI];
		skip_source_type = DSI_CMD_SET_MI_SKIP_SOURCE_0_30NIT_MODE;
		dbi_bwg_type = DSI_CMD_SET_MI_DBI_BWG_0_30NIT_MODE;
		mi_dsi_panel_lgs_cie_by_temperature(&dbi_bwg_type, bl_lvl, temp);
	}

	if ((mi_cfg->skip_source_type != skip_source_type) && (skip_source_type != DSI_CMD_SET_MAX)) {
		rc = dsi_panel_tx_cmd_set(panel, skip_source_type);
		if (rc) {
			DISP_ERROR("(%s) send  cmds(%s) failed! bl_lvl(%d),rc(%d)\n",
					panel->name, cmd_set_prop_map[skip_source_type], bl_lvl, rc);
			return rc;
		}
		mi_cfg->skip_source_type = skip_source_type;
		DISP_DEBUG("Send SKip Source(%s), temp = %d, bl_lvl = 0x%x\n",
				cmd_set_prop_map[skip_source_type], temp, bl_lvl);
	}

	if ((mi_cfg->dbi_bwg_type != dbi_bwg_type) && (dbi_bwg_type != DSI_CMD_SET_MAX)) {
		rc = dsi_panel_tx_cmd_set(panel, dbi_bwg_type);
		if (rc) {
			DISP_ERROR("(%s) send  cmds(%s) failed! bl_lvl(%d),rc(%d)\n",
					panel->name, cmd_set_prop_map[dbi_bwg_type], bl_lvl, rc);
			return rc;
		}
		mi_cfg->dbi_bwg_type = dbi_bwg_type;
		DISP_DEBUG("Send DBI BWG(%s), temp = %d, bl_lvl = 0x%x\n",
				cmd_set_prop_map[dbi_bwg_type], temp, bl_lvl);
	}

	return rc;
}

int mi_dsi_panel_set_3dlut_temperature_compensation(struct dsi_panel *panel,
			int level, bool force_update)
{
	int rc = 0;
	static bool set_lutcmd_booton = true;
	int build_id = 0;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	build_id = panel->id_config.build_id;

	if (panel->power_mode != SDE_MODE_DPMS_ON && !force_update) {
		DISP_INFO("panel not in SDE_MODE_DPMS_ON\n");
		return 0;
	}

	DISP_INFO("level : 0x%02X \n", level);
	if (build_id == 0x80 && (set_lutcmd_booton || force_update)) {
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_TEMPERATURE_CODE);
		set_lutcmd_booton = false;
	}

	if (panel->cur_mode->timing.refresh_rate == 90) {
		level = 0x80;
		DISP_INFO("fps 90 set dbi compensation off\n");
	}

	if (level == 0x01) {
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_TEMPERATURE_LUT3);
	} else if (level == 0x20) {
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_TEMPERATURE_LUT2);
	} else if (level == 0x40) {
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_TEMPERATURE_LUT1);
	} else if (level == 0x80) {
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_TEMPERATURE_LUTOFF);
	}
	return rc;
}

bool mi_dsi_panel_new_aod(struct dsi_panel *panel) {
	if (panel->mi_cfg.aod_layer_remove) {
		switch (mi_get_panel_id(panel->mi_cfg.mi_panel_id)) {
			case M2_PANEL_PA:
			case M3_PANEL_PA:
				return true;
				break;
			default:
				break;
		}
	}
	return false;
}

int mi_dsi_panel_set_nolp_locked(struct dsi_panel *panel)
{
	int rc = 0;
	u32 doze_brightness = panel->mi_cfg.doze_brightness;
	int update_bl = 0;
	if (panel->mi_cfg.panel_state == PANEL_STATE_ON) {
		DISP_INFO("panel already PANEL_STATE_ON, skip nolp");
		return rc;
	}

	if (doze_brightness == DOZE_TO_NORMAL)
		doze_brightness = panel->mi_cfg.last_doze_brightness;

	switch (doze_brightness) {
		case DOZE_BRIGHTNESS_HBM:
			DISP_INFO("set doze_hbm_dbv_level in nolp");
			update_bl = panel->mi_cfg.doze_hbm_dbv_level;
			break;
		case DOZE_BRIGHTNESS_LBM:
			DISP_INFO("set doze_lbm_dbv_level in nolp");
			update_bl = panel->mi_cfg.doze_lbm_dbv_level;
			break;
		default:
			break;
	}

	mi_dsi_update_51_mipi_cmd(panel, DSI_CMD_SET_NOLP, update_bl);
	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_NOLP);

	if (mi_get_panel_id_by_dsi_panel(panel) == M2_PANEL_PA)
		panel->mi_cfg.is_em_cycle_16_pulse = true;

	panel->power_mode = SDE_MODE_DPMS_ON;
	return rc;
}
