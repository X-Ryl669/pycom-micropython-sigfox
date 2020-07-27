/*
 * Copyright (c) 2020, Pycom Limited.
 *
 * This software is licensed under the GNU GPL version 3 or any
 * later version, with permitted additional terms. For more information
 * see the Pycom Licence v1.0 document supplied with this file, or
 * available at https://www.pycom.io/opensource/licensing
 */

#include <string.h>
#include "py/mpconfig.h"
#include "py/obj.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "tcpip_adapter.h"
#include "netif/ppp/pppos.h"
#include "netif/ppp/ppp.h"
#include "netif/ppp/pppapi.h"

#include "machpin.h"
#include "lteppp.h"
#include "pins.h"
#include "mpsleep.h"
#include "esp32_mphal.h"
#include "lwip/dns.h"
#include "modlte.h"

/******************************************************************************
 DEFINE CONSTANTS
 ******************************************************************************/

#define LTE_TRX_WAIT_MS(len)                                    (((len + 1) * 12 * 1000) / MICROPY_LTE_UART_BAUDRATE)
#define LTE_TASK_PERIOD_MS                                      (2)
#define LTE_AT_CMD_TRIALS                                       (5)

/******************************************************************************
 DEFINE TYPES
 ******************************************************************************/
typedef enum
{
    LTE_PPP_IDLE = 0,
    LTE_PPP_RESUMED,
    LTE_PPP_SUSPENDED
}ltepppconnstatus_t;
/******************************************************************************
 DECLARE EXPORTED DATA
 ******************************************************************************/
extern TaskHandle_t xLTETaskHndl;
extern TaskHandle_t xLTEUartEvtTaskHndl;
SemaphoreHandle_t xLTE_modem_Conn_Sem;
/******************************************************************************
 DECLARE PRIVATE DATA
 ******************************************************************************/
static char lteppp_trx_buffer[LTE_UART_BUFFER_SIZE + 1];
#ifdef LTE_DEBUG_BUFF
static lte_log_t lteppp_log;
#endif
static char lteppp_queue_buffer[LTE_UART_BUFFER_SIZE];
static uart_dev_t* lteppp_uart_reg;
static QueueHandle_t xCmdQueue;
static QueueHandle_t xRxQueue;
static lte_state_t lteppp_lte_state;
static lte_legacy_t lteppp_lte_legacy;
static SemaphoreHandle_t xLTESem;
static ppp_pcb *lteppp_pcb;         // PPP control block
struct netif lteppp_netif;          // PPP net interface

static uint32_t lte_ipv4addr;
static uint32_t lte_gw;
static uint32_t lte_netmask;
static ip6_addr_t lte_ipv6addr;

static lte_modem_conn_state_t lteppp_modem_conn_state = E_LTE_MODEM_DISCONNECTED;

static bool ltepp_ppp_conn_up = false;

static ltepppconnstatus_t lteppp_connstatus = LTE_PPP_IDLE;

static ip_addr_t ltepp_dns_info[2]={0};

static QueueHandle_t uart0_queue;

static bool lte_uart_break_evt = false;

/******************************************************************************
 DECLARE PRIVATE FUNCTIONS
 ******************************************************************************/
static void TASK_LTE (void *pvParameters);
static void TASK_UART_EVT (void *pvParameters);
static bool lteppp_send_at_cmd_exp(const char *cmd, uint32_t timeout, const char *expected_rsp, void* data_rem, size_t len);
static bool lteppp_send_at_cmd(const char *cmd, uint32_t timeout);
static bool lteppp_check_sim_present(void);
static void lteppp_status_cb (ppp_pcb *pcb, int err_code, void *ctx);
static uint32_t lteppp_output_callback(ppp_pcb *pcb, u8_t *data, u32_t len, void *ctx);

/******************************************************************************
 DEFINE PUBLIC FUNCTIONS
 ******************************************************************************/

