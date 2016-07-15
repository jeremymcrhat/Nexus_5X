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

#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/delay.h>
#include <linux/mmc/mmc.h>
#include <linux/slab.h>

#include "sdhci-pltfm.h"

#define CORE_MCI_VERSION		0x50
#define CORE_VERSION_MAJOR_SHIFT	28
#define CORE_VERSION_MAJOR_MASK		(0xf << CORE_VERSION_MAJOR_SHIFT)
#define CORE_VERSION_MINOR_MASK		0xff

#define CORE_HC_MODE		0x78
#define HC_MODE_EN		0x1
#define CORE_POWER		0x0
#define CORE_SW_RST		BIT(7)

#define MAX_PHASES		16
#define CORE_DLL_LOCK		BIT(7)
#define CORE_DLL_EN		BIT(16)
#define CORE_CDR_EN		BIT(17)
#define CORE_CK_OUT_EN		BIT(18)
#define CORE_CDR_EXT_EN		BIT(19)
#define CORE_DLL_PDN		BIT(29)
#define CORE_DLL_RST		BIT(30)
#define CORE_DLL_CONFIG		0x100
#define CORE_DLL_STATUS		0x108

#define CORE_VENDOR_SPEC	0x10c
#define CORE_CLK_PWRSAVE	BIT(1)
#define CORE_VENDOR_SPEC_POR_VAL	0xa1c

#define CORE_VENDOR_SPEC_CAPABILITIES0	0x11c

#define CDR_SELEXT_SHIFT	20
#define CDR_SELEXT_MASK		(0xf << CDR_SELEXT_SHIFT)
#define CMUX_SHIFT_PHASE_SHIFT	24
#define CMUX_SHIFT_PHASE_MASK	(7 << CMUX_SHIFT_PHASE_SHIFT)


#define CORE_DLL_RST  BIT(30)

#define CORE_DLL_PDN            (1 << 29)

struct sdhci_msm_host {
	struct platform_device *pdev;
	void __iomem *core_mem;	/* MSM SDCC mapped address */
	struct clk *clk;	/* main SD/MMC bus clock */
	struct clk *pclk;	/* SDHC peripheral bus clock */
	struct clk *bus_clk;	/* SDHC bus voter clock */
	struct mmc_host *mmc;
	u32 curr_pwr_state;
	u32 curr_io_level;
	struct completion pwr_irq_completion;
};

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
	writel_relaxed((readl_relaxed(host->ioaddr + CORE_DLL_CONFIG)
			| CORE_CK_OUT_EN), host->ioaddr + CORE_DLL_CONFIG);

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
 * timing mode) or for eMMC4.5 card read operation (in HS200
 * timing mode).
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
	int wait_cnt = 50;
	unsigned long flags;

	spin_lock_irqsave(&host->lock, flags);

	/*
	 * Make sure that clock is always enabled when DLL
	 * tuning is in progress. Keeping PWRSAVE ON may
	 * turn off the clock.
	 */
	writel_relaxed((readl_relaxed(host->ioaddr + CORE_VENDOR_SPEC)
			& ~CORE_CLK_PWRSAVE), host->ioaddr + CORE_VENDOR_SPEC);

	/* Write 1 to DLL_RST bit of DLL_CONFIG register */
	writel_relaxed((readl_relaxed(host->ioaddr + CORE_DLL_CONFIG)
			| CORE_DLL_RST), host->ioaddr + CORE_DLL_CONFIG);

	/* Write 1 to DLL_PDN bit of DLL_CONFIG register */
	writel_relaxed((readl_relaxed(host->ioaddr + CORE_DLL_CONFIG)
			| CORE_DLL_PDN), host->ioaddr + CORE_DLL_CONFIG);
	msm_cm_dll_set_freq(host);

	/* Write 0 to DLL_RST bit of DLL_CONFIG register */
	writel_relaxed((readl_relaxed(host->ioaddr + CORE_DLL_CONFIG)
			& ~CORE_DLL_RST), host->ioaddr + CORE_DLL_CONFIG);

	/* Write 0 to DLL_PDN bit of DLL_CONFIG register */
	writel_relaxed((readl_relaxed(host->ioaddr + CORE_DLL_CONFIG)
			& ~CORE_DLL_PDN), host->ioaddr + CORE_DLL_CONFIG);

	/* Set DLL_EN bit to 1. */
	writel_relaxed((readl_relaxed(host->ioaddr + CORE_DLL_CONFIG)
			| CORE_DLL_EN), host->ioaddr + CORE_DLL_CONFIG);

	/* Set CK_OUT_EN bit to 1. */
	writel_relaxed((readl_relaxed(host->ioaddr + CORE_DLL_CONFIG)
			| CORE_CK_OUT_EN), host->ioaddr + CORE_DLL_CONFIG);

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

