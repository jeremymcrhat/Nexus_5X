/*
 * drivers/mmc/host/sdhci-msm.c - Qualcomm SDHCI Platform driver
 *
 * Copyright (c) 2013-2014, The Linux Foundation. All rights reserved.
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

#define DEBUG

#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/delay.h>
#include <linux/mmc/mmc.h>
#include <linux/slab.h>
#include <linux/iopoll.h>
#include <linux/regulator/consumer.h>

#include "sdhci-pltfm.h"

#define CORE_MCI_VERSION		0x50
#define CORE_VERSION_MAJOR_SHIFT	28
#define CORE_VERSION_MAJOR_MASK		(0xf << CORE_VERSION_MAJOR_SHIFT)
#define CORE_VERSION_MINOR_MASK		0xff

#define CORE_HC_MODE		0x78
#define HC_MODE_EN		0x1
#define CORE_POWER		0x0
#define CORE_SW_RST		BIT(7)
#define FF_CLK_SW_RST_DIS	BIT(13)

#define CORE_PWRCTL_STATUS	0xdc
#define CORE_PWRCTL_MASK	0xe0
#define CORE_PWRCTL_CLEAR	0xe4
#define CORE_PWRCTL_CTL		0xe8
#define CORE_PWRCTL_BUS_OFF	BIT(0)
#define CORE_PWRCTL_BUS_ON	BIT(1)
#define CORE_PWRCTL_IO_LOW	BIT(2)
#define CORE_PWRCTL_IO_HIGH	BIT(3)
#define CORE_PWRCTL_BUS_SUCCESS BIT(0)
#define CORE_PWRCTL_IO_SUCCESS	BIT(2)
#define REQ_BUS_OFF		BIT(0)
#define REQ_BUS_ON		BIT(1)
#define REQ_IO_LOW		BIT(2)
#define REQ_IO_HIGH		BIT(3)
#define INT_MASK		0xf

#define CORE_PWRCTL_BUS_FAIL	BIT(1)
#define CORE_PWRCTL_IO_SUCCESS	BIT(2)
#define CORE_PWRCTL_IO_FAIL	BIT(3)
#define MAX_PHASES		16
#define CORE_DLL_LOCK		BIT(7)
#define CORE_DDR_DLL_LOCK	BIT(11)
#define CORE_DLL_EN		BIT(16)
#define CORE_CDR_EN		BIT(17)
#define CORE_CK_OUT_EN		BIT(18)
#define CORE_CDR_EXT_EN		BIT(19)
#define CORE_DLL_PDN		BIT(29)
#define CORE_DLL_RST		BIT(30)
#define CORE_DLL_CONFIG		0x100
#define CORE_CMD_DAT_TRACK_SEL	BIT(0)
#define CORE_DLL_STATUS		0x108

#define CORE_DLL_CONFIG_2	0x1b4
#define CORE_DDR_CAL_EN		BIT(0)
#define CORE_FLL_CYCLE_CNT	BIT(18)
#define CORE_DLL_CLOCK_DISABLE	BIT(21)

#define CORE_VENDOR_SPEC	0x10c
#define CORE_CLK_PWRSAVE	BIT(1)
#define CORE_VENDOR_SPEC_POR_VAL	0xa1c
#define CORE_IO_PAD_PWR_SWITCH	BIT(16)
#define CORE_HC_MCLK_SEL_DFLT	(2 << 8)
#define CORE_HC_MCLK_SEL_HS400	(3 << 8)
#define CORE_HC_MCLK_SEL_MASK	(3 << 8)
#define CORE_HC_SELECT_IN_EN	BIT(18)
#define CORE_HC_SELECT_IN_HS400	(6 << 19)
#define CORE_HC_SELECT_IN_MASK	(7 << 19)

#define CORE_CSR_CDC_CTLR_CFG0		0x130
#define CORE_SW_TRIG_FULL_CALIB		BIT(16)
#define CORE_HW_AUTOCAL_ENA		BIT(17)

#define CORE_CSR_CDC_CTLR_CFG1		0x134
#define CORE_CSR_CDC_CAL_TIMER_CFG0	0x138
#define CORE_TIMER_ENA			BIT(16)

#define CORE_CSR_CDC_CAL_TIMER_CFG1	0x13C
#define CORE_CSR_CDC_REFCOUNT_CFG	0x140
#define CORE_CSR_CDC_COARSE_CAL_CFG	0x144
#define CORE_CDC_OFFSET_CFG		0x14C
#define CORE_CSR_CDC_DELAY_CFG		0x150
#define CORE_CDC_SLAVE_DDA_CFG		0x160
#define CORE_CSR_CDC_STATUS0		0x164
#define CORE_CALIBRATION_DONE		BIT(0)

#define CORE_CDC_ERROR_CODE_MASK	0x7000000

#define CORE_CSR_CDC_GEN_CFG		0x178
#define CORE_CDC_SWITCH_BYPASS_OFF	BIT(0)
#define CORE_CDC_SWITCH_RC_EN		BIT(1)

#define CORE_DDR_200_CFG		0x184
#define CORE_CDC_T4_DLY_SEL		BIT(0)
#define CORE_START_CDC_TRAFFIC		BIT(6)
#define CORE_VENDOR_SPEC3	0x1b0
#define CORE_PWRSAVE_DLL	BIT(3)

#define CORE_DDR_CONFIG		0x1b8
#define DDR_CONFIG_POR_VAL	0x80040853

#define CORE_VENDOR_SPEC_CAPABILITIES0	0x11c

#define INVALID_TUNING_PHASE	-1
#define TCXO_FREQ		19200000
#define SDHCI_MSM_MIN_CLOCK	400000
#define CORE_FREQ_100MHZ	(100 * 1000 * 1000)

	bool use_14lpp_dll_reset;
#define CDR_SELEXT_SHIFT	20
#define CDR_SELEXT_MASK		(0xf << CDR_SELEXT_SHIFT)
#define CMUX_SHIFT_PHASE_SHIFT	24
#define CMUX_SHIFT_PHASE_MASK	(7 << CMUX_SHIFT_PHASE_SHIFT)

#define CORE_DLL_RST  BIT(30)

/* This structure keeps information per regulator */
struct sdhci_msm_reg_data {
	struct regulator *reg;	/* voltage regulator handle */
	const char *name;	/* regulator name */

	/* voltage level to be set */
	bool tuning_done;
	bool calibration_done;
	u8 saved_tuning_phase;
	bool use_cdclp533;
	u32 low_vol_level;
	u32 high_vol_level;

	/* Load values for low power and high power mode */
	u32 lpm_uA;
	u32 hpm_uA;

	bool is_enabled;
	bool is_always_on;

	/* is low power mode setting required for this regulator? */
	bool lpm_sup;
};

/*
 * This structure keeps information for all the
 * regulators required for a SDCC slot.
 */
struct sdhci_msm_slot_reg_data {
	struct sdhci_msm_reg_data *vdd_data;
	struct sdhci_msm_reg_data *vdd_io_data;
};

struct sdhci_msm_pltfm_data {
	struct sdhci_msm_slot_reg_data *vreg_data;
};

enum vdd_io_level {
	/* set vdd_io_data->low_vol_level */
	VDD_IO_LOW,
	/* set vdd_io_data->high_vol_level */
	VDD_IO_HIGH,
	/*
	 * set whatever there in voltage_level (third argument) of
	 * sdhci_msm_set_vdd_io_vol() function.
	 */
	VDD_IO_SET_LEVEL,
};

struct sdhci_msm_host {
	struct platform_device *pdev;
	void __iomem *core_mem;	/* MSM SDCC mapped address */
	int pwr_irq;		/* power irq */
	struct clk *clk;	/* main SD/MMC bus clock */
	struct clk *pclk;	/* SDHC peripheral bus clock */
	struct clk *bus_clk;	/* SDHC bus voter clock */
	unsigned long clk_rate;
	struct mmc_host *mmc;
	struct sdhci_msm_pltfm_data *pdata;
	u32 curr_pwr_state;
	u32 curr_io_level;
	struct completion pwr_irq_completion;
	spinlock_t pwr_irq_lock;
	bool use_updated_dll_reset;
	bool tuning_done;
	bool calibration_done;
	bool use_14lpp_dll_reset;
	u8 saved_tuning_phase;
	bool use_cdclp533;
};

#define MAX_PROP_SIZE 32
static int sdhci_msm_dt_parse_vreg_info(struct device *dev,
		struct sdhci_msm_reg_data **vreg_data, const char *vreg_name)
{
	int len, ret = 0;
	const __be32 *prop;
	char prop_name[MAX_PROP_SIZE];
	struct sdhci_msm_reg_data *vreg;
	struct device_node *np = dev->of_node;

	snprintf(prop_name, MAX_PROP_SIZE, "%s-supply", vreg_name);
	if (!of_parse_phandle(np, prop_name, 0)) {
		dev_err(dev, "No vreg data found for %s\n", vreg_name);
		ret = -EINVAL;
		return ret;
	}

	vreg = devm_kzalloc(dev, sizeof(*vreg), GFP_KERNEL);
	if (!vreg) {
		ret = -ENOMEM;
		return ret;
	}

	vreg->name = vreg_name;

	snprintf(prop_name, MAX_PROP_SIZE,
			"qcom,%s-always-on", vreg_name);
	if (of_get_property(np, prop_name, NULL))
		vreg->is_always_on = true;

	snprintf(prop_name, MAX_PROP_SIZE,
			"qcom,%s-lpm-sup", vreg_name);
	if (of_get_property(np, prop_name, NULL))
		vreg->lpm_sup = true;

	snprintf(prop_name, MAX_PROP_SIZE,
			"qcom,%s-voltage-level", vreg_name);
	prop = of_get_property(np, prop_name, &len);
	if (!prop || (len != (2 * sizeof(__be32)))) {
		dev_warn(dev, "%s %s property\n",
			prop ? "invalid format" : "no", prop_name);
	} else {
		vreg->low_vol_level = be32_to_cpup(&prop[0]);
		vreg->high_vol_level = be32_to_cpup(&prop[1]);
	}

	snprintf(prop_name, MAX_PROP_SIZE,
			"qcom,%s-current-level", vreg_name);
	prop = of_get_property(np, prop_name, &len);
	if (!prop || (len != (2 * sizeof(__be32)))) {
		dev_warn(dev, "%s %s property\n",
			prop ? "invalid format" : "no", prop_name);
	} else {
		vreg->lpm_uA = be32_to_cpup(&prop[0]);
		vreg->hpm_uA = be32_to_cpup(&prop[1]);
	}

