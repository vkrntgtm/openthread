/* Copyright (c) 2017, Nordic Semiconductor ASA
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   1. Redistributions of source code must retain the above copyright notice, this
 *      list of conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright notice,
 *      this list of conditions and the following disclaimer in the documentation
 *      and/or other materials provided with the distribution.
 *
 *   3. Neither the name of Nordic Semiconductor ASA nor the names of its
 *      contributors may be used to endorse or promote products derived from
 *      this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/**
 * @file
 *   This file implements SWI manager for nRF 802.15.4 driver.
 *
 */

#include "nrf_drv_radio802154_swi.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "nrf_drv_radio802154.h"
#include "nrf_drv_radio802154_config.h"
#include "nrf_drv_radio802154_critical_section.h"
#include "nrf_drv_radio802154_fsm.h"
#include "nrf_drv_radio802154_rx_buffer.h"
#include "hal/nrf_egu.h"
#include "raal/nrf_raal_api.h"

/** Size of notification queue.
 *
 * One slot for each receive buffer, one for transmission, one for busy channel and one for energy
 * detection.
 */
#define NTF_QUEUE_SIZE (RADIO_RX_BUFFERS + 3)
/** Size of requests queue.
 *
 * Two is minimal queue size. It is not expected in current implementation to queue a few requests.
 */
#define REQ_QUEUE_SIZE 2

#define SWI_EGU        NRF_EGU3                       ///< Label of SWI peripheral.
#define SWI_IRQn       SWI3_EGU3_IRQn                 ///< Symbol of SWI IRQ number.
#define SWI_IRQHandler SWI3_EGU3_IRQHandler           ///< Symbol of SWI IRQ handler.

#define NTF_INT   NRF_EGU_INT_TRIGGERED0              ///< Label of notification interrupt.
#define NTF_TASK  NRF_EGU_TASK_TRIGGER0               ///< Label of notification task.
#define NTF_EVENT NRF_EGU_EVENT_TRIGGERED0            ///< Label of notification event.

#define TIMESLOT_EXIT_INT   NRF_EGU_INT_TRIGGERED1    ///< Label of timeslot exit interrupt.
#define TIMESLOT_EXIT_TASK  NRF_EGU_TASK_TRIGGER1     ///< Label of timeslot exit task.
#define TIMESLOT_EXIT_EVENT NRF_EGU_EVENT_TRIGGERED1  ///< Label of timeslot exit event.

#define REQ_INT   NRF_EGU_INT_TRIGGERED2              ///< Label of request interrupt.
#define REQ_TASK  NRF_EGU_TASK_TRIGGER2               ///< Label of request task.
#define REQ_EVENT NRF_EGU_EVENT_TRIGGERED2            ///< Label of request event.

/// Types of notifications in notification queue.
typedef enum
{
    NTF_TYPE_RECEIVED,         ///< Frame received
    NTF_TYPE_TRANSMITTED,      ///< Frame transmitted
    NTF_TYPE_CHANNEL_BUSY,     ///< Frame transmission failure
    NTF_TYPE_ENERGY_DETECTED,  ///< Energy detection procedure ended
} nrf_drv_radio802154_ntf_type_t;

/// Notification data in the notification queue.
typedef struct
{
    nrf_drv_radio802154_ntf_type_t type;  ///< Notification type.
    union
    {
        struct
        {
            uint8_t * p_psdu;             ///< Pointer to received frame PSDU.
            int8_t    power;              ///< RSSI of received frame.
            int8_t    lqi;                ///< LQI of received frame.
        } received;                       ///< Received frame details.

        struct
        {
            uint8_t * p_psdu;             ///< Pointer to received ACK PSDU or NULL.
            int8_t    power;              ///< RSSI of received ACK or 0.
            int8_t    lqi;                ///< LQI of received ACK or 0.
        } transmitted;                    ///< Transmitted frame details.

        struct
        {
            int8_t    result;             ///< Energy detection result.
        } energy_detected;                ///< Energy detection details.
    } data;                               ///< Notification data depending on it's type.
} nrf_drv_radio802154_ntf_data_t;

