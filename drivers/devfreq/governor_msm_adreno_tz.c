/* Copyright (c) 2010-2016, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/devfreq.h>
#include <linux/math64.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/ftrace.h>
#include <linux/mm.h>
#include <linux/msm_adreno_devfreq.h>
#include <linux/display_state.h>
#include <asm/cacheflush.h>
#include <soc/qcom/scm.h>
#include "governor.h"

static DEFINE_SPINLOCK(tz_lock);
static DEFINE_SPINLOCK(sample_lock);
static DEFINE_SPINLOCK(suspend_lock);
/*
 * FLOOR is 5msec to capture up to 3 re-draws
 * per frame for 60fps content.
 */
#define FLOOR		        5000
/*
 * MIN_BUSY is 1 msec for the sample to be sent
 */
#define MIN_BUSY		1000
/*
 * Use BUSY_BIN to check for fully busy rendering
 * intervals that may need early intervention when
 * seen with LONG_FRAME lengths
 */
#define BUSY_BIN		95
#define LONG_FRAME		25000
#define MAX_TZ_VERSION		0

/*
 * CEILING is 50msec, larger than any standard
 * frame length, but less than the idle timer.
 */
#define CEILING			50000
#define TZ_RESET_ID		0x3
#define TZ_UPDATE_ID		0x4
#define TZ_INIT_ID		0x6

#define TZ_RESET_ID_64          0x7
#define TZ_UPDATE_ID_64         0x8
#define TZ_INIT_ID_64           0x9

#define TZ_V2_UPDATE_ID_64         0xA
#define TZ_V2_INIT_ID_64           0xB

#define TAG "msm_adreno_tz: "

static u64 suspend_time;
static u64 suspend_start;
static unsigned long acc_total, acc_relative_busy;

static struct msm_adreno_extended_profile *partner_gpu_profile;
static void do_partner_start_event(struct work_struct *work);
static void do_partner_stop_event(struct work_struct *work);
static void do_partner_suspend_event(struct work_struct *work);
static void do_partner_resume_event(struct work_struct *work);

/* Display and suspend state booleans */ 
static bool display_on;
static bool suspended = false;

static struct workqueue_struct *workqueue;

/*
 * Returns GPU suspend time in millisecond.
 */
u64 suspend_time_ms(void)
{
	u64 suspend_sampling_time;
	u64 time_diff = 0;

	if (suspend_start == 0)
		return 0;

	suspend_sampling_time = (u64)ktime_to_ms(ktime_get());
	time_diff = suspend_sampling_time - suspend_start;
	/* Update the suspend_start sample again */
	suspend_start = suspend_sampling_time;
	return time_diff;
}

static ssize_t gpu_load_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	unsigned long sysfs_busy_perc;
	/*
	 * Average out the samples taken since last read
	 * This will keep the average value in sync with
	 * with the client sampling duration.
	 */
	spin_lock(&sample_lock);
	sysfs_busy_perc = (acc_relative_busy * 100) / acc_total;

	/* Reset the parameters */
	acc_total = 0;
	acc_relative_busy = 0;
	spin_unlock(&sample_lock);
	return snprintf(buf, PAGE_SIZE, "%lu\n", sysfs_busy_perc);
}

/*
 * Returns the time in ms for which gpu was in suspend state
 * since last time the entry is read.
 */
static ssize_t suspend_time_show(struct device *dev,
	struct device_attribute *attr,
	char *buf)
{
	u64 time_diff = 0;

	spin_lock(&suspend_lock);
	time_diff = suspend_time_ms();
	/*
	 * Adding the previous suspend time also as the gpu
	 * can go and come out of suspend states in between
	 * reads also and we should have the total suspend
	 * since last read.
	 */
	time_diff += suspend_time;
	suspend_time = 0;
	spin_unlock(&suspend_lock);

	return snprintf(buf, PAGE_SIZE, "%llu\n", time_diff);
}

static DEVICE_ATTR(gpu_load, 0444, gpu_load_show, NULL);

static DEVICE_ATTR(suspend_time, 0444,
		suspend_time_show,
		NULL);

static const struct device_attribute *adreno_tz_attr_list[] = {
		&dev_attr_gpu_load,
		&dev_attr_suspend_time,
		NULL
};