	*vreg_data = vreg;
	dev_dbg(dev, "%s: %s %s vol=[%d %d]uV, curr=[%d %d]uA\n",
		vreg->name, vreg->is_always_on ? "always_on," : "",
		vreg->lpm_sup ? "lpm_sup," : "", vreg->low_vol_level,
		vreg->high_vol_level, vreg->lpm_uA, vreg->hpm_uA);

	return ret;
}

/* Parse platform data */
static struct sdhci_msm_pltfm_data *sdhci_msm_populate_pdata(struct device *dev)
{
	struct sdhci_msm_pltfm_data *pdata = NULL;

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		goto out;

	pdata->vreg_data = devm_kzalloc(dev, sizeof(struct
						    sdhci_msm_slot_reg_data),
					GFP_KERNEL);
	if (!pdata->vreg_data)
		goto out;

	if (sdhci_msm_dt_parse_vreg_info(dev, &pdata->vreg_data->vdd_data,
					 "vdd")) {
		dev_err(dev, "failed parsing vdd data\n");
		goto out;
	}

	if (sdhci_msm_dt_parse_vreg_info(dev,
					 &pdata->vreg_data->vdd_io_data,
					 "vdd-io")) {
		dev_err(dev, "failed parsing vdd-io data\n");
		goto out;
	}

	return pdata;
out:
	return NULL;
}

/* Regulator utility functions */
static int sdhci_msm_vreg_init_reg(struct device *dev,
					struct sdhci_msm_reg_data *vreg)
{
	int ret = 0;

	/* check if regulator is already initialized? */
	if (vreg->reg)
		goto out;

	/* Get the regulator handle */
	vreg->reg = devm_regulator_get(dev, vreg->name);
	if (IS_ERR(vreg->reg)) {
		ret = PTR_ERR(vreg->reg);
		pr_err("%s: devm_regulator_get(%s) failed. ret=%d\n",
			__func__, vreg->name, ret);
		goto out;
	}

	/* sanity check */
	if (!vreg->high_vol_level || !vreg->hpm_uA) {
		pr_err("%s: %s invalid constraints specified\n",
		       __func__, vreg->name);
		ret = -EINVAL;
	}

out:
	return ret;
}

static void sdhci_msm_vreg_deinit_reg(struct sdhci_msm_reg_data *vreg)
{
	if (vreg->reg)
		devm_regulator_put(vreg->reg);
}

static int sdhci_msm_vreg_set_optimum_mode(struct sdhci_msm_reg_data
						  *vreg, int uA_load)
{
	int ret = 0;

	/*
	 * regulators that do not support regulator_set_voltage also
	 * do not support regulator_set_load
	 */
	ret = regulator_set_load(vreg->reg, uA_load);
	if (ret < 0)
		pr_err("%s: regulator_set_load(reg=%s,uA_load=%d) failed. ret=%d\n",
			       __func__, vreg->name, uA_load, ret);
	else
		/*
		 * regulator_set_load() can return non zero
		 * value even for success case.
		 */
		ret = 0;

	return ret;
}

static int sdhci_msm_vreg_set_voltage(struct sdhci_msm_reg_data *vreg,
					int min_uV, int max_uV)
{
	int ret = 0;

	ret = regulator_set_voltage(vreg->reg, min_uV, max_uV);
	if (ret)
		pr_err("%s: regulator_set_voltage(%s)failed. min_uV=%d,max_uV=%d,ret=%d\n",
			       __func__, vreg->name, min_uV, max_uV, ret);

	return ret;
}

static int sdhci_msm_vreg_enable(struct sdhci_msm_reg_data *vreg)
{
	int ret = 0;

	/* Put regulator in HPM (high power mode) */
	ret = sdhci_msm_vreg_set_optimum_mode(vreg, vreg->hpm_uA);
	if (ret < 0)
		goto out;

	if (!vreg->is_enabled) {
		/* Set voltage level */
		ret = sdhci_msm_vreg_set_voltage(vreg, vreg->high_vol_level,
						vreg->high_vol_level);
		if (ret)
			goto out;
	}
	ret = regulator_enable(vreg->reg);
	if (ret) {
		pr_err("%s: regulator_enable(%s) failed. ret=%d\n",
				__func__, vreg->name, ret);
		goto out;
	}
	vreg->is_enabled = true;

out:
	return ret;
}

static int sdhci_msm_vreg_disable(struct sdhci_msm_reg_data *vreg)
{
	int ret = 0;

	/* Never disable regulator marked as always_on */
	if (vreg->is_enabled && !vreg->is_always_on) {
		ret = regulator_disable(vreg->reg);
		if (ret) {
			pr_err("%s: regulator_disable(%s) failed. ret=%d\n",
				__func__, vreg->name, ret);
			goto out;
		}
		vreg->is_enabled = false;

		ret = sdhci_msm_vreg_set_optimum_mode(vreg, 0);
		if (ret < 0)
			goto out;

		/* Set min. voltage level to 0 */
		ret = sdhci_msm_vreg_set_voltage(vreg, 0, vreg->high_vol_level);
		if (ret)
			goto out;
	} else if (vreg->is_enabled && vreg->is_always_on) {
		if (vreg->lpm_sup) {
			/* Put always_on regulator in LPM (low power mode) */
			ret = sdhci_msm_vreg_set_optimum_mode(vreg,
							      vreg->lpm_uA);
			if (ret < 0)
				goto out;
		}
	}
out:
	return ret;
}

static int sdhci_msm_setup_vreg(struct sdhci_msm_pltfm_data *pdata,
			bool enable, bool is_init)
{
	int ret = 0, i;
	struct sdhci_msm_slot_reg_data *curr_slot;
	struct sdhci_msm_reg_data *vreg_table[2];

	curr_slot = pdata->vreg_data;
	if (!curr_slot) {
		pr_debug("%s: vreg info unavailable,assuming the slot is powered by always on domain\n",
			 __func__);
		goto out;
	}

	vreg_table[0] = curr_slot->vdd_data;
	vreg_table[1] = curr_slot->vdd_io_data;

	for (i = 0; i < ARRAY_SIZE(vreg_table); i++) {
		if (vreg_table[i]) {
			if (enable)
				ret = sdhci_msm_vreg_enable(vreg_table[i]);
			else
				ret = sdhci_msm_vreg_disable(vreg_table[i]);
			if (ret)
				goto out;
		}
	}
out:
	return ret;
}

/*
 * Reset vreg by ensuring it is off during probe. A call
 * to enable vreg is needed to balance disable vreg
 */
static int sdhci_msm_vreg_reset(struct sdhci_msm_pltfm_data *pdata)
{
	int ret;

	ret = sdhci_msm_setup_vreg(pdata, 1, true);
	if (ret)
		return ret;
	ret = sdhci_msm_setup_vreg(pdata, 0, true);
	return ret;
}

/* This init function should be called only once for each SDHC slot */
static int sdhci_msm_vreg_init(struct device *dev,
				struct sdhci_msm_pltfm_data *pdata,
				bool is_init)
{
	int ret = 0;
	struct sdhci_msm_slot_reg_data *curr_slot;
	struct sdhci_msm_reg_data *curr_vdd_reg, *curr_vdd_io_reg;

	curr_slot = pdata->vreg_data;
	if (!curr_slot)
		goto out;

	curr_vdd_reg = curr_slot->vdd_data;
	curr_vdd_io_reg = curr_slot->vdd_io_data;

	if (!is_init)
		/* Deregister all regulators from regulator framework */
		goto vdd_io_reg_deinit;

	/*
	 * Get the regulator handle from voltage regulator framework
	 * and then try to set the voltage level for the regulator
	 */
	if (curr_vdd_reg) {
		ret = sdhci_msm_vreg_init_reg(dev, curr_vdd_reg);
		if (ret)
			goto out;
	}
	if (curr_vdd_io_reg) {
		ret = sdhci_msm_vreg_init_reg(dev, curr_vdd_io_reg);
		if (ret)
			goto vdd_reg_deinit;
	}
	ret = sdhci_msm_vreg_reset(pdata);
	if (ret)
		dev_err(dev, "vreg reset failed (%d)\n", ret);
	goto out;

vdd_io_reg_deinit:
	if (curr_vdd_io_reg)
		sdhci_msm_vreg_deinit_reg(curr_vdd_io_reg);
vdd_reg_deinit:
	if (curr_vdd_reg)
		sdhci_msm_vreg_deinit_reg(curr_vdd_reg);
out:
	return ret;
}


static int sdhci_msm_set_vdd_io_vol(struct sdhci_msm_pltfm_data *pdata,
			enum vdd_io_level level,
			unsigned int voltage_level)
{
	int ret = 0;
	int set_level;
	struct sdhci_msm_reg_data *vdd_io_reg;

	if (!pdata->vreg_data)
		return ret;

	vdd_io_reg = pdata->vreg_data->vdd_io_data;
	if (vdd_io_reg && vdd_io_reg->is_enabled) {
		switch (level) {
		case VDD_IO_LOW:
			set_level = vdd_io_reg->low_vol_level;
			break;
		case VDD_IO_HIGH:
			set_level = vdd_io_reg->high_vol_level;
			break;
		case VDD_IO_SET_LEVEL:
			set_level = voltage_level;
			break;
		default:
			pr_err("%s: invalid argument level = %d",
					__func__, level);
			ret = -EINVAL;
			return ret;
		}
		ret = sdhci_msm_vreg_set_voltage(vdd_io_reg, set_level,
				set_level);
	}
	return ret;
}

