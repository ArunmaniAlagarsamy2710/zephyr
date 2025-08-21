#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/dhcpv4_server.h>

#define MACSTR "%02X:%02X:%02X:%02X:%02X:%02X"
#define NET_EVENT_WIFI_MASK                                                                        \
	 (NET_EVENT_WIFI_AP_ENABLE_RESULT | NET_EVENT_WIFI_AP_DISABLE_RESULT |                      \
	 NET_EVENT_WIFI_AP_STA_CONNECTED | NET_EVENT_WIFI_AP_STA_DISCONNECTED)

/* AP Mode Configuration */
#define WIFI_AP_SSID       "arun_silabs"
#define WIFI_AP_PSK        "12345678"

#define WIFI_AP_IP_ADDRESS "192.168.4.1"
#define WIFI_AP_NETMASK    "255.255.255.0"

LOG_MODULE_REGISTER(test_ap);

static struct net_if *ap_iface;
static struct wifi_connect_req_params ap_config;
static struct net_mgmt_event_callback cb;

static K_SEM_DEFINE(wlan_sem, 0, 1);

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
}

static void wifi_event_handler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
			       struct net_if *iface)
{
	switch (mgmt_event) {
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

static int siwx91x_disable_ap(void)
{
	int ret;

	ret = net_mgmt(NET_REQUEST_WIFI_AP_DISABLE, ap_iface, NULL, 0);
	if (ret) {
		printf("AP mode disable failed: %d\n", ret);
	}

	return ret;
}

int siwx91x_wifi_init(void)
{
	net_mgmt_init_event_callback(&cb, wifi_event_handler, NET_EVENT_WIFI_MASK);
	net_mgmt_add_event_callback(&cb);

	ap_iface = net_if_get_wifi_sap();

	return 0;
}


int siwx91x_wifi_operation(void)
{
	siwx91x_enable_ap();
	k_sleep(K_SECONDS(2));
	k_sem_take(&wlan_sem, K_FOREVER);
	if (IS_ENABLED(CONFIG_AP_DATA_TRANSFER_TEST)) {
		enable_dhcpv4_server();
		k_sleep(K_MINUTES(10));
	} else {
		siwx91x_disable_ap();
	}

	return 0;
}
