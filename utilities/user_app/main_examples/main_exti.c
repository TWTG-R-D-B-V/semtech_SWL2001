/*!
 * \file      main_exti.c
 *
 * \brief     main program for exti example
 *
 * The Clear BSD License
 * Copyright Semtech Corporation 2021. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted (subject to the limitations in the disclaimer
 * below) provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Semtech corporation nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE GRANTED BY
 * THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT
 * NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL SEMTECH CORPORATION BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * -----------------------------------------------------------------------------
 * --- DEPENDENCIES ------------------------------------------------------------
 */
#include <stdint.h>   // C99 types
#include <stdbool.h>  // bool type

#include "main.h"

#include "smtc_modem_api.h"
#include "smtc_modem_utilities.h"

#include "smtc_modem_hal.h"
#include "smtc_hal_dbg_trace.h"

#include "example_options.h"

#include "smtc_hal_mcu.h"
#include "smtc_hal_gpio.h"
#include "smtc_hal_watchdog.h"

#include "modem_pinout.h"

#if defined( SX128X )
#include "ralf_sx128x.h"
#elif defined( SX126X )
#include "ralf_sx126x.h"
#elif defined( LR11XX )
#include "ralf_lr11xx.h"
#endif

#include <string.h>

/*
 * -----------------------------------------------------------------------------
 * --- PRIVATE MACROS-----------------------------------------------------------
 */

/**
 * @brief Returns the minimum value between a and b
 *
 * @param [in] a 1st value
 * @param [in] b 2nd value
 * @retval Minimum value
 */
#ifndef MIN
#define MIN( a, b ) ( ( ( a ) < ( b ) ) ? ( a ) : ( b ) )
#endif

/*!
 * @brief Stringify constants
 */
#define xstr( a ) str( a )
#define str( a ) #a

/*!
 * @brief Helper macro that returned a human-friendly message if a command does not return SMTC_MODEM_RC_OK
 *
 * @remark The macro is implemented to be used with functions returning a @ref smtc_modem_return_code_t
 *
 * @param[in] rc  Return code
 */
#define ASSERT_SMTC_MODEM_RC( rc )          \
    do                                      \
    {                                       \
        if( rc != SMTC_MODEM_RC_OK )        \
        {                                   \
            assert_smtc_modem_rc_dbg( rc ); \
        }                                   \
    } while( 0 )

/*
 * -----------------------------------------------------------------------------
 * --- PRIVATE CONSTANTS -------------------------------------------------------
 */

/**
 * Stack id value (multistacks modem is not yet available)
 */
#define STACK_ID 0

/**
 * @brief Stack credentials
 */
#if !defined( USE_LR11XX_CREDENTIALS )
static const uint8_t user_dev_eui[8]  = USER_LORAWAN_DEVICE_EUI;
static const uint8_t user_join_eui[8] = USER_LORAWAN_JOIN_EUI;
static const uint8_t user_app_key[16] = USER_LORAWAN_APP_KEY;
#endif

#if defined( SX128X )
const ralf_t modem_radio = RALF_SX128X_INSTANTIATE( NULL );
#elif defined( SX126X )
const ralf_t modem_radio = RALF_SX126X_INSTANTIATE( NULL );
#elif defined( LR11XX )
const ralf_t modem_radio = RALF_LR11XX_INSTANTIATE( NULL );
#else
#error "Please select radio board.."
#endif

/**
 * @brief Watchdog counter reload value during sleep (The period must be lower than MCU watchdog period (here 32s))
 */
#define WATCHDOG_RELOAD_PERIOD_MS 20000

/*
 * -----------------------------------------------------------------------------
 * --- PRIVATE TYPES -----------------------------------------------------------
 */

/*
 * -----------------------------------------------------------------------------
 * --- PRIVATE VARIABLES -------------------------------------------------------
 */
static volatile bool user_button_is_press = false;  // Flag for button status
static uint8_t       rx_payload[255]      = { 0 };  // Buffer for rx payload
static uint8_t       rx_payload_size      = 0;      // Size of the payload in the rx_payload buffer
static uint32_t      uplink_counter       = 0;      // uplink raising counter

/**
 * @brief Internal credentials
 */