static irqreturn_t sdhci_msm_pwr_irq(int irq, void *data)
{
	struct sdhci_host *host = (struct sdhci_host *)data;
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_msm_host *msm_host = sdhci_pltfm_priv(pltfm_host);
	u8 irq_status = 0;
	u8 irq_ack = 0;
	int ret = 0;
	int pwr_state = 0, io_level = 0;
	unsigned long flags;

	irq_status = readb_relaxed(msm_host->core_mem + CORE_PWRCTL_STATUS);
	pr_debug("%s: Received IRQ(%d), status=0x%x\n",
		mmc_hostname(msm_host->mmc), irq, irq_status);

	/* Clear the interrupt */
	writeb_relaxed(irq_status, (msm_host->core_mem + CORE_PWRCTL_CLEAR));
	/*
	 * SDHC has core_mem and hc_mem device memory and these memory
	 * addresses do not fall within 1KB region. Hence, any update to
	 * core_mem address space would require an mb() to ensure this gets
	 * completed before its next update to registers within hc_mem.
	 */
	mb();

	/* Handle BUS ON/OFF*/
	if (irq_status & CORE_PWRCTL_BUS_ON) {
		ret = sdhci_msm_setup_vreg(msm_host->pdata, true, false);
		if (!ret) {
			ret = sdhci_msm_set_vdd_io_vol(msm_host->pdata,
					VDD_IO_HIGH, 0);
		}
		if (ret)
			irq_ack |= CORE_PWRCTL_BUS_FAIL;
		else
			irq_ack |= CORE_PWRCTL_BUS_SUCCESS;

		pwr_state = REQ_BUS_ON;
		io_level = REQ_IO_HIGH;
	}
	if (irq_status & CORE_PWRCTL_BUS_OFF) {
		ret = sdhci_msm_setup_vreg(msm_host->pdata, false, false);
		if (!ret) {
			ret = sdhci_msm_set_vdd_io_vol(msm_host->pdata,
					VDD_IO_LOW, 0);
		}
		if (ret)
			irq_ack |= CORE_PWRCTL_BUS_FAIL;
		else
			irq_ack |= CORE_PWRCTL_BUS_SUCCESS;

		pwr_state = REQ_BUS_OFF;
		io_level = REQ_IO_LOW;
	}
	/* Handle IO LOW/HIGH */
	if (irq_status & CORE_PWRCTL_IO_LOW) {
		/* Switch voltage Low */
		ret = sdhci_msm_set_vdd_io_vol(msm_host->pdata, VDD_IO_LOW, 0);
		if (ret)
			irq_ack |= CORE_PWRCTL_IO_FAIL;
		else
			irq_ack |= CORE_PWRCTL_IO_SUCCESS;

		io_level = REQ_IO_LOW;
	}
	if (irq_status & CORE_PWRCTL_IO_HIGH) {
		/* Switch voltage High */
		ret = sdhci_msm_set_vdd_io_vol(msm_host->pdata, VDD_IO_HIGH, 0);
		if (ret)
			irq_ack |= CORE_PWRCTL_IO_FAIL;
		else
			irq_ack |= CORE_PWRCTL_IO_SUCCESS;

		io_level = REQ_IO_HIGH;
	}

	/* ACK status to the core */
	writeb_relaxed(irq_ack, (msm_host->core_mem + CORE_PWRCTL_CTL));
	/*
	 * SDHC has core_mem and hc_mem device memory and these memory
	 * addresses do not fall within 1KB region. Hence, any update to
	 * core_mem address space would require an mb() to ensure this gets
	 * completed before its next update to registers within hc_mem.
	 */
	mb();

	if (io_level & REQ_IO_HIGH)
		writel_relaxed((readl_relaxed(host->ioaddr + CORE_VENDOR_SPEC) &
				~CORE_IO_PAD_PWR_SWITCH),
				host->ioaddr + CORE_VENDOR_SPEC);
	else if (io_level & REQ_IO_LOW)
		writel_relaxed((readl_relaxed(host->ioaddr + CORE_VENDOR_SPEC) |
				CORE_IO_PAD_PWR_SWITCH),
				host->ioaddr + CORE_VENDOR_SPEC);

	/* Ensure before completion that above writes are propagated */
	mb();

	pr_debug("%s: Handled IRQ(%d), ret=%d, ack=0x%x\n",
		mmc_hostname(msm_host->mmc), irq, ret, irq_ack);
	spin_lock_irqsave(&msm_host->pwr_irq_lock, flags);
	if (pwr_state)
		msm_host->curr_pwr_state = pwr_state;
	if (io_level)
		msm_host->curr_io_level = io_level;
	complete(&msm_host->pwr_irq_completion);
	spin_unlock_irqrestore(&msm_host->pwr_irq_lock, flags);

	return IRQ_HANDLED;
}

static void sdhci_msm_check_power_status(struct sdhci_host *host, u32 req_type)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_msm_host *msm_host = sdhci_pltfm_priv(pltfm_host);
	unsigned long flags;
	bool locked = false;
	bool done = false;

	pr_debug("%s: %s: power status before waiting 0x%x\n",
		mmc_hostname(host->mmc), __func__,
		readb_relaxed(msm_host->core_mem + CORE_PWRCTL_CTL));

	if (spin_is_locked(&host->lock))
		locked = true;

	spin_lock_irqsave(&msm_host->pwr_irq_lock, flags);
	pr_debug("%s: %s: request %d curr_pwr_state %x curr_io_level %x\n",
			mmc_hostname(host->mmc), __func__, req_type,
			msm_host->curr_pwr_state, msm_host->curr_io_level);
	if ((req_type & msm_host->curr_pwr_state) ||
			(req_type & msm_host->curr_io_level))
		done = true;
	spin_unlock_irqrestore(&msm_host->pwr_irq_lock, flags);

	if (locked)
		spin_unlock_irq(&host->lock);
	/*
	 * This is needed here to hanlde a case where IRQ gets
	 * triggered even before this function is called so that
	 * x->done counter of completion gets reset. Otherwise,
	 * next call to wait_for_completion returns immediately
	 * without actually waiting for the IRQ to be handled.
	 */
	if (done)
		init_completion(&msm_host->pwr_irq_completion);
	else
		wait_for_completion(&msm_host->pwr_irq_completion);

	if (locked)
		spin_lock_irq(&host->lock);

	pr_debug("%s: %s: request %d done\n", mmc_hostname(host->mmc),
			__func__, req_type);
}

/* Platform specific tuning */
static inline int msm_dll_poll_ck_out_en(struct sdhci_host *host, u8 poll)
{
	u32 wait_cnt = 50;
	u8 ck_out_en;
	struct mmc_host *mmc = host->mmc;

	/* Poll for CK_OUT_EN bit.  max. poll time = 50us */
	ck_out_en = !!(readl_relaxed(host->ioaddr + CORE_DLL_CONFIG) &
			CORE_CK_OUT_EN);

	while (ck_out_en != poll) {
		if (--wait_cnt == 0) {
			dev_err(mmc_dev(mmc), "%s: CK_OUT_EN bit is not %d\n",
			       mmc_hostname(mmc), poll);
			return -ETIMEDOUT;
		}
		udelay(1);

		ck_out_en = !!(readl_relaxed(host->ioaddr + CORE_DLL_CONFIG) &
				CORE_CK_OUT_EN);
	}

	return 0;
}

static int msm_config_cm_dll_phase(struct sdhci_host *host, u8 phase)
{
	int rc;
	static const u8 grey_coded_phase_table[] = {
		0x0, 0x1, 0x3, 0x2, 0x6, 0x7, 0x5, 0x4,
		0xc, 0xd, 0xf, 0xe, 0xa, 0xb, 0x9, 0x8
	};
	unsigned long flags;
	u32 config;
	struct mmc_host *mmc = host->mmc;

	if (phase > 0xf)
		return -EINVAL;

	spin_lock_irqsave(&host->lock, flags);

	config = readl_relaxed(host->ioaddr + CORE_DLL_CONFIG);
	config &= ~(CORE_CDR_EN | CORE_CK_OUT_EN);
	config |= (CORE_CDR_EXT_EN | CORE_DLL_EN);
	writel_relaxed(config, host->ioaddr + CORE_DLL_CONFIG);

	/* Wait until CK_OUT_EN bit of DLL_CONFIG register becomes '0' */
	rc = msm_dll_poll_ck_out_en(host, 0);
	if (rc)
		goto err_out;

	/*
	 * Write the selected DLL clock output phase (0 ... 15)
	 * to CDR_SELEXT bit field of DLL_CONFIG register.
	 */
	config = readl_relaxed(host->ioaddr + CORE_DLL_CONFIG);
	config &= ~CDR_SELEXT_MASK;
	config |= grey_coded_phase_table[phase] << CDR_SELEXT_SHIFT;
	writel_relaxed(config, host->ioaddr + CORE_DLL_CONFIG);

	/* Set CK_OUT_EN bit of DLL_CONFIG register to 1. */
	config = readl_relaxed(host->ioaddr + CORE_DLL_CONFIG);
	config |= CORE_CK_OUT_EN;
	writel_relaxed(config, host->ioaddr + CORE_DLL_CONFIG);

	/* Wait until CK_OUT_EN bit of DLL_CONFIG register becomes '1' */
	rc = msm_dll_poll_ck_out_en(host, 1);
	if (rc)
		goto err_out;

	config = readl_relaxed(host->ioaddr + CORE_DLL_CONFIG);
	config |= CORE_CDR_EN;
	config &= ~CORE_CDR_EXT_EN;
	writel_relaxed(config, host->ioaddr + CORE_DLL_CONFIG);
	goto out;

err_out:
	dev_err(mmc_dev(mmc), "%s: Failed to set DLL phase: %d\n",
	       mmc_hostname(mmc), phase);
out:
	spin_unlock_irqrestore(&host->lock, flags);
	return rc;
}

/*
 * Find out the greatest range of consecuitive selected
 * DLL clock output phases that can be used as sampling
 * setting for SD3.0 UHS-I card read operation (in SDR104
 * timing mode) or for eMMC4.5 card read operation (in
 * HS400/HS200 timing mode).
 * Select the 3/4 of the range and configure the DLL with the
 * selected DLL clock output phase.
 */