void compute_work_load(struct devfreq_dev_status *stats,
		struct devfreq_msm_adreno_tz_data *priv,
		struct devfreq *devfreq)
{
	spin_lock(&sample_lock);
	/*
	 * Keep collecting the stats till the client
	 * reads it. Average of all samples and reset
	 * is done when the entry is read
	 */
	acc_total += stats->total_time;
	acc_relative_busy += (stats->busy_time * stats->current_frequency) /
				devfreq->profile->freq_table[0];
	spin_unlock(&sample_lock);
}

/* Trap into the TrustZone, and call funcs there. */
static int __secure_tz_reset_entry2(unsigned int *scm_data, u32 size_scm_data,
					bool is_64)
{
	int ret;
	/* sync memory before sending the commands to tz */
	__iowmb();

	if (!is_64) {
		spin_lock(&tz_lock);
		ret = scm_call_atomic2(SCM_SVC_IO, TZ_RESET_ID, scm_data[0],
					scm_data[1]);
		spin_unlock(&tz_lock);
	} else {
		if (is_scm_armv8()) {
			struct scm_desc desc = {0};
			desc.arginfo = 0;
			ret = scm_call2(SCM_SIP_FNID(SCM_SVC_DCVS,
					 TZ_RESET_ID_64), &desc);
		} else {
			ret = scm_call(SCM_SVC_DCVS, TZ_RESET_ID_64, scm_data,
				size_scm_data, NULL, 0);
		}
	}
	return ret;
}

static int __secure_tz_update_entry3(unsigned int *scm_data, u32 size_scm_data,
					int *val, u32 size_val, bool is_64)
{
	int ret;
	/* sync memory before sending the commands to tz */
	__iowmb();

	if (!is_64) {
		spin_lock(&tz_lock);
		ret = scm_call_atomic3(SCM_SVC_IO, TZ_UPDATE_ID,
					scm_data[0], scm_data[1], scm_data[2]);
		spin_unlock(&tz_lock);
		*val = ret;
	} else {
		if (is_scm_armv8()) {
			struct scm_desc desc = {0};
			desc.args[0] = scm_data[0];
			desc.args[1] = scm_data[1];
			desc.args[2] = scm_data[2];
			desc.arginfo = SCM_ARGS(3);
			ret = scm_call2(SCM_SIP_FNID(SCM_SVC_DCVS,
					TZ_V2_UPDATE_ID_64), &desc);
			*val = desc.ret[0];
		} else {
			ret = scm_call(SCM_SVC_DCVS, TZ_UPDATE_ID_64, scm_data,
				size_scm_data, val, size_val);
		}
	}
	return ret;
}

static int tz_init(struct devfreq_msm_adreno_tz_data *priv,
			unsigned int *tz_pwrlevels, u32 size_pwrlevels,
			unsigned int *version, u32 size_version)
{
	int ret;
	/* Make sure all CMD IDs are avaialble */
	if (scm_is_call_available(SCM_SVC_DCVS, TZ_INIT_ID)) {
		ret = scm_call(SCM_SVC_DCVS, TZ_INIT_ID, tz_pwrlevels,
				size_pwrlevels, NULL, 0);
		*version = 0;

	} else if (scm_is_call_available(SCM_SVC_DCVS, TZ_INIT_ID_64) &&
			scm_is_call_available(SCM_SVC_DCVS, TZ_UPDATE_ID_64) &&
			scm_is_call_available(SCM_SVC_DCVS, TZ_RESET_ID_64)) {
		struct scm_desc desc = {0};
		unsigned int *tz_buf;

		if (!is_scm_armv8()) {
			ret = scm_call(SCM_SVC_DCVS, TZ_INIT_ID_64,
				       tz_pwrlevels, size_pwrlevels,
				       version, size_version);
			if (!ret)
				priv->is_64 = true;
			return ret;
		}

		tz_buf = kzalloc(PAGE_ALIGN(size_pwrlevels), GFP_KERNEL);
		if (!tz_buf)
			return -ENOMEM;
		memcpy(tz_buf, tz_pwrlevels, size_pwrlevels);
		/* Ensure memcpy completes execution */
		mb();
		dmac_flush_range(tz_buf, tz_buf + PAGE_ALIGN(size_pwrlevels));

		desc.args[0] = virt_to_phys(tz_buf);
		desc.args[1] = size_pwrlevels;
		desc.arginfo = SCM_ARGS(2, SCM_RW, SCM_VAL);

		ret = scm_call2(SCM_SIP_FNID(SCM_SVC_DCVS, TZ_V2_INIT_ID_64),
				&desc);
		*version = desc.ret[0];
		if (!ret)
			priv->is_64 = true;
		kzfree(tz_buf);
	} else
		ret = -EINVAL;

	return ret;
}

