# SPDX-License-Identifier: GPL-2.0

KBUILD_EXTRA_SYMBOLS := \
    $(OUT_DIR)/../sm8550-modules/qcom/opensource/mm-drivers/hw_fence/Module.symvers \
    $(OUT_DIR)/../sm8550-modules/qcom/opensource/mm-drivers/msm_ext_display/Module.symvers \
    $(OUT_DIR)/../sm8550-modules/qcom/opensource/mm-drivers/sync_fence/Module.symvers \
    $(OUT_DIR)/../sm8550-modules/qcom/opensource/mmrm-driver/Module.symvers \
    $(OUT_DIR)/../sm8550-modules/qcom/opensource/securemsm-kernel/Module.symvers

KBUILD_OPTIONS += DISPLAY_ROOT=$(KERNEL_SRC)/$(M)/../
KBUILD_OPTIONS += BOARD_PLATFORM=$(TARGET_BOARD_PLATFORM)

all: modules

modules_install:
	$(MAKE) INSTALL_MOD_STRIP=1 -C $(KERNEL_SRC) M=$(M) modules_install

%:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) $@ $(KBUILD_OPTIONS) $(KBUILD_EXTRA_SYMBOLS)

clean:
	rm -f *.o *.ko *.mod.c *.mod.o *~ .*.cmd Module.symvers
	rm -rf .tmp_versions

