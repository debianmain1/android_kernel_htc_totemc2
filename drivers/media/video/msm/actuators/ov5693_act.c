/* Copyright (c) 2011, Code Aurora Forum. All rights reserved.
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

#include "msm_actuator.h"
#include "msm_camera_i2c.h"
#include <mach/gpio.h>

#ifdef USE_RAWCHIP_AF
#define	OV5693_TOTAL_STEPS_NEAR_TO_FAR			256
#else
#define	OV5693_TOTAL_STEPS_NEAR_TO_FAR			52
#endif

#define REG_VCM_NEW_CODE			0x30F2
#define REG_VCM_I2C_ADDR			0x1C
#define REG_VCM_CODE_MSB			0x03
#define REG_VCM_CODE_LSB			0x04
/*HTC_START steven_wu fix vcm damping 20120611*/
#define REG_VCM_MODE			0x06
#define REG_VCM_FREQ			0x07
#define REG_VCM_RING_CTRL		0x02 /* de-ring setting */
/*HTC_END steven_wu fix vcm damping 20120611*/

#define DIV_CEIL(x, y) (x/y + (x%y) ? 1 : 0)
#if 0
#undef LINFO
#define LINFO pr_info
#endif
DEFINE_MUTEX(ov5693_act_mutex);
static struct msm_actuator_ctrl_t ov5693_act_t;

static struct region_params_t g_regions[] = {
	/* step_bound[0] - macro side boundary
	 * step_bound[1] - infinity side boundary
	 */
	/* Region 1 */
	{
		.step_bound = {OV5693_TOTAL_STEPS_NEAR_TO_FAR, 0},
		.code_per_step = 2,
	},
};

static uint16_t g_scenario[] = {
	/* MOVE_NEAR and MOVE_FAR dir*/
	OV5693_TOTAL_STEPS_NEAR_TO_FAR,
};

static struct damping_params_t g_damping[] = {
	/* MOVE_NEAR Dir */
	/* Scene 1 => Damping params */
	{
		.damping_step = 2,
		.damping_delay = 0,
	},
};

static struct damping_t g_damping_params[] = {
	/* MOVE_NEAR and MOVE_FAR dir */
	/* Region 1 */
	{
		.ringing_params = g_damping,
	},
};

static struct msm_actuator_info *ov5693_msm_actuator_info;

static int32_t ov5693_poweron_af(void)
{
	int32_t rc = 0;
	pr_info("%s enable AF actuator, gpio = %d\n", __func__,
			ov5693_msm_actuator_info->vcm_pwd);
	mdelay(1);
	rc = gpio_request(ov5693_msm_actuator_info->vcm_pwd, "ov5693");
	if (!rc)
		gpio_direction_output(ov5693_msm_actuator_info->vcm_pwd, 1);
	else
		pr_err("%s: AF PowerON gpio_request failed %d\n", __func__, rc);
	gpio_free(ov5693_msm_actuator_info->vcm_pwd);
	mdelay(1);
	return rc;
}

static void ov5693_poweroff_af(void)
{
	int32_t rc = 0;

	pr_info("%s disable AF actuator, gpio = %d\n", __func__,
			ov5693_msm_actuator_info->vcm_pwd);

	msleep(1);
	rc = gpio_request(ov5693_msm_actuator_info->vcm_pwd, "ov5693");
	if (!rc)
		gpio_direction_output(ov5693_msm_actuator_info->vcm_pwd, 0);
	else
		pr_err("%s: AF PowerOFF gpio_request failed %d\n", __func__, rc);
	gpio_free(ov5693_msm_actuator_info->vcm_pwd);
	msleep(1);
}

int32_t ov5693_msm_actuator_init_table(
	struct msm_actuator_ctrl_t *a_ctrl)
{
	int32_t rc = 0;

	LINFO("%s called\n", __func__);

	if (a_ctrl->func_tbl.actuator_set_params)
		a_ctrl->func_tbl.actuator_set_params(a_ctrl);

	if (ov5693_act_t.step_position_table) {
		LINFO("%s table inited\n", __func__);
		return rc;
	}

    /* HTC_START harvey 20120904 VCM mode setting */
	/* de-ring setting */
	rc = msm_camera_i2c_write(&a_ctrl->i2c_client,
		REG_VCM_RING_CTRL,
		0x02,
		MSM_CAMERA_I2C_BYTE_DATA);
	if (rc < 0) {
		pr_err("%s REG_VCM_RING_CTRL i2c write failed (%d)\n", __func__, rc);
		return rc;
	}