void connect_lte_uart (void) {

    // initialize the UART interface
    uart_config_t config;
    config.baud_rate = MICROPY_LTE_UART_BAUDRATE;
    config.data_bits = UART_DATA_8_BITS;
    config.parity = UART_PARITY_DISABLE;
    config.stop_bits = UART_STOP_BITS_1;
    config.flow_ctrl = UART_HW_FLOWCTRL_CTS_RTS;
    config.rx_flow_ctrl_thresh = 64;
    config.use_ref_tick = false;
    uart_param_config(LTE_UART_ID, &config);

    // configure the UART pins
    pin_config(MICROPY_LTE_TX_PIN, -1, U2TXD_OUT_IDX, GPIO_MODE_OUTPUT, MACHPIN_PULL_NONE, 1);
    pin_config(MICROPY_LTE_RX_PIN, U2RXD_IN_IDX, -1, GPIO_MODE_INPUT, MACHPIN_PULL_NONE, 1);
    pin_config(MICROPY_LTE_RTS_PIN, -1, U2RTS_OUT_IDX, GPIO_MODE_OUTPUT, MACHPIN_PULL_NONE, 1);
    pin_config(MICROPY_LTE_CTS_PIN, U2CTS_IN_IDX, -1, GPIO_MODE_INPUT, MACHPIN_PULL_NONE, 1);

    vTaskDelay(5 / portTICK_RATE_MS);

    uart_set_hw_flow_ctrl(LTE_UART_ID, UART_HW_FLOWCTRL_DISABLE, 0);

    // install the UART driver
    uart_driver_install(LTE_UART_ID, LTE_UART_BUFFER_SIZE, LTE_UART_BUFFER_SIZE, 1, &uart0_queue, 0, NULL);
    lteppp_uart_reg = &UART2;

    // disable the delay between transfers
    lteppp_uart_reg->idle_conf.tx_idle_num = 0;

    // configure the rx timeout threshold
    lteppp_uart_reg->conf1.rx_tout_thrhd = 20 & UART_RX_TOUT_THRHD_V;

    uart_set_rts(LTE_UART_ID, false);

    xTaskCreatePinnedToCore(TASK_UART_EVT, "LTE_UART_EVT", 2048 / sizeof(StackType_t), NULL, 12, &xLTEUartEvtTaskHndl, 1);
}


void lteppp_init(void) {
    if (!xLTETaskHndl)
    {
        lteppp_lte_state = E_LTE_INIT;

        xCmdQueue = xQueueCreate(LTE_CMD_QUEUE_SIZE_MAX, sizeof(lte_task_cmd_data_t));
        xRxQueue = xQueueCreate(LTE_RSP_QUEUE_SIZE_MAX, LTE_AT_RSP_SIZE_MAX + 1);

        xLTESem = xSemaphoreCreateMutex();
        xLTE_modem_Conn_Sem = xSemaphoreCreateMutex();

        lteppp_pcb = pppapi_pppos_create(&lteppp_netif, lteppp_output_callback, lteppp_status_cb, NULL);

        //wait on connecting modem until it is allowed
        lteppp_modem_conn_state = E_LTE_MODEM_DISCONNECTED;

        xTaskCreatePinnedToCore(TASK_LTE, "LTE", LTE_TASK_STACK_SIZE / sizeof(StackType_t), NULL, LTE_TASK_PRIORITY, &xLTETaskHndl, 1);

        lteppp_connstatus = LTE_PPP_IDLE;
#ifdef LTE_DEBUG_BUFF
        lteppp_log.log = malloc(LTE_LOG_BUFF_SIZE);
#endif
    }
}

void lteppp_start (void) {
    uart_set_hw_flow_ctrl(LTE_UART_ID, UART_HW_FLOWCTRL_CTS_RTS, 64);
    vTaskDelay(5);
}
#ifdef LTE_DEBUG_BUFF
char* lteppp_get_log_buff(void)
{
    if(lteppp_log.truncated)
    {
        if(lteppp_log.ptr < LTE_LOG_BUFF_SIZE - strlen("\n********BUFFER WRAPAROUND********\n") - 1)
        {
            memcpy(&(lteppp_log.log[lteppp_log.ptr]), "\n********BUFFER WRAPAROUND********\n", strlen("\n********BUFFER WRAPAROUND********\n"));
            lteppp_log.ptr += strlen("\n********BUFFER WRAPAROUND********\n");
        }
        lteppp_log.log[LTE_LOG_BUFF_SIZE - 1] = '\0';
    }
    else
    {
        lteppp_log.log[lteppp_log.ptr] = '\0';
    }
    return lteppp_log.log;
}
#endif
lte_modem_conn_state_t lteppp_modem_state(void)
{
    lte_modem_conn_state_t state;
    xSemaphoreTake(xLTESem, portMAX_DELAY);
    state = lteppp_modem_conn_state;
	xSemaphoreGive(xLTESem);
	return state;
}

