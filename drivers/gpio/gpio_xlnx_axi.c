/*
 * Copyright (c) 2020 Henrik Brix Andersen <henrik@brixandersen.dk>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT xlnx_xps_gpio_1_00_a

#include <device.h>
#include <drivers/gpio.h>
#include <sys/sys_io.h>

#include "gpio_utils.h"

/* AXI GPIO v2 register offsets (See Xilinx PG144 for details) */
#define GPIO_DATA_OFFSET  0x0000
#define GPIO_TRI_OFFSET   0x0004
#define GPIO2_DATA_OFFSET 0x0008
#define GPIO2_TRI_OFFSET  0x000c
#define GIER_OFFSET       0x011c
#define IPISR_OFFSET      0x0120
#define IPIER_OFFSET      0x0128

/* GIER bit definitions */
#define GIER_GIE BIT(31)

/* IPISR and IPIER bit definitions */
#define IPIXX_CH1_IE BIT(0)
#define IPIXX_CH2_IE BIT(1)

/* Maximum number of GPIOs supported per channel */
#define MAX_GPIOS 32

struct gpio_xlnx_axi_config {
	/* gpio_driver_config needs to be first */
	struct gpio_driver_config common;
	mm_reg_t base;
#ifdef CONFIG_MMU
	size_t size;
	bool is_dual;
#endif
	bool all_inputs : 1;
	bool all_outputs : 1;
};

struct gpio_xlnx_axi_data {
	/* gpio_driver_data needs to be first */
	struct gpio_driver_data common;
#ifdef CONFIG_MMU
	mm_reg_t mmio;
	const struct device *gpio2;
#endif
	/* Shadow registers for data out and tristate */
	uint32_t dout;
	uint32_t tri;
};

static inline mm_reg_t gpio_xlnx_axi_get_addr(const struct device *dev)
{
#if CONFIG_MMU
	return ((struct gpio_xlnx_axi_data *)dev->data)->mmio;
#else
	return ((struct gpio_xlnx_axi_config *)dev->config)->base;
#endif
}

static inline uint32_t gpio_xlnx_axi_read_data(const struct device *dev)
{
	return sys_read32(gpio_xlnx_axi_get_addr(dev) + GPIO_DATA_OFFSET);
}

static inline void gpio_xlnx_axi_write_data(const struct device *dev,
					    uint32_t val)
{
	sys_write32(val, gpio_xlnx_axi_get_addr(dev) + GPIO_DATA_OFFSET);
}

static inline void gpio_xlnx_axi_write_tri(const struct device *dev,
					   uint32_t val)
{
	sys_write32(val, gpio_xlnx_axi_get_addr(dev) + GPIO_TRI_OFFSET);
}

static int gpio_xlnx_axi_pin_configure(const struct device *dev,
				       gpio_pin_t pin,
				       gpio_flags_t flags)
{
	const struct gpio_xlnx_axi_config *config = dev->config;
	struct gpio_xlnx_axi_data *data = dev->data;
	unsigned int key;

	if (!(BIT(pin) & config->common.port_pin_mask)) {
		return -EINVAL;
	}

	if (((flags & GPIO_INPUT) != 0) && ((flags & GPIO_OUTPUT) != 0)) {
		return -ENOTSUP;
	}

	if ((flags & GPIO_SINGLE_ENDED) != 0) {
		return -ENOTSUP;
	}

	if ((flags & (GPIO_PULL_UP | GPIO_PULL_DOWN)) != 0) {
		return -ENOTSUP;
	}

	if (((flags & GPIO_INPUT) != 0) && config->all_outputs) {
		return -ENOTSUP;
	}

	if (((flags & GPIO_OUTPUT) != 0) && config->all_inputs) {
		return -ENOTSUP;
	}

	key = irq_lock();

	switch (flags & GPIO_DIR_MASK) {
	case GPIO_INPUT:
		data->tri |= BIT(pin);
		break;
	case GPIO_OUTPUT:
		if ((flags & GPIO_OUTPUT_INIT_HIGH) != 0) {
			data->dout |= BIT(pin);
		} else if ((flags & GPIO_OUTPUT_INIT_LOW) != 0) {
			data->dout &= ~BIT(pin);
		}
		data->tri &= ~BIT(pin);
		break;
	default:
		return -ENOTSUP;
	}

	gpio_xlnx_axi_write_data(dev, data->dout);
	gpio_xlnx_axi_write_tri(dev, data->tri);

	irq_unlock(key);

	return 0;
}