	//RING_MODE:bit 0
	// 0: 2x(1/fVCM)
	// 1: 1x(1/fVCM) <-- Optical comment

	//PWM/LIN:bit 1
	// 0:PWM mode	<-- used it
	// 1:Linear mode
	rc = msm_camera_i2c_write(&a_ctrl->i2c_client,
		REG_VCM_MODE,
		0x03, //0x01,
		MSM_CAMERA_I2C_BYTE_DATA);
	if (rc < 0) {
		pr_err("%s REG_VCM_MODE i2c write failed (%d)\n", __func__, rc);
		return rc;
	}
	//VCM frequence
	//VCM _ FREQ:  383 - (19200/VCM mechanical ringing frequency)
	//						VCM mechanical ringing frequency = 75.3 Hz
	//						383 - (19200/75.3) = 128
	rc = msm_camera_i2c_write(&a_ctrl->i2c_client,
		REG_VCM_FREQ,
		0xAF,
		MSM_CAMERA_I2C_BYTE_DATA);
	if (rc < 0) {
		pr_err("%s REG_VCM_FREQ i2c write failed (%d)\n", __func__, rc);
		return rc;
	}
    /* HTC_END */

	/* Fill step position table */
	if (a_ctrl->step_position_table != NULL) {
		kfree(a_ctrl->step_position_table);
		a_ctrl->step_position_table = NULL;
	}
	a_ctrl->step_position_table =
		kmalloc(sizeof(uint16_t) * (a_ctrl->set_info.total_steps + 1),
			GFP_KERNEL);

	if (a_ctrl->step_position_table != NULL) {
		uint16_t i = 0;
		uint16_t ov5693_nl_region_boundary1 = 2;
		uint16_t ov5693_nl_region_code_per_step1 = 32;
		uint16_t ov5693_l_region_code_per_step = 16;
		uint16_t ov5693_max_value = 1023;

		a_ctrl->step_position_table[0] = a_ctrl->initial_code;

		for (i = 1; i <= a_ctrl->set_info.total_steps; i++) {
#ifdef USE_RAWCHIP_AF
			if (ov5693_msm_actuator_info->use_rawchip_af)
				a_ctrl->step_position_table[i] =
					a_ctrl->step_position_table[i-1] + 4;
			else
#endif
			{
			if (i <= ov5693_nl_region_boundary1) {
				a_ctrl->step_position_table[i] =
					a_ctrl->step_position_table[i-1]
					+ ov5693_nl_region_code_per_step1;
			} else {
				a_ctrl->step_position_table[i] =
					a_ctrl->step_position_table[i-1]
					+ ov5693_l_region_code_per_step;
			}

			if (a_ctrl->step_position_table[i] > ov5693_max_value)
				a_ctrl->step_position_table[i] = ov5693_max_value;
			}
		}
		a_ctrl->curr_step_pos = 0;
		a_ctrl->curr_region_index = 0;
	} else {
		pr_err("%s table init failed\n", __func__);
		rc = -EFAULT;
	}

	return rc;
}

int32_t ov5693_msm_actuator_move_focus(
	struct msm_actuator_ctrl_t *a_ctrl,
	int dir,
	int32_t num_steps)
{
	int32_t rc = 0;
	int8_t sign_dir = 0;
	int16_t dest_step_pos = 0;

	LINFO("%s called, dir %d, num_steps %d\n",
		__func__,
		dir,
		num_steps);

	/* Determine sign direction */
	if (dir == MOVE_NEAR)
		sign_dir = 1;
	else if (dir == MOVE_FAR)
		sign_dir = -1;
	else {
		pr_err("Illegal focus direction\n");
		rc = -EINVAL;
		return rc;
	}

	/* Determine destination step position */
	dest_step_pos = a_ctrl->curr_step_pos +
		(sign_dir * num_steps);

	if (dest_step_pos < 0)
		dest_step_pos = 0;
	else if (dest_step_pos > a_ctrl->set_info.total_steps)
		dest_step_pos = a_ctrl->set_info.total_steps;

	if (dest_step_pos == a_ctrl->curr_step_pos)
		return rc;

	rc = a_ctrl->func_tbl.actuator_i2c_write(a_ctrl,
		a_ctrl->step_position_table[dest_step_pos], NULL);
	if (rc < 0) {
		pr_err("%s focus move failed\n", __func__);
		return rc;
	} else {
		a_ctrl->curr_step_pos = dest_step_pos;
		LINFO("%s current step: %d\n", __func__, a_ctrl->curr_step_pos);
	}

	return rc;
}

