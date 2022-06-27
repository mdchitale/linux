// SPDX-License-Identifier: GPL-2.0
/*
 * Clock driver based on SBI Ventana extension.
 *
 * Copyright (C) 2022 Ventana Micro Systems Ltd.
 */

#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <asm/sbi.h>

/* Vendor extension : Ventana Micro systems (JEDEC id 0x1f, Bank 13)*/
#define SBI_EXT_VENTANA		(SBI_EXT_VENDOR_START + 0x61F)
#define SBI_EXT_VENTANA_GROUP_PROBE	0x0

/* Ventana SBI extension - Clock group defines */
#define SBI_EXT_CLK			0x1
#define SBI_EXT_CLK_FID(fn)		(SBI_EXT_CLK << 8 | fn)

/* Ventana SBI extension - Clock group function number (LSB)*/
#define SBI_CLK_GET_SYS_CLK_ATTR	SBI_EXT_CLK_FID(0x1)
#define SBI_CLK_GET_ATTR		SBI_EXT_CLK_FID(0x2)
#define SBI_CLK_GET_RATES		SBI_EXT_CLK_FID(0x3)
#define SBI_CLK_SET_CONFIG		SBI_EXT_CLK_FID(0x4)
#define SBI_CLK_GET_CONFIG		SBI_EXT_CLK_FID(0x5)
#define SBI_CLK_SET_RATE		SBI_EXT_CLK_FID(0x6)
#define SBI_CLK_GET_RATE		SBI_EXT_CLK_FID(0x7)
#define SBI_CLK_GET_RATE_HI		SBI_EXT_CLK_FID(0x8)

#define SBI_CLK_NAME_LEN		32
#define SBI_CLK_MAX_NUM_RATES		((PAGE_SIZE - 16) / sizeof(u64))

enum {
	SBI_CLK_DISABLE,
	SBI_CLK_ENABLE,
};

enum {
	SBI_CLK_TYPE_RANGE,
	SBI_CLK_TYPE_DISCRETE,
};

union sbi_clk_rates {
	/* For clocks supporting discrete rates */
	u64 list[SBI_CLK_MAX_NUM_RATES];
	/* For clocks supporting a range of rates */
	struct {
		u64 min_rate;
		u64 max_rate;
		u64 step_size;
	} range;
};

struct sbi_clk_rates_info {
	u32 flags;
	u32 resvd;
	u32 remaining;
	u32 returned;
	union sbi_clk_rates rates;
};

struct sbi_clk {
	u32 id;
	char name[SBI_CLK_NAME_LEN];
	bool type;
	u32 num_rates;
	union sbi_clk_rates rates;
	struct clk_hw hw;
};

#define to_sbi_clk(clk) container_of(clk, struct sbi_clk, hw)

static int sbi_ventana_probe_group(unsigned long group_id)
{
	struct sbiret sbiret;

	sbiret = sbi_ecall(SBI_EXT_VENTANA, SBI_EXT_VENTANA_GROUP_PROBE,
			group_id, 0, 0, 0, 0, 0);

	return sbiret.value;
}

static int sbi_clk_num_clocks(void)
{
	struct sbiret sbiret;

	sbiret = sbi_ecall(SBI_EXT_VENTANA,
			SBI_CLK_GET_SYS_CLK_ATTR,
			0, 0, 0, 0, 0, 0);
	return sbiret.value;
}

static int sbi_clk_describe_rates(unsigned long clock_id,
		unsigned long rate_idx,
		unsigned long output_buf_pa_divby_64,
		unsigned long output_buf_size)
{
	struct sbiret sbiret;

	sbiret = sbi_ecall(SBI_EXT_VENTANA, SBI_CLK_GET_RATES,
			clock_id, rate_idx, output_buf_pa_divby_64,
			output_buf_size, 0, 0);
	return sbi_err_map_linux_errno(sbiret.error);
}

static int sbi_clk_describe_name(unsigned long clock_id,
		unsigned long output_buf_pa_divby_4,
		unsigned long output_buf_size)
{
	struct sbiret sbiret;

	sbiret = sbi_ecall(SBI_EXT_VENTANA,
			SBI_CLK_GET_ATTR,
			clock_id, output_buf_pa_divby_4,
			output_buf_size, 0, 0, 0);
	return sbi_err_map_linux_errno(sbiret.error);
}

