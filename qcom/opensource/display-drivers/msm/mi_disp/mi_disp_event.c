/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2020 XiaoMi, Inc. All rights reserved.
 */
#define pr_fmt(fmt) "mi_disp_event:[%s:%d] " fmt, __func__, __LINE__

#include "dsi_panel.h"
#include "mi_disp_print.h"
#include "mi_disp_event.h"

#if IS_ENABLED(CONFIG_MIEV)
#include "mievent.h"

const char *get_mievent_type_name(int event_type)
{
	switch (event_type) {
	case MI_EVENT_PRI_PANEL_REG_ESD:
		return "pri_panel_reg_esd";
	case MI_EVENT_PRI_PANEL_IRQ_ESD:
		return "pri_panel_irq_esd";
	case MI_EVENT_PRI_PLATFORM_ESD:
		return "pri_platform_esd";
	case MI_EVENT_DSI_ERROR:
		return "dsi_error";
	case MI_EVENT_PANEL_HW_RESOURCE_GET_FAILED:
		return "panel_hw_resource_get_failed";
	case MI_EVENT_PANEL_RECOGNIZE_ERR:
		return "panel_recognize_err";
	case MI_EVENT_PANEL_WP_READ_FAILED:
		return "panel_wp_read_failed";
	case MI_EVENT_PANEL_UNDERRUN:
		return "panel_underrun";
	case MI_EVENT_SEC_PANEL_REG_ESD:
		return "sec_panel_reg_esd";
	case MI_EVENT_SEC_PANEL_IRQ_ESD:
		return "sec_panel_irq_esd";
	case MI_EVENT_SEC_PLATFORM_ESD:
		return "sec_platform_esd";
	case MI_EVENT_PANEL_HARDWARE_ERR:
		return "panel_hardware_err";
	case MI_EVENT_PRI_PANEL_REG_ESD_RECOVERY:
		return "pri_panel_reg_esd_recover";
	case MI_EVENT_PRI_PANEL_IRQ_ESD_RECOVERY:
		return "pri_panel_irq_esd_recover";
	case MI_EVENT_PRI_PLATFORM_ESD_RECOVERY:
		return "pri_platform_esd_recover";
	case MI_EVENT_SEC_PANEL_REG_ESD_RECOVERY:
		return "sec_panel_reg_esd_recover";
	case MI_EVENT_SEC_PANEL_IRQ_ESD_RECOVERY:
		return "sec_panel_irq_esd_recover";
	case MI_EVENT_SEC_PLATFORM_ESD_RECOVERY:
		return "sec_platform_esd_recover";
	default:
		return "Unknown";
	}
}

unsigned int get_mievent_recovery_type(int event_type)
{
	switch (event_type) {
	case MI_EVENT_PRI_PANEL_REG_ESD:
		return MI_EVENT_PRI_PANEL_REG_ESD_RECOVERY;
	break;
	case MI_EVENT_PRI_PANEL_IRQ_ESD:
		return MI_EVENT_PRI_PANEL_IRQ_ESD_RECOVERY;
	break;
	case MI_EVENT_PRI_PLATFORM_ESD:
		return MI_EVENT_PRI_PLATFORM_ESD_RECOVERY;
	break;
	case MI_EVENT_SEC_PANEL_REG_ESD:
		return MI_EVENT_SEC_PANEL_REG_ESD_RECOVERY;
	break;
	case MI_EVENT_SEC_PANEL_IRQ_ESD:
		return MI_EVENT_SEC_PANEL_IRQ_ESD_RECOVERY;
	break;
	case MI_EVENT_SEC_PLATFORM_ESD:
		return MI_EVENT_SEC_PLATFORM_ESD_RECOVERY;
	break;
	default:
		return 0;
	}
}
#endif

unsigned int ESD_TYPE = 0;

void mi_disp_mievent_str(unsigned int event_type){
#if IS_ENABLED(CONFIG_MIEV)
	const char *event_describe;
	const char *event_name = NULL;
	struct misight_mievent *event  = cdev_tevent_alloc(event_type);

	event_name = get_mievent_type_name(event_type);
	DISP_INFO("event_type[%d],event_name[%s]\n",event_type,event_name);

	switch(event_type){
	case MI_EVENT_DSI_ERROR:
		event_describe = "panel dsi error";
	break;
	case MI_EVENT_PANEL_HW_RESOURCE_GET_FAILED:
		event_describe = "panel HW Resource Get Failed";
	break;
	case MI_EVENT_PANEL_RECOGNIZE_ERR:
		event_describe = "wrong panel";
	break;
	case MI_EVENT_PANEL_WP_READ_FAILED:
		event_describe = "panel WP Read Failed";
	break;
	case MI_EVENT_PANEL_UNDERRUN:
		event_describe = "panel underrun";
	break;
	case MI_EVENT_PANEL_HARDWARE_ERR:
		event_describe = "panel Hardware Err";
	break;
	default:
		DISP_ERROR("It's a invalid event_type");
		cdev_tevent_destroy(event);
		return;
	}

	cdev_tevent_add_str(event, event_name, event_describe);
	cdev_tevent_write(event);
	cdev_tevent_destroy(event);
#endif
}

