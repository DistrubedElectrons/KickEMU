/*
 * qtest for the SD card slot of the kick machine: hot insert and eject
 * through QMP (blockdev-change-medium / eject on drive "sd"), as seen by
 * the guest in uSDHC2 PRES_STATE / INT_STATUS, its interrupt, and the
 * card-detect GPIO1_IO10 (active low) of the device tree.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

#define USDHC2_BASE         0x30b50000
#define PRES_STATE          0x24
#define INT_STATUS          0x30
#define INT_STATUS_EN       0x34
#define INT_SIGNAL_EN       0x38
#define PRES_CINST          (1u << 16)
#define PRES_CDPL           (1u << 18)
#define INT_CINS            (1u << 6)
#define INT_CRM             (1u << 7)

#define GPIO1_BASE          0x30200000
#define GPIO_PSR            0x08
#define GPIO_ICR1           0x0c
#define GPIO_IMR            0x14
#define GPIO_ISR            0x18
#define GPIO_EDGE_SEL       0x1c
#define CD_PIN              10

#define GICD_BASE           0x38800000
#define USDHC2_SPI          23
#define GPIO1_LOW_SPI       64

/* QEMU's SDHCI delays a re-insertion while a removal is still pending */
#define INSERTION_DELAY_NS  NANOSECONDS_PER_SECOND

static bool spi_pending(QTestState *qts, int spi)
{
    int intid = spi + 32;

    return qtest_readl(qts, GICD_BASE + 0x200 + (intid / 32) * 4) &
           (1u << (intid % 32));
}

static uint32_t pres(QTestState *qts)
{
    return qtest_readl(qts, USDHC2_BASE + PRES_STATE) &
           (PRES_CINST | PRES_CDPL);
}

static bool cd_pin(QTestState *qts)
{
    return qtest_readl(qts, GPIO1_BASE + GPIO_PSR) & (1u << CD_PIN);
}

static bool cd_edge(QTestState *qts)
{
    return qtest_readl(qts, GPIO1_BASE + GPIO_ISR) & (1u << CD_PIN);
}

static void clear_events(QTestState *qts)
{
    qtest_writel(qts, USDHC2_BASE + INT_STATUS, INT_CINS | INT_CRM);
    qtest_writel(qts, GPIO1_BASE + GPIO_ISR, 1u << CD_PIN);
    g_assert_false(spi_pending(qts, USDHC2_SPI));
    g_assert_false(spi_pending(qts, GPIO1_LOW_SPI));
}

/* "inserted" of the query-block entry for drive "sd" */
static bool medium_present(QTestState *qts)
{
    QDict *rsp = qtest_qmp(qts, "{'execute': 'query-block'}");
    QList *list = qdict_get_qlist(rsp, "return");
    bool found = false, inserted = false;
    QListEntry *e;

    QLIST_FOREACH_ENTRY(list, e) {
        QDict *blk = qobject_to(QDict, qlist_entry_obj(e));

        if (!strcmp(qdict_get_str(blk, "device"), "sd")) {
            found = true;
            inserted = qdict_haskey(blk, "inserted");
        }
    }
    qobject_unref(rsp);
    g_assert_true(found);
    return inserted;
}

static void insert(QTestState *qts, const char *path)
{
    qtest_qmp_assert_success(qts,
        "{'execute': 'blockdev-change-medium', 'arguments': "
        "{'device': 'sd', 'filename': %s, 'format': 'raw'}}", path);
}

static void eject(QTestState *qts)
{
    qtest_qmp_assert_success(qts,
        "{'execute': 'eject', 'arguments': {'device': 'sd', 'force': true}}");
}

