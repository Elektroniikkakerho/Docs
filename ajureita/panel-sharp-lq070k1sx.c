/*
 * Sharp LQ070K1SX DSI video mode panel driver
 *
 * Copyright 2012 Elektrobit Inc.
 * Author: Chris Minerva <chris.minerva@elektrobit.com>
 *
 * based on panel-sharp.c
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/backlight.h>
#include <linux/fb.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/regulator/consumer.h>
#include <linux/mutex.h>
#include <linux/i2c.h>
#include <linux/uaccess.h>
#include <video/omapdss.h>

#define DRIVER_NAME "drv-sharp-lq070k1sx"
#define DEVICE_NAME "dev-sharp-lq070k1sx"

/***************************/
/* DSI Display Command Set */
/***************************/
#define DCS_SLEEP_IN         0x10
#define DCS_SLEEP_OUT        0x11
#define DCS_DISPLAY_OFF      0x28
#define DCS_DISPLAY_ON       0x29

const u8 sharp_cmd_sleep_in[] = {
	DCS_SLEEP_IN,
};

const u8 sharp_cmd_sleep_out[] = {
	DCS_SLEEP_OUT,
};

static const u8 sharp_cmd_display_on[] = {
	DCS_DISPLAY_ON,
};

static const u8 sharp_cmd_display_off[] = {
	DCS_DISPLAY_OFF,
};

struct panel_sharp_data {
	int x_res;
	int y_res;
};

static struct panel_sharp_data sharp_pdata = {
	.x_res = 800,
	.y_res = 1280,
};

static struct omap_video_timings sharp_panel_timings = {
	.x_res		= 800,
	.y_res		= 1280,
	.hsw		= 64,
	.hfp		= 64,
	.hbp		= 64,
	.vsw		= 1,
	.vfp		= 6, /* according to spec */
	.vbp		= 12  /* spec + 2 to avoid shift */
};

/* device private data structure */
struct sharp_data {
	struct mutex lock;
	struct omap_dss_device *dssdev;
	struct omap_video_timings *timings;

	int channel0; /* Virtual Channel 0 - Video Data */
	int channel1; /* Virtual Channel 1 - Cmd */
};

static struct panel_sharp_data *get_panel_data(struct omap_dss_device *dssdev)
{
	return (struct panel_sharp_data *)dssdev->data;
}

static int sharp_write_block(struct omap_dss_device *dssdev, const u8 *data,
		int len)
{
	struct sharp_data *sd = dev_get_drvdata(&dssdev->dev);

	return dsi_vc_dcs_write(dssdev, sd->channel1, (u8 *)data, len);
}

static void sharp_get_timings(struct omap_dss_device *dssdev,
			    struct omap_video_timings *timings)
{
	*timings = dssdev->panel.timings;
}

static void sharp_set_timings(struct omap_dss_device *dssdev,
			    struct omap_video_timings *timings)
{
}

static int sharp_check_timings(struct omap_dss_device *dssdev,
			     struct omap_video_timings *timings)
{
	if (sharp_panel_timings.x_res != timings->x_res ||
			sharp_panel_timings.y_res != timings->y_res ||
			sharp_panel_timings.pixel_clock != timings->pixel_clock ||
			sharp_panel_timings.hsw != timings->hsw ||
			sharp_panel_timings.hfp != timings->hfp ||
			sharp_panel_timings.hbp != timings->hbp ||
			sharp_panel_timings.vsw != timings->vsw ||
			sharp_panel_timings.vfp != timings->vfp ||
			sharp_panel_timings.vbp != timings->vbp)
		return -EINVAL;

	return 0;
}

static void sharp_get_resolution(struct omap_dss_device *dssdev,
			       u16 *xres, u16 *yres)
{
	*xres = sharp_panel_timings.x_res;
	*yres = sharp_panel_timings.y_res;
}