/// Type of requests in request queue.
typedef enum
{
    REQ_TYPE_SLEEP,
    REQ_TYPE_RECEIVE,
    REQ_TYPE_TRANSMIT,
    REQ_TYPE_ENERGY_DETECTION,
    REQ_TYPE_BUFFER_FREE,
} nrf_drv_radio802154_req_type_t;

/// Request data in request queue.
typedef struct
{
    nrf_drv_radio802154_req_type_t type;  ///< Type of the request.
    union
    {
        struct
        {
            bool * p_result;              ///< Sleep request result.
        } sleep;                          ///< Sleep request details.

        struct
        {
            bool  * p_result;             ///< Receive request result.
            uint8_t channel;              ///< Channel to receive on.
        } receive;                        ///< Receive request details.

        struct
        {
            bool          * p_result;     ///< Transmit request result.
            const uint8_t * p_data;       ///< Pointer to PSDU to transmit.
            uint8_t         channel;      ///< Channel to transmit on.
            int8_t          power;        ///< Requested transmission power.
            bool            cca;          ///< If CCA was requested prior to transmission.
        } transmit;                       ///< Transmit request details.

        struct
        {
            bool   * p_result;            ///< Energy detection request result.
            uint8_t  channel;             ///< Channel to perform energy detection on.
            uint32_t time_us;             ///< Requested time of energy detection procedure.
        } energy_detection;               ///< Energy detection request details.

        struct
        {
            rx_buffer_t * p_data;         ///< Pointer to receive buffer to free.
        } buffer_free;                    ///< Buffer free request details.
    } data;                               ///< Request data depending on it's type.
} nrf_drv_radio802154_req_data_t;

static nrf_drv_radio802154_ntf_data_t m_ntf_queue[NTF_QUEUE_SIZE];  ///< Notification queue.
static uint8_t                        m_ntf_r_ptr;  ///< Notification queue read index.
static uint8_t                        m_ntf_w_ptr;  ///< Notification queue write index.

static nrf_drv_radio802154_req_data_t m_req_queue[REQ_QUEUE_SIZE];  ///< Request queue.
static uint8_t                        m_req_r_ptr;  // Request queue read index.
static uint8_t                        m_req_w_ptr;  // Request queue write index.

/**
 * Increment given index for any queue.
 *
 * @param[inout]  p_ptr       Index to increment.
 * @param[in]     queue_size  Number of elements in the queue.
 */
static void queue_ptr_increment(uint8_t * p_ptr, uint8_t queue_size)
{
    if (++(*p_ptr) >= queue_size)
    {
        *p_ptr = 0;
    }
}

/**
 * Check if given queue is full.
 *
 * @param[in]  r_ptr       Read index associated with given queue.
 * @param[in]  w_ptr       Write index associated with given queue.
 * @param[in]  queue_size  Number of elements in the queue.
 *
 * @retval  true   Given queue is full.
 * @retval  false  Given queue is not full.
 */
static bool queue_is_full(uint8_t r_ptr, uint8_t w_ptr, uint8_t queue_size)
{
    if (w_ptr == (r_ptr - 1))
    {
        return true;
    }

    if ((r_ptr == 0) && (w_ptr == queue_size - 1))
    {
        return true;
    }

    return false;
}

/**
 * Check if given queue is empty.
 *
 * @param[in]  r_ptr  Read index associated with given queue.
 * @param[in]  w_ptr  Write index associated with given queue.
 *
 * @retval  true   Given queue is empty.
 * @retval  false  Given queue is not empty.
 */
static bool queue_is_empty(uint8_t r_ptr, uint8_t w_ptr)
{
    return (r_ptr == w_ptr);
}

/**
 * Increment given index associated with notification queue.
 *
 * @param[inout]  p_ptr  Pointer to the index to increment.
 */
