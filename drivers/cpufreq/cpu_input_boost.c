// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018-2019 Sultan Alsawaf <sultan@kerneltoast.com>.
 * Copyright (C) 2021 ZyCromerZ <neetroid97@gmail.com>.
 */

#define pr_fmt(fmt) "cpu_input_boost: " fmt

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/msm_drm_notify.h>
#include <linux/input.h>
#include <linux/kthread.h>
#include <linux/version.h>
#include <linux/slab.h>
#include <linux/moduleparam.h>

/* The sched_param struct is located elsewhere in newer kernels */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
#include <uapi/linux/sched/types.h>
#endif

static unsigned int __read_mostly enabled = 0;

static int __init read_enabled_status(char *s)
{
    int EStats = 0;
	if (s)
		EStats = simple_strtoul(s, NULL, 0);

    if (EStats > 0)
        enabled = 1;
    else
        enabled = 0;

	return 1;
}
__setup("zyc.cib=", read_enabled_status);

module_param(enabled, uint, 0644);
static bool __read_mostly skip_prime_cores = false;
module_param(skip_prime_cores, bool, 0644);
static unsigned int __read_mostly input_boost_duration = CONFIG_INPUT_BOOST_DURATION_MS;
module_param(input_boost_duration, uint, 0644);
static unsigned int __read_mostly wake_boost_duration = CONFIG_WAKE_BOOST_DURATION_MS;
module_param(wake_boost_duration, uint, 0644);
static unsigned int __read_mostly input_boost_freq_lp = CONFIG_INPUT_BOOST_FREQ_LP;
module_param(input_boost_freq_lp, uint, 0644);
static unsigned int __read_mostly input_boost_freq_hp = CONFIG_INPUT_BOOST_FREQ_PERF;
module_param(input_boost_freq_hp, uint, 0644);
static unsigned int __read_mostly input_boost_freq_prime = CONFIG_INPUT_BOOST_FREQ_PRIME;
module_param(input_boost_freq_prime, uint, 0644);
static unsigned int __read_mostly max_boost_freq_lp = CONFIG_MAX_BOOST_FREQ_LP;
module_param(max_boost_freq_lp, uint, 0644);
static unsigned int __read_mostly max_boost_freq_hp = CONFIG_MAX_BOOST_FREQ_PERF;
module_param(max_boost_freq_hp, uint, 0644);
static unsigned int __read_mostly max_boost_freq_prime = CONFIG_MAX_BOOST_FREQ_PRIME;
module_param(max_boost_freq_prime, uint, 0644);
static unsigned int __read_mostly min_freq_lp = CONFIG_MIN_FREQ_LP;
module_param(min_freq_lp, uint, 0644);
static unsigned int __read_mostly min_freq_hp = CONFIG_MIN_FREQ_PERF;
module_param(min_freq_hp, uint, 0644);
static unsigned int __read_mostly min_freq_prime = CONFIG_MIN_FREQ_PRIME;
module_param(min_freq_prime, uint, 0644);
static unsigned int __read_mostly idle_freq_lp = CONFIG_IDLE_FREQ_LP;
module_param(idle_freq_lp, uint, 0644);
static unsigned int __read_mostly idle_freq_hp = CONFIG_IDLE_FREQ_PERF;
module_param(idle_freq_hp, uint, 0644);
static unsigned int __read_mostly idle_freq_prime = CONFIG_IDLE_FREQ_PRIME;
module_param(idle_freq_prime, uint, 0644);

// for cpu0-cpu3 / 00001111
static const unsigned long real_lp_cpu_bits = 15;
const struct cpumask *const real_cpu_lp_mask = to_cpumask(&real_lp_cpu_bits);
// for cpu4-cpu6 / 01110000
static const unsigned long real_perf_cpu_bits = 112;
const struct cpumask *const real_cpu_perf_mask = to_cpumask(&real_perf_cpu_bits);
// for cpu7 / 10000000
static const unsigned long real_prime_cpu_bits = 128;
const struct cpumask *const real_cpu_prime_mask = to_cpumask(&real_prime_cpu_bits);

