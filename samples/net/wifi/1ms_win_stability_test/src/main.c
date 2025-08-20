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
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/irq.h>
#include <zephyr/net/dhcpv4_server.h>

LOG_MODULE_REGISTER(MAIN);

#define MACSTR "%02X:%02X:%02X:%02X:%02X:%02X"
#define NET_EVENT_WIFI_MASK                                                                        \
	(NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT |                        \
	 NET_EVENT_WIFI_AP_ENABLE_RESULT | NET_EVENT_WIFI_AP_DISABLE_RESULT |                      \
	 NET_EVENT_WIFI_AP_STA_CONNECTED | NET_EVENT_WIFI_AP_STA_DISCONNECTED)

#define WIFI_SHELL_SCAN_EVENTS (                   \
				NET_EVENT_WIFI_SCAN_RESULT        |\
				NET_EVENT_WIFI_SCAN_DONE)
/* AP Mode Configuration */
#define WIFI_AP_SSID       "arun"
#define WIFI_AP_PSK        "12345678"

/* STA Mode Configuration */
#define WIFI_SSID "arun"     /* Replace `SSID` with WiFi ssid. */
#define WIFI_PSK  "12345678" /* Replace `PASSWORD` with Router password. */

#define WIFI_AP_IP_ADDRESS "192.168.4.1"
#define WIFI_AP_NETMASK    "255.255.255.0"

#ifdef CONFIG_WATCHDOG
#define WDT_NODE DT_INST(0, silabs_siwx91x_wdt)
const struct device *const wdt_dev = DEVICE_DT_GET(WDT_NODE);
static struct wdt_timeout_cfg m_cfg_wdt0;
#endif

#define WDT_MAX_WINDOW	1
#define WDT_TIMEOUT		K_MSEC(1000)

#define TRACE(FMT, ...) do { \
	uint32_t time_now_ms = k_cycle_get_32() * 1000LLU / sys_clock_hw_cycles_per_sec(); \
	printk("%03u.%03u: " FMT, time_now_ms / 1000, time_now_ms % 1000, ##__VA_ARGS__);   \
} while(0)

static struct gpio_dt_spec gpio_pin = GPIO_DT_SPEC_GET(DT_NODELABEL(led0), gpios);
static struct gpio_dt_spec timer_pin = GPIO_DT_SPEC_GET(DT_NODELABEL(toggle0), gpios);

static struct net_if *ap_iface;
static struct net_if *sta_iface;
static struct net_mgmt_event_callback cb;
static struct net_mgmt_event_callback wifi_shell_scan_cb;

static struct wifi_connect_req_params ap_config;
static struct wifi_connect_req_params sta_config;

static K_SEM_DEFINE(wlan_sem, 0, 1);

static struct {
        const struct shell *sh;
        uint32_t scan_result;

        union {
                struct {
                        uint8_t connecting: 1;
                        uint8_t disconnecting: 1;
                        uint8_t _unused: 6;
                };
                uint8_t all;
        };
} context;

static void handle_wifi_scan_result(struct net_mgmt_event_callback *cb)
{
	const struct wifi_scan_result *entry =
		(const struct wifi_scan_result *)cb->info;
	uint8_t ssid_print[WIFI_SSID_MAX_LEN + 1];
	context.scan_result++;

	if (context.scan_result == 1U) {
		printf("\n%-4s | %-32s %-5s | %-13s | %-4s | %-15s | %-8s\n",
				"Num", "SSID", "(len)", "Chan (Band)", "RSSI", "Security", "MFP");
	}

	strncpy(ssid_print, entry->ssid, sizeof(ssid_print) - 1);
	ssid_print[sizeof(ssid_print) - 1] = '\0';
	printf("%-4d | %-32s %-5u | %-4u (%-6s) | %-4d | %-15s | %-8s\n",
			context.scan_result, ssid_print, entry->ssid_length, entry->channel,
			wifi_band_txt(entry->band),
			entry->rssi,
			wifi_security_txt(entry->security),
			wifi_mfp_txt(entry->mfp));
}