static void ntf_queue_ptr_increment(uint8_t * p_ptr)
{
    queue_ptr_increment(p_ptr, NTF_QUEUE_SIZE);
}

/**
 * Check if notification queue is full.
 *
 * @retval  true   Notification queue is full.
 * @retval  false  Notification queue is not full.
 */
static bool ntf_queue_is_full(void)
{
    return queue_is_full(m_ntf_r_ptr, m_ntf_w_ptr, NTF_QUEUE_SIZE);
}

/**
 * Check if notification queue is empty.
 *
 * @retval  true   Notification queue is empty.
 * @retval  false  Notification queue is not empty.
 */
static bool ntf_queue_is_empty(void)
{
    return queue_is_empty(m_ntf_r_ptr, m_ntf_w_ptr);
}

/**
 * Increment given index associated with request queue.
 *
 * @param[inout]  p_ptr  Pointer to the index to increment.
 */
static void req_queue_ptr_increment(uint8_t * p_ptr)
{
    queue_ptr_increment(p_ptr, REQ_QUEUE_SIZE);
}

/**
 * Check if request queue is full.
 *
 * @retval  true   Request queue is full.
 * @retval  false  Request queue is not full.
 */
static bool req_queue_is_full(void)
{
    return queue_is_full(m_req_r_ptr, m_req_w_ptr, REQ_QUEUE_SIZE);
}

/**
 * Check if request queue is empty.
 *
 * @retval  true   Request queue is empty.
 * @retval  false  Request queue is not empty.
 */
static bool req_queue_is_empty(void)
{
    return queue_is_empty(m_req_r_ptr, m_req_w_ptr);
}

void nrf_drv_radio802154_swi_init(void)
{
    m_ntf_r_ptr = 0;
    m_ntf_w_ptr = 0;

    nrf_egu_int_enable(SWI_EGU,
                       NTF_INT |
                       TIMESLOT_EXIT_INT |
                       REQ_INT);

    NVIC_SetPriority(SWI_IRQn, RADIO_NOTIFICATION_SWI_PRIORITY);
    NVIC_ClearPendingIRQ(SWI_IRQn);
    NVIC_EnableIRQ(SWI_IRQn);
}

void nrf_drv_radio802154_swi_notify_received(uint8_t * p_data, int8_t power, int8_t lqi)
{
    assert(!ntf_queue_is_full());

    nrf_drv_radio802154_ntf_data_t * p_slot = &m_ntf_queue[m_ntf_w_ptr];

    p_slot->type                 = NTF_TYPE_RECEIVED;
    p_slot->data.received.p_psdu = p_data;
    p_slot->data.received.power  = power;
    p_slot->data.received.lqi    = lqi;

    ntf_queue_ptr_increment(&m_ntf_w_ptr);

    nrf_egu_task_trigger(SWI_EGU, NTF_TASK);
}

void nrf_drv_radio802154_swi_notify_transmitted(uint8_t * p_data, int8_t power, int8_t lqi)
{
    assert(!ntf_queue_is_full());

    nrf_drv_radio802154_ntf_data_t * p_slot = &m_ntf_queue[m_ntf_w_ptr];

    p_slot->type                    = NTF_TYPE_TRANSMITTED;
    p_slot->data.transmitted.p_psdu = p_data;
    p_slot->data.transmitted.power  = power;
    p_slot->data.transmitted.lqi    = lqi;

    ntf_queue_ptr_increment(&m_ntf_w_ptr);

    nrf_egu_task_trigger(SWI_EGU, NTF_TASK);
}

void nrf_drv_radio802154_swi_notify_busy_channel(void)
{
    assert(!ntf_queue_is_full());

    nrf_drv_radio802154_ntf_data_t * p_slot = &m_ntf_queue[m_ntf_w_ptr];

    p_slot->type = NTF_TYPE_CHANNEL_BUSY;

    ntf_queue_ptr_increment(&m_ntf_w_ptr);

    nrf_egu_task_trigger(SWI_EGU, NTF_TASK);
}