enum {
	SCREEN_OFF,
	INPUT_BOOST,
	MAX_BOOST
};

struct boost_drv {
	struct delayed_work input_unboost;
	struct delayed_work max_unboost;
	struct notifier_block cpu_notif;
	struct notifier_block msm_drm_notif;
	wait_queue_head_t boost_waitq;
	atomic_long_t max_boost_expires;
	unsigned long state;
};

static void input_unboost_worker(struct work_struct *work);
static void max_unboost_worker(struct work_struct *work);

static struct boost_drv boost_drv_g __read_mostly = {
	.input_unboost = __DELAYED_WORK_INITIALIZER(boost_drv_g.input_unboost,
						    input_unboost_worker, 0),
	.max_unboost = __DELAYED_WORK_INITIALIZER(boost_drv_g.max_unboost,
						  max_unboost_worker, 0),
	.boost_waitq = __WAIT_QUEUE_HEAD_INITIALIZER(boost_drv_g.boost_waitq)
};

static unsigned int get_input_boost_freq(struct cpufreq_policy *policy)
{
	unsigned int freq;

	if (cpumask_test_cpu(policy->cpu, real_cpu_lp_mask)) {
		freq = max(input_boost_freq_lp, min_freq_lp);
	} else if (cpumask_test_cpu(policy->cpu, real_cpu_perf_mask)) {
		freq = max(input_boost_freq_hp, min_freq_hp);
	} else {
		freq = max(input_boost_freq_prime, min_freq_prime);
		if ( skip_prime_cores ) {
			freq = policy->cpuinfo.min_freq;
		}
	}

	if (freq == 0)
		freq = policy->min;

	return min(freq, policy->max);
}

static unsigned int get_max_boost_freq(struct cpufreq_policy *policy)
{
	unsigned int freq;

	if (cpumask_test_cpu(policy->cpu, real_cpu_lp_mask)) {
		freq = max_boost_freq_lp;
		if (freq == 0) {
			freq = input_boost_freq_lp;
		}
	} else if (cpumask_test_cpu(policy->cpu, real_cpu_perf_mask)) {
		freq = max_boost_freq_hp;
		if (freq == 0) {
			freq = input_boost_freq_hp;
		}
	} else {
		freq = max_boost_freq_prime;
		if (freq == 0) {
			freq = input_boost_freq_prime;
		}
		if ( skip_prime_cores ) {
			freq = policy->cpuinfo.min_freq;
		}
	}

	if (freq == 0)
		freq = policy->cpuinfo.max_freq;

	return min(freq, policy->max);
}

static unsigned int get_min_freq(struct cpufreq_policy *policy)
{
	unsigned int freq;

	if (cpumask_test_cpu(policy->cpu, real_cpu_lp_mask)) {
			freq = min_freq_lp;
	} else if (cpumask_test_cpu(policy->cpu, real_cpu_perf_mask)) {
			freq = min_freq_hp;
	} else {
			freq = min_freq_prime;
			if ( skip_prime_cores ) {
				freq = policy->cpuinfo.min_freq;
			}
	}

	return max(freq, policy->cpuinfo.min_freq);
}

static unsigned int get_idle_freq(struct cpufreq_policy *policy)
{
	unsigned int freq;

	if (cpumask_test_cpu(policy->cpu, real_cpu_lp_mask)) {
		freq = idle_freq_lp;
	} else if (cpumask_test_cpu(policy->cpu, real_cpu_perf_mask)) {
		freq = idle_freq_hp;
	} else {
		freq = idle_freq_prime;
		if ( skip_prime_cores ) {
			freq = policy->cpuinfo.min_freq;
		}
	}
	if ( freq == 0 )
		freq = policy->cpuinfo.min_freq;

	return min(freq, policy->cpuinfo.min_freq);
}

static void update_online_cpu_policy(void)
{
	unsigned int cpu;

	get_online_cpus();
	for_each_possible_cpu(cpu) {
		if (cpu_online(cpu)) {
			if (cpumask_intersects(cpumask_of(cpu), real_cpu_lp_mask))
				cpufreq_update_policy(cpu);
			if (cpumask_intersects(cpumask_of(cpu), real_cpu_perf_mask))
				cpufreq_update_policy(cpu);
			if (cpumask_intersects(cpumask_of(cpu), real_cpu_prime_mask))
				cpufreq_update_policy(cpu);
		}
	}
	put_online_cpus();
}

