/*
 * test_net_stats.c - 网络统计纯函数
 */

#include <glib.h>

#include <string.h>

#include "net_stats.h"

static void test_pct(void)
{
    g_assert_cmpfloat_with_epsilon(net_stats_pct(5, 1000), 0.5, 1e-9);
    g_assert_cmpfloat(net_stats_pct(0, 0), ==, 0.0);
    g_assert_cmpfloat(net_stats_pct(2000, 1000), ==, 100.0);
}

static void test_ifconfig_parse(void)
{
    static const char SAMPLE[] =
        "wlan0: flags=8843<UP,BROADCAST,RUNNING,SIMPLEX,MULTICAST> metric 0\n"
        "\tssid HomeNet channel 6 (2437 MHz 11g) bssid aa:bb:cc:dd:ee:ff\n"
        "\tmedia: IEEE 802.11 Wireless Ethernet OFDM/54Mbps mode 11g\n"
        "re0: flags=8843<UP,BROADCAST,RUNNING> metric 0\n";

    NetStats s;

    memset(&s, 0, sizeof(s));
    g_strlcpy(s.ifs[0].name, "wlan0", sizeof(s.ifs[0].name));
    s.nifs = 1;

    g_assert_cmpint(net_stats_parse_ifconfig(SAMPLE, &s), ==, 1);
    g_assert_cmpstr(s.ifs[0].ssid, ==, "HomeNet");
    g_assert_cmpint(s.ifs[0].channel, ==, 6);
    g_assert_nonnull(strstr(s.ifs[0].media, "802.11"));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/net_stats/pct", test_pct);
    g_test_add_func("/net_stats/ifconfig", test_ifconfig_parse);
    return g_test_run();
}