void nrf_drv_radio802154_swi_notify_energy_detected(int8_t result)
{
    assert(!ntf_queue_is_full());

    nrf_drv_radio802154_ntf_data_t * p_slot = &m_ntf_queue[m_ntf_w_ptr];

    p_slot->type                        = NTF_TYPE_ENERGY_DETECTED;
    p_slot->data.energy_detected.result = result;

    ntf_queue_ptr_increment(&m_ntf_w_ptr);

    nrf_egu_task_trigger(SWI_EGU, NTF_TASK);
}

void nrf_drv_radio802154_swi_timeslot_exit(void)
{
    assert(!nrf_egu_event_check(SWI_EGU, TIMESLOT_EXIT_EVENT));

    nrf_egu_task_trigger(SWI_EGU, TIMESLOT_EXIT_TASK);
}

void nrf_drv_radio802154_swi_sleep(bool * p_result)
{
    nrf_drv_radio802154_critical_section_enter();
    assert(!req_queue_is_full());

    nrf_drv_radio802154_req_data_t * p_slot = &m_req_queue[m_req_w_ptr];

    p_slot->type                = REQ_TYPE_SLEEP;
    p_slot->data.sleep.p_result = p_result;

    req_queue_ptr_increment(&m_req_w_ptr);

    nrf_egu_task_trigger(SWI_EGU, REQ_TASK);
    nrf_drv_radio802154_critical_section_exit();
}

void nrf_drv_radio802154_swi_receive(uint8_t channel, bool * p_result)
{
    nrf_drv_radio802154_critical_section_enter();
    assert(!req_queue_is_full());

    nrf_drv_radio802154_req_data_t * p_slot = &m_req_queue[m_req_w_ptr];

    p_slot->type                  = REQ_TYPE_RECEIVE;
    p_slot->data.receive.channel  = channel;
    p_slot->data.receive.p_result = p_result;

    req_queue_ptr_increment(&m_req_w_ptr);

    nrf_egu_task_trigger(SWI_EGU, REQ_TASK);
    nrf_drv_radio802154_critical_section_exit();
}

void nrf_drv_radio802154_swi_transmit(const uint8_t * p_data,
                                      uint8_t         channel,
                                      int8_t          power,
                                      bool            cca,
                                      bool          * p_result)
{
    nrf_drv_radio802154_critical_section_enter();
    assert(!req_queue_is_full());

    nrf_drv_radio802154_req_data_t * p_slot = &m_req_queue[m_req_w_ptr];

    p_slot->type                   = REQ_TYPE_TRANSMIT;
    p_slot->data.transmit.p_data   = p_data;
    p_slot->data.transmit.channel  = channel;
    p_slot->data.transmit.power    = power;
    p_slot->data.transmit.cca      = cca;
    p_slot->data.transmit.p_result = p_result;

    req_queue_ptr_increment(&m_req_w_ptr);

    nrf_egu_task_trigger(SWI_EGU, REQ_TASK);
    nrf_drv_radio802154_critical_section_exit();
}

void nrf_drv_radio802154_swi_energy_detection(uint8_t channel, uint32_t time_us, bool * p_result)
{
    nrf_drv_radio802154_critical_section_enter();
    assert(!req_queue_is_full());

    nrf_drv_radio802154_req_data_t * p_slot = &m_req_queue[m_req_w_ptr];

    p_slot->type                           = REQ_TYPE_ENERGY_DETECTION;
    p_slot->data.energy_detection.channel  = channel;
    p_slot->data.energy_detection.time_us  = time_us;
    p_slot->data.energy_detection.p_result = p_result;

    req_queue_ptr_increment(&m_req_w_ptr);

    nrf_egu_task_trigger(SWI_EGU, REQ_TASK);
    nrf_drv_radio802154_critical_section_exit();
}

