/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "adt.h"
#include "assert.h"
#include "malloc.h"
#include "smc.h"
#include "string.h"
#include "types.h"
#include "utils.h"

#define SMC_READ_KEY         0x10
#define SMC_WRITE_KEY        0x11
#define SMC_GET_KEY_BY_INDEX 0x12
#define SMC_GET_KEY_INFO     0x13
#define SMC_INITIALIZE       0x17
#define SMC_NOTIFICATION     0x18
#define SMC_RW_KEY           0x20

#define SMC_MSG_TYPE GENMASK(7, 0)
#define SMC_MSG_ID   GENMASK(15, 12)

#define SMC_WRITE_KEY_SIZE GENMASK(23, 16)
#define SMC_WRITE_KEY_KEY  GENMASK(63, 32)

#define SMC_RESULT_RESULT GENMASK(7, 0)
#define SMC_RESULT_ID     GENMASK(15, 12)
#define SMC_RESULT_SIZE   GENMASK(31, 16)
#define SMC_RESULT_VALUE  GENMASK(63, 32)

#define SMC_NUM_IDS 16

#define SMC_ENDPOINT 0x20

/* How long an answer may take: SMC_TIMEOUT_MS of Linux, drivers/mfd/macsmc.c */
#define SMC_TIMEOUT_USEC 500000
/* How big the shared memory is: SMC_SHMEM_SIZE of the same file */
#define SMC_SHMEM_SIZE 0x1000

struct smc_dev {
    asc_dev_t *asc;
    rtkit_dev_t *rtkit;

    void *shmem;
    u32 msgid;
    u32 notifications; /* passed over, and not said yet */

    bool outstanding[SMC_NUM_IDS];
    u64 ret[SMC_NUM_IDS];
};

static void smc_handle_msg(smc_dev_t *smc, u64 msg)
{
    u8 result = FIELD_GET(SMC_RESULT_RESULT, msg);
    u8 id = FIELD_GET(SMC_RESULT_ID, msg);

    /*
     * A notification can come at any time, also before the answer to SMC_INITIALIZE, which
     * is the address of the shared memory: an SMC that was running before may have them
     * turned on (SiliconNT docs/findings/0067). They are counted (smc_notifications()).
     */
    if (result == SMC_NOTIFICATION) {
        smc->notifications++;
        return;
    }
    if (!smc->shmem) {
        smc->shmem = (void *)msg;
        return;
    }
    smc->outstanding[id] = false;
    smc->ret[id] = msg;
}

/*
 * Notifications passed over are said in one line: printed one by one, an SMC that keeps
 * sending them would push everything else out of the console's buffer (src/iodev.c), which is
 * what is shown when a guest is stopped without a host (src/hv/hv_stop.c)
 */
static void smc_notifications(smc_dev_t *smc)
{
    if (smc->notifications)
        printf("SMC: %u notifications passed over\n", smc->notifications);
    smc->notifications = 0;
}

/*
 * Takes one message; -1 if the SMC is gone, or if the timeout has passed. The time is looked
 * at every round, whatever came in the one before.
 */
static int smc_work(smc_dev_t *smc, u64 timeout)
{
    int ret;
    struct rtkit_message msg;

    do {
        if (timeout_expired(timeout)) {
            printf("SMC: no answer within %d ms\n", SMC_TIMEOUT_USEC / 1000);
            return -1;
        }
    } while ((ret = rtkit_recv(smc->rtkit, &msg)) == 0);

    if (ret < 0) {
        printf("SMC: rtkit_recv failed!\n");
        return ret;
    }

    if (msg.ep != SMC_ENDPOINT) {
        printf("SMC: received message for unexpected endpoint 0x%02x\n", msg.ep);
        return 0;
    }

    smc_handle_msg(smc, msg.msg);

    return 0;
}

static void smc_send(smc_dev_t *smc, u64 message)
{
    struct rtkit_message msg;

    msg.ep = SMC_ENDPOINT;
    msg.msg = message;

    rtkit_send(smc->rtkit, &msg);
}