void lteppp_set_state(lte_state_t state) {
    xSemaphoreTake(xLTESem, portMAX_DELAY);
    lteppp_lte_state = state;
    xSemaphoreGive(xLTESem);
}

void lteppp_set_default_inf(void)
{
    pppapi_set_default(lteppp_pcb);
    //Restore DNS
    dns_setserver(0, &(ltepp_dns_info[0]));
    dns_setserver(1, &(ltepp_dns_info[1]));
}

void lteppp_set_legacy(lte_legacy_t legacy) {
    xSemaphoreTake(xLTESem, portMAX_DELAY);
    lteppp_lte_legacy = legacy;
    xSemaphoreGive(xLTESem);
}

void lteppp_connect (void) {
    uart_flush(LTE_UART_ID);
    vTaskDelay(25);
    pppapi_set_default(lteppp_pcb);
    ppp_set_usepeerdns(lteppp_pcb, 1);
    pppapi_set_auth(lteppp_pcb, PPPAUTHTYPE_PAP, "", "");
    pppapi_connect(lteppp_pcb, 0);
    lteppp_connstatus = LTE_PPP_IDLE;
}

void lteppp_disconnect(void) {
    pppapi_close(lteppp_pcb, 0);
    vTaskDelay(150);
    lteppp_connstatus = LTE_PPP_IDLE;
}

void lteppp_send_at_command (lte_task_cmd_data_t *cmd, lte_task_rsp_data_t *rsp) {
    xQueueSend(xCmdQueue, (void *)cmd, (TickType_t)portMAX_DELAY);
    xQueueReceive(xRxQueue, rsp, (TickType_t)portMAX_DELAY);
}