static int gpio_xlnx_axi_port_get_raw(const struct device *dev,
				      gpio_port_value_t *value)
{
	*value = gpio_xlnx_axi_read_data(dev);
	return 0;
}

static int gpio_xlnx_axi_port_set_masked_raw(const struct device *dev,
					     gpio_port_pins_t mask,
					     gpio_port_value_t value)
{
	struct gpio_xlnx_axi_data *data = dev->data;
	unsigned int key;

	key = irq_lock();
	data->dout = (data->dout & ~mask) | (mask & value);
	gpio_xlnx_axi_write_data(dev, data->dout);
	irq_unlock(key);

	return 0;
}

static int gpio_xlnx_axi_port_set_bits_raw(const struct device *dev,
					   gpio_port_pins_t pins)
{
	struct gpio_xlnx_axi_data *data = dev->data;
	unsigned int key;

	key = irq_lock();
	data->dout |= pins;
	gpio_xlnx_axi_write_data(dev, data->dout);
	irq_unlock(key);

	return 0;
}

static int gpio_xlnx_axi_port_clear_bits_raw(const struct device *dev,
					     gpio_port_pins_t pins)
{
	struct gpio_xlnx_axi_data *data = dev->data;
	unsigned int key;

	key = irq_lock();
	data->dout &= ~pins;
	gpio_xlnx_axi_write_data(dev, data->dout);
	irq_unlock(key);

	return 0;
}

static int gpio_xlnx_axi_port_toggle_bits(const struct device *dev,
					  gpio_port_pins_t pins)
{
	struct gpio_xlnx_axi_data *data = dev->data;
	unsigned int key;

	key = irq_lock();
	data->dout ^= pins;
	gpio_xlnx_axi_write_data(dev, data->dout);
	irq_unlock(key);

	return 0;
}

static int gpio_xlnx_axi_pin_interrupt_configure(const struct device *dev,
						 gpio_pin_t pin,
						 enum gpio_int_mode mode,
						 enum gpio_int_trig trig)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(pin);
	ARG_UNUSED(mode);
	ARG_UNUSED(trig);

	/*
	 * The Xilinx AXI GPIO IP only supports a port-wide pin change
	 * interrupt. This does not map well to the current Zephyr GPIO IRQ API.
	 */
	return -ENOTSUP;
}

static int gpio_xlnx_axi_manage_callback(const struct device *dev,
					 struct gpio_callback *cb,
					 bool set)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(set);

	return -ENOTSUP;
}

static uint32_t gpio_xlnx_axi_get_pending_int(const struct device *dev)
{
	return 0;
}

static int gpio_xlnx_axi_init(const struct device *dev)
{
	struct gpio_xlnx_axi_data *data = dev->data;

#ifdef CONFIG_MMU
	const struct gpio_xlnx_axi_config *config = dev->config;

	if ((config->is_dual && data->gpio2 != NULL) || !config->is_dual) {
		device_map(&data->mmio, config->base, config->size, K_MEM_CACHE_NONE);

		if (data->gpio2 != NULL) {
			((struct gpio_xlnx_axi_data *)data->gpio2->data)->mmio = data->mmio + GPIO2_DATA_OFFSET;
		}
	}
#endif

	gpio_xlnx_axi_write_data(dev, data->dout);
	gpio_xlnx_axi_write_tri(dev, data->tri);

	return 0;
}

static const struct gpio_driver_api gpio_xlnx_axi_driver_api = {
	.pin_configure = gpio_xlnx_axi_pin_configure,
	.port_get_raw = gpio_xlnx_axi_port_get_raw,
	.port_set_masked_raw = gpio_xlnx_axi_port_set_masked_raw,
	.port_set_bits_raw = gpio_xlnx_axi_port_set_bits_raw,
	.port_clear_bits_raw = gpio_xlnx_axi_port_clear_bits_raw,
	.port_toggle_bits = gpio_xlnx_axi_port_toggle_bits,
	.pin_interrupt_configure = gpio_xlnx_axi_pin_interrupt_configure,
	.manage_callback = gpio_xlnx_axi_manage_callback,
	.get_pending_int = gpio_xlnx_axi_get_pending_int,
};