int ov5693_actuator_af_power_down(void *params)
{
	int rc = 0;
	LINFO("%s called\n", __func__);

	rc = (int) msm_actuator_af_power_down(&ov5693_act_t);
	ov5693_poweroff_af();
	return rc;
}

static int32_t ov5693_wrapper_i2c_write(struct msm_actuator_ctrl_t *a_ctrl,
	int16_t next_lens_position, void *params)
{
	int32_t rc = 0;

	rc = msm_camera_i2c_write(&a_ctrl->i2c_client,
		REG_VCM_CODE_MSB,
		((next_lens_position & 0x0300) >> 8),	/*HTC_START steven_wu fix vcm damping 20120611*/
		MSM_CAMERA_I2C_BYTE_DATA);
	if (rc < 0) {
		pr_err("%s VCM_CODE_MSB i2c write failed (%d)\n", __func__, rc);
		return rc;
	}

	rc = msm_camera_i2c_write(&a_ctrl->i2c_client,
		REG_VCM_CODE_LSB,
		(next_lens_position & 0x00FF),	/*HTC_START steven_wu fix vcm damping 20120611*/
		MSM_CAMERA_I2C_BYTE_DATA);
	if (rc < 0) {
		pr_err("%s VCM_CODE_LSB i2c write failed (%d)\n", __func__, rc);
		return rc;
	}

	return rc;
}

int32_t ov5693_act_write_focus(
	struct msm_actuator_ctrl_t *a_ctrl,
	uint16_t curr_lens_pos,
	struct damping_params_t *damping_params,
	int8_t sign_direction,
	int16_t code_boundary)
{
	int32_t rc = 0;
	uint16_t dac_value = 0;

	LINFO("%s called, curr lens pos = %d, code_boundary = %d\n",
		  __func__,
		  curr_lens_pos,
		  code_boundary);

	if (sign_direction == 1)
		dac_value = (code_boundary - curr_lens_pos);
	else
		dac_value = (curr_lens_pos - code_boundary);

	LINFO("%s dac_value = %d\n",
		  __func__,
		  dac_value);

	rc = a_ctrl->func_tbl.actuator_i2c_write(a_ctrl, dac_value, NULL);

	return rc;
}

static int32_t ov5693_act_init_focus(struct msm_actuator_ctrl_t *a_ctrl)
{
	int32_t rc = 0;

	rc = a_ctrl->func_tbl.actuator_i2c_write(a_ctrl, a_ctrl->initial_code,
		NULL);
	if (rc < 0)
		pr_err("%s i2c write failed\n", __func__);
	else
		a_ctrl->curr_step_pos = 0;

	return rc;
}

static const struct i2c_device_id ov5693_act_i2c_id[] = {
	{"ov5693_act", (kernel_ulong_t)&ov5693_act_t},
	{ }
};

static int ov5693_act_config(
	void __user *argp)
{
	LINFO("%s called\n", __func__);
	return (int) msm_actuator_config(&ov5693_act_t,
		ov5693_msm_actuator_info, argp); /* HTC Angie 20111212 - Rawchip */
}

static int ov5693_i2c_add_driver_table(
	void)
{
	int32_t rc = 0;

	pr_info("%s called\n", __func__);

	rc = ov5693_poweron_af();
	if (rc < 0) {
		pr_err("%s power on failed\n", __func__);
		return (int) rc;
	}

	//RING_MODE:bit 0
	// 0: 2x(1/fVCM)
	// 1: 1x(1/fVCM) <-- Optical comment

	//PWM/LIN:bit 1
	// 0:PWM mode	<-- used it
	// 1:Linear mode
	rc = msm_camera_i2c_write(&ov5693_act_t.i2c_client,
		REG_VCM_MODE,
		03, //0x01,
		MSM_CAMERA_I2C_BYTE_DATA);
	if (rc < 0) {
		pr_err("%s REG_VCM_MODE i2c write failed (%d)\n", __func__, rc);
		return rc;
	}

	//VCM frequence
	//VCM _ FREQ:  383 - (19200/VCM mechanical ringing frequency)
	//						VCM mechanical ringing frequency = 75.3 Hz
	//						383 - (19200/75.3) = 128
	rc = msm_camera_i2c_write(&ov5693_act_t.i2c_client,
		REG_VCM_FREQ,
		0xAF,//0x80,
		MSM_CAMERA_I2C_BYTE_DATA);
	if (rc < 0) {
		pr_err("%s VCM_CODE_LSB i2c write failed (%d)\n", __func__, rc);
		return rc;
	}

	ov5693_act_t.step_position_table = NULL;
	rc = ov5693_act_t.func_tbl.actuator_init_table(&ov5693_act_t);
	if (rc < 0) {
		pr_err("%s init table failed\n", __func__);
		return (int) rc;
	}

	rc = msm_camera_i2c_write(&(ov5693_act_t.i2c_client),
		0x0001, 0x01,
		MSM_CAMERA_I2C_BYTE_DATA);
	if (rc < 0) {
		pr_err("%s i2c write failed\n", __func__);
		return (int) rc;
	}

	return (int) rc;
}

