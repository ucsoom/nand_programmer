/* NAND */
#include "fsmc_nand.h"
/* SPL */
#include <stm32f10x.h>
/* USB */
#include <usb_lib.h>
#include <usb_pwr.h>
#include "hw_config.h"
/* STD */
#include <stdio.h>
#include <string.h>
#include <stddef.h>

#define NAND_PAGE_NUM      2
#define NAND_BUFFER_SIZE   (NAND_PAGE_NUM * NAND_PAGE_SIZE)

#define USB_BUF_SIZE 60

enum
{
    CMD_NAND_READ_ID = 0x00,
    CMD_NAND_ERASE   = 0x01,
    CMD_NAND_READ    = 0x02,
    CMD_NAND_WRITE   = 0x03,
};

typedef struct __attribute__((__packed__))
{
    uint8_t code;
} cmd_t;

typedef struct __attribute__((__packed__))
{
    cmd_t cmd;
    uint32_t addr;
    uint32_t len;
} read_cmd_t;

enum
{
    RESP_DATA   = 0x00,
    RESP_STATUS = 0x01,
};

typedef struct __attribute__((__packed__))
{
    uint8_t code;
    uint8_t info;
    uint8_t data[];
} resp_t;

enum
{
    STATUS_OK    = 0x00,
    STATUS_ERROR = 0x01,
};

typedef struct __attribute__((__packed__))
{
    resp_t header;
    NAND_IDTypeDef nand_id;
} resp_id_t;

typedef struct
{
    NAND_ADDRESS addr;
    int is_valid;
} nand_addr_t;

typedef struct
{
    uint8_t buf[NAND_PAGE_SIZE];
    uint32_t offset;
} page_t;

NAND_ADDRESS nand_write_read_addr = { 0x00, 0x00, 0x00 };
uint8_t nand_write_buf[NAND_BUFFER_SIZE], nand_read_buf[NAND_BUFFER_SIZE];

extern __IO uint8_t Receive_Buffer[USB_BUF_SIZE];
extern __IO uint32_t Receive_length;
uint32_t packet_sent = 1;
uint32_t packet_receive = 1;
uint8_t usb_send_buf[USB_BUF_SIZE];

static void jtag_init()
{
    /* Enable JTAG in low power mode */
    DBGMCU_Config(DBGMCU_SLEEP | DBGMCU_STANDBY | DBGMCU_STOP, ENABLE);
}

static void usb_init()
{
    Set_System();
    Set_USBClock();
    USB_Interrupts_Config();
    USB_Init();
}

static void nand_init()
{
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_FSMC, ENABLE);

    NAND_Init();
}

static int make_status(uint8_t *buf, size_t buf_size, int is_ok)
{
    resp_t status = { RESP_STATUS,  is_ok ? STATUS_OK : STATUS_ERROR };
    size_t len = sizeof(status);

    if (len > buf_size)
        return -1;

    memcpy(buf, &status, len);

    return len;
}

void fill_buffer(uint8_t *buf, uint16_t buf_len, uint32_t offset)
{
    uint16_t i;

    /* Put in global buffer same values */
    for (i = 0; i < buf_len; i++)
        buf[i] = i + offset;
}

static int nand_read_id(uint8_t *buf, size_t buf_size)
{
    resp_id_t resp;
    size_t resp_len = sizeof(resp);

    if (buf_size < resp_len)
        goto Error;

    resp.header.code = RESP_DATA;
    resp.header.info = resp_len - sizeof(resp.header);
    NAND_ReadID(&resp.nand_id);

    memcpy(buf, &resp, resp_len);

    return resp_len;

Error:
    return make_status(usb_send_buf, buf_size, 0);
}

static int nand_erase(uint8_t *buf, size_t buf_size)
{
    uint32_t status;

    /* Erase the NAND first Block */
    status = NAND_EraseBlock(nand_write_read_addr);

    return make_status(buf, buf_size, status == NAND_READY);
}