static unsigned long sbi_clk_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{
	struct sbi_clk *clk = to_sbi_clk(hw);
	struct sbiret sbiret;

	sbiret = sbi_ecall(SBI_EXT_VENTANA, SBI_CLK_GET_RATE,
			clk->id, 0, 0, 0, 0, 0);
	if (sbiret.error)
		return 0;
	else
		return sbiret.value;
}

static long sbi_clk_round_rate(struct clk_hw *hw, unsigned long rate,
		unsigned long *parent_rate)
{
	struct sbi_clk *clk = to_sbi_clk(hw);
	u64 fmin, fmax, ftmp;

	if (clk->type ==  SBI_CLK_TYPE_DISCRETE)
		return rate;

	fmin = clk->rates.range.min_rate;
	fmax = clk->rates.range.max_rate;
	if (rate <= fmin)
		return fmin;
	else if (rate >= fmax)
		return fmax;

	ftmp = rate - fmin;
	ftmp += clk->rates.range.step_size - 1; /* to round up */
	do_div(ftmp, clk->rates.range.step_size);

	return ftmp * clk->rates.range.step_size + fmin;
}

static int sbi_clk_set_rate(struct clk_hw *hw, unsigned long rate,
		unsigned long parent_rate)
{
	struct sbi_clk *clk = to_sbi_clk(hw);
	struct sbiret sbiret;

	sbiret = sbi_ecall(SBI_EXT_VENTANA, SBI_CLK_SET_RATE, clk->id, rate,
			0, 0, 0, 0);
	return sbi_err_map_linux_errno(sbiret.error);
}

static int sbi_clk_enable(struct clk_hw *hw)
{
	struct sbi_clk *clk = to_sbi_clk(hw);
	struct sbiret sbiret;

	sbiret = sbi_ecall(SBI_EXT_VENTANA, SBI_CLK_SET_CONFIG,
			clk->id, SBI_CLK_ENABLE, 0, 0, 0, 0);
	return sbi_err_map_linux_errno(sbiret.error);
}

static void sbi_clk_disable(struct clk_hw *hw)
{
	struct sbi_clk *clk = to_sbi_clk(hw);

	sbi_ecall(SBI_EXT_VENTANA, SBI_CLK_SET_CONFIG,
			clk->id, SBI_CLK_DISABLE, 0, 0, 0, 0);
}

static const struct clk_ops sbi_clk_ops = {
	.recalc_rate = sbi_clk_recalc_rate,
	.round_rate = sbi_clk_round_rate,
	.set_rate = sbi_clk_set_rate,
	.prepare = sbi_clk_enable,
	.unprepare = sbi_clk_disable,
};

static int sbi_clk_get_rates(int idx, struct device *dev,
		struct sbi_clk *clk_ptr)
{
	struct sbi_clk_rates_info *rate_buf;
	struct page *page;
	int i, ret = 0;

	page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	rate_buf = page_address(page);
	ret = sbi_clk_describe_rates(idx, 0, (unsigned long)
			(page_to_phys(page)) >> 6,
			sizeof(*rate_buf));
	if (ret)
		goto out;

	clk_ptr->type = rate_buf->flags >> 31;

	/* We support max 16 rates for any clock */
	if (rate_buf->remaining)
		dev_warn(dev, "Clock %d has %d rates more than max\n",
				idx, rate_buf->remaining);

	/* returned rate info should contain at least 1 rate */
	if (!rate_buf->returned) {
		ret = -EINVAL;
		goto out;
	}

	clk_ptr->num_rates = rate_buf->returned;
	if (clk_ptr->type == SBI_CLK_TYPE_DISCRETE)
		for (i = 0; i < rate_buf->returned; i++)
			clk_ptr->rates.list[i] = rate_buf->rates.list[i];
	else {
		clk_ptr->rates.range.min_rate = rate_buf->rates.range.min_rate;
		clk_ptr->rates.range.max_rate = rate_buf->rates.range.max_rate;
		clk_ptr->rates.range.step_size = rate_buf->rates.range.step_size;
	}
out:
	free_page((unsigned long)rate_buf);
	return ret;
}