static void __cpu_input_boost_kick(struct boost_drv *b)
{
	if (enabled == 0)
		return;

	if (test_bit(SCREEN_OFF, &b->state) || (input_boost_duration == 0))
		return;

	set_bit(INPUT_BOOST, &b->state);
	if (!mod_delayed_work(system_unbound_wq, &b->input_unboost,
			      msecs_to_jiffies(input_boost_duration)))
		wake_up(&b->boost_waitq);
}

void cpu_input_boost_kick(void)
{
	struct boost_drv *b = &boost_drv_g;

	__cpu_input_boost_kick(b);
}

static void __cpu_input_boost_kick_max(struct boost_drv *b,
				       unsigned int duration_ms)
{
	unsigned long boost_jiffies = msecs_to_jiffies(duration_ms);
	unsigned long curr_expires, new_expires;

	if (enabled == 0)
		return;

	if (test_bit(SCREEN_OFF, &b->state))
		return;

	do {
		curr_expires = atomic_long_read(&b->max_boost_expires);
		new_expires = jiffies + boost_jiffies;

		/* Skip this boost if there's a longer boost in effect */
		if (time_after(curr_expires, new_expires))
			return;
	} while (atomic_long_cmpxchg(&b->max_boost_expires, curr_expires,
				     new_expires) != curr_expires);

	set_bit(MAX_BOOST, &b->state);
	if (!mod_delayed_work(system_unbound_wq, &b->max_unboost,
			      boost_jiffies))
		wake_up(&b->boost_waitq);
}

void cpu_input_boost_kick_max(unsigned int duration_ms)
{
	struct boost_drv *b = &boost_drv_g;

	__cpu_input_boost_kick_max(b, duration_ms);
}

static void input_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(to_delayed_work(work),
					   typeof(*b), input_unboost);

	clear_bit(INPUT_BOOST, &b->state);
	wake_up(&b->boost_waitq);
}

static void max_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(to_delayed_work(work),
					   typeof(*b), max_unboost);

	clear_bit(MAX_BOOST, &b->state);
	wake_up(&b->boost_waitq);
}

static int cpu_boost_thread(void *data)
{
	static const struct sched_param sched_max_rt_prio = {
		.sched_priority = MAX_RT_PRIO - 1
	};
	struct boost_drv *b = data;
	unsigned long old_state = 0;

	sched_setscheduler_nocheck(current, SCHED_FIFO, &sched_max_rt_prio);

	while (1) {
		bool should_stop = false;
		unsigned long curr_state;

		wait_event(b->boost_waitq,
			(curr_state = READ_ONCE(b->state)) != old_state ||
			(should_stop = kthread_should_stop()));

		if (should_stop)
			break;

		old_state = curr_state;
		update_online_cpu_policy();
	}

	return 0;
}

static int cpu_notifier_cb(struct notifier_block *nb, unsigned long action,
			   void *data)
{
	struct boost_drv *b = container_of(nb, typeof(*b), cpu_notif);
	struct cpufreq_policy *policy = data;

	if (enabled == 0) {
		policy->min = policy->cpuinfo.min_freq;
		return NOTIFY_OK;
	}

	if (enabled > 1)
		enabled = 1;

	else if (enabled < 0)
		enabled = 0;

	if (action != CPUFREQ_ADJUST)
		return NOTIFY_OK;

	/* Unboost when the screen is off */
	if (test_bit(SCREEN_OFF, &b->state)) {
		policy->min = get_idle_freq(policy);
		return NOTIFY_OK;
	}

	/* Boost CPU to max frequency for max boost */
	if (test_bit(MAX_BOOST, &b->state)) {
		policy->min = get_max_boost_freq(policy);
		return NOTIFY_OK;
	}

	/*
	 * Boost to policy->max if the boost frequency is higher. When
	 * unboosting, set policy->min to the absolute min freq for the CPU.
	 */
	if (test_bit(INPUT_BOOST, &b->state))
		policy->min = get_input_boost_freq(policy);
	else
		policy->min = get_min_freq(policy);

	return NOTIFY_OK;
}