static int sharp_probe(struct omap_dss_device *dssdev)
{
	struct sharp_data *sd;
	struct panel_sharp_data *panel_data;
	int r;

	dev_info(&dssdev->dev, "%s\n", __func__);

	/* Init Locals */
	dssdev->data = &sharp_pdata;
	panel_data = get_panel_data(dssdev);

	if (dssdev->data == NULL) {
		dev_err(&dssdev->dev, "no platform data!\n");
		r = -EINVAL;
		goto err0;
	}

	sharp_panel_timings.x_res = panel_data->x_res;
	sharp_panel_timings.y_res = panel_data->y_res;
	dssdev->panel.config = OMAP_DSS_LCD_TFT;
	dssdev->panel.timings = sharp_panel_timings;
	dssdev->ctrl.pixel_size = 24;
	dssdev->panel.acbi = 0;
	dssdev->panel.acb = 40;

	sd = kzalloc(sizeof(*sd), GFP_KERNEL);
	if (!sd) {
		r = -ENOMEM;
		goto err0;
	}
	sd->dssdev = dssdev;

	mutex_init(&sd->lock);

	dev_set_drvdata(&dssdev->dev, sd);

	/* Request virtual channel 0 for video data */
	r = omap_dsi_request_vc(dssdev, &sd->channel0);
	if (r) {
		dev_err(&dssdev->dev, "failed to get virtual channel0\n");
		goto err2;
	}

	r = omap_dsi_set_vc_id(dssdev, sd->channel0, 0);
	if (r) {
		dev_err(&dssdev->dev, "failed to set VC_ID0\n");
		goto err3;
	}

	/* Request virtual channel 1 for command data */
	r = omap_dsi_request_vc(dssdev, &sd->channel1);
	if (r) {
		dev_err(&dssdev->dev, "failed to get virtual channel1\n");
		goto err3;
	}

	r = omap_dsi_set_vc_id(dssdev, sd->channel1, 0);
	if (r) {
		dev_err(&dssdev->dev, "failed to set VC_ID1\n");
		goto err4;
	}

	dev_info(&dssdev->dev, "Probe OK\n");
	return 0;

err4:
	omap_dsi_release_vc(dssdev, sd->channel1);
err3:
	omap_dsi_release_vc(dssdev, sd->channel0);
err2:
	mutex_destroy(&sd->lock);
	kfree(sd);
err0:
	dev_err(&dssdev->dev, "Probe failed!\n");
	return r;
}

static void sharp_remove(struct omap_dss_device *dssdev)
{
	struct sharp_data *sd;

	/* Init Locals */
	sd = dev_get_drvdata(&dssdev->dev);

	mutex_destroy(&sd->lock);
	omap_dsi_release_vc(dssdev, sd->channel0);
	omap_dsi_release_vc(dssdev, sd->channel1);
	kfree(sd);
}

/*
 * sharp_config - Configure sharp Panel
 *
 * Initial configuration for sharp configuration registers, etc.
 */
static void sharp_config(struct omap_dss_device *dssdev)
{
	struct sharp_data *sd;
	struct panel_sharp_data *panel_data;

	/* Init Locals */
	sd = dev_get_drvdata(&dssdev->dev);
	panel_data = get_panel_data(dssdev);

	/* Issue SLEEP OUT */
	msleep(100);
	sharp_write_block(dssdev, sharp_cmd_sleep_out,
		ARRAY_SIZE(sharp_cmd_sleep_out));

	/* Issue DISPLAY ON */
	msleep(120);
	sharp_write_block(dssdev, sharp_cmd_display_on,
		ARRAY_SIZE(sharp_cmd_display_on));
	msleep(10);
}

static int sharp_power_on(struct omap_dss_device *dssdev)
{
	struct sharp_data *sd;
	int r;

	dev_dbg(&dssdev->dev, "%s\n", __func__);

	/* Init Locals */
	sd = dev_get_drvdata(&dssdev->dev);

	/* At power on the first vsync has not been received yet*/
	dssdev->first_vsync = false;

	r = omapdss_dsi_display_enable(dssdev);
	if (r) {
		dev_err(&dssdev->dev, "failed to enable DSI\n");
		goto err1;
	}

	if (dssdev->platform_enable) {
		r = dssdev->platform_enable(dssdev);
		if (r)
			goto err0;
	}

	/* Switch video and command VCs to high speed mode. */
	omapdss_dsi_vc_enable_hs(dssdev, sd->channel0, true);
	omapdss_dsi_vc_enable_hs(dssdev, sd->channel1, true);

	/* Issue configuration commands in HS mode */
	sharp_config(dssdev);
	msleep(10);

	/* Turn on the video stream */
	dsi_video_mode_enable(dssdev, 0x3E);

	return r;

err1:
	if (dssdev->platform_disable)
		dssdev->platform_disable(dssdev);

err0:
	return r;
}

