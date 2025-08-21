#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/wifi_mgmt.h>

#define NET_EVENT_WIFI_MASK	(NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT)

/* STA Mode Configuration */
#define WIFI_SSID "arun"     /* Replace `SSID` with WiFi ssid. */
#define WIFI_PSK  "12345678" /* Replace `PASSWORD` with Router password. */

LOG_MODULE_REGISTER(test_sta);

static struct net_if *sta_iface;
static struct wifi_connect_req_params sta_config;
static struct net_mgmt_event_callback cb;

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

int siwx91x_wifi_init(void)
{
	net_mgmt_init_event_callback(&cb, wifi_event_handler, NET_EVENT_WIFI_MASK);
	net_mgmt_add_event_callback(&cb);

	sta_iface = net_if_get_wifi_sta();

	return 0;
}

int siwx91x_wifi_operation(void)
{
	int ret;

	ret = siwx91x_device_connect();
	if (ret < 0) {
		return ret;
	}
	k_sem_take(&wlan_sem, K_FOREVER);
	k_sleep(K_SECONDS(1));
disconnect:
	ret = siwx91x_device_disconnect();
	if (ret < 0) {
		printf("Disconnection retry\n");
		goto disconnect;
	}

	return 0;
}
