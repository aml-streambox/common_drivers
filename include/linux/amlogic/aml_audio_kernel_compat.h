/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/*
 * Narrow compatibility helpers for building the selected Amlogic vendor audio
 * stack against the local Linux v7.1 tree.
 */

#ifndef __AML_AUDIO_KERNEL_COMPAT_H__
#define __AML_AUDIO_KERNEL_COMPAT_H__

#include <linux/amlogic/aml_kernel_compat.h>
#include <linux/of_platform.h>
#include <linux/timer.h>
#include <linux/uio.h>
#include <sound/core.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>

#ifndef SND_SOC_DAIFMT_CBM_CFM
#define SND_SOC_DAIFMT_CBM_CFM SND_SOC_DAIFMT_CBP_CFP
#endif
#ifndef SND_SOC_DAIFMT_CBM_CFS
#define SND_SOC_DAIFMT_CBM_CFS SND_SOC_DAIFMT_CBP_CFC
#endif
#ifndef SND_SOC_DAIFMT_CBS_CFM
#define SND_SOC_DAIFMT_CBS_CFM SND_SOC_DAIFMT_CBC_CFP
#endif
#ifndef SND_SOC_DAIFMT_CBS_CFS
#define SND_SOC_DAIFMT_CBS_CFS SND_SOC_DAIFMT_CBC_CFC
#endif

#ifndef asoc_rtd_to_cpu
#define asoc_rtd_to_cpu snd_soc_rtd_to_cpu
#endif
#ifndef asoc_rtd_to_codec
#define asoc_rtd_to_codec snd_soc_rtd_to_codec
#endif

#ifndef snd_soc_kcontrol_component
#define snd_soc_kcontrol_component(kcontrol) snd_kcontrol_chip(kcontrol)
#endif

#ifndef from_timer
#define from_timer(var, callback_timer, timer_fieldname) \
	timer_container_of(var, callback_timer, timer_fieldname)
#endif

#ifndef del_timer_sync
#define del_timer_sync(timer) timer_delete_sync(timer)
#endif

#define __aml_snd_soc_of_get_dai_name_2(of_node, dai_name) \
	snd_soc_of_get_dai_name(of_node, dai_name, 0)
#define __aml_snd_soc_of_get_dai_name_3(of_node, dai_name, index) \
	snd_soc_of_get_dai_name(of_node, dai_name, index)
#define __aml_snd_soc_of_get_dai_name_pick(_1, _2, _3, name, ...) name
#define snd_soc_of_get_dai_name(...) \
	__aml_snd_soc_of_get_dai_name_pick(__VA_ARGS__, \
		__aml_snd_soc_of_get_dai_name_3, \
		__aml_snd_soc_of_get_dai_name_2)(__VA_ARGS__)

#endif /* __AML_AUDIO_KERNEL_COMPAT_H__ */
