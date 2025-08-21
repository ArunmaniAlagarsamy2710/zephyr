#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/wifi_mgmt.h>

#define WIFI_SHELL_SCAN_EVENTS (                   \
				NET_EVENT_WIFI_SCAN_RESULT        |\
				NET_EVENT_WIFI_SCAN_DONE)
LOG_MODULE_REGISTER(test_scan);

static K_SEM_DEFINE(wlan_sem, 0, 1);
static struct net_if *sta_iface;
static struct net_mgmt_event_callback wifi_shell_scan_cb;

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

int siwx91x_wifi_init(void)
{
	net_mgmt_init_event_callback(&wifi_shell_scan_cb,
				     wifi_mgmt_scan_event_handler,
				     WIFI_SHELL_SCAN_EVENTS);

	sta_iface = net_if_get_wifi_sta();

	return 0;
}

int siwx91x_wifi_operation(void)
{
	siwx91x_enable_scan();
	return 0;
}
