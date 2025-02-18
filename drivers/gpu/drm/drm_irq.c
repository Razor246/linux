/*
 * drm_irq.c IRQ and vblank support
 *
 * \author Rickard E. (Rik) Faith <faith@valinux.com>
 * \author Gareth Hughes <gareth@valinux.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * VA LINUX SYSTEMS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Created: Fri Mar 19 14:30:16 1999 by faith@valinux.com
 *
 * Copyright 1999, 2000 Precision Insight, Inc., Cedar Park, Texas.
 * Copyright 2000 VA Linux Systems, Inc., Sunnyvale, California.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * VA LINUX SYSTEMS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <drm/drm_irq.h>
#include <drm/drmP.h>

#include <linux/interrupt.h>	/* For task queue support */

#include <linux/vgaarb.h>
#include <linux/export.h>

#include "drm_internal.h"

/**
 * DOC: irq helpers
 *
 * The DRM core provides very simple support helpers to enable IRQ handling on a
 * device through the drm_irq_install() and drm_irq_uninstall() functions. This
 * only supports devices with a single interrupt on the main device stored in
 * &drm_device.dev and set as the device paramter in drm_dev_alloc().
 *
 * These IRQ helpers are strictly optional. Drivers which roll their own only
 * need to set &drm_device.irq_enabled to signal the DRM core that vblank
 * interrupts are working. Since these helpers don't automatically clean up the
 * requested interrupt like e.g. devm_request_irq() they're not really
 * recommended.
 */

/**
 * drm_irq_install - install IRQ handler
 * @dev: DRM device
 * @irq: IRQ number to install the handler for
 *
 * Initializes the IRQ related data. Installs the handler, calling the driver
 * &drm_driver.irq_preinstall and &drm_driver.irq_postinstall functions before
 * and after the installation.
 *
 * This is the simplified helper interface provided for drivers with no special
 * needs. Drivers which need to install interrupt handlers for multiple
 * interrupts must instead set &drm_device.irq_enabled to signal the DRM core
 * that vblank interrupts are available.
 *
 * @irq must match the interrupt number that would be passed to request_irq(),
 * if called directly instead of using this helper function.
 *
 * &drm_driver.irq_handler is called to handle the registered interrupt.
 *
 * Returns:
 * Zero on success or a negative error code on failure.
 */
int drm_irq_install(struct drm_device *dev, int irq)
{
	int ret;
	unsigned long sh_flags = 0;

	if (irq == 0)
		return -EINVAL;

	/* Driver must have been initialized */
	if (!dev->dev_private)
		return -EINVAL;

	if (dev->irq_enabled)
		return -EBUSY;
	dev->irq_enabled = true;

	DRM_DEBUG("irq=%d\n", irq);

	/* Before installing handler */
	if (dev->driver->irq_preinstall)
		dev->driver->irq_preinstall(dev);

	/* PCI devices require shared interrupts. */
	if (dev->pdev)
		sh_flags = IRQF_SHARED;

	ret = request_irq(irq, dev->driver->irq_handler,
			  sh_flags, dev->driver->name, dev);

	if (ret < 0) {
		dev->irq_enabled = false;
		return ret;
	}

	/* After installing handler */
	if (dev->driver->irq_postinstall)
		ret = dev->driver->irq_postinstall(dev);

	if (ret < 0) {
		dev->irq_enabled = false;
		if (drm_core_check_feature(dev, DRIVER_LEGACY))
			vga_client_register(dev->pdev, NULL, NULL, NULL);
		free_irq(irq, dev);
	} else {
		dev->irq = irq;
	}

	return ret;
}
EXPORT_SYMBOL(drm_irq_install);

/**
 * drm_irq_uninstall - uninstall the IRQ handler
 * @dev: DRM device
 *
 * Calls the driver's &drm_driver.irq_uninstall function and unregisters the IRQ
 * handler.  This should only be called by drivers which used drm_irq_install()
 * to set up their interrupt handler. Other drivers must only reset
 * &drm_device.irq_enabled to false.
 *
 * Note that for kernel modesetting drivers it is a bug if this function fails.
 * The sanity checks are only to catch buggy user modesetting drivers which call
 * the same function through an ioctl.
 *
 * Returns:
 * Zero on success or a negative error code on failure.
 */
int drm_irq_uninstall(struct drm_device *dev)
{
	unsigned long irqflags;
	bool irq_enabled;
	int i;

	irq_enabled = dev->irq_enabled;
	dev->irq_enabled = false;

	/*
	 * Wake up any waiters so they don't hang. This is just to paper over
	 * issues for UMS drivers which aren't in full control of their
	 * vblank/irq handling. KMS drivers must ensure that vblanks are all
	 * disabled when uninstalling the irq handler.
	 */
	if (dev->num_crtcs) {
		spin_lock_irqsave(&dev->vbl_lock, irqflags);
		for (i = 0; i < dev->num_crtcs; i++) {
			struct drm_vblank_crtc *vblank = &dev->vblank[i];

			if (!vblank->enabled)
				continue;

			WARN_ON(drm_core_check_feature(dev, DRIVER_MODESET));

			drm_vblank_disable_and_save(dev, i);
			wake_up(&vblank->queue);
		}
		spin_unlock_irqrestore(&dev->vbl_lock, irqflags);
	}

	if (!irq_enabled)
		return -EINVAL;

	DRM_DEBUG("irq=%d\n", dev->irq);

	if (drm_core_check_feature(dev, DRIVER_LEGACY))
		vga_client_register(dev->pdev, NULL, NULL, NULL);

	if (dev->driver->irq_uninstall)
		dev->driver->irq_uninstall(dev);

	free_irq(dev->irq, dev);

	return 0;
}
EXPORT_SYMBOL(drm_irq_uninstall);

#if IS_ENABLED(CONFIG_DRM_LEGACY)
int drm_legacy_irq_control(struct drm_device *dev, void *data,
			   struct drm_file *file_priv)
{
	struct drm_control *ctl = data;
	int ret = 0, irq;

	/* if we haven't irq we fallback for compatibility reasons -
	 * this used to be a separate function in drm_dma.h
	 */

	if (!drm_core_check_feature(dev, DRIVER_HAVE_IRQ))
		return 0;
	if (!drm_core_check_feature(dev, DRIVER_LEGACY))
		return 0;
	/* UMS was only ever supported on pci devices. */
	if (WARN_ON(!dev->pdev))
		return -EINVAL;

