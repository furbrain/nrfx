/**
 * Copyright (c) 2015 - 2017, Nordic Semiconductor ASA
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * 3. Neither the name of Nordic Semiconductor ASA nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY, AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL NORDIC SEMICONDUCTOR ASA OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <nrfx.h>

#if NRFX_CHECK(NRFX_SPIM_ENABLED)

#if !(NRFX_CHECK(NRFX_SPIM0_ENABLED) || NRFX_CHECK(NRFX_SPIM1_ENABLED) || \
      NRFX_CHECK(NRFX_SPIM2_ENABLED))
#error "No enabled SPI instances. Check <nrfx_config.h>."
#endif

#include <nrfx_spim.h>
#include "prs/nrfx_prs.h"
#include <hal/nrf_gpio.h>

#define NRFX_LOG_MODULE SPIM
#include <nrfx_log.h>

// Control block - driver instance local data.
typedef struct
{
    nrfx_spim_evt_handler_t handler;
    void *                  p_context;
    nrfx_spim_evt_t         evt;  // Keep the struct that is ready for event handler. Less memcpy.
    nrfx_drv_state_t        state;
    volatile bool           transfer_in_progress;

    // [no need for 'volatile' attribute for the following members, as they
    //  are not concurrently used in IRQ handlers and main line code]
    uint8_t         ss_pin;
    uint8_t         orc;
    uint8_t         bytes_transferred;

#if NRFX_CHECK(NRFX_SPIM_NRF52_ANOMALY_109_WORKAROUND_ENABLED)
    uint8_t         tx_length;
    uint8_t         rx_length;
#endif
} spim_control_block_t;
static spim_control_block_t m_cb[NRFX_SPIM_ENABLED_COUNT];


nrfx_err_t nrfx_spim_init(nrfx_spim_t  const * const p_instance,
                          nrfx_spim_config_t const * p_config,
                          nrfx_spim_evt_handler_t    handler,
                          void                     * p_context)
{
    NRFX_ASSERT(p_config);
    spim_control_block_t * p_cb = &m_cb[p_instance->drv_inst_idx];
    nrfx_err_t err_code;

    if (p_cb->state != NRFX_DRV_STATE_UNINITIALIZED)
    {
        err_code = NRFX_ERROR_INVALID_STATE;
        NRFX_LOG_WARNING("Function: %s, error code: %s.",
                         __func__,
                         NRFX_LOG_ERROR_STRING_GET(err_code));
        return err_code;
    }

#if NRFX_CHECK(NRFX_PRS_ENABLED)
    static nrfx_irq_handler_t const irq_handlers[NRFX_SPIM_ENABLED_COUNT] = {
        #if NRFX_CHECK(NRFX_SPIM0_ENABLED)
        nrfx_spim_0_irq_handler,
        #endif
        #if NRFX_CHECK(NRFX_SPIM1_ENABLED)
        nrfx_spim_1_irq_handler,
        #endif
        #if NRFX_CHECK(NRFX_SPIM2_ENABLED)
        nrfx_spim_2_irq_handler,
        #endif
    };
    if (nrfx_prs_acquire(p_instance->p_reg,
            irq_handlers[p_instance->drv_inst_idx]) != NRFX_SUCCESS)
    {
        err_code = NRFX_ERROR_BUSY;
        NRFX_LOG_WARNING("Function: %s, error code: %s.",
                         __func__,
                         NRFX_LOG_ERROR_STRING_GET(err_code));
        return err_code;
    }
#endif // NRFX_CHECK(NRFX_PRS_ENABLED)

    p_cb->handler = handler;
    p_cb->p_context = p_context;

    uint32_t mosi_pin;
    uint32_t miso_pin;
    // Configure pins used by the peripheral:
    // - SCK - output with initial value corresponding with the SPI mode used:
    //   0 - for modes 0 and 1 (CPOL = 0), 1 - for modes 2 and 3 (CPOL = 1);
    //   according to the reference manual guidelines this pin and its input
    //   buffer must always be connected for the SPI to work.
    if (p_config->mode <= NRFX_SPIM_MODE_1)
    {
        nrf_gpio_pin_clear(p_config->sck_pin);
    }
    else
    {
        nrf_gpio_pin_set(p_config->sck_pin);
    }
    nrf_gpio_cfg(p_config->sck_pin,
                 NRF_GPIO_PIN_DIR_OUTPUT,
                 NRF_GPIO_PIN_INPUT_CONNECT,
                 NRF_GPIO_PIN_NOPULL,
                 NRF_GPIO_PIN_S0S1,
                 NRF_GPIO_PIN_NOSENSE);
    // - MOSI (optional) - output with initial value 0,
    if (p_config->mosi_pin != NRFX_SPIM_PIN_NOT_USED)
    {
        mosi_pin = p_config->mosi_pin;
        nrf_gpio_pin_clear(mosi_pin);
        nrf_gpio_cfg_output(mosi_pin);
    }
    else
    {
        mosi_pin = NRF_SPIM_PIN_NOT_CONNECTED;
    }
    // - MISO (optional) - input,
    if (p_config->miso_pin != NRFX_SPIM_PIN_NOT_USED)
    {
        miso_pin = p_config->miso_pin;
        nrf_gpio_cfg_input(miso_pin, NRF_GPIO_PIN_NOPULL);
    }
    else
    {
        miso_pin = NRF_SPIM_PIN_NOT_CONNECTED;
    }
    // - Slave Select (optional) - output with initial value 1 (inactive).
    if (p_config->ss_pin != NRFX_SPIM_PIN_NOT_USED)
    {
        nrf_gpio_pin_set(p_config->ss_pin);
        nrf_gpio_cfg_output(p_config->ss_pin);
    }
    m_cb[p_instance->drv_inst_idx].ss_pin = p_config->ss_pin;

    NRF_SPIM_Type * p_spim = (NRF_SPIM_Type *)p_instance->p_reg;
    nrf_spim_pins_set(p_spim, p_config->sck_pin, mosi_pin, miso_pin);
    nrf_spim_frequency_set(p_spim,
        (nrf_spim_frequency_t)p_config->frequency);
    nrf_spim_configure(p_spim,
        (nrf_spim_mode_t)p_config->mode,
        (nrf_spim_bit_order_t)p_config->bit_order);

    nrf_spim_orc_set(p_spim, p_config->orc);

    if (p_cb->handler)
    {
        nrf_spim_int_enable(p_spim, NRF_SPIM_INT_END_MASK);
    }

    nrf_spim_enable(p_spim);

    if (p_cb->handler)
    {
        NRFX_IRQ_PRIORITY_SET(nrfx_get_irq_number(p_instance->p_reg),
            p_config->irq_priority);
        NRFX_IRQ_ENABLE(nrfx_get_irq_number(p_instance->p_reg));
    }

    p_cb->transfer_in_progress = false;
    p_cb->state = NRFX_DRV_STATE_INITIALIZED;

    err_code = NRFX_SUCCESS;
    NRFX_LOG_INFO("Function: %s, error code: %s.", __func__, NRFX_LOG_ERROR_STRING_GET(err_code));
    return err_code;
}

void nrfx_spim_uninit(nrfx_spim_t const * const p_instance)
{
    spim_control_block_t * p_cb = &m_cb[p_instance->drv_inst_idx];
    NRFX_ASSERT(p_cb->state != NRFX_DRV_STATE_UNINITIALIZED);

    if (p_cb->handler)
    {
        NRFX_IRQ_DISABLE(nrfx_get_irq_number(p_instance->p_reg));
    }

    NRF_SPIM_Type * p_spim = (NRF_SPIM_Type *)p_instance->p_reg;
    if (p_cb->handler)
    {
        nrf_spim_int_disable(p_spim, NRF_SPIM_ALL_INTS_MASK);
        if (p_cb->transfer_in_progress)
        {
            // Ensure that SPI is not performing any transfer.
            nrf_spim_task_trigger(p_spim, NRF_SPIM_TASK_STOP);
            while (!nrf_spim_event_check(p_spim, NRF_SPIM_EVENT_STOPPED))
            {}
            p_cb->transfer_in_progress = false;
        }
    }
    nrf_spim_disable(p_spim);

#if NRFX_CHECK(NRFX_PRS_ENABLED)
    nrfx_prs_release(p_instance->p_reg);
#endif

    p_cb->state = NRFX_DRV_STATE_UNINITIALIZED;
}

nrfx_err_t nrfx_spim_transfer(nrfx_spim_t const * const p_instance,
                              uint8_t           const * p_tx_buffer,
                              uint8_t                   tx_buffer_length,
                              uint8_t                 * p_rx_buffer,
                              uint8_t                   rx_buffer_length)
{
    nrfx_spim_xfer_desc_t xfer_desc;
    xfer_desc.p_tx_buffer = p_tx_buffer;
    xfer_desc.p_rx_buffer = p_rx_buffer;
    xfer_desc.tx_length   = tx_buffer_length;
    xfer_desc.rx_length   = rx_buffer_length;
    return nrfx_spim_xfer(p_instance, &xfer_desc, 0);
}

static void finish_transfer(spim_control_block_t * p_cb)
{
    // If Slave Select signal is used, this is the time to deactivate it.
    if (p_cb->ss_pin != NRFX_SPIM_PIN_NOT_USED)
    {
        nrf_gpio_pin_set(p_cb->ss_pin);
    }

    // By clearing this flag before calling the handler we allow subsequent
    // transfers to be started directly from the handler function.
    p_cb->transfer_in_progress = false;

    p_cb->evt.type = NRFX_SPIM_EVENT_DONE;
    p_cb->handler(&p_cb->evt, p_cb->p_context);
}

__STATIC_INLINE void spim_int_enable(NRF_SPIM_Type * p_spim, bool enable)
{
    if (!enable)
    {
        nrf_spim_int_disable(p_spim, NRF_SPIM_INT_END_MASK);
    }
    else
    {
        nrf_spim_int_enable(p_spim, NRF_SPIM_INT_END_MASK);
    }
}

__STATIC_INLINE void spim_list_enable_handle(NRF_SPIM_Type * p_spim, uint32_t flags)
{
    if (NRFX_SPIM_FLAG_TX_POSTINC & flags)
    {
        nrf_spim_tx_list_enable(p_spim);
    }
    else
    {
        nrf_spim_tx_list_disable(p_spim);
    }

    if (NRFX_SPIM_FLAG_RX_POSTINC & flags)
    {
        nrf_spim_rx_list_enable(p_spim);
    }
    else
    {
        nrf_spim_rx_list_disable(p_spim);
    }
}

static nrfx_err_t spim_xfer(NRF_SPIM_Type               * p_spim,
                            spim_control_block_t        * p_cb,
                            nrfx_spim_xfer_desc_t const * p_xfer_desc,
                            uint32_t                      flags)
{
    nrfx_err_t err_code;
    // EasyDMA requires that transfer buffers are placed in Data RAM region;
    // signal error if they are not.
    if ((p_xfer_desc->p_tx_buffer != NULL && !nrfx_is_in_ram(p_xfer_desc->p_tx_buffer)) ||
        (p_xfer_desc->p_rx_buffer != NULL && !nrfx_is_in_ram(p_xfer_desc->p_rx_buffer)))
    {
        p_cb->transfer_in_progress = false;
        err_code = NRFX_ERROR_INVALID_ADDR;
        NRFX_LOG_WARNING("Function: %s, error code: %s.",
                         __func__,
                         NRFX_LOG_ERROR_STRING_GET(err_code));
        return err_code;
    }

#if NRFX_CHECK(NRFX_SPIM_NRF52_ANOMALY_109_WORKAROUND_ENABLED)
    p_cb->tx_length = 0;
    p_cb->rx_length = 0;
#endif

    nrf_spim_tx_buffer_set(p_spim, p_xfer_desc->p_tx_buffer, p_xfer_desc->tx_length);
    nrf_spim_rx_buffer_set(p_spim, p_xfer_desc->p_rx_buffer, p_xfer_desc->rx_length);

    nrf_spim_event_clear(p_spim, NRF_SPIM_EVENT_END);

    spim_list_enable_handle(p_spim, flags);

    if (!(flags & NRFX_SPIM_FLAG_HOLD_XFER))
    {
        nrf_spim_task_trigger(p_spim, NRF_SPIM_TASK_START);
    }
#if NRFX_CHECK(NRFX_SPIM_NRF52_ANOMALY_109_WORKAROUND_ENABLED)
    if (flags & NRFX_SPIM_FLAG_HOLD_XFER)
    {
        nrf_spim_event_clear(p_spim, NRF_SPIM_EVENT_STARTED);
        p_cb->tx_length = p_xfer_desc->tx_length;
        p_cb->rx_length = p_xfer_desc->rx_length;
        nrf_spim_tx_buffer_set(p_spim, p_xfer_desc->p_tx_buffer, 0);
        nrf_spim_rx_buffer_set(p_spim, p_xfer_desc->p_rx_buffer, 0);
        nrf_spim_int_enable(p_spim, NRF_SPIM_INT_STARTED_MASK);
    }
#endif

    if (!p_cb->handler)
    {
        while (!nrf_spim_event_check(p_spim, NRF_SPIM_EVENT_END)){}
        if (p_cb->ss_pin != NRFX_SPIM_PIN_NOT_USED)
        {
            nrf_gpio_pin_set(p_cb->ss_pin);
        }
    }
    else
    {
        spim_int_enable(p_spim, !(flags & NRFX_SPIM_FLAG_NO_XFER_EVT_HANDLER));
    }
    err_code = NRFX_SUCCESS;
    NRFX_LOG_INFO("Function: %s, error code: %s.", __func__, NRFX_LOG_ERROR_STRING_GET(err_code));
    return err_code;
}

nrfx_err_t nrfx_spim_xfer(nrfx_spim_t     const * const p_instance,
                          nrfx_spim_xfer_desc_t const * p_xfer_desc,
                          uint32_t                      flags)
{
    spim_control_block_t * p_cb = &m_cb[p_instance->drv_inst_idx];
    NRFX_ASSERT(p_cb->state != NRFX_DRV_STATE_UNINITIALIZED);
    NRFX_ASSERT(p_xfer_desc->p_tx_buffer != NULL || p_xfer_desc->tx_length == 0);
    NRFX_ASSERT(p_xfer_desc->p_rx_buffer != NULL || p_xfer_desc->rx_length == 0);

    nrfx_err_t err_code = NRFX_SUCCESS;

    if (p_cb->transfer_in_progress)
    {
        err_code = NRFX_ERROR_BUSY;
        NRFX_LOG_WARNING("Function: %s, error code: %s.",
                         __func__,
                         NRFX_LOG_ERROR_STRING_GET(err_code));
        return err_code;
    }
    else
    {
        if (p_cb->handler && !(flags & (NRFX_SPIM_FLAG_REPEATED_XFER |
                                        NRFX_SPIM_FLAG_NO_XFER_EVT_HANDLER)))
        {
            p_cb->transfer_in_progress = true;
        }
    }

    p_cb->evt.xfer_desc = *p_xfer_desc;

    if (p_cb->ss_pin != NRFX_SPIM_PIN_NOT_USED)
    {
        nrf_gpio_pin_clear(p_cb->ss_pin);
    }

    return spim_xfer(p_instance->p_reg, p_cb,  p_xfer_desc, flags);
}

void nrfx_spim_abort(nrfx_spim_t const * p_instance)
{
    spim_control_block_t * p_cb = &m_cb[p_instance->drv_inst_idx];
    NRFX_ASSERT(p_cb->state != NRFX_DRV_STATE_UNINITIALIZED);

    nrf_spim_task_trigger(p_instance->p_reg, NRF_SPIM_TASK_STOP);
    while (!nrf_spim_event_check(p_instance->p_reg, NRF_SPIM_EVENT_STOPPED))
    {}
    p_cb->transfer_in_progress = false;
}

uint32_t nrfx_spim_start_task_get(nrfx_spim_t const * p_instance)
{
    NRF_SPIM_Type * p_spim = (NRF_SPIM_Type *)p_instance->p_reg;
    return nrf_spim_task_address_get(p_spim, NRF_SPIM_TASK_START);
}

uint32_t nrfx_spim_end_event_get(nrfx_spim_t const * p_instance)
{
    NRF_SPIM_Type * p_spim = (NRF_SPIM_Type *)p_instance->p_reg;
    return nrf_spim_event_address_get(p_spim, NRF_SPIM_EVENT_END);
}

static void irq_handler(NRF_SPIM_Type * p_spim, spim_control_block_t * p_cb)
{

#if NRFX_CHECK(NRFX_SPIM_NRF52_ANOMALY_109_WORKAROUND_ENABLED)
    if ((nrf_spim_int_enable_check(p_spim, NRF_SPIM_INT_STARTED_MASK)) &&
        (nrf_spim_event_check(p_spim, NRF_SPIM_EVENT_STARTED)) )
    {
        /* Handle first, zero-length, auxiliary transmission. */
        nrf_spim_event_clear(p_spim, NRF_SPIM_EVENT_STARTED);
        nrf_spim_event_clear(p_spim, NRF_SPIM_EVENT_END);

        NRFX_ASSERT(p_spim->TXD.MAXCNT == 0);
        p_spim->TXD.MAXCNT = p_cb->tx_length;

        NRFX_ASSERT(p_spim->RXD.MAXCNT == 0);
        p_spim->RXD.MAXCNT = p_cb->rx_length;

        /* Disable STARTED interrupt, used only in auxiliary transmission. */
        nrf_spim_int_disable(p_spim, NRF_SPIM_INT_STARTED_MASK);

        /* Start the actual, glitch-free transmission. */
        nrf_spim_task_trigger(p_spim, NRF_SPIM_TASK_START);
        return;
    }
#endif

    if (nrf_spim_event_check(p_spim, NRF_SPIM_EVENT_END))
    {
        nrf_spim_event_clear(p_spim, NRF_SPIM_EVENT_END);
        NRFX_ASSERT(p_cb->handler);
        NRFX_LOG_DEBUG("Event: NRF_SPIM_EVENT_END.");
        finish_transfer(p_cb);
    }
}

#if NRFX_CHECK(NRFX_SPIM0_ENABLED)
void nrfx_spim_0_irq_handler(void)
{
    irq_handler(NRF_SPIM0, &m_cb[NRFX_SPIM0_INST_IDX]);
}
#endif

#if NRFX_CHECK(NRFX_SPIM1_ENABLED)
void nrfx_spim_1_irq_handler(void)
{
    irq_handler(NRF_SPIM1, &m_cb[NRFX_SPIM1_INST_IDX]);
}
#endif

#if NRFX_CHECK(NRFX_SPIM2_ENABLED)
void nrfx_spim_2_irq_handler(void)
{
    irq_handler(NRF_SPIM2, &m_cb[NRFX_SPIM2_INST_IDX]);
}
#endif

#endif // NRFX_CHECK(NRFX_SPIM_ENABLED)