#if defined( USE_LR11XX_CREDENTIALS )
static uint8_t chip_eui[SMTC_MODEM_EUI_LENGTH] = { 0 };
static uint8_t chip_pin[SMTC_MODEM_PIN_LENGTH] = { 0 };
#endif
/*
 * -----------------------------------------------------------------------------
 * --- PRIVATE FUNCTIONS DECLARATION -------------------------------------------
 */

/**
 * @brief Assert to check modem return code
 *
 * @param rc
 */
static void assert_smtc_modem_rc_dbg( smtc_modem_return_code_t rc );

/**
 * @brief Check if modem is joined
 *
 * @return true if joined, false otherwise
 */
static bool is_joined( void );

/**
 * @brief User callback for modem event
 *
 *  This callback is called every time an event ( see smtc_modem_event_t ) appears in the modem.
 *  Several events may have to be read from the modem when this callback is called.
 */
static void modem_event_callback( void );

/**
 * @brief User callback for button EXTI
 *
 * @param context Define by the user at the init
 */
static void user_button_callback( void* context );

/**
 * @brief Action to be done after button pressed
 */
static void main_handle_push_button( void );

/*
 * -----------------------------------------------------------------------------
 * --- PUBLIC FUNCTIONS DEFINITION ---------------------------------------------
 */

/**
 * @brief Example to send a user payload on an external event
 *
 */
void main_exti( void )
{
    // Disable IRQ to avoid unwanted behaviour during init
    hal_mcu_disable_irq( );

    // Configure all the µC periph (clock, gpio, timer, ...)
    hal_mcu_init( );

    // Init the modem and use modem_event_callback as event callback, please note that the callback will be
    // called immediately after the first call to smtc_modem_run_engine because of the reset detection
    smtc_modem_init( &modem_radio, &modem_event_callback );

    // Configure Nucleo blue button as EXTI
    hal_gpio_irq_t nucleo_blue_button = {
        .pin      = EXTI_BUTTON,
        .context  = NULL,                  // context pass to the callback - not used in this example
        .callback = user_button_callback,  // callback called when EXTI is triggered
    };
    hal_gpio_init_in( EXTI_BUTTON, BSP_GPIO_PULL_MODE_NONE, BSP_GPIO_IRQ_MODE_FALLING, &nucleo_blue_button );

    // Re-enable IRQ
    hal_mcu_enable_irq( );

    SMTC_HAL_TRACE_INFO( "EXTI example is starting \n" );

    uint32_t sleep_time_ms = 0;
    while( 1 )
    {
        // Check button
        if( user_button_is_press == true )
        {
            user_button_is_press = false;

            main_handle_push_button( );
        }

        // Modem process launch
        sleep_time_ms = smtc_modem_run_engine( );

        // Atomically check sleep conditions (button was not pressed)
        hal_mcu_disable_irq( );
        if( user_button_is_press == false )
        {
            hal_watchdog_reload( );
            hal_mcu_set_sleep_for_ms( MIN( sleep_time_ms, WATCHDOG_RELOAD_PERIOD_MS ) );
        }
        hal_watchdog_reload( );
        hal_mcu_enable_irq( );
    }
}

/*
 * -----------------------------------------------------------------------------
 * --- PRIVATE FUNCTIONS DEFINITION --------------------------------------------
 */

