ifeq ($(KGSL_MODULE_ROOT),)
CUR_MKFILE = $(abspath $(lastword $(MAKEFILE_LIST)))
KGSL_MODULE_ROOT = $(dir $(CUR_MKFILE))
endif

KBUILD_OPTIONS += KGSL_PATH=$(KGSL_MODULE_ROOT)
KBUILD_OPTIONS += MODNAME=msm_kgsl
KBUILD_OPTIONS += BOARD_PLATFORM=$(TARGET_BOARD_PLATFORM)

all: modules

modules_install:
	$(MAKE) INSTALL_MOD_STRIP=1 -C $(KERNEL_SRC) M=$(M) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) clean

%:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) $@ $(KBUILD_OPTIONS)