	switch (ctl->func) {
	case DRM_INST_HANDLER:
		irq = dev->pdev->irq;

		if (dev->if_version < DRM_IF_VERSION(1, 2) &&
		    ctl->irq != irq)
			return -EINVAL;
		mutex_lock(&dev->struct_mutex);
		ret = drm_irq_install(dev, irq);
		mutex_unlock(&dev->struct_mutex);

		return ret;
	case DRM_UNINST_HANDLER:
		mutex_lock(&dev->struct_mutex);
		ret = drm_irq_uninstall(dev);
		mutex_unlock(&dev->struct_mutex);

		return ret;
	default:
		return -EINVAL;
	}
}
#endif
/**
 * drm_calc_timestamping_constants - calculate vblank timestamp constants
 * @crtc: drm_crtc whose timestamp constants should be updated.
 * @mode: display mode containing the scanout timings
 *
 * Calculate and store various constants which are later
 * needed by vblank and swap-completion timestamping, e.g,
 * by drm_calc_vbltimestamp_from_scanoutpos(). They are
 * derived from CRTC's true scanout timing, so they take
 * things like panel scaling or other adjustments into account.
 */
void drm_calc_timestamping_constants(struct drm_crtc *crtc,
				     const struct drm_display_mode *mode)
{
	struct drm_device *dev = crtc->dev;
	unsigned int pipe = drm_crtc_index(crtc);
	struct drm_vblank_crtc *vblank = &dev->vblank[pipe];
	int linedur_ns = 0, framedur_ns = 0;
	int dotclock = mode->crtc_clock;
	if (!dev->num_crtcs)
		return;
	if (WARN_ON(pipe >= dev->num_crtcs))
		return;
	/* Valid dotclock? */
	if (dotclock > 0) {
		int frame_size = mode->crtc_htotal * mode->crtc_vtotal;
		/*
		 * Convert scanline length in pixels and video
		 * dot clock to line duration and frame duration
		 * in nanoseconds:
		 */
		linedur_ns  = div_u64((u64) mode->crtc_htotal * 1000000, dotclock);
		framedur_ns = div_u64((u64) frame_size * 1000000, dotclock);
		/*
		 * Fields of interlaced scanout modes are only half a frame duration.
		 */
		if (mode->flags & DRM_MODE_FLAG_INTERLACE)
			framedur_ns /= 2;
	} else
		DRM_ERROR("crtc %u: Can't calculate constants, dotclock = 0!\n",
			  crtc->base.id);
	vblank->linedur_ns  = linedur_ns;
	vblank->framedur_ns = framedur_ns;
	DRM_DEBUG("crtc %u: hwmode: htotal %d, vtotal %d, vdisplay %d\n",
		  crtc->base.id, mode->crtc_htotal,
		  mode->crtc_vtotal, mode->crtc_vdisplay);
	DRM_DEBUG("crtc %u: clock %d kHz framedur %d linedur %d\n",
		  crtc->base.id, dotclock, framedur_ns, linedur_ns);
}
EXPORT_SYMBOL(drm_calc_timestamping_constants);
/**
 * drm_calc_vbltimestamp_from_scanoutpos - precise vblank timestamp helper
 * @dev: DRM device
 * @pipe: index of CRTC whose vblank timestamp to retrieve
 * @max_error: Desired maximum allowable error in timestamps (nanosecs)
 *             On return contains true maximum error of timestamp
 * @vblank_time: Pointer to struct timeval which should receive the timestamp
 * @flags: Flags to pass to driver:
 *         0 = Default,
 *         DRM_CALLED_FROM_VBLIRQ = If function is called from vbl IRQ handler
 * @mode: mode which defines the scanout timings
 *
 * Implements calculation of exact vblank timestamps from given drm_display_mode
 * timings and current video scanout position of a CRTC. This can be called from
 * within get_vblank_timestamp() implementation of a kms driver to implement the
 * actual timestamping.
 *
 * Should return timestamps conforming to the OML_sync_control OpenML
 * extension specification. The timestamp corresponds to the end of
 * the vblank interval, aka start of scanout of topmost-leftmost display
 * pixel in the following video frame.
 *
 * Requires support for optional dev->driver->get_scanout_position()
 * in kms driver, plus a bit of setup code to provide a drm_display_mode
 * that corresponds to the true scanout timing.
 *
 * The current implementation only handles standard video modes. It
 * returns as no operation if a doublescan or interlaced video mode is
 * active. Higher level code is expected to handle this.
 *
 * Returns:
 * Negative value on error, failure or if not supported in current
 * video mode:
 *
 * -EINVAL    Invalid CRTC.
 * -EAGAIN    Temporary unavailable, e.g., called before initial modeset.
 * -ENOTSUPP  Function not supported in current display mode.
 * -EIO       Failed, e.g., due to failed scanout position query.
 *
 * Returns or'ed positive status flags on success:
 *
 * DRM_VBLANKTIME_SCANOUTPOS_METHOD - Signal this method used for timestamping.
 * DRM_VBLANKTIME_INVBL - Timestamp taken while scanout was in vblank interval.
 *
 */
