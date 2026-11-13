# SPDX-License-Identifier: GPL-2.0-only

snd-soc-tfa987x-dsp-y := src/tfa98xx.o src/tfa_container.o src/tfa_dsp.o src/tfa_init.o

ccflags-y += -I$(src)/inc -DCONFIG_TARGET_PRODUCT_LISA

obj-$(CONFIG_SND_SOC_TFA987X_DSP) += snd-soc-tfa987x-dsp.o
