/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2020 XiaoMi, Inc. All rights reserved.
 */

#ifndef _MI_PANEL_ID_H_
#define _MI_PANEL_ID_H_

#include <linux/types.h>
#include "dsi_panel.h"

/*
   Naming Rules,Wiki Dec .
   4Byte : Project ASCII Value .Exemple "L18" ASCII Is 004C3138
   1Byte : Prim Panel Is 'P' ASCII Value , Sec Panel Is 'S' Value
   1Byte : Panel Vendor
   1Byte : DDIC Vendor ,Samsung 0x0C Novatek 0x02
   1Byte : Production Batch Num
*/
#define M2_38_0C_0A_PANEL_ID   0x00004D3200380C00
#define M3_38_0C_0A_PANEL_ID   0x00004D3300380C00
#define M11_42_02_0A_PANEL_ID  0x004D313100420200
#define M1_42_02_0A_PANEL_ID   0x00004D3100420200
#define M18_38_0C_0A_PRIM_PANEL_ID  0x004D313850380C00
#define M18_38_0C_0A_SEC_PANEL_ID   0x004D313853380C00
#define N11_42_02_0A_PANEL_ID  0x004E313100420200

/* PA: Primary display, First selection screen
 * PB: Primary display, Second selection screen
 * SA: Secondary display, First selection screen
 * SB: Secondary display, Second selection screen
 */
enum mi_project_panel_id {
	PANEL_ID_INVALID = 0,
	M2_PANEL_PA,
	M3_PANEL_PA,
	M11_PANEL_PA,
	M1_PANEL_PA,
	M18_PANEL_PA,
	M18_PANEL_SA,
	N11_PANEL_PA,
	PANEL_ID_MAX
};

#define M1_PANEL_PA_P21 0x90
#define M1_PANEL_PA_P20 0x80
#define M1_PANEL_PA_P11 0x50
#define M1_PANEL_PA_P10 0x40

static inline enum mi_project_panel_id mi_get_panel_id(u64 mi_panel_id)
{
	switch(mi_panel_id) {
	case M2_38_0C_0A_PANEL_ID:
		return M2_PANEL_PA;
	case M3_38_0C_0A_PANEL_ID:
		return M3_PANEL_PA;
	case M11_42_02_0A_PANEL_ID:
		return M11_PANEL_PA;
	case M1_42_02_0A_PANEL_ID:
		return M1_PANEL_PA;
	case M18_38_0C_0A_PRIM_PANEL_ID:
		return M18_PANEL_PA;
	case M18_38_0C_0A_SEC_PANEL_ID:
		return M18_PANEL_SA;
	case N11_42_02_0A_PANEL_ID:
		return N11_PANEL_PA;
	default:
		return PANEL_ID_INVALID;
	}
}

static inline const char *mi_get_panel_id_name(u64 mi_panel_id)
{
	switch (mi_get_panel_id(mi_panel_id)) {
	case M2_PANEL_PA:
		return "M2_PANEL_PA";
	case M3_PANEL_PA:
		return "M3_PANEL_PA";
	case M11_PANEL_PA:
		return "M11_PANEL_PA";
	case M1_PANEL_PA:
		return "M1_PANEL_PA";
	case M18_PANEL_PA:
		return "M18_PANEL_PA";
	case M18_PANEL_SA:
		return "M18_PANEL_SA";
	case N11_PANEL_PA:
		return "N11_PANEL_PA";
	default:
		return "unknown";
	}
}

static inline bool is_use_nvt_dsc_config(u64 mi_panel_id)
{
	switch(mi_panel_id) {
		case M11_42_02_0A_PANEL_ID:
			return true;
		case M1_42_02_0A_PANEL_ID:
			return true;
		case N11_42_02_0A_PANEL_ID:
			return true;
		default:
			return false;
	}
}

static inline bool is_use_nt37705_dsc_config(u64 mi_panel_id)
{
	return false;
}

enum mi_project_panel_id mi_get_panel_id_by_dsi_panel(struct dsi_panel *panel);
enum mi_project_panel_id mi_get_panel_id_by_disp_id(int disp_id);

#endif /* _MI_PANEL_ID_H_ */