#ifdef CONFIG_ADRENO_IDLER
extern int adreno_idler(struct devfreq_dev_status stats, struct devfreq *devfreq,
		 unsigned long *freq);
#endif
static int tz_get_target_freq(struct devfreq *devfreq, unsigned long *freq,
				u32 *flag)
{
	int result = 0;
	struct devfreq_msm_adreno_tz_data *priv = devfreq->data;
	struct devfreq_dev_status stats;
	int val, level = 0;
	unsigned int scm_data[3];
	static int busy_bin, frame_flag;

	/* keeps stats.private_data == NULL   */
	result = devfreq->profile->get_dev_status(devfreq->dev.parent, &stats);
	if (result) {
		pr_err(TAG "get_status failed %d\n", result);
		return result;
	}

	/* Prevent overflow */
	if (stats.busy_time >= (1 << 24) || stats.total_time >= (1 << 24)) {
		stats.busy_time >>= 7;
		stats.total_time >>= 7;
	}

	*freq = stats.current_frequency;

	/*
	 * Force to use & record as min freq when system has
	 * entered pm-suspend or screen-off state.
	 */
	if (suspended || !display_on) {
		*freq = devfreq->profile->freq_table[devfreq->profile->max_state - 1];
		return 0;
	}

#ifdef CONFIG_ADRENO_IDLER
	if (adreno_idler(stats, devfreq, freq)) {
		/* adreno_idler has asked to bail out now */
		return 0;
	}
#endif

	priv->bin.total_time += stats.total_time;
	priv->bin.busy_time += stats.busy_time;

	/* Update the GPU load statistics */
	compute_work_load(&stats, priv, devfreq);
	/*
	 * Do not waste CPU cycles running this algorithm if
	 * the GPU just started, or if less than FLOOR time
	 * has passed since the last run or the gpu hasn't been
	 * busier than MIN_BUSY.
	 */
	if ((stats.total_time == 0) ||
		(priv->bin.total_time < FLOOR) ||
		(unsigned int) priv->bin.busy_time < MIN_BUSY) {
		return 0;
	}

	if ((stats.busy_time * 100 / stats.total_time) > BUSY_BIN) {
		busy_bin += stats.busy_time;
		if (stats.total_time > LONG_FRAME)
			frame_flag = 1;
	} else {
		busy_bin = 0;
		frame_flag = 0;
	}

	level = devfreq_get_freq_level(devfreq, stats.current_frequency);
	if (level < 0) {
		pr_err(TAG "bad freq %ld\n", stats.current_frequency);
		return level;
	}

	/*
	 * If there is an extended block of busy processing,
	 * increase frequency.  Otherwise run the normal algorithm.
	 */
	if (priv->bin.busy_time > CEILING ||
		(busy_bin > CEILING && frame_flag)) {
		val = -1 * level;
		busy_bin = 0;
		frame_flag = 0;
	} else {

		scm_data[0] = level;
		scm_data[1] = priv->bin.total_time;
		scm_data[2] = priv->bin.busy_time;
		__secure_tz_update_entry3(scm_data, sizeof(scm_data),
					&val, sizeof(val), priv->is_64);
	}

	// AP: Tweak 27 MHz frequency to be used a bit more
	if ((val == 0) && (level == 5) &&	// (5 = 180 MHz step)
		((priv->bin.busy_time * 100 / priv->bin.total_time) < 98))
		val = 1;

	priv->bin.total_time = 0;
	priv->bin.busy_time = 0;

	// AP: Tweak not to peak up when we come from 27 MHz and need to ramp up
	if ((val < -1) && (level == 6))
		val = -1;

	// AP: In general we do not ramp up more than 2 steps at once
	if (val < -2)
		val = -2;

	/*
	 * If the decision is to move to a different level, make sure the GPU
	 * frequency changes.
	 */
	if (val) {
		level += val;
		level = max(level, 0);
		level = min_t(int, level, devfreq->profile->max_state - 1);
	}

	*freq = devfreq->profile->freq_table[level];
	return 0;
}