static int msm_find_most_appropriate_phase(struct sdhci_host *host,
					   u8 *phase_table, u8 total_phases)
{
	int ret;
	u8 ranges[MAX_PHASES][MAX_PHASES] = { {0}, {0} };
	u8 phases_per_row[MAX_PHASES] = { 0 };
	int row_index = 0, col_index = 0, selected_row_index = 0, curr_max = 0;
	int i, cnt, phase_0_raw_index = 0, phase_15_raw_index = 0;
	bool phase_0_found = false, phase_15_found = false;
	struct mmc_host *mmc = host->mmc;

	if (!total_phases || (total_phases > MAX_PHASES)) {
		dev_err(mmc_dev(mmc), "%s: Invalid argument: total_phases=%d\n",
		       mmc_hostname(mmc), total_phases);
		return -EINVAL;
	}

	for (cnt = 0; cnt < total_phases; cnt++) {
		ranges[row_index][col_index] = phase_table[cnt];
		phases_per_row[row_index] += 1;
		col_index++;

		if ((cnt + 1) == total_phases) {
			continue;
		/* check if next phase in phase_table is consecutive or not */
		} else if ((phase_table[cnt] + 1) != phase_table[cnt + 1]) {
			row_index++;
			col_index = 0;
		}
	}

	if (row_index >= MAX_PHASES)
		return -EINVAL;

	/* Check if phase-0 is present in first valid window? */
	if (!ranges[0][0]) {
		phase_0_found = true;
		phase_0_raw_index = 0;
		/* Check if cycle exist between 2 valid windows */
		for (cnt = 1; cnt <= row_index; cnt++) {
			if (phases_per_row[cnt]) {
				for (i = 0; i < phases_per_row[cnt]; i++) {
					if (ranges[cnt][i] == 15) {
						phase_15_found = true;
						phase_15_raw_index = cnt;
						break;
					}
				}
			}
		}
	}

	/* If 2 valid windows form cycle then merge them as single window */
	if (phase_0_found && phase_15_found) {
		/* number of phases in raw where phase 0 is present */
		u8 phases_0 = phases_per_row[phase_0_raw_index];
		/* number of phases in raw where phase 15 is present */
		u8 phases_15 = phases_per_row[phase_15_raw_index];

		if (phases_0 + phases_15 >= MAX_PHASES)
			/*
			 * If there are more than 1 phase windows then total
			 * number of phases in both the windows should not be
			 * more than or equal to MAX_PHASES.
			 */
			return -EINVAL;

		/* Merge 2 cyclic windows */
		i = phases_15;
		for (cnt = 0; cnt < phases_0; cnt++) {
			ranges[phase_15_raw_index][i] =
			    ranges[phase_0_raw_index][cnt];
			if (++i >= MAX_PHASES)
				break;
		}

		phases_per_row[phase_0_raw_index] = 0;
		phases_per_row[phase_15_raw_index] = phases_15 + phases_0;
	}

	for (cnt = 0; cnt <= row_index; cnt++) {
		if (phases_per_row[cnt] > curr_max) {
			curr_max = phases_per_row[cnt];
			selected_row_index = cnt;
		}
	}

	i = (curr_max * 3) / 4;
	if (i)
		i--;

	ret = ranges[selected_row_index][i];

	if (ret >= MAX_PHASES) {
		ret = -EINVAL;
		dev_err(mmc_dev(mmc), "%s: Invalid phase selected=%d\n",
		       mmc_hostname(mmc), ret);
	}

	return ret;
}

static inline void msm_cm_dll_set_freq(struct sdhci_host *host)
{
	u32 mclk_freq = 0, config;

	/* Program the MCLK value to MCLK_FREQ bit field */
	if (host->clock <= 112000000)
		mclk_freq = 0;
	else if (host->clock <= 125000000)
		mclk_freq = 1;
	else if (host->clock <= 137000000)
		mclk_freq = 2;
	else if (host->clock <= 150000000)
		mclk_freq = 3;
	else if (host->clock <= 162000000)
		mclk_freq = 4;
	else if (host->clock <= 175000000)
		mclk_freq = 5;
	else if (host->clock <= 187000000)
		mclk_freq = 6;
	else if (host->clock <= 200000000)
		mclk_freq = 7;

	config = readl_relaxed(host->ioaddr + CORE_DLL_CONFIG);
	config &= ~CMUX_SHIFT_PHASE_MASK;
	config |= mclk_freq << CMUX_SHIFT_PHASE_SHIFT;
	writel_relaxed(config, host->ioaddr + CORE_DLL_CONFIG);
}

/* Initialize the DLL (Programmable Delay Line) */
static int msm_init_cm_dll(struct sdhci_host *host)
{
	struct mmc_host *mmc = host->mmc;
	struct sdhci_pltfm_host *pltfm_host;
	struct sdhci_msm_host *msm_host;
	int wait_cnt = 50;
	unsigned long flags;
	u32 config = 0;

	pltfm_host = sdhci_priv(host);
	msm_host = sdhci_pltfm_priv(pltfm_host);

	spin_lock_irqsave(&host->lock, flags);

	/*
	 * Make sure that clock is always enabled when DLL
	 * tuning is in progress. Keeping PWRSAVE ON may
	 * turn off the clock.
	 */
	config = readl_relaxed(host->ioaddr + CORE_VENDOR_SPEC);
	config &= ~CORE_CLK_PWRSAVE;
	writel_relaxed(config, host->ioaddr + CORE_VENDOR_SPEC);

	if (msm_host->use_updated_dll_reset) {
		writel_relaxed((readl_relaxed(host->ioaddr + CORE_DLL_CONFIG)
				& ~CORE_CK_OUT_EN),
				host->ioaddr + CORE_DLL_CONFIG);

		writel_relaxed((readl_relaxed(host->ioaddr + CORE_DLL_CONFIG_2)
				| CORE_DLL_CLOCK_DISABLE),
				host->ioaddr + CORE_DLL_CONFIG_2);
	}


	if (msm_host->use_14lpp_dll_reset) {
		config = readl_relaxed(host->ioaddr + CORE_DLL_CONFIG);
		config &= ~CORE_CK_OUT_EN;
		writel_relaxed(config, host->ioaddr + CORE_DLL_CONFIG);

		config = readl_relaxed(host->ioaddr + CORE_DLL_CONFIG_2);
		config |= CORE_DLL_CLOCK_DISABLE;
		writel_relaxed(config, host->ioaddr + CORE_DLL_CONFIG_2);
	}

	/* Write 1 to DLL_RST bit of DLL_CONFIG register */
	config = readl_relaxed(host->ioaddr + CORE_DLL_CONFIG);
	config |= CORE_DLL_RST;
	writel_relaxed(config, host->ioaddr + CORE_DLL_CONFIG);

	/* Write 1 to DLL_PDN bit of DLL_CONFIG register */
	config = readl_relaxed(host->ioaddr + CORE_DLL_CONFIG);
	config |= CORE_DLL_PDN;
	writel_relaxed(config, host->ioaddr + CORE_DLL_CONFIG);
	msm_cm_dll_set_freq(host);

	if (msm_host->use_updated_dll_reset) {
		u32 mclk_freq = 0;

		if ((readl_relaxed(host->ioaddr + CORE_DLL_CONFIG_2)
					& CORE_FLL_CYCLE_CNT))
			mclk_freq = (u32) ((host->clock / TCXO_FREQ) * 8);
		else
			mclk_freq = (u32) ((host->clock / TCXO_FREQ) * 4);

		writel_relaxed(((readl_relaxed(host->ioaddr + CORE_DLL_CONFIG_2)
				& ~(0xFF << 10)) | (mclk_freq << 10)),
				host->ioaddr + CORE_DLL_CONFIG_2);
		/* wait for 5us before enabling DLL clock */
		udelay(5);
	}


	if (msm_host->use_14lpp_dll_reset) {
		u32 mclk_freq = 0;

		if ((readl_relaxed(host->ioaddr + CORE_DLL_CONFIG_2)
					& CORE_FLL_CYCLE_CNT))
			mclk_freq = (u32)((host->clock / TCXO_FREQ) * 8);
		else
			mclk_freq = (u32)((host->clock / TCXO_FREQ) * 4);

		config = readl_relaxed(host->ioaddr + CORE_DLL_CONFIG_2);
		config &= ~(0xFF << 10);
		config |= mclk_freq << 10;

		writel_relaxed(config, host->ioaddr + CORE_DLL_CONFIG_2);
		/* wait for 5us before enabling DLL clock */
		udelay(5);
	}

	/* Write 0 to DLL_RST bit of DLL_CONFIG register */
	config = readl_relaxed(host->ioaddr + CORE_DLL_CONFIG);
	config &= ~CORE_DLL_RST;
	writel_relaxed(config, host->ioaddr + CORE_DLL_CONFIG);


	/* Write 0 to DLL_PDN bit of DLL_CONFIG register */
	config = readl_relaxed(host->ioaddr + CORE_DLL_CONFIG);
	config &= ~CORE_DLL_PDN;
	writel_relaxed(config, host->ioaddr + CORE_DLL_CONFIG);


	if (msm_host->use_updated_dll_reset) {
		msm_cm_dll_set_freq(host);
		/* Enable the DLL clock */
		writel_relaxed((readl_relaxed(host->ioaddr + CORE_DLL_CONFIG_2)
				& ~CORE_DLL_CLOCK_DISABLE),
				host->ioaddr + CORE_DLL_CONFIG_2);
	}

	if (msm_host->use_14lpp_dll_reset) {
		msm_cm_dll_set_freq(host);
		/* Enable the DLL clock */
		config = readl_relaxed(host->ioaddr + CORE_DLL_CONFIG_2);
		config &= ~CORE_DLL_CLOCK_DISABLE;
		writel_relaxed(config, host->ioaddr + CORE_DLL_CONFIG_2);
	}

	/* Set DLL_EN bit to 1. */
	config = readl_relaxed(host->ioaddr + CORE_DLL_CONFIG);
	config |= CORE_DLL_EN;
	writel_relaxed(config, host->ioaddr + CORE_DLL_CONFIG);


	/* Set CK_OUT_EN bit to 1. */
	config = readl_relaxed(host->ioaddr + CORE_DLL_CONFIG);
	config |= CORE_CK_OUT_EN;
	writel_relaxed(config, host->ioaddr + CORE_DLL_CONFIG);


	/* Wait until DLL_LOCK bit of DLL_STATUS register becomes '1' */
	while (!(readl_relaxed(host->ioaddr + CORE_DLL_STATUS) &
		 CORE_DLL_LOCK)) {
		/* max. wait for 50us sec for LOCK bit to be set */
		if (--wait_cnt == 0) {
			dev_err(mmc_dev(mmc), "%s: DLL failed to LOCK\n",
			       mmc_hostname(mmc));
			spin_unlock_irqrestore(&host->lock, flags);
			return -ETIMEDOUT;
		}
		udelay(1);
	}

	spin_unlock_irqrestore(&host->lock, flags);
	return 0;
}