#define GPIO_XLNX_AXI_GPIO2_HAS_COMPAT_STATUS_OKAY(n)		    \
	UTIL_AND(						    \
		DT_NODE_HAS_COMPAT(DT_CHILD(DT_DRV_INST(n), gpio2), \
				   xlnx_xps_gpio_1_00_a_gpio2),	    \
		DT_NODE_HAS_STATUS(DT_CHILD(DT_DRV_INST(n), gpio2), \
				   okay)			    \
		)

#define GPIO_XLNX_AXI_GPIO2_COND_INIT(n)				 \
	IF_ENABLED(UTIL_AND(						 \
			   DT_INST_PROP_OR(n, xlnx_is_dual, 1),		 \
			   GPIO_XLNX_AXI_GPIO2_HAS_COMPAT_STATUS_OKAY(n) \
			   ),						 \
		   (GPIO_XLNX_AXI_GPIO2_INIT(n)));

#ifdef CONFIG_MMU
#define GPIO_XLNX_AXI_GPIO2_INIT(n)					 \
	static struct gpio_xlnx_axi_data gpio_xlnx_axi_##n##_2_data = {	 \
		.dout = DT_INST_PROP_OR(n, xlnx_dout_default_2, 0),	 \
		.tri = DT_INST_PROP_OR(n, xlnx_tri_default_2,		 \
				       GENMASK(MAX_GPIOS - 1, 0)),	 \
		.gpio2 = NULL,						 \
	};								 \
									 \
	static const struct gpio_xlnx_axi_config			 \
			    gpio_xlnx_axi_##n##_2_config = {		 \
		.common = {						 \
			.port_pin_mask = GPIO_PORT_PIN_MASK_FROM_NGPIOS( \
				DT_INST_PROP_OR(n, xlnx_gpio2_width,	 \
						MAX_GPIOS)),		 \
		},							 \
		.base = DT_INST_REG_ADDR(n) + GPIO2_DATA_OFFSET,	 \
		.size = DT_INST_REG_SIZE(n) - GPIO2_DATA_OFFSET,	 \
		.is_dual = true,					 \
		.all_inputs = DT_INST_PROP_OR(n, xlnx_all_inputs2, 0),	 \
		.all_outputs = DT_INST_PROP_OR(n, xlnx_all_outputs2, 0), \
	};								 \
									 \
	DEVICE_DT_DEFINE(DT_CHILD(DT_DRV_INST(n), gpio2),		 \
			 &gpio_xlnx_axi_init,				 \
			 NULL,						 \
			 &gpio_xlnx_axi_##n##_2_data,			 \
			 &gpio_xlnx_axi_##n##_2_config,			 \
			 PRE_KERNEL_2,					 \
			 CONFIG_GPIO_INIT_PRIORITY,			 \
			 &gpio_xlnx_axi_driver_api);			 \

#define GPIO_XLNX_AXI_INIT(n)							 \
	static struct gpio_xlnx_axi_data gpio_xlnx_axi_##n##_data = {		 \
		.dout = DT_INST_PROP_OR(n, xlnx_dout_default, 0),		 \
		.tri = DT_INST_PROP_OR(n, xlnx_tri_default,			 \
				       GENMASK(MAX_GPIOS - 1, 0)),		 \
		.gpio2 = DEVICE_DT_GET_OR_NULL(DT_CHILD(DT_DRV_INST(n), gpio2)), \
	};									 \
										 \
	static const struct gpio_xlnx_axi_config				 \
			    gpio_xlnx_axi_##n##_config = {			 \
		.common = {							 \
			.port_pin_mask = GPIO_PORT_PIN_MASK_FROM_NGPIOS(	 \
				DT_INST_PROP_OR(n, xlnx_gpio_width,		 \
						MAX_GPIOS)),			 \
		},								 \
		.base = DT_INST_REG_ADDR(n),					 \
		.size = DT_INST_REG_SIZE(n),					 \
		.is_dual = DT_INST_PROP_OR(n, xlnx_is_dual, 0),			 \
		.all_inputs = DT_INST_PROP_OR(n, xlnx_all_inputs, 0),		 \
		.all_outputs = DT_INST_PROP_OR(n, xlnx_all_outputs, 0),		 \
	};									 \
										 \
	DEVICE_DT_INST_DEFINE(n,						 \
			      &gpio_xlnx_axi_init,				 \
			      NULL,						 \
			      &gpio_xlnx_axi_##n##_data,			 \
			      &gpio_xlnx_axi_##n##_config,			 \
			      PRE_KERNEL_1,					 \
			      CONFIG_GPIO_INIT_PRIORITY,			 \
			      &gpio_xlnx_axi_driver_api);			 \
	GPIO_XLNX_AXI_GPIO2_COND_INIT(n);