static int tz_notify(struct notifier_block *nb, unsigned long type, void *devp)
{
	int result = 0;
	struct devfreq *devfreq = devp;

	switch (type) {
	case ADRENO_DEVFREQ_NOTIFY_IDLE:
	case ADRENO_DEVFREQ_NOTIFY_RETIRE:
		mutex_lock(&devfreq->lock);
		result = update_devfreq(devfreq);
		mutex_unlock(&devfreq->lock);
		/* Nofifying partner bus governor if any */
		if (partner_gpu_profile && partner_gpu_profile->bus_devfreq) {
			mutex_lock(&partner_gpu_profile->bus_devfreq->lock);
			update_devfreq(partner_gpu_profile->bus_devfreq);
			mutex_unlock(&partner_gpu_profile->bus_devfreq->lock);
		}
		break;
	/* ignored by this governor */
	case ADRENO_DEVFREQ_NOTIFY_SUBMIT:
	default:
		break;
	}
	return notifier_from_errno(result);
}

static int tz_start(struct devfreq *devfreq)
{
	struct devfreq_msm_adreno_tz_data *priv;
	unsigned int tz_pwrlevels[MSM_ADRENO_MAX_PWRLEVELS + 1];
	int i, out, ret;
	unsigned int version;

	struct msm_adreno_extended_profile *gpu_profile = container_of(
					(devfreq->profile),
					struct msm_adreno_extended_profile,
					profile);

	/*
	 * Assuming that we have only one instance of the adreno device
	 * connected to this governor,
	 * can safely restore the pointer to the governor private data
	 * from the container of the device profile
	 */
	devfreq->data = gpu_profile->private_data;
	partner_gpu_profile = gpu_profile;

	priv = devfreq->data;
	priv->nb.notifier_call = tz_notify;

	out = 1;
	if (devfreq->profile->max_state < MSM_ADRENO_MAX_PWRLEVELS) {
		for (i = 0; i < devfreq->profile->max_state; i++)
			tz_pwrlevels[out++] = devfreq->profile->freq_table[i];
		tz_pwrlevels[0] = i;
	} else {
		pr_err(TAG "tz_pwrlevels[] is too short\n");
		return -EINVAL;
	}

	INIT_WORK(&gpu_profile->partner_start_event_ws,
					do_partner_start_event);
	INIT_WORK(&gpu_profile->partner_stop_event_ws,
					do_partner_stop_event);
	INIT_WORK(&gpu_profile->partner_suspend_event_ws,
					do_partner_suspend_event);
	INIT_WORK(&gpu_profile->partner_resume_event_ws,
					do_partner_resume_event);

	ret = tz_init(priv, tz_pwrlevels, sizeof(tz_pwrlevels), &version,
				sizeof(version));
	if (ret != 0 || version > MAX_TZ_VERSION) {
		pr_err(TAG "tz_init failed\n");
		return ret;
	}

	for (i = 0; adreno_tz_attr_list[i] != NULL; i++)
		device_create_file(&devfreq->dev, adreno_tz_attr_list[i]);

	return kgsl_devfreq_add_notifier(devfreq->dev.parent, &priv->nb);
}

static int tz_stop(struct devfreq *devfreq)
{
	int i;
	struct devfreq_msm_adreno_tz_data *priv = devfreq->data;

	kgsl_devfreq_del_notifier(devfreq->dev.parent, &priv->nb);

	for (i = 0; adreno_tz_attr_list[i] != NULL; i++)
		device_remove_file(&devfreq->dev, adreno_tz_attr_list[i]);

	flush_workqueue(workqueue);

	/* leaving the governor and cleaning the pointer to private data */
	devfreq->data = NULL;
	partner_gpu_profile = NULL;
	return 0;
}

static int tz_suspend(struct devfreq *devfreq)
{
	struct devfreq_msm_adreno_tz_data *priv = devfreq->data;
	unsigned int scm_data[2] = {0, 0};
	__secure_tz_reset_entry2(scm_data, sizeof(scm_data), priv->is_64);
	display_on = is_display_on();
	suspended = true;

	priv->bin.total_time = 0;
	priv->bin.busy_time = 0;

	return 0;
}