static int msm_drm_notifier_cb(struct notifier_block *nb, unsigned long action,
			  void *data)
{
	struct boost_drv *b = container_of(nb, typeof(*b), msm_drm_notif);
	struct msm_drm_notifier *evdata = data;
	int *blank = evdata->data;

	if (enabled == 0)
		return NOTIFY_OK;

	/* Parse framebuffer blank events as soon as they occur */
	if (action != MSM_DRM_EARLY_EVENT_BLANK)
		return NOTIFY_OK;

	/* Boost when the screen turns on and unboost when it turns off */
	if (*blank == MSM_DRM_BLANK_UNBLANK) {
		clear_bit(SCREEN_OFF, &b->state);
		__cpu_input_boost_kick_max(b, wake_boost_duration);
	} else {
		set_bit(SCREEN_OFF, &b->state);
		wake_up(&b->boost_waitq);
	}

	return NOTIFY_OK;
}

static void cpu_input_boost_input_event(struct input_handle *handle,
					unsigned int type, unsigned int code,
					int value)
{
	struct boost_drv *b = handle->handler->private;

	__cpu_input_boost_kick(b);

}

static int cpu_input_boost_input_connect(struct input_handler *handler,
					 struct input_dev *dev,
					 const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "cpu_input_boost_handle";

	ret = input_register_handle(handle);
	if (ret)
		goto free_handle;

	ret = input_open_device(handle);
	if (ret)
		goto unregister_handle;

	return 0;

unregister_handle:
	input_unregister_handle(handle);
free_handle:
	kfree(handle);
	return ret;
}

static void cpu_input_boost_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id cpu_input_boost_ids[] = {
	/* Multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) }
	},
	/* Touchpad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) }
	},
	/* Keypad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) }
	},
	/* Power Key */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(KEY_POWER) }
	},
	{ }
};

static struct input_handler cpu_input_boost_input_handler = {
	.event		= cpu_input_boost_input_event,
	.connect	= cpu_input_boost_input_connect,
	.disconnect	= cpu_input_boost_input_disconnect,
	.name		= "cpu_input_boost_handler",
	.id_table	= cpu_input_boost_ids
};

static int __init cpu_input_boost_init(void)
{
	struct boost_drv *b = &boost_drv_g;
	struct task_struct *thread;
	int ret;

	b->cpu_notif.notifier_call = cpu_notifier_cb;
	ret = cpufreq_register_notifier(&b->cpu_notif, CPUFREQ_POLICY_NOTIFIER);
	if (ret) {
		pr_err("Failed to register cpufreq notifier, err: %d\n", ret);
		return ret;
	}

	cpu_input_boost_input_handler.private = b;
	ret = input_register_handler(&cpu_input_boost_input_handler);
	if (ret) {
		pr_err("Failed to register input handler, err: %d\n", ret);
		goto unregister_cpu_notif;
	}

	b->msm_drm_notif.notifier_call = msm_drm_notifier_cb;
	b->msm_drm_notif.priority = INT_MAX;
	ret = msm_drm_register_client(&b->msm_drm_notif);
	if (ret) {
		pr_err("Failed to register msm_drm notifier, err: %d\n", ret);
		goto unregister_handler;
	}

	thread = kthread_run_perf_critical(cpu_perf_mask, cpu_boost_thread, b, "cpu_boostd");
	if (IS_ERR(thread)) {
		ret = PTR_ERR(thread);
		pr_err("Failed to start CPU boost thread, err: %d\n", ret);
		goto unregister_fb_notif;
	}

	return 0;

unregister_fb_notif:
	msm_drm_unregister_client(&b->msm_drm_notif);
unregister_handler:
	input_unregister_handler(&cpu_input_boost_input_handler);
unregister_cpu_notif:
	cpufreq_unregister_notifier(&b->cpu_notif, CPUFREQ_POLICY_NOTIFIER);
	return ret;
}
subsys_initcall(cpu_input_boost_init);