static void handle_wifi_scan_done(struct net_mgmt_event_callback *cb)
{
        const struct wifi_status *status =
                (const struct wifi_status *)cb->info;

        if (status->status) {
                printf("Scan request failed (%d)\n", status->status);
        } else {
                printf("Scan request done\n");
        }

	net_mgmt_del_event_callback(&wifi_shell_scan_cb);
	context.scan_result = 0U;
	k_sem_give(&wlan_sem);
}


static void handle_wifi_connect_result(struct net_mgmt_event_callback *cb)
{
        const struct wifi_status *status =
                (const struct wifi_status *) cb->info;

        if (status->status) {
                LOG_ERR("Connection request failed (%d)\n", status->status);
        } else {
                LOG_INF("Connected to Wi-Fi\n");
		k_sem_give(&wlan_sem);
        }

        context.connecting = false;
}

static void handle_wifi_disconnect_result(struct net_mgmt_event_callback *cb)
{
        const struct wifi_status *status =
                (const struct wifi_status *) cb->info;

        if (context.disconnecting) {
                if (status->status) {
                        LOG_ERR("Disconnection request failed (%d)\n", status->status);
                } else {
                        LOG_INF("Disconnection request done (%d)\n", status->status);
                }
                context.disconnecting = false;
        } else {
                LOG_INF("Disconnected\n");
        }
}

static void handle_wifi_ap_enable_result(struct net_mgmt_event_callback *cb)
{
        const struct wifi_status *status =
                (const struct wifi_status *)cb->info;

        if (status->status) {
                LOG_ERR("AP enable request failed (%d)\n", status->status);
        } else {
                LOG_INF("AP enabled\n");
        }
	k_sem_give(&wlan_sem);
}

static void handle_wifi_ap_disable_result(struct net_mgmt_event_callback *cb)
{
        const struct wifi_status *status =
                (const struct wifi_status *)cb->info;

        if (status->status) {
                LOG_ERR("AP disable request failed (%d)\n", status->status);
        } else {
                LOG_INF("AP disabled\n");
        }
	k_sem_give(&wlan_sem);
}

static void wifi_mgmt_scan_event_handler(struct net_mgmt_event_callback *cb,
					 uint64_t mgmt_event, struct net_if *iface)
{
	switch (mgmt_event) {
	case NET_EVENT_WIFI_SCAN_RESULT:
		handle_wifi_scan_result(cb);
		break;
	case NET_EVENT_WIFI_SCAN_DONE:
		handle_wifi_scan_done(cb);
		break;
	default:
		break;
	}
}

static void wifi_event_handler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
			       struct net_if *iface)
{
	switch (mgmt_event) {
	case NET_EVENT_WIFI_CONNECT_RESULT: {
		handle_wifi_connect_result(cb);
		break;
	}
	case NET_EVENT_WIFI_DISCONNECT_RESULT: {
		handle_wifi_disconnect_result(cb);
		break;
	}
	case NET_EVENT_WIFI_AP_ENABLE_RESULT: {
		handle_wifi_ap_enable_result(cb);
		break;
	}
	case NET_EVENT_WIFI_AP_DISABLE_RESULT: {
		handle_wifi_ap_disable_result(cb);
		break;
	}
	case NET_EVENT_WIFI_AP_STA_CONNECTED: {
		struct wifi_ap_sta_info *sta_info = (struct wifi_ap_sta_info *)cb->info;

		LOG_INF("station: " MACSTR " joined \n", sta_info->mac[0], sta_info->mac[1],
			sta_info->mac[2], sta_info->mac[3], sta_info->mac[4], sta_info->mac[5]);
		break;
	}
	case NET_EVENT_WIFI_AP_STA_DISCONNECTED: {
		struct wifi_ap_sta_info *sta_info = (struct wifi_ap_sta_info *)cb->info;

		LOG_INF("station: " MACSTR " leave \n", sta_info->mac[0], sta_info->mac[1],
			sta_info->mac[2], sta_info->mac[3], sta_info->mac[4], sta_info->mac[5]);
		break;
	}
	default:
		break;
	}
}