bool lteppp_wait_at_rsp (const char *expected_rsp, uint32_t timeout, bool from_mp, void* data_rem) {

    uint32_t rx_len = 0;
    uint32_t timeout_cnt = timeout;
    // wait until characters start arriving
    do {
        // being called from the MicroPython interpreter
        if (from_mp) {
            mp_hal_delay_ms(1);
        }
        else {
            vTaskDelay(1 / portTICK_RATE_MS);
        }
        uart_get_buffered_data_len(LTE_UART_ID, &rx_len);
        if (timeout_cnt > 0) {
            timeout_cnt--;
        }
    } while ((timeout_cnt > 0 || timeout == 0) && 0 == rx_len);

    memset(lteppp_trx_buffer, 0, LTE_UART_BUFFER_SIZE);
    uint16_t len_count = 0;

    while (rx_len > 0) {
        if (len_count == 0) {
            // try to read up to the size of the buffer minus null terminator (minus 2 because we store the OK status in the last byte)
            rx_len = uart_read_bytes(LTE_UART_ID, (uint8_t *)lteppp_trx_buffer, LTE_UART_BUFFER_SIZE - 2, LTE_TRX_WAIT_MS(LTE_UART_BUFFER_SIZE) / portTICK_RATE_MS);
        }
        else
        {
            // try to read up to the size of the buffer minus null terminator (minus 2 because we store the OK status in the last byte)
            rx_len = uart_read_bytes(LTE_UART_ID, (uint8_t *)(&(lteppp_trx_buffer[len_count])), LTE_UART_BUFFER_SIZE - len_count - 2, LTE_TRX_WAIT_MS(LTE_UART_BUFFER_SIZE) / portTICK_RATE_MS);
        }
        len_count += rx_len;

        if (rx_len > 0) {
            // NULL terminate the string
            lteppp_trx_buffer[len_count] = '\0';
#ifdef LTE_DEBUG_BUFF
            if (lteppp_log.ptr < LTE_LOG_BUFF_SIZE - rx_len) {
                if (len_count == rx_len) {
                    memcpy(&(lteppp_log.log[lteppp_log.ptr]), "[RSP]: ", strlen("[RSP]: "));
                    lteppp_log.ptr += strlen("[RSP]: ");
                }
                memcpy(&(lteppp_log.log[lteppp_log.ptr]), lteppp_trx_buffer, rx_len);
                lteppp_log.ptr += rx_len;
                lteppp_log.log[lteppp_log.ptr] = '\n';
                lteppp_log.ptr++;
            }
            else
            {
                lteppp_log.ptr = 0;
                lteppp_log.truncated = true;
            }
#endif

            if (expected_rsp != NULL) {
                if (strstr(lteppp_trx_buffer, expected_rsp) != NULL) {
                    //printf("RESP: %s\n", lteppp_trx_buffer);
                    return true;
                }
            }

            uart_get_buffered_data_len(LTE_UART_ID, &rx_len);

            if((len_count + rx_len) >= (LTE_UART_BUFFER_SIZE - 2))
            {
                if (data_rem != NULL) {
                    *((bool *)data_rem) = true;
                    return true;
                }
            }
            else if(rx_len == 0)
            {
                uint8_t timeout_buff = 10;
                while((!strstr(lteppp_trx_buffer,"\r\nOK\r\n")) && (!strstr(lteppp_trx_buffer,"\r\nERROR\r\n")) && (!strstr(lteppp_trx_buffer,"+SYSSTART")) && (!strstr(lteppp_trx_buffer,"\r\nCONNECT\r\n")) &&
                        rx_len == 0 && timeout_buff > 0)
                {
#ifdef LTE_DEBUG_BUFF
                    memcpy(&(lteppp_log.log[lteppp_log.ptr]), "[Waiting]:\n", strlen("[Waiting]:\n"));
                    lteppp_log.ptr += strlen("[Waiting]:\n");
#endif

                    uart_get_buffered_data_len(LTE_UART_ID, &rx_len);

                    if (from_mp) {
                        mp_hal_delay_ms(100);
                    }
                    else {
                        vTaskDelay(100 / portTICK_RATE_MS);
                    }
                    timeout_buff--;
                }
                //check size again
                if((len_count + rx_len) >= (LTE_UART_BUFFER_SIZE - 2))
                {
                    if (data_rem != NULL) {
                        *((bool *)data_rem) = true;
                        return true;
                    }
                }
            }
        }
        else
        {
            // Do Nothing
        }
    }
    if (data_rem != NULL) {
        *((bool *)data_rem) = false;
    }
    return false;
}

lte_state_t lteppp_get_state(void) {
    lte_state_t state;
    xSemaphoreTake(xLTESem, portMAX_DELAY);
    state = lteppp_lte_state;
    xSemaphoreGive(xLTESem);
    return state;
}

lte_legacy_t lteppp_get_legacy(void) {
    lte_legacy_t legacy;
    xSemaphoreTake(xLTESem, portMAX_DELAY);
    legacy = lteppp_lte_legacy;
    xSemaphoreGive(xLTESem);
    return legacy;
}

void lteppp_deinit (void) {

    uart_set_hw_flow_ctrl(LTE_UART_ID, UART_HW_FLOWCTRL_DISABLE, 0);
    uart_set_rts(LTE_UART_ID, false);
    xSemaphoreTake(xLTESem, portMAX_DELAY);
    lteppp_lte_state = E_LTE_INIT;
    lteppp_modem_conn_state = E_LTE_MODEM_DISCONNECTED;
	xSemaphoreGive(xLTESem);
}

uint32_t lteppp_ipv4(void) {
    return lte_ipv4addr;
}

bool ltepp_is_ppp_conn_up(void)
{
	return ltepp_ppp_conn_up;
}

void lteppp_suspend(void)
{
    lteppp_connstatus = LTE_PPP_SUSPENDED;
}

void lteppp_resume(void)
{
    lteppp_connstatus = LTE_PPP_RESUMED;
}
/******************************************************************************
 DEFINE PRIVATE FUNCTIONS
 ******************************************************************************/
