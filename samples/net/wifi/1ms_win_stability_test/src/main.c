/*
 * Copyright (c) 2024 Muhammad Haziq
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/irq.h>
#include "wifi_test.h"

#define WDT_MAX_WINDOW	1
#define WDT_TIMEOUT		K_MSEC(1000)
#define TRACE(FMT, ...) do { \
	uint32_t time_now_ms = k_cycle_get_32() * 1000LLU / sys_clock_hw_cycles_per_sec(); \
	printk("%03u.%03u: " FMT, time_now_ms / 1000, time_now_ms % 1000, ##__VA_ARGS__);   \
} while(0)

#ifdef CONFIG_WATCHDOG
#define WDT_NODE DT_INST(0, silabs_siwx91x_wdt)
const struct device *const wdt_dev = DEVICE_DT_GET(WDT_NODE);
static struct wdt_timeout_cfg m_cfg_wdt0;
#endif

LOG_MODULE_REGISTER(MAIN);

static struct gpio_dt_spec gpio_pin = GPIO_DT_SPEC_GET(DT_NODELABEL(led0), gpios);
static struct gpio_dt_spec timer_pin = GPIO_DT_SPEC_GET(DT_NODELABEL(toggle0), gpios);

#ifdef CONFIG_WATCHDOG
static void wdt_callback(const struct device *dev, int channel_id)
{
	EGPIO1->PIN_CONFIG[1].BIT_LOAD_REG = 1;
	EGPIO1->PIN_CONFIG[1].BIT_LOAD_REG = 0;
}
#endif

int main(void)
{
	int ret;

#ifdef CONFIG_WATCHDOG
	if (!device_is_ready(wdt_dev)) {
		printk("WDT device not found\n");
		return -ENODEV;
	}
#endif
	if (!gpio_is_ready_dt(&gpio_pin)) {
		printk("GPIO device not found\n");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&timer_pin)) {
		printk("GPIO device not found\n");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&gpio_pin, GPIO_OUTPUT_INACTIVE);
	if (ret) {
		printk("Cannot configure GPIO pin\n");
		return -EIO;
	}

	ret = gpio_pin_configure_dt(&timer_pin, GPIO_OUTPUT_INACTIVE);
	if (ret) {
		printk("Cannot configure GPIO pin\n");
		return -EIO;
	}

#ifdef CONFIG_WATCHDOG
	m_cfg_wdt0.window.min = 0U;
	m_cfg_wdt0.window.max = WDT_MAX_WINDOW;
	m_cfg_wdt0.flags = WDT_FLAG_RESET_NONE;
	m_cfg_wdt0.callback = wdt_callback;

	ret = wdt_install_timeout(wdt_dev, &m_cfg_wdt0);
	if (ret) {
		printk("Cannot configure WDT\n");
		return -EIO;
	}

	ret = wdt_setup(wdt_dev, WDT_OPT_PAUSE_HALTED_BY_DBG);
	if(ret) {
		printk("Cannot setup WDT\n");
		return -EIO;
	}
#endif
	siwx91x_wifi_init();

	for (;;) {
		k_sleep(K_SECONDS(1));

		gpio_pin_toggle_dt(&gpio_pin);
		ret = siwx91x_wifi_operation();
		if (ret < 0) {
			return ret;
		}
		gpio_pin_toggle_dt(&gpio_pin);
	}

	return 0;
}