static int siwx91x_mode_switch(uint8_t mode, struct net_if *iface)
{
	struct wifi_mode_info mode_info = {0};
	int ret;

	mode_info.oper = WIFI_MGMT_SET;
	mode_info.mode = mode;

	ret = net_mgmt(NET_REQUEST_WIFI_MODE, iface, &mode_info, sizeof(mode_info));
	if (ret < 0) {
		LOG_ERR("mode %s operation failed with reason %d\n",
			mode_info.oper == WIFI_MGMT_GET ? "get" : "set", ret);
	}

	return ret;

}

static int siwx91x_enable_ap(void)
{
	int ret;

	if (!ap_iface) {
		LOG_ERR("AP: is not initialized");
		return -EIO;
	}

	ret = siwx91x_mode_switch(WIFI_SOFTAP_MODE, ap_iface);
	if (ret < 0) {
		return ret;
	}

	LOG_INF("Turning on AP Mode");
	ap_config.ssid = (const uint8_t *)WIFI_AP_SSID;
	ap_config.ssid_length = strlen(WIFI_AP_SSID);
	ap_config.psk = (const uint8_t *)WIFI_AP_PSK;
	ap_config.psk_length = strlen(WIFI_AP_PSK);
	ap_config.channel = WIFI_CHANNEL_ANY;
	ap_config.band = WIFI_FREQ_BAND_2_4_GHZ;
	ap_config.bandwidth = WIFI_FREQ_BANDWIDTH_20MHZ;

	if (strlen(WIFI_AP_PSK) == 0) {
		ap_config.security = WIFI_SECURITY_TYPE_NONE;
	} else {

		ap_config.security = WIFI_SECURITY_TYPE_PSK;
	}

	ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, ap_iface, &ap_config,
			   sizeof(struct wifi_connect_req_params));
	if (ret) {
		LOG_ERR("NET_REQUEST_WIFI_AP_ENABLE failed, err: %d", ret);
	}

	return ret;
}

static int siwx91x_disable_ap()
{
	int ret;

	ret = net_mgmt(NET_REQUEST_WIFI_AP_DISABLE, ap_iface, NULL, 0);
	if (ret) {
		printf("AP mode disable failed: %d\n", ret);
	}

	return ret;
}

static int siwx91x_device_connect(void)
{
	int ret;

	if (!sta_iface) {
		LOG_INF("STA: interface no initialized");
		return -EIO;
	}

	ret = siwx91x_mode_switch(WIFI_STA_MODE, sta_iface);
	if (ret < 0) {
		return ret;
	}

	sta_config.ssid = (const uint8_t *)WIFI_SSID;
	sta_config.ssid_length = strlen(WIFI_SSID);
	sta_config.psk = (const uint8_t *)WIFI_PSK;
	sta_config.psk_length = strlen(WIFI_PSK);
	sta_config.security = WIFI_SECURITY_TYPE_PSK;
	sta_config.channel = WIFI_CHANNEL_ANY;
	sta_config.band = WIFI_FREQ_BAND_2_4_GHZ;

	LOG_INF("Connecting to SSID: %s\n", sta_config.ssid);

	ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, sta_iface, &sta_config,
			   sizeof(struct wifi_connect_req_params));
	if (ret) {
		LOG_ERR("Unable to Connect to (%s)", WIFI_SSID);
	}

	return ret;
}

static int siwx91x_device_disconnect(void)
{
	int ret;

	ret = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, sta_iface, NULL, 0);
	if (ret < 0) {
		LOG_ERR("Disconnection failed:%d\n", ret);
	}

	return ret;
}