static struct i2c_driver ov5693_act_i2c_driver = {
	.id_table = ov5693_act_i2c_id,
	.probe  = msm_actuator_i2c_probe,
	.remove = __exit_p(ov5693_act_i2c_remove),
	.driver = {
		.name = "ov5693_act",
	},
};

static int __init ov5693_i2c_add_driver(
	void)
{
	pr_info("%s called\n", __func__);
	return i2c_add_driver(ov5693_act_t.i2c_driver);
}

static struct v4l2_subdev_core_ops ov5693_act_subdev_core_ops;

static struct v4l2_subdev_ops ov5693_act_subdev_ops = {
	.core = &ov5693_act_subdev_core_ops,
};

static int32_t ov5693_act_create_subdevice(
	void *board_info,
	void *sdev)
{
	LINFO("%s called\n", __func__);

	ov5693_msm_actuator_info = (struct msm_actuator_info *)board_info;

	return (int) msm_actuator_create_subdevice(&ov5693_act_t,
		ov5693_msm_actuator_info->board_info,
		(struct v4l2_subdev *)sdev);
}

static struct msm_actuator_ctrl_t ov5693_act_t = {
	.i2c_driver = &ov5693_act_i2c_driver,
	.i2c_addr = REG_VCM_I2C_ADDR,
	.act_v4l2_subdev_ops = &ov5693_act_subdev_ops,
	.actuator_ext_ctrl = {
		.a_init_table = ov5693_i2c_add_driver_table,
		.a_power_down = ov5693_actuator_af_power_down,
		.a_create_subdevice = ov5693_act_create_subdevice,
		.a_config = ov5693_act_config,
	},

	.i2c_client = {
		.addr_type = MSM_CAMERA_I2C_BYTE_ADDR,
	},

	.set_info = {
		.total_steps = OV5693_TOTAL_STEPS_NEAR_TO_FAR,
		.gross_steps = 3,	/*[TBD]*/
		.fine_steps = 1,	/*[TBD]*/
	},

	.curr_step_pos = 0,
	.curr_region_index = 0,
	.initial_code = 0,	/*[TBD]*/
	.actuator_mutex = &ov5693_act_mutex,

	.func_tbl = {
		.actuator_init_table = ov5693_msm_actuator_init_table,
		.actuator_move_focus = ov5693_msm_actuator_move_focus,
		.actuator_write_focus = ov5693_act_write_focus,
		.actuator_set_default_focus = msm_actuator_set_default_focus,
		.actuator_init_focus = ov5693_act_init_focus,
		.actuator_i2c_write = ov5693_wrapper_i2c_write,
	},

	.get_info = {	/*[TBD]*/
		.focal_length_num = 46,
		.focal_length_den = 10,
		.f_number_num = 265,
		.f_number_den = 100,
		.f_pix_num = 14,
		.f_pix_den = 10,
		.total_f_dist_num = 197681,
		.total_f_dist_den = 1000,
	},

	/* Initialize scenario */
	.ringing_scenario[MOVE_NEAR] = g_scenario,
	.scenario_size[MOVE_NEAR] = ARRAY_SIZE(g_scenario),
	.ringing_scenario[MOVE_FAR] = g_scenario,
	.scenario_size[MOVE_FAR] = ARRAY_SIZE(g_scenario),

	/* Initialize region params */
	.region_params = g_regions,
	.region_size = ARRAY_SIZE(g_regions),

	/* Initialize damping params */
	.damping[MOVE_NEAR] = g_damping_params,
	.damping[MOVE_FAR] = g_damping_params,
};

subsys_initcall(ov5693_i2c_add_driver);
MODULE_DESCRIPTION("OV5693 actuator");
MODULE_LICENSE("GPL v2");