static int nand_write(uint8_t *buf, size_t buf_size)
{
    uint32_t status;
    int len;

    /* Write data to FSMC NAND memory */
    /* Fill the buffer to send */
    fill_buffer(nand_write_buf, NAND_BUFFER_SIZE , 0x66);

    status = NAND_WriteSmallPage(nand_write_buf, nand_write_read_addr,
        NAND_PAGE_NUM);
    len = snprintf((char *)buf, buf_size, "0x%x\r\n", (unsigned int)status);
    if (len < 0 || len >= buf_size)
        return -1;

    len++;
    return len;
}

static int nand_read(uint8_t *rx_buf, size_t rx_buf_size, uint8_t *tx_buf,
    size_t tx_buf_size)
{
    nand_addr_t nand_addr;
    static page_t page;
    uint32_t status, write_len;
    uint32_t page_size = sizeof(page.buf);
    uint32_t resp_header_size = offsetof(resp_t, data);
    uint32_t tx_data_len = tx_buf_size - resp_header_size;
    read_cmd_t *read_cmd = (read_cmd_t *)rx_buf;
    resp_t *resp = (resp_t *)tx_buf;

    if (NAND_RawAddressToNandAddress(read_cmd->addr, &nand_addr.addr)
        != NAND_VALID_ADDRESS)
    {
        goto Error;
    }

    page.offset = read_cmd->addr % page_size;

    resp->code = RESP_DATA;

    while (read_cmd->len)
    {
        status = NAND_ReadSmallPage(page.buf, nand_addr.addr, 1);
        if (!(status & NAND_READY))
            goto Error;

        while (page.offset < page_size && read_cmd->len)
        {
            if (page_size - page.offset >= tx_data_len)
                write_len = tx_data_len;
            else
                write_len = page_size - page.offset;

            if (write_len > read_cmd->len)
                write_len = read_cmd->len;
 
            memcpy(resp->data, page.buf + page.offset, write_len);

            while (!packet_sent);

            resp->info = write_len;
            CDC_Send_DATA(tx_buf, resp_header_size + write_len);

            page.offset += write_len;
            if (page.offset == page_size)
                page.offset = 0;
            read_cmd->len -= write_len;
        }

        if (read_cmd->len)
        {
            status = NAND_AddressIncrement(&nand_addr.addr);
            if (!(status & NAND_VALID_ADDRESS))
                goto Error;
        }
    }

    return 0;

Error:
    return make_status(tx_buf, tx_buf_size, 0);
}

static int cmd_handler(uint8_t *rx_buf, size_t rx_buf_size, uint8_t *tx_buf,
    size_t tx_buf_size)
{
    cmd_t *cmd = (cmd_t *)rx_buf;
    int ret = -1;

    switch (cmd->code)
    {
    case CMD_NAND_READ_ID:
        ret = nand_read_id(tx_buf, tx_buf_size);
        break;
    case CMD_NAND_ERASE:
        ret = nand_erase(tx_buf, tx_buf_size);
        break;
    case CMD_NAND_READ:
        ret = nand_read(rx_buf, rx_buf_size, tx_buf, tx_buf_size);
        break;
    case CMD_NAND_WRITE:
        ret = nand_write(tx_buf, tx_buf_size);
        break;
    default:
        break;
    }

    return ret;
}

static void usb_handler()
{
    int len;

    if (bDeviceState != CONFIGURED)
        return;

    CDC_Receive_DATA();
    if (!Receive_length)
        return;

    len = cmd_handler((uint8_t *)Receive_Buffer, sizeof(Receive_Buffer),
        usb_send_buf, sizeof(usb_send_buf));
    if (len <= 0)
        goto Exit;

    if (packet_sent)
        CDC_Send_DATA(usb_send_buf, len);

Exit:
    Receive_length = 0;
}

int main()
{
    jtag_init();

    usb_init();

    nand_init();

    while (1)
        usb_handler();

    return 0;
}