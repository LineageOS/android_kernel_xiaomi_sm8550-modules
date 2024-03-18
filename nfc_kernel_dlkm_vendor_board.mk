# Build NFC kernel driver
ifeq ($(call is-board-platform-in-list, kalama bengal crow trinket bengal_515),true)
BOARD_VENDOR_KERNEL_MODULES += $(KERNEL_MODULES_OUT)/nxp-nci.ko
endif