static void TASK_LTE (void *pvParameters) {
    bool sim_present;
    lte_task_cmd_data_t *lte_task_cmd = (lte_task_cmd_data_t *)lteppp_trx_buffer;
    lte_task_rsp_data_t *lte_task_rsp = (lte_task_rsp_data_t *)lteppp_trx_buffer;
    uint8_t at_trials = 0;
    static uint32_t thread_notification;

    connect_lte_uart();

modem_init:

    thread_notification = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    if (thread_notification)
    {
        xSemaphoreTake(xLTE_modem_Conn_Sem, portMAX_DELAY);
        xSemaphoreTake(xLTESem, portMAX_DELAY);
        lteppp_modem_conn_state = E_LTE_MODEM_CONNECTING;
        xSemaphoreGive(xLTESem);
        uart_set_rts(LTE_UART_ID, true);
        vTaskDelay(500/portTICK_PERIOD_MS);
        uart_set_hw_flow_ctrl(LTE_UART_ID, UART_HW_FLOWCTRL_CTS_RTS, 64);
        // exit PPP session if applicable
        if(lteppp_send_at_cmd("+++", LTE_PPP_BACK_OFF_TIME_MS))
        {
            vTaskDelay(LTE_PPP_BACK_OFF_TIME_MS / portTICK_RATE_MS);
            while(!lteppp_send_at_cmd("AT", LTE_RX_TIMEOUT_MIN_MS))
            {
                if (at_trials >= LTE_AT_CMD_TRIALS) {
                    uart_set_hw_flow_ctrl(LTE_UART_ID, UART_HW_FLOWCTRL_DISABLE, 0);
                    uart_set_rts(LTE_UART_ID, false);
                    xSemaphoreTake(xLTESem, portMAX_DELAY);
                    lteppp_modem_conn_state = E_LTE_MODEM_DISCONNECTED;
                    xSemaphoreGive(xLTESem);
                    xSemaphoreGive(xLTE_modem_Conn_Sem);
                    at_trials = 0;
                    goto modem_init;
                }
                at_trials++;
            }
        }
        else
        {
            lteppp_send_at_cmd("AT", LTE_RX_TIMEOUT_MIN_MS);
            if (!lteppp_send_at_cmd("AT", LTE_RX_TIMEOUT_MIN_MS)) {
                vTaskDelay(LTE_PPP_BACK_OFF_TIME_MS / portTICK_RATE_MS);
                if (lteppp_send_at_cmd("+++", LTE_PPP_BACK_OFF_TIME_MS))
                {
                    while(!lteppp_send_at_cmd("AT", LTE_RX_TIMEOUT_MIN_MS))
                    {
                        if (at_trials >= LTE_AT_CMD_TRIALS) {
                            uart_set_hw_flow_ctrl(LTE_UART_ID, UART_HW_FLOWCTRL_DISABLE, 0);
                            uart_set_rts(LTE_UART_ID, false);
                            xSemaphoreTake(xLTESem, portMAX_DELAY);
                            lteppp_modem_conn_state = E_LTE_MODEM_DISCONNECTED;
                            xSemaphoreGive(xLTESem);
                            xSemaphoreGive(xLTE_modem_Conn_Sem);
                            at_trials = 0;
                            goto modem_init;
                        }
                        at_trials++;
                    }
                }
                else
                {
                    while(!lteppp_send_at_cmd("AT", LTE_RX_TIMEOUT_MIN_MS))
                    {
                        if (at_trials >= LTE_AT_CMD_TRIALS) {
                            uart_set_hw_flow_ctrl(LTE_UART_ID, UART_HW_FLOWCTRL_DISABLE, 0);
                            uart_set_rts(LTE_UART_ID, false);
                            xSemaphoreTake(xLTESem, portMAX_DELAY);
                            lteppp_modem_conn_state = E_LTE_MODEM_DISCONNECTED;
                            xSemaphoreGive(xLTESem);
                            xSemaphoreGive(xLTE_modem_Conn_Sem);
                            at_trials = 0;
                            goto modem_init;
                        }
                        at_trials++;
                    }
                }
            }
        }
        at_trials = 0;
        // Disable char echo
        lteppp_send_at_cmd("ATE0", LTE_RX_TIMEOUT_MIN_MS);
        // disable PSM if enabled by default
        lteppp_send_at_cmd("AT+CPSMS=0", LTE_RX_TIMEOUT_MIN_MS);

        // at least enable access to the SIM
        lteppp_send_at_cmd("AT+CFUN?", LTE_RX_TIMEOUT_MAX_MS);
        char *pos = strstr(lteppp_trx_buffer, "+CFUN: ");
        if (pos && (pos[7] != '1') && (pos[7] != '4')) {
            lteppp_send_at_cmd("AT+CFUN=4", LTE_RX_TIMEOUT_MAX_MS);
        }
        // check for SIM card inserted
        sim_present = lteppp_check_sim_present();

        // if we are coming from a power on reset, disable the LTE radio
        if (mpsleep_get_reset_cause() < MPSLEEP_WDT_RESET) {
            lteppp_send_at_cmd("AT+CFUN?", LTE_RX_TIMEOUT_MAX_MS);
            if (!sim_present) {
                lteppp_send_at_cmd("AT+CFUN=0", LTE_RX_TIMEOUT_MAX_MS);
            }
        }

        // enable airplane low power mode
        lteppp_send_at_cmd("AT!=\"setlpm airplane=1 enable=1\"", LTE_RX_TIMEOUT_MAX_MS);
        // enable Break Signal for URC on UART0
        lteppp_send_at_cmd("AT+SQNIBRCFG?", LTE_RX_TIMEOUT_MAX_MS);
        if(!strstr(lteppp_trx_buffer, "+SQNIBRCFG: 1,100"))
        {
            lteppp_send_at_cmd("AT+SQNIBRCFG=1,100", LTE_RX_TIMEOUT_MAX_MS);
        }
        xSemaphoreTake(xLTESem, portMAX_DELAY);
        lteppp_modem_conn_state = E_LTE_MODEM_CONNECTED;
        xSemaphoreGive(xLTESem);
        xSemaphoreGive(xLTE_modem_Conn_Sem);
        lte_state_t state;
        for (;;) {
            vTaskDelay(LTE_TASK_PERIOD_MS);
            xSemaphoreTake(xLTESem, portMAX_DELAY);
            if(E_LTE_MODEM_DISCONNECTED == lteppp_modem_conn_state)
            {
                xSemaphoreGive(xLTESem);
                // restart the task
                goto modem_init;
            }
            xSemaphoreGive(xLTESem);
            state = lteppp_get_state();
            if (xQueueReceive(xCmdQueue, lteppp_trx_buffer, 0)) {
                lteppp_send_at_cmd_exp(lte_task_cmd->data, lte_task_cmd->timeout, NULL, &(lte_task_rsp->data_remaining), lte_task_cmd->dataLen);
                xQueueSend(xRxQueue, (void *)lte_task_rsp, (TickType_t)portMAX_DELAY);
            }
            else if(state == E_LTE_PPP && lte_uart_break_evt)
            {
                lteppp_send_at_cmd("+++", LTE_PPP_BACK_OFF_TIME_MS);
                lteppp_suspend();
            }
            else
            {
                if (state == E_LTE_PPP) {
                    uint32_t rx_len;
                    // check for IP connection
                    if(lteppp_ipv4() > 0)
                    {
                        ltepp_ppp_conn_up = true;
                    }
                    else
                    {
                        if(ltepp_ppp_conn_up == true)
                        {
                            ltepp_ppp_conn_up = false;
                            lteppp_set_state(E_LTE_ATTACHED);
                        }
                    }
                    // wait for characters received
                    uart_get_buffered_data_len(LTE_UART_ID, &rx_len);
                    if (rx_len > 0) {
                        // try to read up to the size of the buffer
                        rx_len = uart_read_bytes(LTE_UART_ID, (uint8_t *)lteppp_trx_buffer, LTE_UART_BUFFER_SIZE,
                                                 LTE_TRX_WAIT_MS(LTE_UART_BUFFER_SIZE) / portTICK_RATE_MS);
                        if (rx_len > 0) {
                            pppos_input_tcpip(lteppp_pcb, (uint8_t *)lteppp_trx_buffer, rx_len);
                        }
                    }
                }
                else
                {
                    ltepp_ppp_conn_up = false;
                }
            }
        }
    }
    goto modem_init;
}