void nrf_drv_radio802154_swi_buffer_free(uint8_t * p_data)
{
    nrf_drv_radio802154_critical_section_enter();
    assert(!req_queue_is_full());

    nrf_drv_radio802154_req_data_t * p_slot = &m_req_queue[m_req_w_ptr];

    p_slot->type                    = REQ_TYPE_BUFFER_FREE;
    p_slot->data.buffer_free.p_data = (rx_buffer_t *)p_data;

    req_queue_ptr_increment(&m_req_w_ptr);

    nrf_egu_task_trigger(SWI_EGU, REQ_TASK);
    nrf_drv_radio802154_critical_section_exit();
}

void SWI_IRQHandler(void)
{
    if (nrf_egu_event_check(SWI_EGU, NTF_EVENT))
    {
        nrf_egu_event_clear(SWI_EGU, NTF_EVENT);

        while (!ntf_queue_is_empty())
        {
            nrf_drv_radio802154_ntf_data_t * p_slot = &m_ntf_queue[m_ntf_r_ptr];

            switch (p_slot->type)
            {
                case NTF_TYPE_RECEIVED:
                    nrf_drv_radio802154_received(p_slot->data.received.p_psdu,
                                                 p_slot->data.received.power,
                                                 p_slot->data.received.lqi);
                    break;

                case NTF_TYPE_TRANSMITTED:
                    nrf_drv_radio802154_transmitted(p_slot->data.transmitted.p_psdu,
                                                    p_slot->data.transmitted.power,
                                                    p_slot->data.transmitted.lqi);
                    break;

                case NTF_TYPE_CHANNEL_BUSY:
                    nrf_drv_radio802154_busy_channel();
                    break;

                case NTF_TYPE_ENERGY_DETECTED:
                    nrf_drv_radio802154_energy_detected(p_slot->data.energy_detected.result);
                    break;

                default:
                    assert(false);
            }

            ntf_queue_ptr_increment(&m_ntf_r_ptr);
        }
    }

    if (nrf_egu_event_check(SWI_EGU, TIMESLOT_EXIT_EVENT))
    {
        nrf_raal_continuous_mode_exit();

        nrf_egu_event_clear(SWI_EGU, TIMESLOT_EXIT_EVENT);
    }

    if (nrf_egu_event_check(SWI_EGU, REQ_EVENT))
    {
        nrf_egu_event_clear(SWI_EGU, REQ_EVENT);

        while (!req_queue_is_empty())
        {
            nrf_drv_radio802154_req_data_t * p_slot = &m_req_queue[m_req_r_ptr];

            switch (p_slot->type)
            {
                case REQ_TYPE_SLEEP:
                    *(p_slot->data.sleep.p_result) = nrf_drv_radio802154_fsm_sleep();
                    break;

                case REQ_TYPE_RECEIVE:
                    *(p_slot->data.receive.p_result) = nrf_drv_radio802154_fsm_receive(
                            p_slot->data.receive.channel);
                    break;

                case REQ_TYPE_TRANSMIT:
                    *(p_slot->data.transmit.p_result) = nrf_drv_radio802154_fsm_transmit(
                            p_slot->data.transmit.p_data,
                            p_slot->data.transmit.channel,
                            p_slot->data.transmit.power,
                            p_slot->data.transmit.cca);
                    break;

                case REQ_TYPE_ENERGY_DETECTION:
                    *(p_slot->data.energy_detection.p_result) =
                            nrf_drv_radio802154_fsm_energy_detection(
                                    p_slot->data.energy_detection.channel,
                                    p_slot->data.energy_detection.time_us);
                    break;

                case REQ_TYPE_BUFFER_FREE:
                    nrf_drv_radio802154_fsm_notify_buffer_free(p_slot->data.buffer_free.p_data);
                    break;

                default:
                    assert(false);
            }

            req_queue_ptr_increment(&m_req_r_ptr);
        }
    }
}