#define CORE_GENERICS           0x70
#define REQ_IO_LOW (1 << 2)
#define REQ_IO_HIGH        (1 << 3)
#define SWITCHABLE_SIGNALLING_VOL (1 << 29)

#define MSM_PWR_IRQ_TIMEOUT_MS 5000

 void sdhci_msm_check_power_status(struct sdhci_host *host, u32 req_type)
{
        unsigned long flags;
        bool done = false;
        u32 io_sig_sts;
        struct sdhci_pltfm_host *pltfm_host;
        struct sdhci_msm_host *msm_host;

        pltfm_host = sdhci_priv(host);
        msm_host = sdhci_pltfm_priv(pltfm_host);

        spin_lock_irqsave(&host->lock, flags);
        pr_debug("%s: %s: request %d curr_pwr_state %x curr_io_level %x\n",
                        mmc_hostname(host->mmc), __func__, req_type,
                        msm_host->curr_pwr_state, msm_host->curr_io_level);
        io_sig_sts = readl_relaxed(msm_host->core_mem + CORE_GENERICS);
        /*
         * The IRQ for request type IO High/Low will be generated when -
         * 1. SWITCHABLE_SIGNALLING_VOL is enabled in HW.
         * 2. If 1 is true and when there is a state change in 1.8V enable
         * bit (bit 3) of SDHCI_HOST_CONTROL2 register. The reset state of
         * that bit is 0 which indicates 3.3V IO voltage. So, when MMC core
         * layer tries to set it to 3.3V before card detection happens, the
         * IRQ doesn't get triggered as there is no state change in this bit.
         * The driver already handles this case by changing the IO voltage
         * level to high as part of controller power up sequence. Hence, check
         * for host->pwr to handle a case where IO voltage high request is
         * issued even before controller power up.
         */

        if (req_type & (REQ_IO_HIGH | REQ_IO_LOW)) {
                if (!(io_sig_sts & SWITCHABLE_SIGNALLING_VOL) ||
                                ((req_type & REQ_IO_HIGH) && !host->pwr)) {
                        pr_debug("%s: do not wait for power IRQ that never comes\n",
                                        mmc_hostname(host->mmc));
                        spin_unlock_irqrestore(&host->lock, flags);
                        return;
                }
        }

        if ((req_type & msm_host->curr_pwr_state) ||
                        (req_type & msm_host->curr_io_level))
                done = true;
        spin_unlock_irqrestore(&host->lock, flags);

        /*
         * This is needed here to hanlde a case where IRQ gets
         * triggered even before this function is called so that
         * x->done counter of completion gets reset. Otherwise,
         * next call to wait_for_completion returns immediately
         * without actually waiting for the IRQ to be handled.
         */
        if (done)
                init_completion(&msm_host->pwr_irq_completion);
        else if (!wait_for_completion_timeout(&msm_host->pwr_irq_completion,
                                msecs_to_jiffies(MSM_PWR_IRQ_TIMEOUT_MS)))
                __WARN_printf("%s: request(%d) timed out waiting for pwr_irq\n",
                                        mmc_hostname(host->mmc), req_type);

        pr_debug("%s: %s: request %d done\n", mmc_hostname(host->mmc),
                        __func__, req_type);
}