static void sharp_power_off(struct omap_dss_device *dssdev)
{
	struct panel_sharp_data *panel_data;

	dev_dbg(&dssdev->dev, "%s\n", __func__);

	/* Init Locals */
	panel_data = get_panel_data(dssdev);

	/* Display Off */
	sharp_write_block(dssdev, sharp_cmd_display_off,
		ARRAY_SIZE(sharp_cmd_display_off));
	msleep(100);

	/* Sleep In */
	sharp_write_block(dssdev, sharp_cmd_sleep_in,
		ARRAY_SIZE(sharp_cmd_sleep_in));
	msleep(100);

	dsi_video_mode_disable(dssdev);

	omapdss_dsi_display_disable(dssdev, false, false);

	if (dssdev->platform_disable)
		dssdev->platform_disable(dssdev);
}

static int sharp_enable(struct omap_dss_device *dssdev)
{
	struct sharp_data *sd;
	int r;

	dev_dbg(&dssdev->dev, "%s\n", __func__);

	/* Init Locals */
	sd = dev_get_drvdata(&dssdev->dev);

	mutex_lock(&sd->lock);

	if (dssdev->state != OMAP_DSS_DISPLAY_DISABLED) {
		r = -EINVAL;
		goto err0;
	}

	dsi_bus_lock(dssdev);

	r = sharp_power_on(dssdev);

	dsi_bus_unlock(dssdev);

	if (r) {
		dev_err(&dssdev->dev, "enable failed\n");
		dssdev->state = OMAP_DSS_DISPLAY_DISABLED;
	} else {
		dssdev->state = OMAP_DSS_DISPLAY_ACTIVE;
	}

err0:
	mutex_unlock(&sd->lock);

	return r;
}

static void sharp_disable(struct omap_dss_device *dssdev)
{
	struct sharp_data *sd;

	dev_dbg(&dssdev->dev, "%s\n", __func__);

	/* Init Locals */
	sd = dev_get_drvdata(&dssdev->dev);

	mutex_lock(&sd->lock);

	if (dssdev->state == OMAP_DSS_DISPLAY_ACTIVE) {
		dsi_bus_lock(dssdev);

		sharp_power_off(dssdev);

		dsi_bus_unlock(dssdev);
	}

	dssdev->state = OMAP_DSS_DISPLAY_DISABLED;

	mutex_unlock(&sd->lock);
}

static int sharp_suspend(struct omap_dss_device *dssdev)
{
	dssdev->driver->disable(dssdev);

	return 0;
}

static int sharp_resume(struct omap_dss_device *dssdev)
{
	dssdev->driver->enable(dssdev);

	return 0;
}

static struct omap_dss_driver sharp_driver = {
	.probe  = sharp_probe,
	.remove = sharp_remove,

	.enable  = sharp_enable,
	.disable = sharp_disable,
	.suspend = sharp_suspend,
	.resume  = sharp_resume,

	.get_resolution      = sharp_get_resolution,
	.get_recommended_bpp = omapdss_default_get_recommended_bpp,

	.get_timings   = sharp_get_timings,
	.set_timings   = sharp_set_timings,
	.check_timings = sharp_check_timings,

	.driver = {
		.name  = "panel-sharp-lq070k1sx",
		.owner = THIS_MODULE,
	},
};

static int __init sharp_init(void)
{
	omap_dss_register_driver(&sharp_driver);

	return 0;
}

static void __exit sharp_exit(void)
{
	omap_dss_unregister_driver(&sharp_driver);
}

module_init(sharp_init);
module_exit(sharp_exit);

MODULE_AUTHOR("Chris Minerva <chris.minerva@elektrobit.com>");
MODULE_DESCRIPTION("sharp Panel Driver");
MODULE_LICENSE("GPL");