static int sdhci_msm_cdclp533_calibration(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_msm_host *msm_host = sdhci_pltfm_priv(pltfm_host);
	u32 config, calib_done;
	int ret;

	printk("%s: %s: Enter\n", mmc_hostname(host->mmc), __func__);

	/*
	 * Retuning in HS400 (DDR mode) will fail, just reset the
	 * tuning block and restore the saved tuning phase.
	 */
	ret = msm_init_cm_dll(host);
	if (ret)
		goto out;

	/* Set the selected phase in delay line hw block */
	ret = msm_config_cm_dll_phase(host, msm_host->saved_tuning_phase);
	if (ret)
		goto out;

	/* Write 1 to CMD_DAT_TRACK_SEL field in DLL_CONFIG */
	config = readl_relaxed(host->ioaddr + CORE_DLL_CONFIG);
	config |= CORE_CMD_DAT_TRACK_SEL;
	writel_relaxed(config, host->ioaddr + CORE_DLL_CONFIG);

	/* Write 0 to CDC_T4_DLY_SEL field in VENDOR_SPEC_DDR200_CFG */
	config = readl_relaxed(host->ioaddr + CORE_DDR_200_CFG);
	config &= ~CORE_CDC_T4_DLY_SEL;
	writel_relaxed(config, host->ioaddr + CORE_DDR_200_CFG);

	/* Write 0 to CDC_SWITCH_BYPASS_OFF field in CORE_CSR_CDC_GEN_CFG */
	config = readl_relaxed(host->ioaddr + CORE_CSR_CDC_GEN_CFG);
	config &= ~CORE_CDC_SWITCH_BYPASS_OFF;
	writel_relaxed(config, host->ioaddr + CORE_CSR_CDC_GEN_CFG);

	/* Write 1 to CDC_SWITCH_RC_EN field in CORE_CSR_CDC_GEN_CFG */
	config = readl_relaxed(host->ioaddr + CORE_CSR_CDC_GEN_CFG);
	config |= CORE_CDC_SWITCH_RC_EN;
	writel_relaxed(config, host->ioaddr + CORE_CSR_CDC_GEN_CFG);

	/* Write 0 to START_CDC_TRAFFIC field in CORE_DDR200_CFG */
	config = readl_relaxed(host->ioaddr + CORE_DDR_200_CFG);
	config &= ~CORE_START_CDC_TRAFFIC;
	writel_relaxed(config, host->ioaddr + CORE_DDR_200_CFG);

	/*
	 * Perform CDC Register Initialization Sequence
	 *
	 * CORE_CSR_CDC_CTLR_CFG0	0x11800EC
	 * CORE_CSR_CDC_CTLR_CFG1	0x3011111
	 * CORE_CSR_CDC_CAL_TIMER_CFG0	0x1201000
	 * CORE_CSR_CDC_CAL_TIMER_CFG1	0x4
	 * CORE_CSR_CDC_REFCOUNT_CFG	0xCB732020
	 * CORE_CSR_CDC_COARSE_CAL_CFG	0xB19
	 * CORE_CSR_CDC_DELAY_CFG	0x3AC
	 * CORE_CDC_OFFSET_CFG		0x0
	 * CORE_CDC_SLAVE_DDA_CFG	0x16334
	 */

	writel_relaxed(0x11800EC, host->ioaddr + CORE_CSR_CDC_CTLR_CFG0);
	writel_relaxed(0x3011111, host->ioaddr + CORE_CSR_CDC_CTLR_CFG1);
	writel_relaxed(0x1201000, host->ioaddr + CORE_CSR_CDC_CAL_TIMER_CFG0);
	writel_relaxed(0x4, host->ioaddr + CORE_CSR_CDC_CAL_TIMER_CFG1);
	writel_relaxed(0xCB732020, host->ioaddr + CORE_CSR_CDC_REFCOUNT_CFG);
	writel_relaxed(0xB19, host->ioaddr + CORE_CSR_CDC_COARSE_CAL_CFG);
	writel_relaxed(0x3AC, host->ioaddr + CORE_CSR_CDC_DELAY_CFG);
	writel_relaxed(0x0, host->ioaddr + CORE_CDC_OFFSET_CFG);
	writel_relaxed(0x16334, host->ioaddr + CORE_CDC_SLAVE_DDA_CFG);

	/* CDC HW Calibration */

	/* Write 1 to SW_TRIG_FULL_CALIB field in CORE_CSR_CDC_CTLR_CFG0 */
	config = readl_relaxed(host->ioaddr + CORE_CSR_CDC_CTLR_CFG0);
	config |= CORE_SW_TRIG_FULL_CALIB;
	writel_relaxed(config, host->ioaddr + CORE_CSR_CDC_CTLR_CFG0);

	/* Write 0 to SW_TRIG_FULL_CALIB field in CORE_CSR_CDC_CTLR_CFG0 */
	config = readl_relaxed(host->ioaddr + CORE_CSR_CDC_CTLR_CFG0);
	config &= ~CORE_SW_TRIG_FULL_CALIB;
	writel_relaxed(config, host->ioaddr + CORE_CSR_CDC_CTLR_CFG0);

	/* Write 1 to HW_AUTOCAL_ENA field in CORE_CSR_CDC_CTLR_CFG0 */
	config = readl_relaxed(host->ioaddr + CORE_CSR_CDC_CTLR_CFG0);
	config |= CORE_HW_AUTOCAL_ENA;
	writel_relaxed(config, host->ioaddr + CORE_CSR_CDC_CTLR_CFG0);

	/* Write 1 to TIMER_ENA field in CORE_CSR_CDC_CAL_TIMER_CFG0 */
	config = readl_relaxed(host->ioaddr + CORE_CSR_CDC_CAL_TIMER_CFG0);
	config |= CORE_TIMER_ENA;
	writel_relaxed(config, host->ioaddr + CORE_CSR_CDC_CAL_TIMER_CFG0);

	/* Poll on CALIBRATION_DONE field in CORE_CSR_CDC_STATUS0 to be 1 */
	ret = readl_relaxed_poll_timeout(host->ioaddr + CORE_CSR_CDC_STATUS0,
					 calib_done,
					 (calib_done & CORE_CALIBRATION_DONE),
					 1, 50);

	if (ret == -ETIMEDOUT) {
		pr_err("%s: %s: CDC calibration was not completed\n",
		       mmc_hostname(host->mmc), __func__);
		goto out;
	}

	/* Verify CDC_ERROR_CODE field in CORE_CSR_CDC_STATUS0 is 0 */
	ret = readl_relaxed(host->ioaddr + CORE_CSR_CDC_STATUS0)
			& CORE_CDC_ERROR_CODE_MASK;
	if (ret) {
		pr_err("%s: %s: CDC error code %d\n",
		       mmc_hostname(host->mmc), __func__, ret);
		ret = -EINVAL;
		goto out;
	}

	/* Write 1 to START_CDC_TRAFFIC field in CORE_DDR200_CFG */
	config = readl_relaxed(host->ioaddr + CORE_DDR_200_CFG);
	config |= CORE_START_CDC_TRAFFIC;
	writel_relaxed(config, host->ioaddr + CORE_DDR_200_CFG);
out:
	pr_debug("%s: %s: Exit, ret %d\n", mmc_hostname(host->mmc),
		 __func__, ret);
	return ret;
}

static int sdhci_msm_cm_dll_sdc4_calibration(struct sdhci_host *host)
{
	u32 dll_status, config;
	int ret;

	pr_debug("%s: %s: Enter\n", mmc_hostname(host->mmc), __func__);

	/*
	 * Currently the CORE_DDR_CONFIG register defaults to desired
	 * configuration on reset. Currently reprogramming the power on
	 * reset (POR) value in case it might have been modified by
	 * bootloaders. In the future, if this changes, then the desired
	 * values will need to be programmed appropriately.
	 */
	writel_relaxed(DDR_CONFIG_POR_VAL, host->ioaddr + CORE_DDR_CONFIG);

	/* Write 1 to DDR_CAL_EN field in CORE_DLL_CONFIG_2 */
	config = readl_relaxed(host->ioaddr + CORE_DLL_CONFIG_2);
	config |= CORE_DDR_CAL_EN;
	writel_relaxed(config, host->ioaddr + CORE_DLL_CONFIG_2);

	/* Poll on DDR_DLL_LOCK bit in CORE_DLL_STATUS to be set */
	ret = readl_relaxed_poll_timeout(host->ioaddr + CORE_DLL_STATUS,
					 dll_status,
					 (dll_status & CORE_DDR_DLL_LOCK),
					 10, 1000);

	if (ret == -ETIMEDOUT) {
		pr_err("%s: %s: CM_DLL_SDC4 calibration was not completed\n",
		       mmc_hostname(host->mmc), __func__);
		goto out;
	}

	/* set CORE_PWRSAVE_DLL bit in CORE_VENDOR_SPEC3 */
	config = readl_relaxed(host->ioaddr + CORE_VENDOR_SPEC3);
	config |= CORE_PWRSAVE_DLL;
	writel_relaxed(config, host->ioaddr + CORE_VENDOR_SPEC3);

	/*
	 * Drain writebuffer to ensure above DLL calibration
	 * and PWRSAVE DLL is enabled.
	 */
	wmb();
out:
	pr_debug("%s: %s: Exit, ret %d\n", mmc_hostname(host->mmc),
		 __func__, ret);
	return ret;
}

static int sdhci_msm_hs400_dll_calibration(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_msm_host *msm_host = sdhci_pltfm_priv(pltfm_host);
	int ret;
	u32 config;

	printk("%s: %s: Enter\n", mmc_hostname(host->mmc), __func__);

	/*
	 * Retuning in HS400 (DDR mode) will fail, just reset the
	 * tuning block and restore the saved tuning phase.
	 */
	ret = msm_init_cm_dll(host);
	if (ret)
		goto out;

	/* Set the selected phase in delay line hw block */
	ret = msm_config_cm_dll_phase(host, msm_host->saved_tuning_phase);
	if (ret)
		goto out;

	/* Write 1 to CMD_DAT_TRACK_SEL field in DLL_CONFIG */
	config = readl_relaxed(host->ioaddr + CORE_DLL_CONFIG);
	config |= CORE_CMD_DAT_TRACK_SEL;
	writel_relaxed(config, host->ioaddr + CORE_DLL_CONFIG);
	if (msm_host->use_cdclp533)
		/* Calibrate CDCLP533 DLL HW */
		ret = sdhci_msm_cdclp533_calibration(host);
	else
		/* Calibrate CM_DLL_SDC4 HW */
		ret = sdhci_msm_cm_dll_sdc4_calibration(host);
out:
	pr_debug("%s: %s: Exit, ret %d\n", mmc_hostname(host->mmc),
		 __func__, ret);
	return ret;
}



#define MAX_TEST_BUS 20
#define CORE_MCI_DATA_CNT 0x30
#define CORE_MCI_FIFO_CNT 0x44
#define CORE_MCI_STATUS 0x34
#define CORE_VENDOR_SPEC_ADMA_ERR_ADDR0        0x114
#define CORE_VENDOR_SPEC_ADMA_ERR_ADDR1        0x118
#define CORE_TESTBUS_SEL2_BIT  4
#define CORE_TESTBUS_SEL2      (1 << CORE_TESTBUS_SEL2_BIT)