int siwx91x_enable_scan()
{
	struct wifi_scan_params params = { 0 };
	int ret;

	net_mgmt_add_event_callback(&wifi_shell_scan_cb);
	ret = net_mgmt(NET_REQUEST_WIFI_SCAN, sta_iface, &params, sizeof(params));
	if (ret < 0) {
		LOG_ERR("Scan failed:%d\n", ret);
	}
	k_sem_take(&wlan_sem, K_FOREVER);
	return ret;
}

static void enable_dhcpv4_server(void)
{
	static struct in_addr addr;
	static struct in_addr netmaskAddr;

	if (net_addr_pton(AF_INET, WIFI_AP_IP_ADDRESS, &addr)) {
		LOG_ERR("Invalid address: %s", WIFI_AP_IP_ADDRESS);
		return;
	}

	if (net_addr_pton(AF_INET, WIFI_AP_NETMASK, &netmaskAddr)) {
		LOG_ERR("Invalid netmask: %s", WIFI_AP_NETMASK);
		return;
	}

	net_if_ipv4_set_gw(ap_iface, &addr);

	if (net_if_ipv4_addr_add(ap_iface, &addr, NET_ADDR_MANUAL, 0) == NULL) {
		LOG_ERR("unable to set IP address for AP interface");
	}

	if (!net_if_ipv4_set_netmask_by_addr(ap_iface, &addr, &netmaskAddr)) {
		LOG_ERR("Unable to set netmask for AP interface: %s", WIFI_AP_NETMASK);
	}

	addr.s4_addr[3] += 10; /* Starting IPv4 address for DHCPv4 address pool. */

	if (net_dhcpv4_server_start(ap_iface, &addr) != 0) {
		LOG_ERR("DHCP server is not started for desired IP");
		return;
	}

	LOG_INF("DHCPv4 server started...\n");
}

static int siwx91x_wifi_init(void)
{
	net_mgmt_init_event_callback(&cb, wifi_event_handler, NET_EVENT_WIFI_MASK);
	net_mgmt_add_event_callback(&cb);

	net_mgmt_init_event_callback(&wifi_shell_scan_cb,
				     wifi_mgmt_scan_event_handler,
				     WIFI_SHELL_SCAN_EVENTS);

	ap_iface = net_if_get_wifi_sap();
	sta_iface = net_if_get_wifi_sta();

	return 0;
}

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

#ifdef CONFIG_TEST_STA
		gpio_pin_toggle_dt(&gpio_pin);
		ret = siwx91x_device_connect();
		if (ret < 0) {
			continue;
		}
		gpio_pin_toggle_dt(&gpio_pin);
		k_sem_take(&wlan_sem, K_FOREVER);
		k_sleep(K_SECONDS(1));
disconnect:
		gpio_pin_toggle_dt(&gpio_pin);
		ret = siwx91x_device_disconnect();
		if (ret < 0) {
			printf("Disconnection retry\n");
			goto disconnect;
		}
		gpio_pin_toggle_dt(&gpio_pin);

#elif CONFIG_TEST_MULTIPLE_SCAN
		gpio_pin_toggle_dt(&gpio_pin);
		siwx91x_enable_scan();
		gpio_pin_toggle_dt(&gpio_pin);

#elif CONFIG_TEST_AP
		gpio_pin_toggle_dt(&gpio_pin);
		siwx91x_enable_ap();
		k_sleep(K_SECONDS(1));
		k_sem_take(&wlan_sem, K_FOREVER);
		siwx91x_disable_ap();
		gpio_pin_toggle_dt(&gpio_pin);

#elif CONFIG_DATA_TRANSFER_TEST
		gpio_pin_toggle_dt(&gpio_pin);
		siwx91x_enable_ap();
		k_sem_take(&wlan_sem, K_FOREVER);
		enable_dhcpv4_server();
		k_sleep(K_SECONDS(120));
		siwx91x_disable_ap();
		gpio_pin_toggle_dt(&gpio_pin);
#endif

	}

	return 0;
}