int drm_calc_vbltimestamp_from_scanoutpos(struct drm_device *dev,
					  unsigned int pipe,
					  int *max_error,
					  struct timeval *vblank_time,
					  unsigned flags,
					  const struct drm_display_mode *mode)
{
	struct timeval tv_etime;
	ktime_t stime, etime;
	unsigned int vbl_status;
	int ret = DRM_VBLANKTIME_SCANOUTPOS_METHOD;
	int vpos, hpos, i;
	int delta_ns, duration_ns;
	if (pipe >= dev->num_crtcs) {
		DRM_ERROR("Invalid crtc %u\n", pipe);
		return -EINVAL;
	}
	/* Scanout position query not supported? Should not happen. */
	if (!dev->driver->get_scanout_position) {
		DRM_ERROR("Called from driver w/o get_scanout_position()!?\n");
		return -EIO;
	}
	/* If mode timing undefined, just return as no-op:
	 * Happens during initial modesetting of a crtc.
	 */
	if (mode->crtc_clock == 0) {
		DRM_DEBUG("crtc %u: Noop due to uninitialized mode.\n", pipe);
		//DRM_DEBUG("crtc %u: Noop due to uninitialized mode.\n", pipe);
		return -EAGAIN;
	}

	/* Get current scanout position with system timestamp.
	 * Repeat query up to DRM_TIMESTAMP_MAXRETRIES times
	 * if single query takes longer than max_error nanoseconds.
	 *
	 * This guarantees a tight bound on maximum error if
	 * code gets preempted or delayed for some reason.
	 */
	for (i = 0; i < DRM_TIMESTAMP_MAXRETRIES; i++) {
		/*
		 * Get vertical and horizontal scanout position vpos, hpos,
		 * and bounding timestamps stime, etime, pre/post query.
		 */
		vbl_status = dev->driver->get_scanout_position(dev, pipe, flags,
							       &vpos, &hpos,
							       &stime, &etime,
							       mode);
		/* Return as no-op if scanout query unsupported or failed. */
		if (!(vbl_status & DRM_SCANOUTPOS_VALID)) {
			DRM_DEBUG("crtc %u : scanoutpos query failed [0x%x].\n",
				  pipe, vbl_status);
			return -EIO;
		}
		/* Compute uncertainty in timestamp of scanout position query. */
		duration_ns = ktime_to_ns(etime) - ktime_to_ns(stime);
		/* Accept result with <  max_error nsecs timing uncertainty. */
		if (duration_ns <= *max_error)
			break;
	}
	/* Noisy system timing? */
	if (i == DRM_TIMESTAMP_MAXRETRIES) {
		DRM_DEBUG("crtc %u: Noisy timestamp %d us > %d us [%d reps].\n",
			  pipe, duration_ns/1000, *max_error/1000, i);
	}
	/* Return upper bound of timestamp precision error. */
	*max_error = duration_ns;
	/* Check if in vblank area:
	 * vpos is >=0 in video scanout area, but negative
	 * within vblank area, counting down the number of lines until
	 * start of scanout.
	 */
	if (vbl_status & DRM_SCANOUTPOS_IN_VBLANK)
		ret |= DRM_VBLANKTIME_IN_VBLANK;
	/* Convert scanout position into elapsed time at raw_time query
	 * since start of scanout at first display scanline. delta_ns
	 * can be negative if start of scanout hasn't happened yet.
	 */
	delta_ns = div_s64(1000000LL * (vpos * mode->crtc_htotal + hpos),
			   mode->crtc_clock);
	if (!drm_timestamp_monotonic)
		etime = ktime_mono_to_real(etime);
	/* save this only for debugging purposes */
	tv_etime = ktime_to_timeval(etime);
	/* Subtract time delta from raw timestamp to get final
	 * vblank_time timestamp for end of vblank.
	 */
	etime = ktime_sub_ns(etime, delta_ns);
	*vblank_time = ktime_to_timeval(etime);
	DRM_DEBUG_VBL("crtc %u : v 0x%x p(%d,%d)@ %ld.%ld -> %ld.%ld [e %d us, %d rep]\n",
		      pipe, vbl_status, hpos, vpos,
		      (long)tv_etime.tv_sec, (long)tv_etime.tv_usec,
		      (long)vblank_time->tv_sec, (long)vblank_time->tv_usec,
		      duration_ns/1000, i);
	return ret;
}
EXPORT_SYMBOL(drm_calc_vbltimestamp_from_scanoutpos);
static struct timeval get_drm_timestamp(void)
{
	ktime_t now;
	now = drm_timestamp_monotonic ? ktime_get() : ktime_get_real();
	return ktime_to_timeval(now);
}
/**
 * drm_get_last_vbltimestamp - retrieve raw timestamp for the most recent
 *                             vblank interval
 * @dev: DRM device
 * @pipe: index of CRTC whose vblank timestamp to retrieve
 * @tvblank: Pointer to target struct timeval which should receive the timestamp
 * @flags: Flags to pass to driver:
 *         0 = Default,
 *         DRM_CALLED_FROM_VBLIRQ = If function is called from vbl IRQ handler
 *
 * Fetches the system timestamp corresponding to the time of the most recent
 * vblank interval on specified CRTC. May call into kms-driver to
 * compute the timestamp with a high-precision GPU specific method.
 *
 * Returns zero if timestamp originates from uncorrected do_gettimeofday()
 * call, i.e., it isn't very precisely locked to the true vblank.
 *
 * Returns:
 * True if timestamp is considered to be very precise, false otherwise.
 */
static bool
drm_get_last_vbltimestamp(struct drm_device *dev, unsigned int pipe,
			  struct timeval *tvblank, unsigned flags)
{
	int ret;
	/* Define requested maximum error on timestamps (nanoseconds). */
	int max_error = (int) drm_timestamp_precision * 1000;
	/* Query driver if possible and precision timestamping enabled. */
	if (dev->driver->get_vblank_timestamp && (max_error > 0)) {
		ret = dev->driver->get_vblank_timestamp(dev, pipe, &max_error,
							tvblank, flags);
		if (ret > 0)
			return true;
	}
	/* GPU high precision timestamp query unsupported or failed.
	 * Return current monotonic/gettimeofday timestamp as best estimate.
	 */
	*tvblank = get_drm_timestamp();
	return false;
}
/**
 * drm_vblank_count - retrieve "cooked" vblank counter value
 * @dev: DRM device
 * @pipe: index of CRTC for which to retrieve the counter
 *
 * Fetches the "cooked" vblank count value that represents the number of
 * vblank events since the system was booted, including lost events due to
 * modesetting activity.
 *
 * This is the legacy version of drm_crtc_vblank_count().
 *
 * Returns:
 * The software vblank counter.
 */
u32 drm_vblank_count(struct drm_device *dev, unsigned int pipe)
{
	struct drm_vblank_crtc *vblank = &dev->vblank[pipe];
	if (WARN_ON(pipe >= dev->num_crtcs))
		return 0;
	return vblank->count;
}
EXPORT_SYMBOL(drm_vblank_count);
/**
 * drm_crtc_vblank_count - retrieve "cooked" vblank counter value
 * @crtc: which counter to retrieve
 *
 * Fetches the "cooked" vblank count value that represents the number of
 * vblank events since the system was booted, including lost events due to
 * modesetting activity.
 *
 * This is the native KMS version of drm_vblank_count().
 *
 * Returns:
 * The software vblank counter.
 */
u32 drm_crtc_vblank_count(struct drm_crtc *crtc)
{
	return drm_vblank_count(crtc->dev, drm_crtc_index(crtc));
}
EXPORT_SYMBOL(drm_crtc_vblank_count);
/**
 * drm_vblank_count_and_time - retrieve "cooked" vblank counter value and the
 *     system timestamp corresponding to that vblank counter value.
 * @dev: DRM device
 * @pipe: index of CRTC whose counter to retrieve
 * @vblanktime: Pointer to struct timeval to receive the vblank timestamp.
 *
 * Fetches the "cooked" vblank count value that represents the number of
 * vblank events since the system was booted, including lost events due to
 * modesetting activity. Returns corresponding system timestamp of the time
 * of the vblank interval that corresponds to the current vblank counter value.
 *
 * This is the legacy version of drm_crtc_vblank_count_and_time().
 */