static void TASK_UART_EVT (void *pvParameters)
{
    uart_event_t event;
    uint8_t buff[50] = {0};
    for(;;) {
        //Waiting for UART event.
        if(xQueueReceive(uart0_queue, (void * )&event, (portTickType)portMAX_DELAY)) {

            switch(event.type)
            {
                case UART_DATA:
                    if (lte_uart_break_evt) {

                        uint32_t rx_len = uart_read_bytes(LTE_UART_ID, buff, LTE_UART_BUFFER_SIZE,
                                                                         LTE_TRX_WAIT_MS(LTE_UART_BUFFER_SIZE) / portTICK_RATE_MS);

                        if ((rx_len) && (strstr((const char *)buff, "OK") != NULL))
                        {
                            if(strstr((const char *)buff, "+CEREG: 4") != NULL)
                            {
                                modlte_urc_events(LTE_EVENT_COVERAGE_LOST);
                            }

                            lte_uart_break_evt = false;
                        }
                    }
                    break;
                case UART_BREAK:
                    if (E_LTE_PPP == lteppp_get_state()) {
                        lte_uart_break_evt = true;
                    }
                    break;
                default:
                    break;
            }
        }
    }
    vTaskDelete(NULL);
}


static bool lteppp_send_at_cmd_exp (const char *cmd, uint32_t timeout, const char *expected_rsp, void* data_rem, size_t len) {

    if(strstr(cmd, "Pycom_Dummy") != NULL)
    {
#ifdef LTE_DEBUG_BUFF
        if (lteppp_log.ptr < (LTE_LOG_BUFF_SIZE - strlen("[CMD]: Dummy") + 1))
        {
            memcpy(&(lteppp_log.log[lteppp_log.ptr]), "[CMD]: Dummy", strlen("[CMD]: Dummy"));
            lteppp_log.ptr += strlen("[CMD]: Dummy");
            lteppp_log.log[lteppp_log.ptr] = '\n';
            lteppp_log.ptr++;
        }
        else
        {
            lteppp_log.ptr = 0;
            lteppp_log.truncated = true;
        }
#endif
        return lteppp_wait_at_rsp(expected_rsp, timeout, false, data_rem);
    }
    else
    {
        size_t cmd_len = len;
        // char tmp_buf[128];
#ifdef LTE_DEBUG_BUFF
        if (lteppp_log.ptr < (LTE_LOG_BUFF_SIZE - strlen("[CMD]:") - cmd_len + 1))
        {
            memcpy(&(lteppp_log.log[lteppp_log.ptr]), "[CMD]:", strlen("[CMD]:"));
            lteppp_log.ptr += strlen("[CMD]:");
            memcpy(&(lteppp_log.log[lteppp_log.ptr]), cmd, cmd_len);
            lteppp_log.ptr += cmd_len;
            lteppp_log.log[lteppp_log.ptr] = '\n';
            lteppp_log.ptr++;
        }
        else
        {
            lteppp_log.ptr = 0;
            lteppp_log.truncated = true;
        }
#endif
        // flush the rx buffer first
        uart_flush(LTE_UART_ID);
        // uart_read_bytes(LTE_UART_ID, (uint8_t *)tmp_buf, sizeof(tmp_buf), 5 / portTICK_RATE_MS);
        // then send the command
        uart_write_bytes(LTE_UART_ID, cmd, cmd_len);
        if (strcmp(cmd, "+++")) {
            uart_write_bytes(LTE_UART_ID, "\r", 1);
        }
        uart_wait_tx_done(LTE_UART_ID, LTE_TRX_WAIT_MS(cmd_len) / portTICK_RATE_MS);
        vTaskDelay(2 / portTICK_RATE_MS);

        return lteppp_wait_at_rsp(expected_rsp, timeout, false, data_rem);
    }
}

