/*
 * ipc_sim.h  —  Simulated inter-core IPC bus using Win32 events
 *
 * Two sides:  SIM_SIDE_MASTER = 0,  SIM_SIDE_REMOTE = 1
 *
 * When side A calls platform_notify(vq_id):
 *   - vq_id is even  → notify REMOTE  (Master just wrote to tvq)
 *   - vq_id is odd   → notify MASTER  (Remote just wrote to tvq)
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIM_SIDE_MASTER (0)
#define SIM_SIDE_REMOTE (1)
#define SIM_NUM_SIDES   (2)

/* Initialise / teardown the IPC bus (call once before starting threads) */
void ipc_sim_init(void);
void ipc_sim_deinit(void);

/* Signal the other side — caller specifies its own side, target is opposite */
void ipc_sim_signal_from(uint32_t source_side, uint32_t vq_id);

/*
 * Block until a notification arrives for this side, then return the vq_id.
 * Called from the interrupt-dispatcher thread on each side.
 * timeout_ms == 0  → infinite wait
 */
int32_t ipc_sim_wait(uint32_t side, uint32_t *vq_id_out, uint32_t timeout_ms);

/* Register the rpmsg_lite instance pointer for a side so the ISR can call
 * virtqueue_notification() on the correct virtqueue.                        */
void ipc_sim_register_side(uint32_t side, void *rpmsg_instance);
void *ipc_sim_get_instance(uint32_t side);

#ifdef __cplusplus
}
#endif