#define CORE_TESTBUS_ENA       (1 << 3)

#define CORE_TESTBUS_CONFIG    0x0CC

#define CORE_SDCC_DEBUG_REG    0x124

void sdhci_msm_dump_vendor_regs(struct sdhci_host *host)
{

        int tbsel, tbsel2;
        int i, index = 0;
        u32 test_bus_val = 0;
        u32 debug_reg[MAX_TEST_BUS] = {0};
       struct sdhci_pltfm_host *pltfm_host;
        struct sdhci_msm_host *msm_host;

       pltfm_host = sdhci_priv(host);
       msm_host = sdhci_pltfm_priv(pltfm_host);

        pr_info("----------- VENDOR REGISTER DUMP -----------\n");
        pr_info("Data cnt: 0x%08x | Fifo cnt: 0x%08x | Int sts: 0x%08x\n",
                readl_relaxed(msm_host->core_mem + CORE_MCI_DATA_CNT),
                readl_relaxed(msm_host->core_mem + CORE_MCI_FIFO_CNT),
                readl_relaxed(msm_host->core_mem + CORE_MCI_STATUS));
        pr_info("DLL cfg:  0x%08x | DLL sts:  0x%08x | SDCC ver: 0x%08x\n",
                readl_relaxed(host->ioaddr + CORE_DLL_CONFIG),
                readl_relaxed(host->ioaddr + CORE_DLL_STATUS),
                readl_relaxed(msm_host->core_mem + CORE_MCI_VERSION));
        pr_info("Vndr func: 0x%08x | Vndr adma err : addr0: 0x%08x addr1: 0x%08x\n",
                readl_relaxed(host->ioaddr + CORE_VENDOR_SPEC),
                readl_relaxed(host->ioaddr + CORE_VENDOR_SPEC_ADMA_ERR_ADDR0),
                readl_relaxed(host->ioaddr + CORE_VENDOR_SPEC_ADMA_ERR_ADDR1));

        /*
         * tbsel indicates [2:0] bits and tbsel2 indicates [7:4] bits
         * of CORE_TESTBUS_CONFIG register.
         *
         * To select test bus 0 to 7 use tbsel and to select any test bus
         * above 7 use (tbsel2 | tbsel) to get the test bus number. For eg,
         * to select test bus 14, write 0x1E to CORE_TESTBUS_CONFIG register
         * i.e., tbsel2[7:4] = 0001, tbsel[2:0] = 110.
         */
        for (tbsel2 = 0; tbsel2 < 3; tbsel2++) {
                for (tbsel = 0; tbsel < 8; tbsel++) {
                        if (index >= MAX_TEST_BUS)
                                break;
                        test_bus_val = (tbsel2 << CORE_TESTBUS_SEL2_BIT) |
                                        tbsel | CORE_TESTBUS_ENA;
                        writel_relaxed(test_bus_val,
                                msm_host->core_mem + CORE_TESTBUS_CONFIG);
                        debug_reg[index++] = readl_relaxed(msm_host->core_mem +
                                                        CORE_SDCC_DEBUG_REG);
                }
        }
        for (i = 0; i < MAX_TEST_BUS; i = i + 4)
                pr_info(" Test bus[%d to %d]: 0x%08x 0x%08x 0x%08x 0x%08x\n",
                                i, i + 3, debug_reg[i], debug_reg[i+1],
                                debug_reg[i+2], debug_reg[i+3]);
        /* Disable test bus */
        writel_relaxed(~CORE_TESTBUS_ENA, msm_host->core_mem +
                        CORE_TESTBUS_CONFIG);
}




static int sdhci_msm_execute_tuning(struct sdhci_host *host, u32 opcode)
{
	int tuning_seq_cnt = 3;
	u8 phase, tuned_phases[16], tuned_phase_cnt = 0;
	int rc;
	struct mmc_host *mmc = host->mmc;
	struct mmc_ios ios = host->mmc->ios;
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_msm_host *msm_host = sdhci_pltfm_priv(pltfm_host);

printk(" %s HOST_clock: %d ios_timing: %d \n", __func__, host->clock, ios.timing);
	/*
	 * Tuning is required for SDR104, HS200 and HS400 cards and
	 * if clock frequency is greater than 100MHz in these modes.
	 */
	if (host->clock <= CORE_FREQ_100MHZ ||
	    !(ios.timing == MMC_TIMING_MMC_HS400 ||
	    ios.timing == MMC_TIMING_MMC_HS200 ||
	    ios.timing == MMC_TIMING_UHS_SDR104))
	{
		printk(" %s returning 0\n", __func__);
		return 0;
	}

retry:
	/* First of all reset the tuning block */
	rc = msm_init_cm_dll(host);
	if (rc)
	{
		printk(" %s init_cm_dll returned (%d) \n", __func__, rc);
		return rc;
	}

	phase = 0;
	do {
		/* Set the phase in delay line hw block */
		rc = msm_config_cm_dll_phase(host, phase);
		if (rc) {
			printk(" %s msm_config_cm_dll_phase returned (%d) \n", __func__, rc);
			return rc;
		}

		msm_host->saved_tuning_phase = phase;
		rc = mmc_send_tuning(mmc, opcode, NULL);
		if (!rc) {
			/* Tuning is successful at this tuning point */
			tuned_phases[tuned_phase_cnt++] = phase;
			dev_dbg(mmc_dev(mmc), "%s: Found good phase = %d\n",
				 mmc_hostname(mmc), phase);
		}
		else {
			printk(" %s Error in send tuning (%d) \n", __func__, rc);
		}
	} while (++phase < ARRAY_SIZE(tuned_phases));

	if (tuned_phase_cnt) {
		rc = msm_find_most_appropriate_phase(host, tuned_phases,
						     tuned_phase_cnt);
		if (rc < 0)
			return rc;
		else
			phase = rc;

		/*
		 * Finally set the selected phase in delay
		 * line hw block.
		 */
		rc = msm_config_cm_dll_phase(host, phase);
		if (rc)
			return rc;
		dev_dbg(mmc_dev(mmc), "%s: Setting the tuning phase to %d\n",
			 mmc_hostname(mmc), phase);
	} else {
		if (--tuning_seq_cnt)
			goto retry;
		/* Tuning failed */
		dev_dbg(mmc_dev(mmc), "%s: No tuning point found\n",
		       mmc_hostname(mmc));
		rc = -EIO;
	}

	if (!rc)
		msm_host->tuning_done = true;
	return rc;
}

static void sdhci_msm_set_uhs_signaling(struct sdhci_host *host,
					unsigned int uhs)
{
	struct mmc_host *mmc = host->mmc;
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_msm_host *msm_host = sdhci_pltfm_priv(pltfm_host);
	u16 ctrl_2;
	u32 config;

	ctrl_2 = sdhci_readw(host, SDHCI_HOST_CONTROL2);

	printk(" %s -> ctrl_2(1):: %x \n", __func__, ctrl_2);

	/* Select Bus Speed Mode for host */
	ctrl_2 &= ~SDHCI_CTRL_UHS_MASK;
	switch (uhs) {
	case MMC_TIMING_UHS_SDR12:
		ctrl_2 |= SDHCI_CTRL_UHS_SDR12;
		break;
	case MMC_TIMING_UHS_SDR25:
		ctrl_2 |= SDHCI_CTRL_UHS_SDR25;
		break;
	case MMC_TIMING_UHS_SDR50:
		ctrl_2 |= SDHCI_CTRL_UHS_SDR50;
		break;
	case MMC_TIMING_MMC_HS400:
	case MMC_TIMING_MMC_HS200:
	case MMC_TIMING_UHS_SDR104:
		ctrl_2 |= SDHCI_CTRL_UHS_SDR104;
		break;
	case MMC_TIMING_UHS_DDR50:
	case MMC_TIMING_MMC_DDR52:
		ctrl_2 |= SDHCI_CTRL_UHS_DDR50;
		break;
	}


	printk(" %s -> ctrl_2(2):: %x \n", __func__, ctrl_2);

	/*
	 * When clock frequency is less than 100MHz, the feedback clock must be
	 * provided and DLL must not be used so that tuning can be skipped. To
	 * provide feedback clock, the mode selection can be any value less
	 * than 3'b011 in bits [2:0] of HOST CONTROL2 register.
	 */
	if (host->clock <= CORE_FREQ_100MHZ) {
		if ((uhs == MMC_TIMING_MMC_HS400) ||
		    (uhs == MMC_TIMING_MMC_HS200) ||
		    (uhs == MMC_TIMING_UHS_SDR104))
			ctrl_2 &= ~SDHCI_CTRL_UHS_MASK;
		/*
		 * Make sure DLL is disabled when not required
		 *
		 * Write 1 to DLL_RST bit of DLL_CONFIG register
		 */
		config = readl_relaxed(host->ioaddr + CORE_DLL_CONFIG);
		config |= CORE_DLL_RST;
		writel_relaxed(config, host->ioaddr + CORE_DLL_CONFIG);

		/* Write 1 to DLL_PDN bit of DLL_CONFIG register */
		config = readl_relaxed(host->ioaddr + CORE_DLL_CONFIG);
		config |= CORE_DLL_PDN;
		writel_relaxed(config, host->ioaddr + CORE_DLL_CONFIG);

		/*
		 * The DLL needs to be restored and CDCLP533 recalibrated
		 * when the clock frequency is set back to 400MHz.
		 */
		msm_host->calibration_done = false;
	}
	dev_dbg(mmc_dev(mmc), "%s: clock=%u uhs=%u ctrl_2=0x%x\n",
		mmc_hostname(host->mmc), host->clock, uhs, ctrl_2);

	printk(" %s -> ctrl_2(3):: %x \n", __func__, ctrl_2);
	sdhci_writew(host, ctrl_2, SDHCI_HOST_CONTROL2);

	spin_unlock_irq(&host->lock);
	/* CDCLP533 HW calibration is only required for HS400 mode*/
	if (host->clock > CORE_FREQ_100MHZ &&
	    msm_host->tuning_done && !msm_host->calibration_done &&
	    (mmc->ios.timing == MMC_TIMING_MMC_HS400))
		if (!sdhci_msm_hs400_dll_calibration(host))
			msm_host->calibration_done = true;
	spin_lock_irq(&host->lock);
}