static bool lteppp_send_at_cmd(const char *cmd, uint32_t timeout) {
    return lteppp_send_at_cmd_exp (cmd, timeout, LTE_OK_RSP, NULL, strlen(cmd) );
}

static bool lteppp_check_sim_present(void) {
    lteppp_send_at_cmd("AT+CPIN?", LTE_RX_TIMEOUT_MAX_MS);
    if (strstr(lteppp_trx_buffer, "ERROR")) {
        return false;
    } else {
        return true;
    }
}

// PPP output callback
static uint32_t lteppp_output_callback(ppp_pcb *pcb, u8_t *data, u32_t len, void *ctx) {
    LWIP_UNUSED_ARG(ctx);
    uint32_t tx_bytes;
    static uint32_t top =0;
    if (lteppp_connstatus == LTE_PPP_IDLE || lteppp_connstatus == LTE_PPP_RESUMED) {
        if(top > 0 && lteppp_connstatus == LTE_PPP_RESUMED)
        {
            uart_write_bytes(LTE_UART_ID, (const char*)lteppp_queue_buffer, top+1);
            uart_wait_tx_done(LTE_UART_ID, LTE_TRX_WAIT_MS(top+1) / portTICK_RATE_MS);
        }
        top = 0;
        tx_bytes = uart_write_bytes(LTE_UART_ID, (const char*)data, len);
        uart_wait_tx_done(LTE_UART_ID, LTE_TRX_WAIT_MS(len) / portTICK_RATE_MS);
    }
    else
    {
        uint32_t temp = top + len;
        if(temp > LTE_UART_BUFFER_SIZE)
        {
            return 0;
        }
        else
        {
            memcpy(&(lteppp_queue_buffer[top]), (const char*)data, len);
            top += len;
            return len;
        }
    }
    return tx_bytes;
}