static u32 drm_vblank_count_and_time(struct drm_device *dev, unsigned int pipe,
				     struct timeval *vblanktime)
{
	struct drm_vblank_crtc *vblank = &dev->vblank[pipe];
	u32 vblank_count;
	unsigned int seq;
	if (WARN_ON(pipe >= dev->num_crtcs))
		return 0;
	do {
		seq = read_seqbegin(&vblank->seqlock);
		vblank_count = vblank->count;
		*vblanktime = vblank->time;
	} while (read_seqretry(&vblank->seqlock, seq));
	return vblank_count;
}
/**
 * drm_crtc_vblank_count_and_time - retrieve "cooked" vblank counter value
 *     and the system timestamp corresponding to that vblank counter value
 * @crtc: which counter to retrieve
 * @vblanktime: Pointer to struct timeval to receive the vblank timestamp.
 *
 * Fetches the "cooked" vblank count value that represents the number of
 * vblank events since the system was booted, including lost events due to
 * modesetting activity. Returns corresponding system timestamp of the time
 * of the vblank interval that corresponds to the current vblank counter value.
 */
u32 drm_crtc_vblank_count_and_time(struct drm_crtc *crtc,
				   struct timeval *vblanktime)
{
	return drm_vblank_count_and_time(crtc->dev, drm_crtc_index(crtc),
					 vblanktime);
}
EXPORT_SYMBOL(drm_crtc_vblank_count_and_time);


static void send_vblank_event(struct drm_device *dev,
		struct drm_pending_vblank_event *e,
		unsigned long seq, struct timeval *now)
{
	e->event.sequence = seq;
	e->event.tv_sec = now->tv_sec;
	e->event.tv_usec = now->tv_usec;
	trace_drm_vblank_event_delivered(e->base.pid, e->pipe,
					 e->event.sequence);
	drm_send_event_locked(dev, &e->base);
}
/**
 * drm_crtc_arm_vblank_event - arm vblank event after pageflip
 * @crtc: the source CRTC of the vblank event
 * @e: the event to send
 *
 * A lot of drivers need to generate vblank events for the very next vblank
 * interrupt. For example when the page flip interrupt happens when the page
 * flip gets armed, but not when it actually executes within the next vblank
 * period. This helper function implements exactly the required vblank arming
 * behaviour.
 *
 * NOTE: Drivers using this to send out the event in struct &drm_crtc_state
 * as part of an atomic commit must ensure that the next vblank happens at
 * exactly the same time as the atomic commit is committed to the hardware. This
 * function itself does **not** protect again the next vblank interrupt racing
 * with either this function call or the atomic commit operation. A possible
 * sequence could be:
 *
 * 1. Driver commits new hardware state into vblank-synchronized registers.
 * 2. A vblank happens, committing the hardware state. Also the corresponding
 *    vblank interrupt is fired off and fully processed by the interrupt
 *    handler.
 * 3. The atomic commit operation proceeds to call drm_crtc_arm_vblank_event().
 * 4. The event is only send out for the next vblank, which is wrong.
 *
 * An equivalent race can happen when the driver calls
 * drm_crtc_arm_vblank_event() before writing out the new hardware state.
 *
 * The only way to make this work safely is to prevent the vblank from firing
 * (and the hardware from committing anything else) until the entire atomic
 * commit sequence has run to completion. If the hardware does not have such a
 * feature (e.g. using a "go" bit), then it is unsafe to use this functions.
 * Instead drivers need to manually send out the event from their interrupt
 * handler by calling drm_crtc_send_vblank_event() and make sure that there's no
 * possible race with the hardware committing the atomic update.
 *
 * Caller must hold event lock. Caller must also hold a vblank reference for
 * the event @e, which will be dropped when the next vblank arrives.
 */
void drm_crtc_arm_vblank_event(struct drm_crtc *crtc,
			       struct drm_pending_vblank_event *e)
{
	struct drm_device *dev = crtc->dev;
	unsigned int pipe = drm_crtc_index(crtc);
	assert_spin_locked(&dev->event_lock);
	e->pipe = pipe;
	e->event.sequence = drm_vblank_count(dev, pipe);
	list_add_tail(&e->base.link, &dev->vblank_event_list);
}
EXPORT_SYMBOL(drm_crtc_arm_vblank_event);
/**
 * drm_crtc_send_vblank_event - helper to send vblank event after pageflip
 * @crtc: the source CRTC of the vblank event
 * @e: the event to send
 *
 * Updates sequence # and timestamp on event for the most recently processed
 * vblank, and sends it to userspace.  Caller must hold event lock.
 *
 * See drm_crtc_arm_vblank_event() for a helper which can be used in certain
 * situation, especially to send out events for atomic commit operations.
 */
void drm_crtc_send_vblank_event(struct drm_crtc *crtc,
				struct drm_pending_vblank_event *e)
{
	struct drm_device *dev = crtc->dev;
	unsigned int seq, pipe = drm_crtc_index(crtc);
	struct timeval now;
	if (dev->num_crtcs > 0) {
		seq = drm_vblank_count_and_time(dev, pipe, &now);
	} else {
		seq = 0;
		now = get_drm_timestamp();
	}
	e->pipe = pipe;
	send_vblank_event(dev, e, seq, &now);
}
EXPORT_SYMBOL(drm_crtc_send_vblank_event);
/**
 * drm_vblank_enable - enable the vblank interrupt on a CRTC
 * @dev: DRM device
 * @pipe: CRTC index
 *
 * Returns:
 * Zero on success or a negative error code on failure.
 */
static int drm_vblank_enable(struct drm_device *dev, unsigned int pipe)
{
	struct drm_vblank_crtc *vblank = &dev->vblank[pipe];
	int ret = 0;
	assert_spin_locked(&dev->vbl_lock);
	spin_lock(&dev->vblank_time_lock);
	if (!vblank->enabled) {
		/*
		 * Enable vblank irqs under vblank_time_lock protection.
		 * All vblank count & timestamp updates are held off
		 * until we are done reinitializing master counter and
		 * timestamps. Filtercode in drm_handle_vblank() will
		 * prevent double-accounting of same vblank interval.
		 */
		ret = dev->driver->enable_vblank(dev, pipe);
		DRM_DEBUG("enabling vblank on crtc %u, ret: %d\n", pipe, ret);
		if (ret)
			atomic_dec(&vblank->refcount);
		else {
			vblank->enabled = true;
			drm_update_vblank_count(dev, pipe, 0);
		}
	}
	spin_unlock(&dev->vblank_time_lock);
	return ret;
}
/**
 * drm_vblank_get - get a reference count on vblank events
 * @dev: DRM device
 * @pipe: index of CRTC to own
 *
 * Acquire a reference count on vblank events to avoid having them disabled
 * while in use.
 *
 * This is the legacy version of drm_crtc_vblank_get().
 *
 * Returns:
 * Zero on success or a negative error code on failure.
 */