static void modem_event_callback( void )
{
    SMTC_HAL_TRACE_MSG_COLOR( "Modem event callback\n", HAL_DBG_TRACE_COLOR_BLUE );

    smtc_modem_event_t current_event;
    uint8_t            event_pending_count;
    uint8_t            stack_id = STACK_ID;

    // Continue to read modem event until all event has been processed
    do
    {
        // Read modem event
        ASSERT_SMTC_MODEM_RC( smtc_modem_get_event( &current_event, &event_pending_count ) );

        switch( current_event.event_type )
        {
        case SMTC_MODEM_EVENT_RESET:
            SMTC_HAL_TRACE_INFO( "Event received: RESET\n" );

#if !defined( USE_LR11XX_CREDENTIALS )
            // Set user credentials
            ASSERT_SMTC_MODEM_RC( smtc_modem_set_deveui( stack_id, user_dev_eui ) );
            ASSERT_SMTC_MODEM_RC( smtc_modem_set_joineui( stack_id, user_join_eui ) );
            ASSERT_SMTC_MODEM_RC( smtc_modem_set_nwkkey( stack_id, user_app_key ) );
#else
            // Get internal credentials
            ASSERT_SMTC_MODEM_RC( smtc_modem_get_chip_eui( stack_id, chip_eui ) );
            SMTC_HAL_TRACE_ARRAY( "CHIP_EUI", chip_eui, SMTC_MODEM_EUI_LENGTH );
            ASSERT_SMTC_MODEM_RC( smtc_modem_get_pin( stack_id, chip_pin ) );
            SMTC_HAL_TRACE_ARRAY( "CHIP_PIN", chip_pin, SMTC_MODEM_PIN_LENGTH );
#endif
            // Set user region
            ASSERT_SMTC_MODEM_RC( smtc_modem_set_region( stack_id, MODEM_EXAMPLE_REGION ) );
            // Schedule a Join LoRaWAN network
            ASSERT_SMTC_MODEM_RC( smtc_modem_join_network( stack_id ) );
            break;

        case SMTC_MODEM_EVENT_ALARM:
            SMTC_HAL_TRACE_INFO( "Event received: ALARM\n" );

            break;

        case SMTC_MODEM_EVENT_JOINED:
            SMTC_HAL_TRACE_INFO( "Event received: JOINED\n" );
            SMTC_HAL_TRACE_INFO( "Modem is now joined \n" );
            break;

        case SMTC_MODEM_EVENT_TXDONE:
            SMTC_HAL_TRACE_INFO( "Event received: TXDONE\n" );
            SMTC_HAL_TRACE_INFO( "Transmission done \n" );
            break;

        case SMTC_MODEM_EVENT_DOWNDATA:
            SMTC_HAL_TRACE_INFO( "Event received: DOWNDATA\n" );
            rx_payload_size = ( uint8_t ) current_event.event_data.downdata.length;
            memcpy( rx_payload, current_event.event_data.downdata.data, rx_payload_size );
            SMTC_HAL_TRACE_PRINTF( "Data received on port %u\n", current_event.event_data.downdata.fport );
            SMTC_HAL_TRACE_ARRAY( "Received payload", rx_payload, rx_payload_size );
            break;

        case SMTC_MODEM_EVENT_UPLOADDONE:
            SMTC_HAL_TRACE_INFO( "Event received: UPLOADDONE\n" );

            break;

        case SMTC_MODEM_EVENT_SETCONF:
            SMTC_HAL_TRACE_INFO( "Event received: SETCONF\n" );

            break;

        case SMTC_MODEM_EVENT_MUTE:
            SMTC_HAL_TRACE_INFO( "Event received: MUTE\n" );

            break;

        case SMTC_MODEM_EVENT_STREAMDONE:
            SMTC_HAL_TRACE_INFO( "Event received: STREAMDONE\n" );

            break;

        case SMTC_MODEM_EVENT_JOINFAIL:
            SMTC_HAL_TRACE_INFO( "Event received: JOINFAIL\n" );
            SMTC_HAL_TRACE_WARNING( "Join request failed \n" );
            break;

        case SMTC_MODEM_EVENT_TIME:
            SMTC_HAL_TRACE_INFO( "Event received: TIME\n" );
            break;

        case SMTC_MODEM_EVENT_TIMEOUT_ADR_CHANGED:
            SMTC_HAL_TRACE_INFO( "Event received: TIMEOUT_ADR_CHANGED\n" );
            break;

        case SMTC_MODEM_EVENT_NEW_LINK_ADR:
            SMTC_HAL_TRACE_INFO( "Event received: NEW_LINK_ADR\n" );
            break;

        case SMTC_MODEM_EVENT_LINK_CHECK:
            SMTC_HAL_TRACE_INFO( "Event received: LINK_CHECK\n" );
            break;

        case SMTC_MODEM_EVENT_ALMANAC_UPDATE:
            SMTC_HAL_TRACE_INFO( "Event received: ALMANAC_UPDATE\n" );
            break;

        case SMTC_MODEM_EVENT_USER_RADIO_ACCESS:
            SMTC_HAL_TRACE_INFO( "Event received: USER_RADIO_ACCESS\n" );
            break;

        case SMTC_MODEM_EVENT_CLASS_B_PING_SLOT_INFO:
            SMTC_HAL_TRACE_INFO( "Event received: CLASS_B_PING_SLOT_INFO\n" );
            break;

        case SMTC_MODEM_EVENT_CLASS_B_STATUS:
            SMTC_HAL_TRACE_INFO( "Event received: CLASS_B_STATUS\n" );
            break;

        default:
            SMTC_HAL_TRACE_ERROR( "Unknown event %u\n", current_event.event_type );
            break;
        }
    } while( event_pending_count > 0 );
}

