# SPDX-License-Identifier: GPL-2.0-only

subdir-ccflags-y += -DWMT_CREATE_NODE_DYNAMIC=1
subdir-ccflags-y += -DWMT_IDC_SUPPORT=0
subdir-ccflags-y += -DMT8163_CONN_PM_WAKEUP_SOURCE
mt8163_conn_dir := $(srctree)/drivers/misc/mediatek/mt8163-connectivity
subdir-ccflags-y += -include $(mt8163_conn_dir)/compat/legacy_time.h
subdir-ccflags-y += -I$(mt8163_conn_dir)/compat
subdir-ccflags-y += -I$(mt8163_conn_dir)/btif/common/inc
subdir-ccflags-y += -I$(mt8163_conn_dir)/btif/common/plat_inc
subdir-ccflags-y += -I$(mt8163_conn_dir)/conn_soc/common
subdir-ccflags-y += -I$(mt8163_conn_dir)/conn_soc/common/include
subdir-ccflags-y += -I$(mt8163_conn_dir)/conn_soc/common/core/include
subdir-ccflags-y += -I$(mt8163_conn_dir)/conn_soc/common/linux/include
subdir-ccflags-y += -I$(mt8163_conn_dir)/conn_soc/common/linux/pri/include
subdir-ccflags-y += -I$(mt8163_conn_dir)/conn_soc/common/mt8163/include
subdir-ccflags-y += -I$(mt8163_conn_dir)/conn_soc/drv_wlan/mt_wifi/wlan/include
subdir-ccflags-y += -I$(mt8163_conn_dir)/conn_soc/drv_wlan/mt_wifi/wlan/include/nic
subdir-ccflags-y += -I$(mt8163_conn_dir)/conn_soc/drv_wlan/mt_wifi/wlan/include/mgmt
subdir-ccflags-y += -I$(mt8163_conn_dir)/conn_soc/drv_wlan/mt_wifi/wlan/os
subdir-ccflags-y += -I$(mt8163_conn_dir)/conn_soc/drv_wlan/mt_wifi/wlan/os/linux/include
subdir-ccflags-y += -I$(mt8163_conn_dir)/conn_soc/drv_wlan/mt_wifi/wlan/os/linux/hif/ahb/include

obj-y += compat/
obj-y += btif/
obj-y += conn_soc/