static void sdhci_msm_voltage_switch(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_msm_host *msm_host = sdhci_pltfm_priv(pltfm_host);
	u32 irq_status, irq_ack = 0;

	irq_status = readl_relaxed(msm_host->core_mem + CORE_PWRCTL_STATUS);
	irq_status &= INT_MASK;

	writel_relaxed(irq_status, msm_host->core_mem + CORE_PWRCTL_CLEAR);

	if (irq_status & (CORE_PWRCTL_BUS_ON | CORE_PWRCTL_BUS_OFF))
		irq_ack |= CORE_PWRCTL_BUS_SUCCESS;
	if (irq_status & (CORE_PWRCTL_IO_LOW | CORE_PWRCTL_IO_HIGH))
		irq_ack |= CORE_PWRCTL_IO_SUCCESS;

	/*
	 * The driver has to acknowledge the interrupt, switch voltages and
	 * report back if it succeded or not to this register. The voltage
	 * switches are handled by the sdhci core, so just report success.
	 */
	writel_relaxed(irq_ack, msm_host->core_mem + CORE_PWRCTL_CTL);
}

#if 0
static irqreturn_t sdhci_msm_pwr_irq(int irq, void *data)
{
	struct sdhci_host *host = (struct sdhci_host *)data;

	sdhci_msm_voltage_switch(host);

	return IRQ_HANDLED;
}
#endif

static unsigned int sdhci_msm_get_max_clock(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_msm_host *msm_host = sdhci_pltfm_priv(pltfm_host);

	return clk_round_rate(msm_host->clk, ULONG_MAX);
}

static unsigned int sdhci_msm_get_min_clock(struct sdhci_host *host)
{
	return SDHCI_MSM_MIN_CLOCK;
}

/**
 * __sdhci_msm_set_clock - sdhci_msm clock control.
 *
 * Description:
 * Implement MSM version of sdhci_set_clock.
 * This is required since MSM controller does not
 * use internal divider and instead directly control
 * the GCC clock as per HW recommendation.
 **/
void __sdhci_msm_set_clock(struct sdhci_host *host, unsigned int clock)
{
	u16 clk;
	unsigned long timeout;

	/*
	 * Keep actual_clock as zero -
	 * - since there is no divider used so no need of having actual_clock.
	 * - MSM controller uses SDCLK for data timeout calculation. If
	 *   actual_clock is zero, host->clock is taken for calculation.
	 */
	host->mmc->actual_clock = 0;

	sdhci_writew(host, 0, SDHCI_CLOCK_CONTROL);

	if (clock == 0)
		return;

	/*
	 * MSM controller do not use clock divider.
	 * Thus read SDHCI_CLOCK_CONTROL and only enable
	 * clock with no divider value programmed.
	 */
	clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);

	clk |= SDHCI_CLOCK_INT_EN;
	sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

	/* Wait max 20 ms */
	timeout = 20;
	while (!((clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL))
		& SDHCI_CLOCK_INT_STABLE)) {
		if (timeout == 0) {
			pr_err("%s: Internal clock never stabilised\n",
			       mmc_hostname(host->mmc));
			return;
		}
		timeout--;
		mdelay(1);
	}

	clk |= SDHCI_CLOCK_CARD_EN;
	sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);
}

/* sdhci_msm_set_clock - Called with (host->lock) spinlock held. */
static void sdhci_msm_set_clock(struct sdhci_host *host, unsigned int clock)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_msm_host *msm_host = sdhci_pltfm_priv(pltfm_host);
	struct mmc_ios curr_ios = host->mmc->ios;
	u32 config, dll_lock;
	int rc;

printk(" %s clock: %u \n", __func__, clock);

	if (!clock) {
		msm_host->clk_rate = clock;
		goto out;
	}

	spin_unlock_irq(&host->lock);
	/*
	 * The SDHC requires internal clock frequency to be double the
	 * actual clock that will be set for DDR mode. The controller
	 * uses the faster clock(100/400MHz) for some of its parts and
	 * send the actual required clock (50/200MHz) to the card.
	 */
	if ((curr_ios.timing == MMC_TIMING_UHS_DDR50) ||
	    (curr_ios.timing == MMC_TIMING_MMC_DDR52) ||
	    (curr_ios.timing == MMC_TIMING_MMC_HS400))
		clock *= 2;
	/*
	 * In general all timing modes are controlled via UHS mode select in
	 * Host Control2 register. eMMC specific HS200/HS400 doesn't have
	 * their respective modes defined here, hence we use these values.
	 *
	 * HS200 - SDR104 (Since they both are equivalent in functionality)
	 * HS400 - This involves multiple configurations
	 *		Initially SDR104 - when tuning is required as HS200
	 *		Then when switching to DDR @ 400MHz (HS400) we use
	 *		the vendor specific HC_SELECT_IN to control the mode.
	 *
	 * In addition to controlling the modes we also need to select the
	 * correct input clock for DLL depending on the mode.
	 *
	 * HS400 - divided clock (free running MCLK/2)
	 * All other modes - default (free running MCLK)
	 */
	if (curr_ios.timing == MMC_TIMING_MMC_HS400) {
		/* Select the divided clock (free running MCLK/2) */
		config = readl_relaxed(host->ioaddr + CORE_VENDOR_SPEC);
		config &= ~CORE_HC_MCLK_SEL_MASK;
		config |= CORE_HC_MCLK_SEL_HS400;

		writel_relaxed(config, host->ioaddr + CORE_VENDOR_SPEC);
		/*
		 * Select HS400 mode using the HC_SELECT_IN from VENDOR SPEC
		 * register
		 */
		if (msm_host->tuning_done && !msm_host->calibration_done) {
			/*
			 * Write 0x6 to HC_SELECT_IN and 1 to HC_SELECT_IN_EN
			 * field in VENDOR_SPEC_FUNC
			 */
			config = readl_relaxed(host->ioaddr + CORE_VENDOR_SPEC);
			config |= CORE_HC_SELECT_IN_HS400;
			config |= CORE_HC_SELECT_IN_EN;
			writel_relaxed(config, host->ioaddr + CORE_VENDOR_SPEC);
		}
		if (!msm_host->clk_rate && !msm_host->use_cdclp533) {
			/*
			 * Poll on DLL_LOCK or DDR_DLL_LOCK bits in
			 * CORE_DLL_STATUS to be set.  This should get set
			 * within 15 us at 200 MHz.
			 */
			rc = readl_relaxed_poll_timeout(host->ioaddr +
							CORE_DLL_STATUS,
							dll_lock,
							(dll_lock &
							(CORE_DLL_LOCK |
							CORE_DDR_DLL_LOCK)), 10,
							1000);
			if (rc == -ETIMEDOUT)
				pr_err("%s: Unable to get DLL_LOCK/DDR_DLL_LOCK, dll_status: 0x%08x\n",
				       mmc_hostname(host->mmc), dll_lock);
		}
	} else {
		if (!msm_host->use_cdclp533) {
			/* set CORE_PWRSAVE_DLL bit in CORE_VENDOR_SPEC3 */
			config = readl_relaxed(host->ioaddr +
					CORE_VENDOR_SPEC3);
			config &= ~CORE_PWRSAVE_DLL;
			writel_relaxed(config, host->ioaddr +
					CORE_VENDOR_SPEC3);
		}

		/* Select the default clock (free running MCLK) */
		config = readl_relaxed(host->ioaddr + CORE_VENDOR_SPEC);
		config &= ~CORE_HC_MCLK_SEL_MASK;
		config |= CORE_HC_MCLK_SEL_DFLT;
		writel_relaxed(config, host->ioaddr + CORE_VENDOR_SPEC);

		/*
		 * Disable HC_SELECT_IN to be able to use the UHS mode select
		 * configuration from Host Control2 register for all other
		 * modes.
		 *
		 * Write 0 to HC_SELECT_IN and HC_SELECT_IN_EN field
		 * in VENDOR_SPEC_FUNC
		 */
		config = readl_relaxed(host->ioaddr + CORE_VENDOR_SPEC);
		config &= ~CORE_HC_SELECT_IN_EN;
		config &= ~CORE_HC_SELECT_IN_MASK;
		writel_relaxed(config, host->ioaddr + CORE_VENDOR_SPEC);
	}

	/*
	 * Make sure above writes impacting free running MCLK are completed
	 * before changing the clk_rate at GCC.
	 */
	wmb();

	if (clock != msm_host->clk_rate) {
		rc = clk_set_rate(msm_host->clk, clock);
		if (rc) {
			pr_err("%s: Failed to set clock at rate %u at timing %d\n",
			       mmc_hostname(host->mmc), clock,
			       curr_ios.timing);
			spin_lock_irq(&host->lock);
			goto out;
		}
		msm_host->clk_rate = clock;
		pr_debug("%s: Setting clock at rate %lu\n",
			 mmc_hostname(host->mmc), clk_get_rate(msm_host->clk));
	}
	spin_lock_irq(&host->lock);
out:
	__sdhci_msm_set_clock(host, clock);
}

static const struct of_device_id sdhci_msm_dt_match[] = {
	{ .compatible = "qcom,sdhci-msm-v4" },
	{},
};

MODULE_DEVICE_TABLE(of, sdhci_msm_dt_match);

static const struct sdhci_ops sdhci_msm_ops = {
	.platform_execute_tuning = sdhci_msm_execute_tuning,
	.reset = sdhci_reset,
	.set_clock = sdhci_msm_set_clock,
	.get_min_clock = sdhci_msm_get_min_clock,
	.get_max_clock = sdhci_msm_get_max_clock,
	.set_bus_width = sdhci_set_bus_width,
	.set_uhs_signaling = sdhci_msm_set_uhs_signaling,
	.voltage_switch = sdhci_msm_voltage_switch,
	.check_power_status = sdhci_msm_check_power_status,
	.dump_vendor_regs = sdhci_msm_dump_vendor_regs,
};

static const struct sdhci_pltfm_data sdhci_msm_pdata = {
	.quirks = SDHCI_QUIRK_BROKEN_CARD_DETECTION |
		  SDHCI_QUIRK_NO_CARD_NO_RESET |
		  SDHCI_QUIRK_SINGLE_POWER_WRITE |
		  SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN,
	.quirks2 = SDHCI_QUIRK2_PRESET_VALUE_BROKEN,
	.ops = &sdhci_msm_ops,
};