static int smc_cmd(smc_dev_t *smc, u64 message)
{
    u8 id = smc->msgid++ & 0xF;
    assert(!smc->outstanding[id]);
    smc->outstanding[id] = true;

    message |= FIELD_PREP(SMC_MSG_ID, id);

    u64 timeout = timeout_calculate(SMC_TIMEOUT_USEC);
    smc_send(smc, message);
    while (smc->outstanding[id])
        if (smc_work(smc, timeout) < 0) {
            smc->outstanding[id] = false;
            smc_notifications(smc);
            return -1;
        }
    smc_notifications(smc);

    u64 result = smc->ret[id];
    u32 ret = FIELD_GET(SMC_RESULT_RESULT, result);
    if (ret) {
        printf("SMC: smc_cmd[0x%x] failed: %u\n", id, ret);
        return ret;
    }

    return 0;
}

/*
 * Whether the shared memory lies in the SMC's own memory, region-base and region-size of the
 * node of its coprocessor (iop-smc-nub), as Linux checks it against the SMC's SRAM
 * (drivers/mfd/macsmc.c, apple_smc_rtkit_shmem_setup()). An answer that points anywhere else,
 * a register of some device for one, is not written to (SiliconNT docs/findings/0069).
 */
static bool smc_shmem_ok(smc_dev_t *smc)
{
    int node = asc_get_iop_node(smc->asc);
    const void *pbase, *psize;
    u32 lbase = 0, lsize = 0;
    u64 base, size, shmem = (u64)smc->shmem;

    pbase = node < 0 ? NULL : adt_getprop(adt, node, "region-base", &lbase);
    psize = node < 0 ? NULL : adt_getprop(adt, node, "region-size", &lsize);
    if (!pbase || !psize || lbase != sizeof(base) || lsize != sizeof(size)) {
        printf("SMC: no region-base and region-size of 64 bits for its memory\n");
        return false;
    }
    memcpy(&base, pbase, sizeof(base));
    memcpy(&size, psize, sizeof(size));
    if (size < SMC_SHMEM_SIZE || shmem < base || shmem - base > size - SMC_SHMEM_SIZE) {
        printf("SMC: the shared memory it gives, 0x%lx, is not in its memory 0x%lx .. 0x%lx: "
               "not written to\n",
               shmem, base, base + size);
        return false;
    }
    return true;
}

void smc_shutdown(smc_dev_t *smc)
{
    rtkit_quiesce(smc->rtkit);
    rtkit_free(smc->rtkit);
    asc_free(smc->asc);
    free(smc);
}

smc_dev_t *smc_init(void)
{
    smc_dev_t *smc = calloc(1, sizeof(smc_dev_t));
    if (!smc)
        return NULL;

    smc->asc = asc_init("/arm-io/smc");
    if (!smc->asc) {
        printf("SMC: failed to initialize ASC\n");
        goto out_free;
    }

    smc->rtkit = rtkit_init("smc", smc->asc, NULL, NULL, NULL, true);
    if (!smc->rtkit) {
        printf("SMC: failed to initialize RTKit\n");
        goto out_asc;
    }

    if (!rtkit_boot(smc->rtkit)) {
        printf("SMC: failed to boot RTKit\n");
        goto out_rtkit;
    }

    if (!rtkit_start_ep(smc->rtkit, SMC_ENDPOINT)) {
        printf("SMC: failed start SMC endpoint\n");
        goto out_rtkit;
    }

    u64 initialize =
        FIELD_PREP(SMC_MSG_TYPE, SMC_INITIALIZE) | FIELD_PREP(SMC_MSG_ID, smc->msgid++);

    u64 timeout = timeout_calculate(SMC_TIMEOUT_USEC);
    smc_send(smc, initialize);

    while (!smc->shmem) {
        int ret = smc_work(smc, timeout);
        if (ret < 0) {
            smc_notifications(smc);
            goto out_rtkit;
        }
    }
    smc_notifications(smc);
    if (!smc_shmem_ok(smc))
        goto out_rtkit;

    return smc;

out_rtkit:
    rtkit_free(smc->rtkit);
out_asc:
    asc_free(smc->asc);
out_free:
    free(smc);
    return NULL;
}

int smc_write_u32(smc_dev_t *smc, u32 key, u32 value)
{
    memcpy(smc->shmem, &value, sizeof(value));
    u64 msg = FIELD_PREP(SMC_MSG_TYPE, SMC_WRITE_KEY);
    msg |= FIELD_PREP(SMC_WRITE_KEY_SIZE, sizeof(value));
    msg |= FIELD_PREP(SMC_WRITE_KEY_KEY, key);

    return smc_cmd(smc, msg);
}