// PPP status callback
static void lteppp_status_cb (ppp_pcb *pcb, int err_code, void *ctx) {
    struct netif *pppif = ppp_netif(pcb);
    LWIP_UNUSED_ARG(ctx);

    switch (err_code) {
    case PPPERR_NONE:
//        printf("status_cb: Connected\n");
        #if PPP_IPV4_SUPPORT
        lte_gw = pppif->gw.u_addr.ip4.addr;
        lte_netmask = pppif->netmask.u_addr.ip4.addr;
        lte_ipv4addr = pppif->ip_addr.u_addr.ip4.addr;
        if(lte_ipv4addr > 0)
        {
            ltepp_dns_info[0] = dns_getserver(0);
            ltepp_dns_info[1] = dns_getserver(1);
        }
//        printf("ipaddr    = %s\n", ipaddr_ntoa(&pppif->ip_addr));
//        printf("gateway   = %s\n", ipaddr_ntoa(&pppif->gw));
//        printf("netmask   = %s\n", ipaddr_ntoa(&pppif->netmask));
        #endif
        #if PPP_IPV6_SUPPORT
        memcpy(lte_ipv6addr.addr, netif_ip6_addr(pppif, 0), sizeof(lte_ipv4addr));
//        printf("ip6addr   = %s\n", ip6addr_ntoa(netif_ip6_addr(pppif, 0)));
        #endif
        break;
    case PPPERR_PARAM:
//        printf("status_cb: Invalid parameter\n");
        break;
    case PPPERR_OPEN:
//        printf("status_cb: Unable to open PPP session\n");
        break;
    case PPPERR_DEVICE:
//        printf("status_cb: Invalid I/O device for PPP\n");
        break;
    case PPPERR_ALLOC:
//        printf("status_cb: Unable to allocate resources\n");
        break;
    case PPPERR_USER:
//        printf("status_cb: User interrupt (disconnected)\n");
        lte_ipv4addr = 0;
        memset(lte_ipv6addr.addr, 0, sizeof(lte_ipv4addr));
        break;
    case PPPERR_CONNECT:
//        printf("status_cb: Connection lost\n");
        lte_ipv4addr = 0;
        memset(lte_ipv6addr.addr, 0, sizeof(lte_ipv4addr));
        break;
    case PPPERR_AUTHFAIL:
//        printf("status_cb: Failed authentication challenge\n");
        break;
    case PPPERR_PROTOCOL:
//        printf("status_cb: Failed to meet protocol\n");
        break;
    case PPPERR_PEERDEAD:
//        printf("status_cb: Connection timeout\n");
        break;
    case PPPERR_IDLETIMEOUT:
//        printf("status_cb: Idle Timeout\n");
        break;
    case PPPERR_CONNECTTIME:
//        printf("status_cb: Max connect time reached\n");
        break;
    case PPPERR_LOOPBACK:
//        printf("status_cb: Loopback detected\n");
        break;
    default:
//        printf("status_cb: Unknown error code %d\n", err_code);
        lte_ipv4addr = 0;
        memset(lte_ipv6addr.addr, 0, sizeof(lte_ipv4addr));
        break;
    }
}