static char *make_card(void)
{
    char *path = NULL;
    int fd = g_file_open_tmp("kick-sd-XXXXXX.img", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    /* SD capacities up to 2 GiB must be a power of 2 */
    g_assert_cmpint(ftruncate(fd, 1 * 1024 * 1024), ==, 0);
    close(fd);
    return path;
}

static QTestState *start_empty_slot(void)
{
    QTestState *qts = qtest_init("-M kick,sd=sd -drive if=none,id=sd");

    /* Card events enabled and signalled, as sdhci-esdhc-imx leaves them */
    qtest_writel(qts, USDHC2_BASE + INT_STATUS_EN,
                 qtest_readl(qts, USDHC2_BASE + INT_STATUS_EN) |
                 INT_CINS | INT_CRM);
    qtest_writel(qts, USDHC2_BASE + INT_SIGNAL_EN, INT_CINS | INT_CRM);
    /* GPIO1_IO10 interrupt on both edges, as the mmc cd-gpio handler asks */
    qtest_writel(qts, GPIO1_BASE + GPIO_EDGE_SEL, 1u << CD_PIN);
    qtest_writel(qts, GPIO1_BASE + GPIO_IMR, 1u << CD_PIN);
    return qts;
}

static void test_empty_slot(void)
{
    QTestState *qts = start_empty_slot();

    g_assert_false(medium_present(qts));
    g_assert_cmphex(pres(qts), ==, 0);
    /* Empty slot: the pad is pulled up, also after the reset at start-up */
    g_assert_true(cd_pin(qts));
    qtest_quit(qts);
}

static void test_insert_eject(void)
{
    QTestState *qts = start_empty_slot();
    g_autofree char *card = make_card();

    clear_events(qts);
    insert(qts, card);
    g_assert_true(medium_present(qts));
    g_assert_cmphex(pres(qts), ==, PRES_CINST | PRES_CDPL);
    g_assert_cmphex(qtest_readl(qts, USDHC2_BASE + INT_STATUS) & INT_CINS, ==,
                    INT_CINS);
    g_assert_true(spi_pending(qts, USDHC2_SPI));
    g_assert_false(cd_pin(qts));
    g_assert_true(cd_edge(qts));
    g_assert_true(spi_pending(qts, GPIO1_LOW_SPI));

    clear_events(qts);
    eject(qts);
    g_assert_false(medium_present(qts));
    g_assert_cmphex(pres(qts), ==, 0);
    g_assert_cmphex(qtest_readl(qts, USDHC2_BASE + INT_STATUS) & INT_CRM, ==,
                    INT_CRM);
    g_assert_true(spi_pending(qts, USDHC2_SPI));
    g_assert_true(cd_pin(qts));
    g_assert_true(cd_edge(qts));
    g_assert_true(spi_pending(qts, GPIO1_LOW_SPI));

    /* A second card after the removal has been acknowledged */
    clear_events(qts);
    insert(qts, card);
    g_assert_cmphex(pres(qts), ==, PRES_CINST | PRES_CDPL);
    g_assert_false(cd_pin(qts));

    qtest_quit(qts);
    unlink(card);
}

/*
 * A card pushed in before the guest acknowledged the removal shows up
 * after the controller's insertion delay, on the pin and in the register
 * at the same moment.
 */
static void test_quick_reinsert(void)
{
    QTestState *qts = start_empty_slot();
    g_autofree char *card = make_card();

    insert(qts, card);
    clear_events(qts);
    eject(qts);
    insert(qts, card);
    g_assert_cmphex(pres(qts), ==, 0);
    g_assert_true(cd_pin(qts));
    qtest_clock_step(qts, INSERTION_DELAY_NS / 2);
    g_assert_cmphex(pres(qts), ==, 0);
    g_assert_true(cd_pin(qts));
    /* The pending removal holds the insertion off: acknowledge it */
    qtest_writel(qts, USDHC2_BASE + INT_STATUS, INT_CRM);
    qtest_clock_step(qts, INSERTION_DELAY_NS);
    g_assert_cmphex(pres(qts), ==, PRES_CINST | PRES_CDPL);
    g_assert_false(cd_pin(qts));
    qtest_quit(qts);
    unlink(card);
}

/* A card present at start-up is visible after the power-on reset */
static void test_card_at_boot(void)
{
    g_autofree char *card = make_card();
    QTestState *qts = qtest_initf("-M kick,sd=sd -drive if=none,id=sd,"
                                  "file=%s,format=raw", card);

    g_assert_true(medium_present(qts));
    g_assert_cmphex(pres(qts), ==, PRES_CINST | PRES_CDPL);
    g_assert_false(cd_pin(qts));
    /* and after a guest-triggered system reset */
    qtest_system_reset(qts);
    g_assert_cmphex(pres(qts), ==, PRES_CINST | PRES_CDPL);
    g_assert_false(cd_pin(qts));
    qtest_quit(qts);
    unlink(card);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/kick/sd/empty-slot", test_empty_slot);
    qtest_add_func("/kick/sd/insert-eject", test_insert_eject);
    qtest_add_func("/kick/sd/quick-reinsert", test_quick_reinsert);
    qtest_add_func("/kick/sd/card-at-boot", test_card_at_boot);

    return g_test_run();
}