/**
* mi_disp_mievent_int
* @param disp_id MI_DISP_PRIMARY or MI_DISP_SECONDARY
* @param event_type the fault code for the problem event，eg：911001001
* @param event_name a fault name for the problem event，eg："pri_panel_reg_esd"
**/
void mi_disp_mievent_int(int disp_id,unsigned int event_type){
#if IS_ENABLED(CONFIG_MIEV)
	static int Esd_Count[2][3] = {0};
	static int UnderRun_Count = 0;
	const char *event_name = NULL;
	int Esd_index = 0;
	static int Esd_Num = 0;
	static ktime_t Time_End;
	static ktime_t Time_Start;

	struct misight_mievent *event  = cdev_tevent_alloc(event_type);
	event_name = get_mievent_type_name(event_type);

	if (event_type == MI_EVENT_PANEL_UNDERRUN) {
		UnderRun_Count++;
		DISP_INFO("event_type[%d],event_name[%s],UnderRun_Count = %d\n",
			event_type,event_name,UnderRun_Count);
		cdev_tevent_add_int(event, event_name, UnderRun_Count);
	} else {
		ESD_TYPE = event_type;
		Esd_index = (event_type % 10) -1;
		if(Esd_index < 0 || Esd_index >2){
			DISP_ERROR("Esd_index is an invalid event_type");
			cdev_tevent_destroy(event);
			return;
		}
		Esd_Count[disp_id][Esd_index]++;
		DISP_INFO("event_type[%d],event_name[%s],Esd_Count[%d][%d] = %d\n",
			event_type,event_name,disp_id,Esd_index,Esd_Count[disp_id][Esd_index]);
		cdev_tevent_add_int(event, event_name, Esd_Count[disp_id][Esd_index]);

		if(Esd_Num == 0)
			Time_Start = ktime_get_boottime();

		if (++Esd_Num > MI_EVENT_ESD_COUNT_MAX){
			Time_End = ktime_get_boottime();
			if(ktime_to_ms(ktime_sub(Time_End,Time_Start)) < (MI_EVENT_ESD_TIMEOUT * 60 * 1000)){
				mi_disp_mievent_str(MI_EVENT_PANEL_HARDWARE_ERR);
			}
			Esd_Num = 0;
		}
	}

	cdev_tevent_write(event);
	cdev_tevent_destroy(event);
#endif
}

void mi_disp_mievent_recovery(unsigned int event_type) {
#if IS_ENABLED(CONFIG_MIEV)
	static int Esd_Done_Count[2][3] = {0};
	int Esd_Done_index = 0;
	int disp_id = 0;
	unsigned int recover_event_type = 0;
	const char * event_name = NULL;
	const char * esd_event_type_name = "problem_code";
	struct misight_mievent *event = NULL;

	ESD_TYPE = 0;
	recover_event_type = get_mievent_recovery_type(event_type);
	event_name = get_mievent_type_name(recover_event_type);

	switch (recover_event_type) {
	case MI_EVENT_PRI_PANEL_REG_ESD_RECOVERY:
	break;
	case MI_EVENT_PRI_PANEL_IRQ_ESD_RECOVERY:
		Esd_Done_index = 1;
	break;
	case MI_EVENT_PRI_PLATFORM_ESD_RECOVERY:
		Esd_Done_index = 2;
	break;
	case MI_EVENT_SEC_PANEL_REG_ESD_RECOVERY:
		disp_id = 1;
	break;
	case MI_EVENT_SEC_PANEL_IRQ_ESD_RECOVERY:
		disp_id = 1;
		Esd_Done_index = 1;
	break;
	case MI_EVENT_SEC_PLATFORM_ESD_RECOVERY:
		disp_id = 1;
		Esd_Done_index = 2;
	break;
	default:
		DISP_ERROR("It is an invalid event_type[%d],recover_event_type[%d]",event_type,recover_event_type);
		return;
	}

	event  = cdev_tevent_alloc(recover_event_type);

	Esd_Done_Count[disp_id][Esd_Done_index]++;
	DISP_INFO("recover_event_type[%d],event_name[%s],Esd_Done_Count[%d][%d] = %d\n",
		recover_event_type,event_name,disp_id,Esd_Done_index,Esd_Done_Count[disp_id][Esd_Done_index]);
	cdev_tevent_add_int(event, event_name, Esd_Done_Count[disp_id][Esd_Done_index]);
	cdev_tevent_add_int(event, esd_event_type_name, event_type);
	cdev_tevent_write(event);
	cdev_tevent_destroy(event);
#endif
}