#else
#define GPIO_XLNX_AXI_GPIO2_INIT(n)												      \
	static struct gpio_xlnx_axi_data gpio_xlnx_axi_##n##_2_data = {								      \
		.dout = DT_INST_PROP_OR(n, xlnx_dout_default_2, 0),								      \
		.tri = DT_INST_PROP_OR(n, xlnx_tri_default_2,									      \
				       GENMASK(MAX_GPIOS - 1, 0)),								      \
	};															      \
																      \
	static const struct gpio_xlnx_axi_config										      \
			    gpio_xlnx_axi_##n##_2_config = {                 \							      \
									     .common = {					      \
										     .port_pin_mask = GPIO_PORT_PIN_MASK_FROM_NGPIOS( \
											     DT_INST_PROP_OR(n, xlnx_gpio2_width,     \
													     MAX_GPIOS)),	      \
									     },							      \
									     .base = DT_INST_REG_ADDR(n) + GPIO2_DATA_OFFSET,	      \
									     .all_inputs = DT_INST_PROP_OR(n, xlnx_all_inputs2, 0),   \
									     .all_outputs = DT_INST_PROP_OR(n, xlnx_all_outputs2, 0), \
	};															      \
																      \
	DEVICE_DT_DEFINE(DT_CHILD(DT_DRV_INST(n), gpio2),									      \
			 &gpio_xlnx_axi_init,											      \
			 NULL,													      \
			 &gpio_xlnx_axi_##n##_2_data,										      \
			 &gpio_xlnx_axi_##n##_2_config,										      \
			 PRE_KERNEL_1,												      \
			 CONFIG_GPIO_INIT_PRIORITY,										      \
			 &gpio_xlnx_axi_driver_api);

#define GPIO_XLNX_AXI_INIT(n)						 \
	static struct gpio_xlnx_axi_data gpio_xlnx_axi_##n##_data = {	 \
		.dout = DT_INST_PROP_OR(n, xlnx_dout_default, 0),	 \
		.tri = DT_INST_PROP_OR(n, xlnx_tri_default,		 \
				       GENMASK(MAX_GPIOS - 1, 0)),	 \
	};								 \
									 \
	static const struct gpio_xlnx_axi_config			 \
			    gpio_xlnx_axi_##n##_config = {		 \
		.common = {						 \
			.port_pin_mask = GPIO_PORT_PIN_MASK_FROM_NGPIOS( \
				DT_INST_PROP_OR(n, xlnx_gpio_width,	 \
						MAX_GPIOS)),		 \
		},							 \
		.base = DT_INST_REG_ADDR(n),				 \
		.all_inputs = DT_INST_PROP_OR(n, xlnx_all_inputs, 0),	 \
		.all_outputs = DT_INST_PROP_OR(n, xlnx_all_outputs, 0),	 \
	};								 \
									 \
	DEVICE_DT_INST_DEFINE(n,					 \
			      &gpio_xlnx_axi_init,			 \
			      NULL,					 \
			      &gpio_xlnx_axi_##n##_data,		 \
			      &gpio_xlnx_axi_##n##_config,		 \
			      PRE_KERNEL_1,				 \
			      CONFIG_GPIO_INIT_PRIORITY,		 \
			      &gpio_xlnx_axi_driver_api);		 \
	GPIO_XLNX_AXI_GPIO2_COND_INIT(n);
#endif

DT_INST_FOREACH_STATUS_OKAY(GPIO_XLNX_AXI_INIT)