static int tz_handler(struct devfreq *devfreq, unsigned int event, void *data)
{
	int result;

	struct msm_adreno_extended_profile *gpu_profile = container_of(
					(devfreq->profile),
					struct msm_adreno_extended_profile,
					profile);
	BUG_ON(devfreq == NULL);

	switch (event) {
	case DEVFREQ_GOV_START:
		result = tz_start(devfreq);
		break;

	case DEVFREQ_GOV_STOP:
		/* Queue the stop work before the TZ is stopped */
		if (partner_gpu_profile && partner_gpu_profile->bus_devfreq)
			queue_work(workqueue,
				&gpu_profile->partner_stop_event_ws);
		spin_lock(&suspend_lock);
		suspend_start = 0;
		spin_unlock(&suspend_lock);
		result = tz_stop(devfreq);
		break;

	case DEVFREQ_GOV_SUSPEND:
		result = tz_suspend(devfreq);
		if (!result) {
			spin_lock(&suspend_lock);
			/* Collect the start sample for suspend time */
			suspend_start = (u64)ktime_to_ms(ktime_get());
			spin_unlock(&suspend_lock);
		}
		break;

	case DEVFREQ_GOV_RESUME:
		spin_lock(&suspend_lock);
		suspend_time += suspend_time_ms();
		display_on = is_display_on();
		suspended = false;
		/* Reset the suspend_start when gpu resumes */
		suspend_start = 0;
		spin_unlock(&suspend_lock);

	case DEVFREQ_GOV_INTERVAL:
		/* ignored, this governor doesn't use polling */
	default:
		result = 0;
		break;
	}

	if (partner_gpu_profile && partner_gpu_profile->bus_devfreq)
		switch (event) {
		case DEVFREQ_GOV_START:
			queue_work(workqueue,
					&gpu_profile->partner_start_event_ws);
			break;
		case DEVFREQ_GOV_SUSPEND:
			queue_work(workqueue,
					&gpu_profile->partner_suspend_event_ws);
			break;
		case DEVFREQ_GOV_RESUME:
			queue_work(workqueue,
					&gpu_profile->partner_resume_event_ws);
			break;
		}

	return result;
}

static void _do_partner_event(struct work_struct *work, unsigned int event)
{
	struct devfreq *bus_devfreq;

	if (partner_gpu_profile == NULL)
		return;

	bus_devfreq = partner_gpu_profile->bus_devfreq;

	if (bus_devfreq != NULL &&
		bus_devfreq->governor &&
		bus_devfreq->governor->event_handler)
		bus_devfreq->governor->event_handler(bus_devfreq, event, NULL);
}

static void do_partner_start_event(struct work_struct *work)
{
	_do_partner_event(work, DEVFREQ_GOV_START);
}

static void do_partner_stop_event(struct work_struct *work)
{
	_do_partner_event(work, DEVFREQ_GOV_STOP);
}

static void do_partner_suspend_event(struct work_struct *work)
{
	_do_partner_event(work, DEVFREQ_GOV_SUSPEND);
}

static void do_partner_resume_event(struct work_struct *work)
{
	_do_partner_event(work, DEVFREQ_GOV_RESUME);
}


static struct devfreq_governor msm_adreno_tz = {
	.name = "msm-adreno-tz",
	.get_target_freq = tz_get_target_freq,
	.event_handler = tz_handler,
};

static int __init msm_adreno_tz_init(void)
{
	workqueue = create_freezable_workqueue("governor_msm_adreno_tz_wq");
	if (workqueue == NULL)
		return -ENOMEM;

	return devfreq_add_governor(&msm_adreno_tz);
}
subsys_initcall(msm_adreno_tz_init);

static void __exit msm_adreno_tz_exit(void)
{
	int ret = devfreq_remove_governor(&msm_adreno_tz);
	if (ret)
		pr_err(TAG "failed to remove governor %d\n", ret);

	if (workqueue != NULL)
		destroy_workqueue(workqueue);
}

module_exit(msm_adreno_tz_exit);

MODULE_LICENSE("GPLv2");