static int drm_vblank_get(struct drm_device *dev, unsigned int pipe)
{
	struct drm_vblank_crtc *vblank = &dev->vblank[pipe];
	unsigned long irqflags;
	int ret = 0;
	if (!dev->num_crtcs)
		return -EINVAL;
	if (WARN_ON(pipe >= dev->num_crtcs))
		return -EINVAL;
	spin_lock_irqsave(&dev->vbl_lock, irqflags);
	/* Going from 0->1 means we have to enable interrupts again */
	if (atomic_add_return(1, &vblank->refcount) == 1) {
		ret = drm_vblank_enable(dev, pipe);
	} else {
		if (!vblank->enabled) {
			atomic_dec(&vblank->refcount);
			ret = -EINVAL;
		}
	}
	spin_unlock_irqrestore(&dev->vbl_lock, irqflags);
	return ret;
}
/**
 * drm_crtc_vblank_get - get a reference count on vblank events
 * @crtc: which CRTC to own
 *
 * Acquire a reference count on vblank events to avoid having them disabled
 * while in use.
 *
 * Returns:
 * Zero on success or a negative error code on failure.
 */
int drm_crtc_vblank_get(struct drm_crtc *crtc)
{
	return drm_vblank_get(crtc->dev, drm_crtc_index(crtc));
}
EXPORT_SYMBOL(drm_crtc_vblank_get);
/**
 * drm_vblank_put - release ownership of vblank events
 * @dev: DRM device
 * @pipe: index of CRTC to release
 *
 * Release ownership of a given vblank counter, turning off interrupts
 * if possible. Disable interrupts after drm_vblank_offdelay milliseconds.
 *
 * This is the legacy version of drm_crtc_vblank_put().
 */
static void drm_vblank_put(struct drm_device *dev, unsigned int pipe)
{
	struct drm_vblank_crtc *vblank = &dev->vblank[pipe];
	if (WARN_ON(pipe >= dev->num_crtcs))
		return;
	if (WARN_ON(atomic_read(&vblank->refcount) == 0))
		return;
	/* Last user schedules interrupt disable */
	if (atomic_dec_and_test(&vblank->refcount)) {
		if (drm_vblank_offdelay == 0)
			return;
		else if (dev->vblank_disable_immediate || drm_vblank_offdelay < 0)
			vblank_disable_fn((unsigned long)vblank);
		else
			mod_timer(&vblank->disable_timer,
				  jiffies + ((drm_vblank_offdelay * HZ)/1000));
	}
}
/**
 * drm_crtc_vblank_put - give up ownership of vblank events
 * @crtc: which counter to give up
 *
 * Release ownership of a given vblank counter, turning off interrupts
 * if possible. Disable interrupts after drm_vblank_offdelay milliseconds.
 */
void drm_crtc_vblank_put(struct drm_crtc *crtc)
{
	drm_vblank_put(crtc->dev, drm_crtc_index(crtc));
}
EXPORT_SYMBOL(drm_crtc_vblank_put);
/**
 * drm_wait_one_vblank - wait for one vblank
 * @dev: DRM device
 * @pipe: CRTC index
 *
 * This waits for one vblank to pass on @pipe, using the irq driver interfaces.
 * It is a failure to call this when the vblank irq for @pipe is disabled, e.g.
 * due to lack of driver support or because the crtc is off.
 */
void drm_wait_one_vblank(struct drm_device *dev, unsigned int pipe)
{
	struct drm_vblank_crtc *vblank = &dev->vblank[pipe];
	int ret;
	u32 last;
	if (WARN_ON(pipe >= dev->num_crtcs))
		return;
	ret = drm_vblank_get(dev, pipe);
	if (WARN(ret, "vblank not available on crtc %i, ret=%i\n", pipe, ret))
		return;
	last = drm_vblank_count(dev, pipe);
	ret = wait_event_timeout(vblank->queue,
				 last != drm_vblank_count(dev, pipe),
				 msecs_to_jiffies(100));
	WARN(ret == 0, "vblank wait timed out on crtc %i\n", pipe);
	drm_vblank_put(dev, pipe);
}
EXPORT_SYMBOL(drm_wait_one_vblank);
/**
 * drm_crtc_wait_one_vblank - wait for one vblank
 * @crtc: DRM crtc
 *
 * This waits for one vblank to pass on @crtc, using the irq driver interfaces.
 * It is a failure to call this when the vblank irq for @crtc is disabled, e.g.
 * due to lack of driver support or because the crtc is off.
 */
void drm_crtc_wait_one_vblank(struct drm_crtc *crtc)
{
	drm_wait_one_vblank(crtc->dev, drm_crtc_index(crtc));
}
EXPORT_SYMBOL(drm_crtc_wait_one_vblank);
/**
 * drm_vblank_off - disable vblank events on a CRTC
 * @dev: DRM device
 * @pipe: CRTC index
 *
 * Drivers can use this function to shut down the vblank interrupt handling when
 * disabling a crtc. This function ensures that the latest vblank frame count is
 * stored so that drm_vblank_on() can restore it again.
 *
 * Drivers must use this function when the hardware vblank counter can get
 * reset, e.g. when suspending.
 *
 * This is the legacy version of drm_crtc_vblank_off().
 */