static int sdhci_msm_probe(struct platform_device *pdev)
{
	struct sdhci_host *host;
	struct sdhci_pltfm_host *pltfm_host;
	struct sdhci_msm_host *msm_host;
	struct resource *core_memres;
	u32 irq_status, irq_ctl;
	int ret;
	u16 host_version, core_minor;
	u32 core_version, config;
	u8 core_major;

	host = sdhci_pltfm_init(pdev, &sdhci_msm_pdata, sizeof(*msm_host));
	if (IS_ERR(host))
		return PTR_ERR(host);

	pltfm_host = sdhci_priv(host);
	msm_host = sdhci_pltfm_priv(pltfm_host);
	msm_host->mmc = host->mmc;
	msm_host->pdev = pdev;

	ret = mmc_of_parse(host->mmc);
	if (ret)
		goto pltfm_free;

	sdhci_get_of_property(pdev);

	/* Extract platform data */
	if (pdev->dev.of_node) {
		msm_host->pdata = sdhci_msm_populate_pdata(&pdev->dev);
		if (!msm_host->pdata) {
			dev_err(&pdev->dev, "DT parsing error\n");
			goto pltfm_free;
		}
	}

	msm_host->saved_tuning_phase = INVALID_TUNING_PHASE;

	/* Setup SDCC bus voter clock. */
	msm_host->bus_clk = devm_clk_get(&pdev->dev, "bus");
	if (!IS_ERR(msm_host->bus_clk)) {
		/* Vote for max. clk rate for max. performance */
		ret = clk_set_rate(msm_host->bus_clk, INT_MAX);
		if (ret)
			goto pltfm_free;
		ret = clk_prepare_enable(msm_host->bus_clk);
		if (ret)
			goto pltfm_free;
	}

	/* Setup main peripheral bus clock */
	msm_host->pclk = devm_clk_get(&pdev->dev, "iface");
	if (IS_ERR(msm_host->pclk)) {
		ret = PTR_ERR(msm_host->pclk);
		dev_err(&pdev->dev, "Peripheral clk setup failed (%d)\n", ret);
		goto bus_clk_disable;
	}

	ret = clk_prepare_enable(msm_host->pclk);
	if (ret)
		goto bus_clk_disable;

	/* Setup SDC MMC clock */
	msm_host->clk = devm_clk_get(&pdev->dev, "core");
	if (IS_ERR(msm_host->clk)) {
		ret = PTR_ERR(msm_host->clk);
		dev_err(&pdev->dev, "SDC MMC clk setup failed (%d)\n", ret);
		goto pclk_disable;
	}

	/* Vote for maximum clock rate for maximum performance */
	ret = clk_set_rate(msm_host->clk, INT_MAX);
	if (ret)
		dev_warn(&pdev->dev, "core clock boost failed\n");

	ret = clk_prepare_enable(msm_host->clk);
	if (ret)
		goto pclk_disable;

	/* Setup regulators */
	ret = sdhci_msm_vreg_init(&pdev->dev, msm_host->pdata, true);
	if (ret) {
		dev_err(&pdev->dev, "Regulator setup failed (%d)\n", ret);
		goto clk_disable;
	}

	core_memres = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	msm_host->core_mem = devm_ioremap_resource(&pdev->dev, core_memres);

	if (IS_ERR(msm_host->core_mem)) {
		dev_err(&pdev->dev, "Failed to remap registers\n");
		ret = PTR_ERR(msm_host->core_mem);
		goto vreg_deinit;
	}

	/* Reset the core and Enable SDHC mode */
	writel_relaxed(readl_relaxed(msm_host->core_mem + CORE_POWER) |
			CORE_SW_RST, msm_host->core_mem + CORE_POWER);

	/* SW reset can take upto 10HCLK + 15MCLK cycles. (min 40us) */
	usleep_range(1000, 5000);
	if (readl(msm_host->core_mem + CORE_POWER) & CORE_SW_RST) {
		dev_err(&pdev->dev, "Stuck in reset\n");
		ret = -ETIMEDOUT;
		goto clk_disable;
	}

	/* Reset the vendor spec register to power on reset state. */
	writel_relaxed(CORE_VENDOR_SPEC_POR_VAL,
			host->ioaddr + CORE_VENDOR_SPEC);

	/* Set HC_MODE_EN bit in HC_MODE register */
	writel_relaxed(HC_MODE_EN, (msm_host->core_mem + CORE_HC_MODE));

	/* Set FF_CLK_SW_RST_DIS bit in HC_MODE register */
	config = readl_relaxed(msm_host->core_mem + CORE_HC_MODE);
	config |= FF_CLK_SW_RST_DIS;
	writel_relaxed(config, msm_host->core_mem + CORE_HC_MODE);

	host_version = readw_relaxed((host->ioaddr + SDHCI_HOST_VERSION));
	dev_dbg(&pdev->dev, "Host Version: 0x%x Vendor Version 0x%x\n",
		host_version, ((host_version & SDHCI_VENDOR_VER_MASK) >>
			       SDHCI_VENDOR_VER_SHIFT));

	core_version = readl_relaxed(msm_host->core_mem + CORE_MCI_VERSION);
	core_major = (core_version & CORE_VERSION_MAJOR_MASK) >>
		      CORE_VERSION_MAJOR_SHIFT;
	core_minor = core_version & CORE_VERSION_MINOR_MASK;
	dev_dbg(&pdev->dev, "MCI Version: 0x%08x, major: 0x%04x, minor: 0x%02x\n",
		core_version, core_major, core_minor);

	if ((core_major == 1) && (core_minor >= 0x42))
		msm_host->use_updated_dll_reset = true;

	if ((core_major == 1) && (core_minor >= 0x42))
		msm_host->use_14lpp_dll_reset = true;

	/*
	 * SDCC 5 controller with major version 1, minor version 0x34 and later
	 * with HS 400 mode support will use CM DLL instead of CDC LP 533 DLL.
	 */
	if ((core_major == 1) && (core_minor < 0x34))
		msm_host->use_cdclp533 = true;

	/*
	 * Support for some capabilities is not advertised by newer
	 * controller versions and must be explicitly enabled.
	 */
	if (core_major >= 1 && core_minor != 0x11 && core_minor != 0x12) {
		config = readl_relaxed(host->ioaddr + SDHCI_CAPABILITIES);
		config |= SDHCI_CAN_VDD_300 | SDHCI_CAN_DO_8BIT;
		writel_relaxed(config, host->ioaddr +
			       CORE_VENDOR_SPEC_CAPABILITIES0);
	}

	/*
	 * POR reset of VENDOR_SPEC/CORE_SW_RST above may trigger power irq
	 * if previous status of PWRCTL was either BUS_ON or IO_HIGH_V.
	 * So before we enable the power irq interrupt in GIC
	 * (by registering the interrupt handler), we need to
	 * ensure that any pending power irq interrupt status is acknowledged
	 * otherwise power irq interrupt handler would be fired prematurely.
	 */
	irq_status = readl_relaxed(msm_host->core_mem + CORE_PWRCTL_STATUS);
	writel_relaxed(irq_status, (msm_host->core_mem + CORE_PWRCTL_CLEAR));
	irq_ctl = readl_relaxed(msm_host->core_mem + CORE_PWRCTL_CTL);
	if (irq_status & (CORE_PWRCTL_BUS_ON | CORE_PWRCTL_BUS_OFF))
		irq_ctl |= CORE_PWRCTL_BUS_SUCCESS;
	if (irq_status & (CORE_PWRCTL_IO_HIGH | CORE_PWRCTL_IO_LOW))
		irq_ctl |= CORE_PWRCTL_IO_SUCCESS;
	writel_relaxed(irq_ctl, (msm_host->core_mem + CORE_PWRCTL_CTL));

	/*
	 * Ensure that above writes are propogated before interrupt enablement
	 * in GIC.
	 */
	mb();

	/* Setup PWRCTL irq */
	msm_host->pwr_irq = platform_get_irq_byname(pdev, "pwr_irq");
	if (msm_host->pwr_irq < 0) {
		dev_err(&pdev->dev, "Failed to get pwr_irq by name (%d)\n",
				msm_host->pwr_irq);
		goto vreg_deinit;
	}
	ret = devm_request_threaded_irq(&pdev->dev, msm_host->pwr_irq, NULL,
					sdhci_msm_pwr_irq, IRQF_ONESHOT,
					dev_name(&pdev->dev), host);
	if (ret) {
		dev_err(&pdev->dev, "Request threaded irq(%d) failed (%d)\n",
				msm_host->pwr_irq, ret);
		goto vreg_deinit;
	}

	init_completion(&msm_host->pwr_irq_completion);
	spin_lock_init(&msm_host->pwr_irq_lock);

	/* Enable pwr irq interrupts */
	writel_relaxed(INT_MASK, (msm_host->core_mem + CORE_PWRCTL_MASK));

	ret = sdhci_add_host(host);
	if (ret)
		goto vreg_deinit;

	return 0;

vreg_deinit:
	sdhci_msm_vreg_init(&pdev->dev, msm_host->pdata, false);
clk_disable:
	clk_disable_unprepare(msm_host->clk);
pclk_disable:
	clk_disable_unprepare(msm_host->pclk);
bus_clk_disable:
	if (!IS_ERR(msm_host->bus_clk))
		clk_disable_unprepare(msm_host->bus_clk);
pltfm_free:
	sdhci_pltfm_free(pdev);
	return ret;
}

static int sdhci_msm_remove(struct platform_device *pdev)
{
	struct sdhci_host *host = platform_get_drvdata(pdev);
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_msm_host *msm_host = sdhci_pltfm_priv(pltfm_host);
	int dead = (readl_relaxed(host->ioaddr + SDHCI_INT_STATUS) ==
		    0xffffffff);

	sdhci_remove_host(host, dead);
	sdhci_msm_vreg_init(&pdev->dev, msm_host->pdata, false);
	clk_disable_unprepare(msm_host->clk);
	clk_disable_unprepare(msm_host->pclk);
	if (!IS_ERR(msm_host->bus_clk))
		clk_disable_unprepare(msm_host->bus_clk);
	sdhci_pltfm_free(pdev);
	return 0;
}

static struct platform_driver sdhci_msm_driver = {
	.probe = sdhci_msm_probe,
	.remove = sdhci_msm_remove,
	.driver = {
		   .name = "sdhci_msm",
		   .of_match_table = sdhci_msm_dt_match,
	},
};

module_platform_driver(sdhci_msm_driver);

MODULE_DESCRIPTION("Qualcomm Secure Digital Host Controller Interface driver");
MODULE_LICENSE("GPL v2");