static struct clk_hw *sbi_clk_enum(int idx, struct device *dev)
{
	unsigned long min_rate, max_rate;
	struct clk_init_data init;
	struct sbi_clk *clk_ptr;
	struct clk_hw *clk_hw;
	int ret;

	clk_ptr = devm_kzalloc(dev, sizeof(struct sbi_clk), GFP_KERNEL);
	ret = sbi_clk_describe_name(idx, (unsigned long)
			(virt_to_phys(clk_ptr->name) >> 2),
			sizeof(clk_ptr->name));
	if (ret) {
		dev_err(dev, "Error. Unable to get name for clock %d\n", idx);
		return ERR_PTR(ret);
	}

	clk_ptr->id = idx;
	ret = sbi_clk_get_rates(idx, dev, clk_ptr);
	if (ret) {
		dev_err(dev, "Error. Unable to get rates for clock %d\n", idx);
		return ERR_PTR(ret);
	}

	init.flags = CLK_GET_RATE_NOCACHE,
		init.num_parents = 0,
		init.ops = &sbi_clk_ops,
		init.name = clk_ptr->name;
	clk_hw = &clk_ptr->hw;
	clk_hw->init = &init;
	ret = devm_clk_hw_register(dev, clk_hw);
	if (ret) {
		dev_err(dev, "Error. Unable to register clock %d %d\n", idx, ret);
		return ERR_PTR(ret);
	}

	if (clk_ptr->type == SBI_CLK_TYPE_DISCRETE) {
		min_rate = clk_ptr->rates.list[0];
		max_rate = clk_ptr->rates.list[clk_ptr->num_rates - 1];
	} else {
		min_rate = clk_ptr->rates.range.min_rate;
		max_rate = clk_ptr->rates.range.max_rate;
	}

	clk_hw_set_rate_range(clk_hw, min_rate, max_rate);
	return clk_hw;
}

static int sbi_clk_probe(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *clk_data;
	struct clk_hw *hw_ptr;
	int i, num_clocks;

	if (sbi_spec_version < sbi_mk_version(1, 0) ||
			sbi_probe_extension(SBI_EXT_VENTANA) <= 0) {
		dev_err(&pdev->dev, "SBI Ventana extension not available\n");
		return -ENODEV;
	}

	if (!sbi_ventana_probe_group(SBI_EXT_CLK)) {
		dev_err(&pdev->dev,
				"SBI Ventana extension clock group not available\n");
		return -ENODEV;
	}

	num_clocks = sbi_clk_num_clocks();
	if (!num_clocks) {
		dev_err(&pdev->dev, "Error. No clocks found\n");
		return -ENODEV;
	}

	dev_info(&pdev->dev, "%d clocks found\n", num_clocks);
	clk_data = devm_kzalloc(&pdev->dev, struct_size(clk_data, hws,
				num_clocks), GFP_KERNEL);
	clk_data->num = num_clocks;
	if (!clk_data)
		return -ENOMEM;

	for (i = 0; i < clk_data->num; i++) {
		hw_ptr = sbi_clk_enum(i, &pdev->dev);
		if (IS_ERR(hw_ptr))
			dev_err(&pdev->dev, "failed to register clock %d\n", i);
		clk_data->hws[i] = hw_ptr;
	}

	return devm_of_clk_add_hw_provider(&pdev->dev, of_clk_hw_onecell_get,
			clk_data);
}

static const struct of_device_id sbi_clk_id[] = {
	{ .compatible = "ventana,sbi-clk" },
	{ }
};
MODULE_DEVICE_TABLE(of, sbi_clk_id);

static struct platform_driver sbi_clk_driver = {
	.driver = {
		.name = "ventana_sbi_clk_driver",
		.of_match_table = sbi_clk_id,
	},
	.probe = sbi_clk_probe,
};
builtin_platform_driver(sbi_clk_driver);

MODULE_AUTHOR("Mayuresh Chitale <mchitale@ventanamicro.com");
MODULE_DESCRIPTION("SBI Ventana extension clock driver");
MODULE_LICENSE("GPL");