void drm_vblank_off(struct drm_device *dev, unsigned int pipe)
{
	struct drm_vblank_crtc *vblank = &dev->vblank[pipe];
	struct drm_pending_vblank_event *e, *t;
	struct timeval now;
	unsigned long irqflags;
	unsigned int seq;
	if (WARN_ON(pipe >= dev->num_crtcs))
		return;
	spin_lock_irqsave(&dev->event_lock, irqflags);
	spin_lock(&dev->vbl_lock);
	DRM_DEBUG_VBL("crtc %d, vblank enabled %d, inmodeset %d\n",
		      pipe, vblank->enabled, vblank->inmodeset);
	/* Avoid redundant vblank disables without previous drm_vblank_on(). */
	if (drm_core_check_feature(dev, DRIVER_ATOMIC) || !vblank->inmodeset)
		vblank_disable_and_save(dev, pipe);
	wake_up(&vblank->queue);
	/*
	 * Prevent subsequent drm_vblank_get() from re-enabling
	 * the vblank interrupt by bumping the refcount.
	 */
	if (!vblank->inmodeset) {
		atomic_inc(&vblank->refcount);
		vblank->inmodeset = 1;
	}
	spin_unlock(&dev->vbl_lock);
	/* Send any queued vblank events, lest the natives grow disquiet */
	seq = drm_vblank_count_and_time(dev, pipe, &now);
	list_for_each_entry_safe(e, t, &dev->vblank_event_list, base.link) {
		if (e->pipe != pipe)
			continue;
		DRM_DEBUG("Sending premature vblank event on disable: "
			  "wanted %u, current %u\n",
			  e->event.sequence, seq);
		list_del(&e->base.link);
		drm_vblank_put(dev, pipe);
		send_vblank_event(dev, e, seq, &now);
	}
	spin_unlock_irqrestore(&dev->event_lock, irqflags);
}
EXPORT_SYMBOL(drm_vblank_off);
/**
 * drm_crtc_vblank_off - disable vblank events on a CRTC
 * @crtc: CRTC in question
 *
 * Drivers can use this function to shut down the vblank interrupt handling when
 * disabling a crtc. This function ensures that the latest vblank frame count is
 * stored so that drm_vblank_on can restore it again.
 *
 * Drivers must use this function when the hardware vblank counter can get
 * reset, e.g. when suspending.
 *
 * This is the native kms version of drm_vblank_off().
 */
void drm_crtc_vblank_off(struct drm_crtc *crtc)
{
	drm_vblank_off(crtc->dev, drm_crtc_index(crtc));
}
EXPORT_SYMBOL(drm_crtc_vblank_off);
/**
 * drm_crtc_vblank_reset - reset vblank state to off on a CRTC
 * @crtc: CRTC in question
 *
 * Drivers can use this function to reset the vblank state to off at load time.
 * Drivers should use this together with the drm_crtc_vblank_off() and
 * drm_crtc_vblank_on() functions. The difference compared to
 * drm_crtc_vblank_off() is that this function doesn't save the vblank counter
 * and hence doesn't need to call any driver hooks.
 */
void drm_crtc_vblank_reset(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	unsigned long irqflags;
	unsigned int pipe = drm_crtc_index(crtc);
	struct drm_vblank_crtc *vblank = &dev->vblank[pipe];
	spin_lock_irqsave(&dev->vbl_lock, irqflags);
	/*
	 * Prevent subsequent drm_vblank_get() from enabling the vblank
	 * interrupt by bumping the refcount.
	 */
	if (!vblank->inmodeset) {
		atomic_inc(&vblank->refcount);
		vblank->inmodeset = 1;
	}
	spin_unlock_irqrestore(&dev->vbl_lock, irqflags);
	WARN_ON(!list_empty(&dev->vblank_event_list));
}
EXPORT_SYMBOL(drm_crtc_vblank_reset);
/**
 * drm_vblank_on - enable vblank events on a CRTC
 * @dev: DRM device
 * @pipe: CRTC index
 *
 * This functions restores the vblank interrupt state captured with
 * drm_vblank_off() again. Note that calls to drm_vblank_on() and
 * drm_vblank_off() can be unbalanced and so can also be unconditionally called
 * in driver load code to reflect the current hardware state of the crtc.
 *
 * This is the legacy version of drm_crtc_vblank_on().
 */
void drm_vblank_on(struct drm_device *dev, unsigned int pipe)
{
	struct drm_vblank_crtc *vblank = &dev->vblank[pipe];
	unsigned long irqflags;
	if (WARN_ON(pipe >= dev->num_crtcs))
		return;
	spin_lock_irqsave(&dev->vbl_lock, irqflags);
	DRM_DEBUG_VBL("crtc %d, vblank enabled %d, inmodeset %d\n",
		      pipe, vblank->enabled, vblank->inmodeset);
	/* Drop our private "prevent drm_vblank_get" refcount */
	if (vblank->inmodeset) {
		atomic_dec(&vblank->refcount);
		vblank->inmodeset = 0;
	}
	drm_reset_vblank_timestamp(dev, pipe);
	/*
	 * re-enable interrupts if there are users left, or the
	 * user wishes vblank interrupts to be enabled all the time.
	 */
	if (atomic_read(&vblank->refcount) != 0 || drm_vblank_offdelay == 0)
		WARN_ON(drm_vblank_enable(dev, pipe));
	spin_unlock_irqrestore(&dev->vbl_lock, irqflags);
}
EXPORT_SYMBOL(drm_vblank_on);
/**
 * drm_crtc_vblank_on - enable vblank events on a CRTC
 * @crtc: CRTC in question
 *
 * This functions restores the vblank interrupt state captured with
 * drm_vblank_off() again. Note that calls to drm_vblank_on() and
 * drm_vblank_off() can be unbalanced and so can also be unconditionally called
 * in driver load code to reflect the current hardware state of the crtc.
 *
 * This is the native kms version of drm_vblank_on().
 */
void drm_crtc_vblank_on(struct drm_crtc *crtc)
{
	drm_vblank_on(crtc->dev, drm_crtc_index(crtc));
}
EXPORT_SYMBOL(drm_crtc_vblank_on);
/**
 * drm_vblank_pre_modeset - account for vblanks across mode sets
 * @dev: DRM device
 * @pipe: CRTC index
 *
 * Account for vblank events across mode setting events, which will likely
 * reset the hardware frame counter.
 *
 * This is done by grabbing a temporary vblank reference to ensure that the
 * vblank interrupt keeps running across the modeset sequence. With this the
 * software-side vblank frame counting will ensure that there are no jumps or
 * discontinuities.
 *
 * Unfortunately this approach is racy and also doesn't work when the vblank
 * interrupt stops running, e.g. across system suspend resume. It is therefore
 * highly recommended that drivers use the newer drm_vblank_off() and
 * drm_vblank_on() instead. drm_vblank_pre_modeset() only works correctly when
 * using "cooked" software vblank frame counters and not relying on any hardware
 * counters.
 *
 * Drivers must call drm_vblank_post_modeset() when re-enabling the same crtc
 * again.
 */