static int sdhci_msm_execute_tuning(struct sdhci_host *host, u32 opcode)
{
	int tuning_seq_cnt = 3;
	u8 phase, tuned_phases[16], tuned_phase_cnt = 0;
	int rc;
	struct mmc_host *mmc = host->mmc;
	struct mmc_ios ios = host->mmc->ios;

	/*
	 * Tuning is required for SDR104, HS200 and HS400 cards and
	 * if clock frequency is greater than 100MHz in these modes.
	 */
	if (host->clock <= 100 * 1000 * 1000 ||
	    !((ios.timing == MMC_TIMING_MMC_HS200) ||
	      (ios.timing == MMC_TIMING_UHS_SDR104)))
		return 0;

retry:
	/* First of all reset the tuning block */
	rc = msm_init_cm_dll(host);
	if (rc)
		return rc;

	phase = 0;
	do {
		/* Set the phase in delay line hw block */
		rc = msm_config_cm_dll_phase(host, phase);
		if (rc)
			return rc;

		rc = mmc_send_tuning(mmc, opcode, NULL);
		if (!rc) {
			/* Tuning is successful at this tuning point */
			tuned_phases[tuned_phase_cnt++] = phase;
			dev_dbg(mmc_dev(mmc), "%s: Found good phase = %d\n",
				 mmc_hostname(mmc), phase);
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

	return rc;
}

static const struct of_device_id sdhci_msm_dt_match[] = {
	{ .compatible = "qcom,sdhci-msm-v4" },
	{},
};

MODULE_DEVICE_TABLE(of, sdhci_msm_dt_match);

static const struct sdhci_ops sdhci_msm_ops = {
	.platform_execute_tuning = sdhci_msm_execute_tuning,
	.reset = sdhci_reset,
	.set_clock = sdhci_set_clock,
	.set_bus_width = sdhci_set_bus_width,
	.set_uhs_signaling = sdhci_set_uhs_signaling,
	.check_power_status = sdhci_msm_check_power_status,
	.dump_vendor_regs = sdhci_msm_dump_vendor_regs,
};

static const struct sdhci_pltfm_data sdhci_msm_pdata = {
	.quirks = SDHCI_QUIRK_BROKEN_CARD_DETECTION |
		  SDHCI_QUIRK_SINGLE_POWER_WRITE,
	.ops = &sdhci_msm_ops,
};

static int sdhci_msm_probe(struct platform_device *pdev)
{
	struct sdhci_host *host;
	struct sdhci_pltfm_host *pltfm_host;
	struct sdhci_msm_host *msm_host;
	struct resource *core_memres;
	int ret;
	u16 host_version, core_minor;
	u32 core_version, caps;
	u8 core_major;


printk(" --> %s <-- \n", __func__);

	host = sdhci_pltfm_init(pdev, &sdhci_msm_pdata, sizeof(*msm_host));
	if (IS_ERR(host))
	{
		printk(" Error sdhci_pltfm_init \n");
		return PTR_ERR(host);
	}

	pltfm_host = sdhci_priv(host);
	msm_host = sdhci_pltfm_priv(pltfm_host);
	msm_host->mmc = host->mmc;
	msm_host->pdev = pdev;

	ret = mmc_of_parse(host->mmc);
	if (ret) {
		printk(" Error parsing mmc-of-parse \n");
		goto pltfm_free;
	}

	sdhci_get_of_property(pdev);
printk(" --->>> getting bus clock \n");
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
//printk("   +++ CLK_CORE -> rate %lu \n", msm_host->bus_clk->core->rate);

printk(" ----->>>>>> getting iface clk \n");
	/* Setup main peripheral bus clock */
	msm_host->pclk = devm_clk_get(&pdev->dev, "iface");
	if (IS_ERR(msm_host->pclk)) {
		ret = PTR_ERR(msm_host->pclk);
		dev_err(&pdev->dev, "Perpheral clk setup failed (%d)\n", ret);
		goto bus_clk_disable;
	}

	ret = clk_prepare_enable(msm_host->pclk);
	if (ret) {
		printk(" Error prep en \n");
		goto bus_clk_disable;
	}
printk(" ------->>>>. getting core clk \n");
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
	{
		dev_warn(&pdev->dev, "clk prep enable failed\n");
		goto pclk_disable;
	}

	core_memres = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	msm_host->core_mem = devm_ioremap_resource(&pdev->dev, core_memres);

	if (IS_ERR(msm_host->core_mem)) {
		dev_err(&pdev->dev, "Failed to remap registers\n");
		ret = PTR_ERR(msm_host->core_mem);
		goto clk_disable;
	}

	/* Reset the vendor spec register to power on reset state. */
	writel_relaxed(CORE_VENDOR_SPEC_POR_VAL,
			host->ioaddr + CORE_VENDOR_SPEC);

	/* Set HC_MODE_EN bit in HC_MODE register */
	writel_relaxed(HC_MODE_EN, (msm_host->core_mem + CORE_HC_MODE));

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

	/*
	 * Support for some capabilities is not advertised by newer
	 * controller versions and must be explicitly enabled.
	 */
	if (core_major >= 1 && core_minor != 0x11 && core_minor != 0x12) {
		caps = readl_relaxed(host->ioaddr + SDHCI_CAPABILITIES);
		caps |= SDHCI_CAN_VDD_300 | SDHCI_CAN_DO_8BIT;
		writel_relaxed(caps, host->ioaddr +
			       CORE_VENDOR_SPEC_CAPABILITIES0);
	}

	ret = sdhci_add_host(host);
	if (ret)
		goto clk_disable;

	return 0;

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
		   .name = "sdhci_msm_JRM",
		   .of_match_table = sdhci_msm_dt_match,
	},
};

module_platform_driver(sdhci_msm_driver);

MODULE_DESCRIPTION("Qualcomm Secure Digital Host Controller Interface driver");
MODULE_LICENSE("GPL v2");