static bool is_joined( void )
{
    uint32_t status = 0;
    ASSERT_SMTC_MODEM_RC( smtc_modem_get_status( STACK_ID, &status ) );
    if( ( status & SMTC_MODEM_STATUS_JOINED ) == SMTC_MODEM_STATUS_JOINED )
    {
        return true;
    }
    else
    {
        return false;
    }
}

static void user_button_callback( void* context )
{
    SMTC_HAL_TRACE_INFO( "Button pushed\n" );

    ( void ) context;  // Not used in the example - avoid warning

    static uint32_t last_press_timestamp_ms = 0;

    // Debounce the button press, avoid multiple triggers
    if( ( int32_t )( smtc_modem_hal_get_time_in_ms( ) - last_press_timestamp_ms ) > 500 )
    {
        last_press_timestamp_ms = smtc_modem_hal_get_time_in_ms( );
        user_button_is_press    = true;
    }
}

static void main_handle_push_button( void )
{
    if( is_joined( ) == true )
    {
        // Send uplink counter on port 102
        uint8_t buff[4] = { 0 };
        buff[0]         = ( uplink_counter >> 24 ) & 0xFF;
        buff[1]         = ( uplink_counter >> 16 ) & 0xFF;
        buff[2]         = ( uplink_counter >> 8 ) & 0xFF;
        buff[3]         = ( uplink_counter & 0xFF );
        ASSERT_SMTC_MODEM_RC( smtc_modem_request_uplink( STACK_ID, 102, false, buff, 4 ) );
        // Increment uplink counter
        if( uplink_counter < 0xFFFFFFFF )
        {
            uplink_counter++;
        }
        else
        {
            uplink_counter = 0;
        }
    }
}

static void assert_smtc_modem_rc_dbg( smtc_modem_return_code_t rc )
{
    if( rc == SMTC_MODEM_RC_NOT_INIT )
    {
        SMTC_HAL_TRACE_ERROR( "In %s - %s (line %d): %s\n", __FILE__, __func__, __LINE__,
                              xstr( SMTC_MODEM_RC_NOT_INIT ) );
    }
    else if( rc == SMTC_MODEM_RC_INVALID )
    {
        SMTC_HAL_TRACE_ERROR( "In %s - %s (line %d): %s\n", __FILE__, __func__, __LINE__,
                              xstr( SMTC_MODEM_RC_INVALID ) );
    }
    else if( rc == SMTC_MODEM_RC_BUSY )
    {
        SMTC_HAL_TRACE_ERROR( "In %s - %s (line %d): %s\n", __FILE__, __func__, __LINE__, xstr( SMTC_MODEM_RC_BUSY ) );
    }
    else if( rc == SMTC_MODEM_RC_FAIL )
    {
        SMTC_HAL_TRACE_ERROR( "In %s - %s (line %d): %s\n", __FILE__, __func__, __LINE__, xstr( SMTC_MODEM_RC_FAIL ) );
    }
    else if( rc == SMTC_MODEM_RC_BAD_SIZE )
    {
        SMTC_HAL_TRACE_ERROR( "In %s - %s (line %d): %s\n", __FILE__, __func__, __LINE__,
                              xstr( SMTC_MODEM_RC_BAD_SIZE ) );
    }
    else if( rc == SMTC_MODEM_RC_NO_TIME )
    {
        SMTC_HAL_TRACE_WARNING( "In %s - %s (line %d): %s\n", __FILE__, __func__, __LINE__,
                                xstr( SMTC_MODEM_RC_NO_TIME ) );
    }
    else if( rc == SMTC_MODEM_RC_INVALID_STACK_ID )
    {
        SMTC_HAL_TRACE_ERROR( "In %s - %s (line %d): %s\n", __FILE__, __func__, __LINE__,
                              xstr( SMTC_MODEM_RC_INVALID_STACK_ID ) );
    }
}

/* --- EOF ------------------------------------------------------------------ */