void drm_vblank_pre_modeset(struct drm_device *dev, unsigned int pipe)
{
	struct drm_vblank_crtc *vblank = &dev->vblank[pipe];
	/* vblank is not initialized (IRQ not installed ?), or has been freed */
	if (!dev->num_crtcs)
		return;
	if (WARN_ON(pipe >= dev->num_crtcs))
		return;
	/*
	 * To avoid all the problems that might happen if interrupts
	 * were enabled/disabled around or between these calls, we just
	 * have the kernel take a reference on the CRTC (just once though
	 * to avoid corrupting the count if multiple, mismatch calls occur),
	 * so that interrupts remain enabled in the interim.
	 */
	if (!vblank->inmodeset) {
		vblank->inmodeset = 0x1;
		if (drm_vblank_get(dev, pipe) == 0)
			vblank->inmodeset |= 0x2;
	}
}
EXPORT_SYMBOL(drm_vblank_pre_modeset);
/**
 * drm_vblank_post_modeset - undo drm_vblank_pre_modeset changes
 * @dev: DRM device
 * @pipe: CRTC index
 *
 * This function again drops the temporary vblank reference acquired in
 * drm_vblank_pre_modeset.
 */
void drm_vblank_post_modeset(struct drm_device *dev, unsigned int pipe)
{
	struct drm_vblank_crtc *vblank = &dev->vblank[pipe];
	unsigned long irqflags;
	/* vblank is not initialized (IRQ not installed ?), or has been freed */
	if (!dev->num_crtcs)
		return;
	if (WARN_ON(pipe >= dev->num_crtcs))
		return;
	if (vblank->inmodeset) {
		spin_lock_irqsave(&dev->vbl_lock, irqflags);
		drm_reset_vblank_timestamp(dev, pipe);
		spin_unlock_irqrestore(&dev->vbl_lock, irqflags);
		if (vblank->inmodeset & 0x2)
			drm_vblank_put(dev, pipe);
		vblank->inmodeset = 0;
	}
}
EXPORT_SYMBOL(drm_vblank_post_modeset);
/*
 * drm_modeset_ctl - handle vblank event counter changes across mode switch
 * @DRM_IOCTL_ARGS: standard ioctl arguments
 *
 * Applications should call the %_DRM_PRE_MODESET and %_DRM_POST_MODESET
 * ioctls around modesetting so that any lost vblank events are accounted for.
 *
 * Generally the counter will reset across mode sets.  If interrupts are
 * enabled around this call, we don't have to do anything since the counter
 * will have already been incremented.
 */
int drm_modeset_ctl(struct drm_device *dev, void *data,
		    struct drm_file *file_priv)
{
	struct drm_modeset_ctl *modeset = data;
	unsigned int pipe;
	/* If drm_vblank_init() hasn't been called yet, just no-op */
	if (!dev->num_crtcs)
		return 0;
	/* KMS drivers handle this internally */
	if (!drm_core_check_feature(dev, DRIVER_LEGACY))
		return 0;
	pipe = modeset->crtc;
	if (pipe >= dev->num_crtcs)
		return -EINVAL;
	switch (modeset->cmd) {
	case _DRM_PRE_MODESET:
		drm_vblank_pre_modeset(dev, pipe);
		break;
	case _DRM_POST_MODESET:
		drm_vblank_post_modeset(dev, pipe);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}
static int drm_queue_vblank_event(struct drm_device *dev, unsigned int pipe,
				  union drm_wait_vblank *vblwait,
				  struct drm_file *file_priv)
{
	struct drm_vblank_crtc *vblank = &dev->vblank[pipe];
	struct drm_pending_vblank_event *e;
	struct timeval now;
	unsigned long flags;
	unsigned int seq;
	int ret;
	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (e == NULL) {
		ret = -ENOMEM;
		goto err_put;
	}
	e->pipe = pipe;
	e->base.pid = current->pid;
	e->event.base.type = DRM_EVENT_VBLANK;
	e->event.base.length = sizeof(e->event);
	e->event.user_data = vblwait->request.signal;
	spin_lock_irqsave(&dev->event_lock, flags);
	/*
	 * drm_vblank_off() might have been called after we called
	 * drm_vblank_get(). drm_vblank_off() holds event_lock
	 * around the vblank disable, so no need for further locking.
	 * The reference from drm_vblank_get() protects against
	 * vblank disable from another source.
	 */
	if (!vblank->enabled) {
		ret = -EINVAL;
		goto err_unlock;
	}
	ret = drm_event_reserve_init_locked(dev, file_priv, &e->base,
					    &e->event.base);
	if (ret)
		goto err_unlock;
	seq = drm_vblank_count_and_time(dev, pipe, &now);
	DRM_DEBUG("event on vblank count %u, current %u, crtc %u\n",
		  vblwait->request.sequence, seq, pipe);
	trace_drm_vblank_event_queued(current->pid, pipe,
				      vblwait->request.sequence);
	e->event.sequence = vblwait->request.sequence;
	if ((seq - vblwait->request.sequence) <= (1 << 23)) {
		drm_vblank_put(dev, pipe);
		send_vblank_event(dev, e, seq, &now);
		vblwait->reply.sequence = seq;
	} else {
		/* drm_handle_vblank_events will call drm_vblank_put */
		list_add_tail(&e->base.link, &dev->vblank_event_list);
		vblwait->reply.sequence = vblwait->request.sequence;
	}
	spin_unlock_irqrestore(&dev->event_lock, flags);
	return 0;
err_unlock:
	spin_unlock_irqrestore(&dev->event_lock, flags);
	kfree(e);
err_put:
	drm_vblank_put(dev, pipe);
	return ret;
}
/*
 * Wait for VBLANK.
 *
 * \param inode device inode.
 * \param file_priv DRM file private.
 * \param cmd command.
 * \param data user argument, pointing to a drm_wait_vblank structure.
 * \return zero on success or a negative number on failure.
 *
 * This function enables the vblank interrupt on the pipe requested, then
 * sleeps waiting for the requested sequence number to occur, and drops
 * the vblank interrupt refcount afterwards. (vblank IRQ disable follows that
 * after a timeout with no further vblank waits scheduled).
 */
int drm_wait_vblank(struct drm_device *dev, void *data,
		    struct drm_file *file_priv)
{
	struct drm_vblank_crtc *vblank;
	union drm_wait_vblank *vblwait = data;
	int ret;
	unsigned int flags, seq, pipe, high_pipe;
	if (!dev->irq_enabled)
		return -EINVAL;
	if (vblwait->request.type & _DRM_VBLANK_SIGNAL)
		return -EINVAL;
	if (vblwait->request.type &
	    ~(_DRM_VBLANK_TYPES_MASK | _DRM_VBLANK_FLAGS_MASK |
	      _DRM_VBLANK_HIGH_CRTC_MASK)) {
		DRM_ERROR("Unsupported type value 0x%x, supported mask 0x%x\n",
			  vblwait->request.type,
			  (_DRM_VBLANK_TYPES_MASK | _DRM_VBLANK_FLAGS_MASK |
			   _DRM_VBLANK_HIGH_CRTC_MASK));
		return -EINVAL;
	}
	flags = vblwait->request.type & _DRM_VBLANK_FLAGS_MASK;
	high_pipe = (vblwait->request.type & _DRM_VBLANK_HIGH_CRTC_MASK);
	if (high_pipe)
		pipe = high_pipe >> _DRM_VBLANK_HIGH_CRTC_SHIFT;
	else
		pipe = flags & _DRM_VBLANK_SECONDARY ? 1 : 0;
	if (pipe >= dev->num_crtcs)
		return -EINVAL;
	vblank = &dev->vblank[pipe];
	ret = drm_vblank_get(dev, pipe);
	if (ret) {
		DRM_DEBUG("failed to acquire vblank counter, %d\n", ret);
		return ret;
	}
	seq = drm_vblank_count(dev, pipe);
	switch (vblwait->request.type & _DRM_VBLANK_TYPES_MASK) {
	case _DRM_VBLANK_RELATIVE:
		vblwait->request.sequence += seq;
		vblwait->request.type &= ~_DRM_VBLANK_RELATIVE;
	case _DRM_VBLANK_ABSOLUTE:
		break;
	default:
		ret = -EINVAL;
		goto done;
	}
	if ((flags & _DRM_VBLANK_NEXTONMISS) &&
	    (seq - vblwait->request.sequence) <= (1 << 23)) {
		vblwait->request.sequence = seq + 1;
	}
	if (flags & _DRM_VBLANK_EVENT) {
		/* must hold on to the vblank ref until the event fires
		 * drm_vblank_put will be called asynchronously
		 */
		return drm_queue_vblank_event(dev, pipe, vblwait, file_priv);
	}
	DRM_DEBUG("waiting on vblank count %u, crtc %u\n",
		  vblwait->request.sequence, pipe);
	DRM_WAIT_ON(ret, vblank->queue, 3 * HZ,
		    (((drm_vblank_count(dev, pipe) -
		       vblwait->request.sequence) <= (1 << 23)) ||
		     !vblank->enabled ||
		     !dev->irq_enabled));
	if (ret != -EINTR) {
		struct timeval now;
		vblwait->reply.sequence = drm_vblank_count_and_time(dev, pipe, &now);
		vblwait->reply.tval_sec = now.tv_sec;
		vblwait->reply.tval_usec = now.tv_usec;
		DRM_DEBUG("returning %u to client\n",
			  vblwait->reply.sequence);
	} else {
		DRM_DEBUG("vblank wait interrupted by signal\n");
	}
done:
	drm_vblank_put(dev, pipe);
	return ret;
}
static void drm_handle_vblank_events(struct drm_device *dev, unsigned int pipe)
{
	struct drm_pending_vblank_event *e, *t;
	struct timeval now;
	unsigned int seq;
	assert_spin_locked(&dev->event_lock);
	seq = drm_vblank_count_and_time(dev, pipe, &now);
	list_for_each_entry_safe(e, t, &dev->vblank_event_list, base.link) {
		if (e->pipe != pipe)
			continue;
		if ((seq - e->event.sequence) > (1<<23))
			continue;
		DRM_DEBUG("vblank event on %u, current %u\n",
			  e->event.sequence, seq);
		list_del(&e->base.link);
		drm_vblank_put(dev, pipe);
		send_vblank_event(dev, e, seq, &now);
	}
	trace_drm_vblank_event(pipe, seq);
}
/**
 * drm_handle_vblank - handle a vblank event
 * @dev: DRM device
 * @pipe: index of CRTC where this event occurred
 *
 * Drivers should call this routine in their vblank interrupt handlers to
 * update the vblank counter and send any signals that may be pending.
 *
 * This is the legacy version of drm_crtc_handle_vblank().
 */
bool drm_handle_vblank(struct drm_device *dev, unsigned int pipe)
{
	struct drm_vblank_crtc *vblank = &dev->vblank[pipe];
	unsigned long irqflags;
	if (WARN_ON_ONCE(!dev->num_crtcs))
		return false;
	if (WARN_ON(pipe >= dev->num_crtcs))
		return false;
	spin_lock_irqsave(&dev->event_lock, irqflags);
	/* Need timestamp lock to prevent concurrent execution with
	 * vblank enable/disable, as this would cause inconsistent
	 * or corrupted timestamps and vblank counts.
	 */
	spin_lock(&dev->vblank_time_lock);
	/* Vblank irq handling disabled. Nothing to do. */
	if (!vblank->enabled) {
		spin_unlock(&dev->vblank_time_lock);
		spin_unlock_irqrestore(&dev->event_lock, irqflags);
		return false;
	}
	drm_update_vblank_count(dev, pipe, DRM_CALLED_FROM_VBLIRQ);
	spin_unlock(&dev->vblank_time_lock);
	wake_up(&vblank->queue);
	drm_handle_vblank_events(dev, pipe);
	spin_unlock_irqrestore(&dev->event_lock, irqflags);
	return true;
}
EXPORT_SYMBOL(drm_handle_vblank);
/**
 * drm_crtc_handle_vblank - handle a vblank event
 * @crtc: where this event occurred
 *
 * Drivers should call this routine in their vblank interrupt handlers to
 * update the vblank counter and send any signals that may be pending.
 *
 * This is the native KMS version of drm_handle_vblank().
 *
 * Returns:
 * True if the event was successfully handled, false on failure.
 */
bool drm_crtc_handle_vblank(struct drm_crtc *crtc)
{
	return drm_handle_vblank(crtc->dev, drm_crtc_index(crtc));
}
EXPORT_SYMBOL(drm_crtc_handle_vblank);
/**
 * drm_vblank_no_hw_counter - "No hw counter" implementation of .get_vblank_counter()
 * @dev: DRM device
 * @pipe: CRTC for which to read the counter
 *
 * Drivers can plug this into the .get_vblank_counter() function if
 * there is no useable hardware frame counter available.
 *
 * Returns:
 * 0
 */
u32 drm_vblank_no_hw_counter(struct drm_device *dev, unsigned int pipe)
{
	WARN_ON_ONCE(dev->max_vblank_count != 0);
	return 0;
}
EXPORT_SYMBOL(drm_vblank_no_hw_counter);
