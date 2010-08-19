/**
 * board.c - wishbone over usb 
 * Original from bfload.c of EMC2 project
 * Modified for Mesa 7i43 USB board with FTDI chip
 *
 * Copyright (C) 2009 Yishin Li <ysli@araisrobo.com>
 **/

/*************************************************************************
*
* bfload - loads xilinx bitfile into the FPGA of
* an Anything I/O board from Mesa Electronics
*
* Copyright (C) 2007 John Kasunich (jmkasunich at fastmail dot fm)
* portions based on m5i20cfg by Peter C. Wallace
*
**************************************************************************

This program is free software; you can redistribute it and/or
modify it under the terms of version 2 of the GNU General
Public License as published by the Free Software Foundation.
This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111 USA

THE AUTHORS OF THIS LIBRARY ACCEPT ABSOLUTELY NO LIABILITY FOR
ANY HARM OR LOSS RESULTING FROM ITS USE.  IT IS _EXTREMELY_ UNWISE
TO RELY ON SOFTWARE ALONE FOR SAFETY.  Any machinery capable of
harming persons must have provisions for completely removing power
from all motors, etc, before persons enter any danger area.  All
machinery must be designed to comply with local and national safety
codes, and the authors of this software can not, and do not, take
any responsibility for such compliance.

This code was written as part of the EMC HAL project.  For more
information, go to www.linuxcnc.org.

*************************************************************************/
#if __CYGWIN__ || __MINGW32__
#include <windows.h>
#endif

// use SWIG with Tcl instead: #include <ncurses.h>

#include <errno.h>
#include <stdarg.h>
#include <inttypes.h> // for printf()
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <sys/io.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <sys/param.h>  // for MIN() and MAX()

#include <config.h>
#ifdef HAVE_LIBFTD2XX
#include <ftd2xx.h>     // from FTDI
#else
#ifdef HAVE_LIBFTDI
#include <ftdi.h>       // from FTDI
#endif  // HAVE_LIBFTDI
#endif  // HAVE_LIBFTD2XX

#include "wb_regs.h"
#include "bitfile.h"
#include "board.h"

// to disable DP(): #define TRACE 1
// to dump more info: #define TRACE 2
#define TRACE 1
#include "dptrace.h"
#if (TRACE!=0)
FILE *dptrace; // dptrace = fopen("dptrace.log","w");
#endif

#define TX_ERR_TEST 1
#if TX_ERR_TEST
static uint32_t count_tx = 0;
#define WOU_BREAK_COUNT 50
#endif
#define RX_ERR_TEST 1
#if RX_ERR_TEST
static uint32_t count_rx = 0;
#define RX_ERR_COUNT 10
#define RX_ERR_FRAME_NUM 1
#endif

// for updating board_status:
static struct timespec time_begin;
static struct timespec time_send_begin;

static int prev_ss;

#define TX_TIMEOUT 10000000  // 50ms

static int m7i43u_program_fpga(struct board *board, struct bitfile_chunk *ch);

// 
// this array describes all the boards we know how to program
//

struct board board_table[] = {
    {
        .board_type = "7i43u\0",
        .chip_type = "3s400tq144\0",
        .io_type = IO_TYPE_USB,
        .program_funct = m7i43u_program_fpga
    }
};


/**
 * the fpga was originally designed to be programmed serially... even
 * though we are doing it using a parallel interface, the bit ordering
 * is based on the serial interface, and the data needs to be reversed
 **/
static uint8_t bit_reverse (uint8_t data)
{
    static const uint8_t swaptab[256] = {
	0x00, 0x80, 0x40, 0xC0, 0x20, 0xA0, 0x60, 0xE0, 0x10, 0x90, 0x50, 0xD0, 0x30, 0xB0, 0x70, 0xF0,
	0x08, 0x88, 0x48, 0xC8, 0x28, 0xA8, 0x68, 0xE8, 0x18, 0x98, 0x58, 0xD8, 0x38, 0xB8, 0x78, 0xF8,
	0x04, 0x84, 0x44, 0xC4, 0x24, 0xA4, 0x64, 0xE4, 0x14, 0x94, 0x54, 0xD4, 0x34, 0xB4, 0x74, 0xF4,
	0x0C, 0x8C, 0x4C, 0xCC, 0x2C, 0xAC, 0x6C, 0xEC, 0x1C, 0x9C, 0x5C, 0xDC, 0x3C, 0xBC, 0x7C, 0xFC,
	0x02, 0x82, 0x42, 0xC2, 0x22, 0xA2, 0x62, 0xE2, 0x12, 0x92, 0x52, 0xD2, 0x32, 0xB2, 0x72, 0xF2,
	0x0A, 0x8A, 0x4A, 0xCA, 0x2A, 0xAA, 0x6A, 0xEA, 0x1A, 0x9A, 0x5A, 0xDA, 0x3A, 0xBA, 0x7A, 0xFA,
	0x06, 0x86, 0x46, 0xC6, 0x26, 0xA6, 0x66, 0xE6, 0x16, 0x96, 0x56, 0xD6, 0x36, 0xB6, 0x76, 0xF6,
	0x0E, 0x8E, 0x4E, 0xCE, 0x2E, 0xAE, 0x6E, 0xEE, 0x1E, 0x9E, 0x5E, 0xDE, 0x3E, 0xBE, 0x7E, 0xFE,
	0x01, 0x81, 0x41, 0xC1, 0x21, 0xA1, 0x61, 0xE1, 0x11, 0x91, 0x51, 0xD1, 0x31, 0xB1, 0x71, 0xF1,
	0x09, 0x89, 0x49, 0xC9, 0x29, 0xA9, 0x69, 0xE9, 0x19, 0x99, 0x59, 0xD9, 0x39, 0xB9, 0x79, 0xF9,
	0x05, 0x85, 0x45, 0xC5, 0x25, 0xA5, 0x65, 0xE5, 0x15, 0x95, 0x55, 0xD5, 0x35, 0xB5, 0x75, 0xF5,
	0x0D, 0x8D, 0x4D, 0xCD, 0x2D, 0xAD, 0x6D, 0xED, 0x1D, 0x9D, 0x5D, 0xDD, 0x3D, 0xBD, 0x7D, 0xFD,
	0x03, 0x83, 0x43, 0xC3, 0x23, 0xA3, 0x63, 0xE3, 0x13, 0x93, 0x53, 0xD3, 0x33, 0xB3, 0x73, 0xF3,
	0x0B, 0x8B, 0x4B, 0xCB, 0x2B, 0xAB, 0x6B, 0xEB, 0x1B, 0x9B, 0x5B, 0xDB, 0x3B, 0xBB, 0x7B, 0xFB,
	0x07, 0x87, 0x47, 0xC7, 0x27, 0xA7, 0x67, 0xE7, 0x17, 0x97, 0x57, 0xD7, 0x37, 0xB7, 0x77, 0xF7,
	0x0F, 0x8F, 0x4F, 0xCF, 0x2F, 0xAF, 0x6F, 0xEF, 0x1F, 0x9F, 0x5F, 0xDF, 0x3F, 0xBF, 0x7F, 0xFF };

    return swaptab[data];
}

struct bitfile *open_bitfile_or_die(const char *filename) 
{
    struct bitfile *bf;
    int r;

    printf ( "Reading '%s'...\n", filename);

    bf = bitfile_read(filename);
    if (bf == NULL) {
	ERRP ("reading bitstream file '%s'", filename);
	exit(EC_FILE);
    }

    r = bitfile_validate_xilinx_info(bf);
    if (r != 0) {
	ERRP ("not a valid Xilinx bitfile\n");
	exit(EC_FILE);
    }
    bitfile_print_xilinx_info(bf);

    return bf;
}


static int board_prog (struct board* board) 
{
    struct bitfile *bf;
    char *bitfile_chip;
    struct bitfile_chunk *ch;
    int r;

    // 
    // open the bitfile
    //
    bf = open_bitfile_or_die(board->io.usb.bitfile);

    // chunk 'b' has the bitfile's target device, the chip type it's for
    ch = bitfile_find_chunk(bf, 'b', 0);
    bitfile_chip = (char *)(ch->body);

    //
    // look up the device type that the caller requested in our table of
    // known device types
    // 
    printf ("before strcasecmp(board->chip_type, bitfile_chip) \n");
    printf ("debug: board(%p)\n", board);
    printf("board_type(%s)\n", 
            board->board_type);
    printf("bitfile_chip(%s)\n", 
           bitfile_chip);
    printf("board->chip_type(%s), bitfile_chip(%s)\n", 
           board->chip_type, bitfile_chip);
    if (strcasecmp(board->chip_type, bitfile_chip) != 0) {
        printf("ERROR: mismatch board->chip_type(%s), bitfile_chip(%s)\n", 
               board->chip_type, bitfile_chip);
        return (-1);
    }
    
    //
    // program the board with the bitfile
    //
    /* chunk 'e' has the bitstream */
    printf ("before bitfile_find_chunk(bf, 'e', 0); \n");
    ch = bitfile_find_chunk(bf, 'e', 0);
    printf ("after bitfile_find_chunk(bf, 'e', 0); \n");

    printf(
        "Loading configuration %s into %s at USB-%x...\n",
        bf->filename,
        board->board_type,
        board->io.usb.usb_devnum
    );
    r = board->program_funct(board, ch);
    if (r != 0) {
        ERRP ("configuration did not load");
        return EC_HDW;
    }

    bitfile_free(bf);

    return r;
}

// init for GO_BACK_N
static void gbn_init (board_t* board)
{
    int i;
    board->rd_dsize = 0;
    board->wr_dsize = 0;
    board->wou->tx_size = 0;
    board->wou->rx_req_size = 0;
    board->wou->rx_req = 0;
    board->wou->rx_size = 0;
    board->wou->rx_state = SYNC;
    board->wou->tid = 0;
    board->wou->tidSb = 0;
    board->wou->clock = 0;
    board->wou->Sn = 0;
    board->wou->Sb = 0;
    board->wou->Sm = NR_OF_WIN - 1;
    for (i=0; i<NR_OF_CLK; i++) {
        board->wou->woufs[i].use = 0;
    }
    wouf_init (board);
}

int board_init (board_t* board, const char* device_type, const int device_id,
                const char* bitfile)
{
    int found_device_type;
    int num_boards;
    int i;

#if (TRACE!=0)
    dptrace = fopen("dptrace.log","w");
    // dptrace = stderr;
#endif
    // init wb_reg_map
    memset (board->wb_reg_map, 0, WB_REG_SIZE);

    // look up the device type that the caller requested in our table of
    // known device types
    // 
    num_boards = sizeof(board_table) / sizeof(board_t);
    found_device_type = 0;
    DP ("num_boards(%d)\n", num_boards);
    for (i = 0; i < num_boards; i ++) {
        if (strcasecmp(board_table[i].board_type, device_type) == 0) {
            found_device_type = 1;
            board->board_type = board_table[i].board_type;
            board->chip_type = board_table[i].chip_type;
            board->io_type = board_table[i].io_type;
            board->program_funct = board_table[i].program_funct;
            if (board->io_type == IO_TYPE_USB) {
                board->io.usb.usb_devnum = device_id;
                board->io.usb.bitfile = bitfile;
            }
            break;
        }
    }

    if (!found_device_type) {
            ERRP("board type '%s' is unknown\n", device_type);
            return (-1);
    }

    DP ("board(%p)\n", board);
    DP ("board_type(%s)\n", board->board_type);
    DP ("chip_type(%s)\n", board->chip_type);
   
    board->wou = (wou_t *) malloc (sizeof(wou_t));
    gbn_init (board);
   
    return 0;
}


int board_connect (board_t* board)
{
#ifdef HAVE_LIBFTD2XX
    char * 	pcBufLD[MAX_DEVICES + 1]; // terminated with NULL
    char 	cBufLD[MAX_DEVICES][64];
    int	        iNumDevs = 0;
    int         devnum;
    struct timespec time;
    int i;
    FT_STATUS	ftStatus;
    FT_HANDLE	ftHandle;
    
    devnum = board->io.usb.usb_devnum;
	
    for(i = 0; i < MAX_DEVICES; i++) {
        pcBufLD[i] = cBufLD[i];
    }
    pcBufLD[MAX_DEVICES] = NULL;
    
    ftStatus = FT_ListDevices(pcBufLD, &iNumDevs, 
                              FT_LIST_ALL | FT_OPEN_BY_SERIAL_NUMBER);
    
    if(ftStatus != FT_OK) {
        ERRP ("FT_ListDevices(): ftStatus(%lu:%s)\n", 
               ftStatus, Ftstat[ftStatus].desc);
        return (-1);
    }
    
    if(iNumDevs <= 0) {
        ERRP ("no USB FTDI device found: iNumDevs(%d)\n", iNumDevs);
        return (-1);
    }
    
    for(i = 0; ( (i <MAX_DEVICES) && (i < iNumDevs) ); i++) {
            DP ("Device %d Serial Number - %s\n", i, cBufLD[i]);
    }

    if ((devnum > (iNumDevs - 1)) || (devnum > (MAX_DEVICES - 1))) {
        ERRP ("devnum(%d) is out of range(0~%d/%d)\n", 
               devnum, (iNumDevs - 1), (MAX_DEVICES - 1));
        return (-1);
    }
   
    /**
     * NB: FT_OpenEx() will put fpga in un-configured mode.
     *     Load fpga bitfile is required after it.
     *     *valid in ubuntu i386
     *     *INVALID in cygwin(i386) and centos(x86_64)
     **/
    // printf ("before FT_OpenEx(), fpga got re-configured?\n");
    // getchar();
    ftStatus = FT_OpenEx(cBufLD[devnum], FT_OPEN_BY_SERIAL_NUMBER, 
                         &ftHandle);
    if(ftStatus != FT_OK) {
        /** 
         * This can fail if the ftdi_sio driver is loaded
         * use lsmod to check this and rmmod ftdi_sio to remove
         * also rmmod usbserial
         **/
        ERRP ("FT_OpenEx(%lu:%s), device(%d)\n", 
               ftStatus, Ftstat[ftStatus].desc, devnum);
        return (-1);
    }
    // printf ("after FT_OpenEx(), fpga got re-configured?\n");
    // getchar();
    
    board->io.usb.ftHandle = ftHandle;
    DP ("Opened device %s\n", cBufLD[devnum]);
    
    time.tv_sec = 0;
    time.tv_nsec = 250000000;   // 250ms
    nanosleep(&time, NULL);
    DP ("For Linux, we need extra delay for FT_OpenEx()\n");
#else   // HAVE_LIBFTD2XX    
#ifdef HAVE_LIBFTDI
    int ret;
    struct ftdi_context *ftdic;

    board->io.usb.rx_tc = NULL;    // init transfer_control for async-read
    board->io.usb.tx_tc = NULL;    // init transfer_control for async-write
    ftdic = &(board->io.usb.ftdic);
    if (ftdi_init(ftdic) < 0)
    {
        ERRP("ftdi_init failed\n");
        return EXIT_FAILURE;
    }
    
    ftdic->usb_read_timeout = 1000;
    ftdic->usb_write_timeout = 1000;
    ftdic->writebuffer_chunksize = TX_CHUNK_SIZE;
    if (ret = ftdi_read_data_set_chunksize(ftdic, RX_CHUNK_SIZE) < 0) {
        ERRP("ftdi_read_data_set_chunksize(): %d (%s)\n", 
              ret, ftdi_get_error_string(ftdic));
        return EXIT_FAILURE;
    }
    
    if ((ret = ftdi_usb_open(ftdic, 0x0403, 0x6001)) < 0)
    {
        ERRP("unable to open ftdi device: %d (%s)\n", ret, ftdi_get_error_string(ftdic));
        return EXIT_FAILURE;
    }
    
    if ((ret = ftdi_set_latency_timer(ftdic, 1)) < 0)
    {
        ERRP("ftdi_set_latency_timer(): %d (%s)\n", ret, ftdi_get_error_string(ftdic));
        return EXIT_FAILURE;
    }

    if ((ret = ftdi_usb_reset (ftdic)) < 0)
    {
        ERRP ("ftdi_usb_reset() failed: %d", ret);
        return EXIT_FAILURE;
    }

    if ((ret = ftdi_usb_purge_buffers (ftdic)) < 0)
    {
        ERRP ("ftdi_usb_purge_buffers() failed: %d", ret);
        return EXIT_FAILURE;
    }

    // Read out FTDIChip-ID of R type chips
    if (ftdic->type == TYPE_R)
    {
        unsigned int chipid;
        printf("ftdi_read_chipid: %d\n", ftdi_read_chipid(ftdic, &chipid));
        printf("FTDI chipid: %X\n", chipid);
    }
    
#endif  // HAVE_LIBFTDI
#endif  // HAVE_LIBFTD2XX

    if (board->io.usb.bitfile) {
        board_prog(board);  // program FPGA if bitfile is provided
    }
    
    DP ("ftdic->max_packet_size(%u)\n", ftdic->max_packet_size);
    
    gbn_init (board);   // go_back_n
    
    // for updating board_status:
    clock_gettime(CLOCK_REALTIME, &time_begin);
    clock_gettime(CLOCK_REALTIME, &time_send_begin);
    prev_ss = 0;

    return (0);
}


int board_close (board_t* board)
{
#ifdef HAVE_LIBFTD2XX
    if (board->io.usb.ftHandle) {
        FT_Close(board->io.usb.ftHandle);
        board->io.usb.ftHandle = NULL;
    }
#else
#ifdef HAVE_LIBFTDI
    int ret;
    struct ftdi_context *ftdic;
    ftdic = &(board->io.usb.ftdic);
    if ((ret = ftdi_usb_close(ftdic)) < 0)
    {
        ERRP("unable to close ftdi device: %d (%s)\n", ret, ftdi_get_error_string(ftdic));
        return EXIT_FAILURE;
    }
    ftdi_deinit(ftdic);
#endif  // HAVE_LIBFTDI
#endif  // HAVE_LIBFTD2XX
    free(board->wou);
    return 0;
}   
    


/**
 * m7i43u_cpld_reset - reset the CPLD on 7i43
 *                     call this only when FPGA is in RECONFIG mode
 * TODO: returns TRUE if the FPGA reset, FALSE on error
 **/
static int m7i43u_cpld_reset(struct board *board) 
{
#ifdef HAVE_LIBFTD2XX
    char 	cBufWrite[10];
    DWORD 	dwBytesWritten/*, dwBytesRead*/;
    FT_STATUS	ftStatus;

    // turn USB_ECHO off
    cBufWrite[0] = 0;
    cBufWrite[1] = 0;
    cBufWrite[2] = 0;
    cBufWrite[3] = 0;
    if((ftStatus = FT_Write(board->io.usb.ftHandle, cBufWrite, 4, 
                            &dwBytesWritten)) != FT_OK) {
            printf("Error FT_Write(%lu)\n", ftStatus);
    }
// 2009-09-15 ysli: I believe the following code are redundant

    /* Write */
    cBufWrite[0] = 0;
    if((ftStatus = FT_Write(board->io.usb.ftHandle, cBufWrite, 1, 
                            &dwBytesWritten)) != FT_OK) {
            printf("Error FT_Write(%lu)\n", ftStatus);
    }
    // TODO: FT_Read for DONE status
		
    /* Write */
    cBufWrite[0] = 1;
    if((ftStatus = FT_Write(board->io.usb.ftHandle, cBufWrite, 1, 
                            &dwBytesWritten)) != FT_OK) {
            printf("Error FT_Write(%lu)\n", ftStatus);
    }
#else
#ifdef HAVE_LIBFTDI
    uint8_t                 *buf_tx;
    int                     ret;
    struct ftdi_context     *ftdic;
    
    ftdic = &(board->io.usb.ftdic);
    buf_tx = board->wou->buf_tx;

    // d[0]: 4bit of 0: turn USB_ECHO off
    buf_tx[0] = 0;
    buf_tx[1] = 0;
    buf_tx[2] = 0;
    buf_tx[3] = 0;
    /* Write */
    buf_tx[4] = 0;
    /* Write */
    buf_tx[5] = 1;
    if ((ret = ftdi_write_data (ftdic, buf_tx, 6)) != 6)
    {
        ERRP("ftdi_write_data: %d (%s)\n", ret, ftdi_get_error_string(ftdic));
        return -1;
    }
#endif
#endif
    return 1;
}

static int m7i43u_cpld_send_firmware(struct board *board, struct bitfile_chunk *ch) 
{
    int i;
    uint8_t *dp;
    
    printf("%s: ch(%p), ch->body(%p)\n", ch, ch->body);
    dp = ch->body;
    for (i = 0; i < ch->len; i ++) {
        *dp = bit_reverse(*dp);
        dp ++;
    }

    int ret;
    struct ftdi_context     *ftdic;
    
    ftdic = &(board->io.usb.ftdic);

    /* sync Write */
    do {
        if ((ret = ftdi_write_data (ftdic, ch->body, ch->len)) < 0)
        {
            ERRP("ftdi_write_data: %d (%s)\n", ret, ftdi_get_error_string(ftdic));
        }
        printf("ftdi_write %d bytes\n", ret);
        if (ret) {
            ch->len -= ret;
            if (ch->len) {
                memmove (ch->body, ch->body + ret, ch->len);
            }
        }
    } while (ch->len != 0);


    return 1;
}


static struct timespec diff(struct timespec start, struct timespec end)
{
	struct timespec temp;
	if ((end.tv_nsec-start.tv_nsec)<0) {
		temp.tv_sec = end.tv_sec-start.tv_sec-1;
		temp.tv_nsec = 1000000000+end.tv_nsec-start.tv_nsec;
	} else {
		temp.tv_sec = end.tv_sec-start.tv_sec;
		temp.tv_nsec = end.tv_nsec-start.tv_nsec;
	}
	return temp;
}


static uint8_t wb_reg_update (board_t* b, const uint8_t *buf)
{
    //obsolete: FT_STATUS   ftStatus;
    //obsolete: DWORD       recvd;
    uint8_t*    wb_regp;   // wb_reg_map pointer
    uint8_t     dsize;
    uint16_t    wb_addr;
    
    // [WOU]FUNC_DSIZE
    dsize = buf[0];    
    assert (dsize <= MAX_DSIZE);

    // [WOU]WB_ADDR
    memcpy (&wb_addr, buf+1, WB_ADDR_SIZE); 
    
    // [WOU]DATA
    wb_regp = &(b->wb_reg_map[wb_addr]);
    memcpy (wb_regp, buf+WOU_HDR_SIZE, dsize);

    //obsolete: ftStatus = FT_Read(b->io.usb.ftHandle, wb_regp, dsize, &recvd);
    //obsolete: if (ftStatus != FT_OK) 
    //obsolete: {
    //obsolete:   ERRP ("FT_Read(): ftStatus(%d)\n", (int)ftStatus);
    //obsolete:   return -1;
    //obsolete: }
    //obsolete: b->rd_dsize += recvd;

#if (TRACE!=0)
    {
        int         i;
        DP ("WB_ADDR(0x%04X), DSIZE(%d), DATA:\n", wb_addr, dsize);
        for (i=0; i < dsize; i++) 
        {
          DPS ("<%.2X>", wb_regp[i]);
        }
        DPS ("\n");
    }
#endif
  
    return (dsize);
}      

static int wouf_parse (board_t* b, const uint8_t *buf_head)
{
    uint16_t tmp;
    uint8_t *Sm;
    uint8_t *Sb;
    uint8_t *Sn;
    uint8_t *tidSb;
    uint16_t pload_size_tx;  // PLOAD_SIZE_TX
    uint8_t wou_dsize;
    uint8_t tidR;           // TID from FPGA
    uint8_t advance;        // Sb advance number (woufs to be flushed)
    wouf_t  *wou_frame_;
    int     i;

    Sm = &(b->wou->Sm);
    Sb = &(b->wou->Sb);
    Sn = &(b->wou->Sn);
    tidSb = &(b->wou->tidSb);
    tidR = buf_head[1];     
    advance = tidR - *tidSb;

    // CRC pass; about to update Rn
    if ((advance > 0) && (advance < NR_OF_WIN)) {
        // If you receive a request number where Rn > Sb
        // Sm = Sm + (Rn – Sb)
        tmp = *Sm + advance;
        if (tmp >= NR_OF_CLK) {
            tmp -= NR_OF_CLK;
        }
        *Sm = (uint8_t) tmp;
        // Sb = Rn
        for (i=0; i<advance; i++) {
            wou_frame_ = &(b->wou->woufs[*Sb]);
            wou_frame_->use = 0;
            tmp = *Sb + 1;
            if (tmp >= NR_OF_CLK) {
                tmp -= NR_OF_CLK;
            }
            *Sb = tmp;
            DP ("adv(%d) Sm(%d) Sn(%d) Sb(%d) Sn.use(%d) clock(%d)\n", 
                advance, *Sm, *Sn, *Sb, b->wou->woufs[*Sn].use, b->wou->clock);
            if(advance >= 1)fprintf (stderr,"adv(%d) Sm(%d) Sn(%d) Sb(%d) Sn.use(%d) clock(%d) tidR(%d)\n",
                            advance, *Sm, *Sn, *Sb, b->wou->woufs[*Sn].use, b->wou->clock,tidR);
        }
        *tidSb = tidR;
        
    } else {
        // re-transmit wou_frames where Sb <= Sn <= Sm
        int ret;
        struct ftdi_context     *ftdic;
        ftdic = &(b->io.usb.ftdic);
        ERRP("We won't meet a NAK\n");

        DP ("got an un-expected tidR: advance(%d)\n", advance);
        DP ("tidR(%02X), tidSb(%02X), Sb(%02X), Sn(%02X), Sm(%02X)\n",
               tidR, *tidSb, *Sb, *Sn, *Sm);
        ERRP ("got an un-expected tidR: advance(%d)\n", advance);
        ERRP ("tidR(%02X), tidSb(%02X), Sb(%02X), Sn(%02X), Sm(%02X)\n",
               tidR, *tidSb, *Sb, *Sn, *Sm);
        assert(0);
        // DEBUG:
        fprintf (stderr, "DEBUG: buf_head: ");
        for (i=0; i < (1 + buf_head[0] + CRC_SIZE); i++) {
            fprintf (stderr, "<%.2X>", buf_head[i]);
        }
        fprintf (stderr, "\n");

        *Sn = *Sb;
        *tidSb = tidR;

        //bug: b->wou->tid = tidR;
        
        // RESET TX&RX Registers
        if (b->io.usb.tx_tc) {
            // finishing pending async write
            ftdi_transfer_data_done (b->io.usb.tx_tc);
            b->io.usb.tx_tc = NULL;
        }
        b->wou->tx_size = 0;
        b->wou->rx_req_size = 0;
        b->wou->rx_req = 0;
        b->wou->rx_size = 0;
        // flushing RX buffer
        // // to clear tx and rx queue
        // if ((ret = ftdi_usb_purge_buffers (ftdic)) < 0)
        // {
        //     ERRP ("ftdi_usb_purge_buffers() failed: %d", ret);
        //     return;
        // }
        // to flush rx queue
        while (ret = ftdi_read_data (ftdic, b->wou->buf_tx, 1) > 0) { 
            printf ("flush %d byte\n", ret);
            if (ftdic->readbuffer_remaining) {
                printf ("flush %u byte\n", ftdic->readbuffer_remaining);
                ftdi_read_data (ftdic, 
                                b->wou->buf_tx,
                                ftdic->readbuffer_remaining);
            }
        }

        
        return (-1);    // about to flush buf_rx[]
    }
    
    // about to parse [WOU][WOU]...
    pload_size_tx = buf_head[0];
    pload_size_tx -= 1;     // TID
    buf_head += 2;          // point to [WOU]
    while (pload_size_tx > 0) {
        wou_dsize = wb_reg_update (b, buf_head);
        pload_size_tx -= (WOU_HDR_SIZE + wou_dsize);
        assert ((pload_size_tx & 0x8000) == 0);   // no negative pload_size_tx
        buf_head += (WOU_HDR_SIZE + wou_dsize);
    }
    DP ("TODO: return parsed pload_size_tx for assertion\n");
    return (0);

} // wouf_parse()

// receive data from USB and update corresponding WB registers
void wou_recv (board_t* b)
{
    int         i;
    int         immediate_state;
    int         cmp;
    uint16_t    crc16;
    int         pload_size_tx;
    uint8_t     *buf_head;
    int         *rx_size;      // size of buf_rx[]
    // int         *rx_req_size;
    int         *rx_req;
    uint8_t     *buf_rx;
    enum rx_state_type *rx_state;
    static uint8_t sync_words[3] = {WOUF_PREAMBLE, WOUF_PREAMBLE, WOUF_SOFD};

    int recvd;
    struct ftdi_context     *ftdic;
    const struct timeval    poll_timeout = {0,0};

    ftdic = &(b->io.usb.ftdic);
    rx_size = &(b->wou->rx_size);
    // rx_req_size = &(b->wou->rx_req_size);
    rx_req = &(b->wou->rx_req);
    buf_rx = b->wou->buf_rx;
    rx_state = &(b->wou->rx_state);
    if (b->io.usb.rx_tc) {
        // rx_tc->transfer could be NULL if (size <= ftdi->readbuffer_remaining)
        // at ftdi_read_data_submit();
        // assert (b->io.usb.rx_tc->transfer != NULL);
        // there's previous pending async read
        if (b->io.usb.rx_tc->transfer) {
            if (libusb_handle_events_timeout(ftdic->usb_ctx, &poll_timeout) < 0)
            {
                ERRP("libusb_handle_events_timeout() (%s)\n", ftdi_get_error_string(ftdic));
            }
            DP ("libusb_handle_events...\n");
            DP ("readbuffer_remaining(%u)\n", ftdic->readbuffer_remaining);
        }
        if (b->io.usb.rx_tc->completed) {
            recvd = ftdi_transfer_data_done (b->io.usb.rx_tc);
            if (recvd < 0) {
                ERRP("recvd(%d) (%s)\n", recvd, ftdi_get_error_string(ftdic));
                ERRP("readbuffer_remaining(%u)\n", ftdic->readbuffer_remaining);
                recvd = 0;  // to issue another ftdi_read_data_submit()
            } 
            b->io.usb.rx_tc = NULL;

        } else {
            return;
        }
    } else {
        recvd = 0;
    }

    assert(b->io.usb.rx_tc == NULL);
        
    DP ("recvd(%d)\n", recvd);
    /* recvd > 0 */
    // append data from USB to buf_rx[]
    b->rd_dsize += recvd;
    *rx_size += recvd;
    *rx_req -= recvd;
    
    // parsing buf_rx[]:
    buf_head = buf_rx;
    do {
        DP ("rx_state(%d), rx_size(%d)\n", *rx_state, *rx_size);
        immediate_state = 0;
        switch (*rx_state) {
        case SYNC:
            if (*rx_size < (WOUF_HDR_SIZE + 1/*TID_SIZE*/ + CRC_SIZE)) {
                // block until receiving enough data
                DP ("block until receiving enough data\n");
                // return; 
                break;
            }
#if(TRACE)
            DP ("buf_rx: ");
            for (i=0; i < *rx_size; i++) {
              DPS ("<%.2X>", buf_rx[i]);
            }
            DPS ("\n");
#endif

            // locate {PREAMBLE_0, PREAMBLE_1, SOFD}
            for (i=0; i<(*rx_size - (WOUF_HDR_SIZE - 1)); i++) {
                cmp = memcmp (buf_rx + i, sync_words, 3);
                if (cmp == 0) {
                    // we got {PREAMBLE_0, PREAMBLE_1, SOFD}
                    break; // break the for-loop
                }
            }

            if (cmp == 0) {
                // we got {PREAMBLE_0, PREAMBLE_1, SOFD}
                // make buf_head point to PLOAD_SIZE_TX
                buf_head = buf_rx + i + (WOUF_HDR_SIZE - 1);
                *rx_size -= (i + (WOUF_HDR_SIZE - 1));
                *rx_state = PLOAD_CRC;
                immediate_state = 1;    // switch to PLOAD_CRC state ASAP
            } else {
                buf_head = buf_rx + i;
                *rx_size -= i;
                memmove (buf_rx, buf_head, *rx_size);
                // next rx_state would still be SYNC;
            }
            break;  // rx_state == SYNC
        
        case PLOAD_CRC:
#if(TRACE)
            DP ("buf_head: ");
            for (i=0; i < *rx_size; i++) {
              DPS ("<%.2X>", buf_head[i]);
            }
            DPS ("\n");
#endif
            pload_size_tx = buf_head[0];    // PLOAD_SIZE_TX
            if (*rx_size < (1/*PLOAD_SIZE_TX*/ + pload_size_tx + CRC_SIZE)) {
                // block until receiving enough data
                if (buf_head != buf_rx) {
                    memmove (buf_rx, buf_head, *rx_size);
                }
                // return; 
                break;
            }
            // calc CRC for {PLOAD_SIZE_TX, TID, WOU_PACKETS}
            crcInit();
            crc16 = crcFast(buf_head, (1/*PLOAD_SIZE_TX*/ + pload_size_tx)); 
            cmp = memcmp(buf_head + (1 + pload_size_tx), &crc16, CRC_SIZE);
#if RX_ERR_TEST
            if(count_rx > RX_ERR_COUNT + RX_ERR_FRAME_NUM) {
            	count_rx = 0;
            }
            if(count_rx > RX_ERR_COUNT) cmp = 1;
            count_rx++;
#endif
            if (cmp == 0 ) {
                // CRC pass; about to parse WOU_FRAME
                if (wouf_parse (b, buf_head)) {
                    // un-expected Rn
                    // invalidate buf_rx[]
                    // // *rx_size = 0;
                    // *rx_size -= (1 + pload_size_tx + CRC_SIZE);
                    // buf_head += (1 + pload_size_tx + CRC_SIZE);

                } else {
                    // expected Rn
                    *rx_size -= (1 + pload_size_tx + CRC_SIZE);
                    buf_head += (1 + pload_size_tx + CRC_SIZE);

                }
                if (*rx_size) {
                    memmove (buf_rx, buf_head, *rx_size);
                    immediate_state = 1;
                }
                *rx_state = SYNC;

                // finished a WOU_FRAME
            } else {
                // CRC fail; throw buf_head back to SYNC state
                if (buf_head != buf_rx) {
                    memmove (buf_rx, buf_head, *rx_size);
                }
                *rx_state = SYNC;
                immediate_state = 1;
                ERRP ("RX_CRC(0x%04X) pload_size_tx(%d)\n", crc16, pload_size_tx);
                ERRP ("buf_rx(%p) buf_head(%p) rx_size(%d)\n", buf_rx, buf_head, *rx_size);
                // assert(0);
            }
            break;  // rx_state == PLOAD_CRC

        default:
            /* should never get here */
            ERRP ("unknown state (%d)\n", *rx_state);
            break;
        } /* end of switch(rx_state) */
    } while (immediate_state);
       
    DP ("readbuffer_remaining(%u)\n",
        b->io.usb.ftdic.readbuffer_remaining);
    
    if (*rx_req) {
        // issue async_read ...
        if ((b->io.usb.rx_tc = ftdi_read_data_submit (
                                ftdic, 
                                buf_rx + *rx_size, 
                                // ftdic->readbuffer_remaining + 1)) 
                                // MAX(1, ftdic->readbuffer_remaining))) 
                                MIN(RX_BURST_MIN, *rx_req))) 
                                == NULL) {
            ERRP("ftdi_read_data_submit(): %s\n", 
                 ftdi_get_error_string (ftdic));
            ERRP("rx_size(%d) rx_req(%d)\n", 
                 *rx_size, *rx_req);
            assert(0);
        }
        DP ("rx_req(%d)\n", *rx_req);
    }
    return;
} // wou_recv()


static void wou_send (board_t* b)
{
//    static struct timespec  time1 = {0, 0};
    struct timespec         time2, dt;
    uint8_t *buf_tx;
    uint8_t *buf_src;
    uint8_t *Sm;
    uint8_t *Sn;
    int     i;

    int         dwBytesWritten;
    int         *tx_size;
    int         *rx_req_size;
    int         *rx_req;
    struct ftdi_context     *ftdic;
    ftdic = &(b->io.usb.ftdic);
    // clock_gettime(CLOCK_REALTIME, &time1);
    // clock_gettime(CLOCK_REALTIME, &time2);
    // dt = diff(time1,time2); 
    // printf ("debug: dt.sec(%lu), dt.nsec(%lu)\n", 
    //          dt.tv_sec, dt.tv_nsec);

    clock_gettime(CLOCK_REALTIME, &time2);
    dt = diff(time_send_begin,time2);

    if (dt.tv_sec > 0 || dt.tv_nsec > TX_TIMEOUT) { // TODO: deal with timeout value for GO-BACK-N
        DP ("dt.sec(%lu), dt.nsec(%lu)\n", dt.tv_sec, dt.tv_nsec);
//        ERRP("TX TIMEOUT, Sm(%d) Sn(%d) Sb(%d)\n", b->wou->Sm, b->wou->Sn, b->wou->Sb);

        // RESET TX&RX Registers
		if (b->io.usb.tx_tc) {
			// finishing pending async write
			ftdi_transfer_data_done (b->io.usb.tx_tc);
			b->io.usb.tx_tc = NULL;
		}
		/*
		while (b->io.usb.tx_tc) {
			// there's previous pending async write
			if (b->io.usb.tx_tc->transfer) {
				const struct timeval poll_timeout = {0,0};
				if (libusb_handle_events_timeout(ftdic->usb_ctx, &poll_timeout) < 0) {
					ERRP("libusb_handle_events_timeout() (%s)\n", ftdi_get_error_string(ftdic));
				}
			}
			if (b->io.usb.tx_tc->completed) {
				dwBytesWritten = ftdi_transfer_data_done (b->io.usb.tx_tc);
				if (dwBytesWritten < 0) {
					ERRP("dwBytesWritten(%d) (%s)\n", dwBytesWritten, ftdi_get_error_string(ftdic));
					dwBytesWritten = 0;  // to issue another ftdi_write_data_submit()
				}
				b->io.usb.tx_tc = NULL;
			}
		}
		 */


		b->wou->tx_size = 0;
		//b->wou->rx_req_size = 0;
		//b->wou->rx_req = 0;
		//b->wou->rx_size = 0;
		b->wou->Sn = b->wou->Sb;
		//b->wou->tidSb = b->wou->Sb;
		ERRP("TX TIMEOUT,Sm,Sn,Sb reconfig Sm(%d) Sn(%d) Sb(%d)\n", b->wou->Sm, b->wou->Sn, b->wou->Sb);
    }

    tx_size = &(b->wou->tx_size);
    rx_req_size = &(b->wou->rx_req_size);
    rx_req = &(b->wou->rx_req);
    buf_tx = b->wou->buf_tx;
    Sm = &(b->wou->Sm);
    Sn = &(b->wou->Sn);
    DP ("Sm(0x%02X) Sn(0x%02X) Sb(0x%02X) use(%d) clock(%d)\n", 
        *Sm, *Sn, b->wou->Sb, b->wou->woufs[*Sn].use, b->wou->clock);

    if (*Sm >= *Sn) {

        if ((*Sm - *Sn) >= NR_OF_WIN) {
            // case: Sm(255), Sn(0): Sn is behind Sm
            // stop sending when exceening Max Window Boundary
            DP("hit Window Boundary\n"); 
        } else {
            for (i=*Sn; i<=*Sm; i++) {
                DP ("Sm(0x%02X) Sn(0x%02X) use(%d)\n", *Sm, *Sn, b->wou->woufs[i].use);
                if (b->wou->woufs[i].use == 0) {
                    break;
                }
                buf_src = b->wou->woufs[i].buf;
                memcpy (buf_tx + *tx_size, buf_src, b->wou->woufs[i].fsize);  
#if TX_ERR_TEST
                if(count_tx > WOU_BREAK_COUNT) {
                	fprintf(stderr,"break tid(%d)\n",(buf_tx+*tx_size)[4]);
					(buf_tx+*tx_size)[3] = 0x00;
					count_tx = 0;
				}
                count_tx ++;
#endif
                *tx_size += b->wou->woufs[i].fsize;
                *rx_req_size += (b->wou->woufs[i].pload_size_rx + WOUF_HDR_SIZE + 1/*TID*/ + CRC_SIZE);
                DP ("rx_req_size(%d)\n", *rx_req_size);
                *Sn += 1;

            }
        }
    } else {
        if ((*Sn - *Sm) == 1) {
            // stop sending when exceening Max Window Boundary
            DP("hit Window Boundary\n"); 
        } else {
            // round a circle
            assert ((NR_OF_CLK - *Sn) <= NR_OF_WIN);
            assert (*Sm <= (NR_OF_WIN - (NR_OF_CLK - *Sn)));
            for (i=*Sn; i<NR_OF_CLK; i++) {
                if (b->wou->woufs[i].use == 0) {
                    break;
                }
                buf_src = b->wou->woufs[i].buf;
                memcpy (buf_tx + *tx_size, buf_src, b->wou->woufs[i].fsize);
#if TX_ERR_TEST
                if(count_tx > WOU_BREAK_COUNT) {
                	fprintf(stderr,"break tid(%d)\n",(buf_tx+*tx_size)[4]);
					(buf_tx+*tx_size)[3] = 0x00;
					count_tx = 0;
				}
                count_tx ++;
#endif
                *tx_size += b->wou->woufs[i].fsize;
                *rx_req_size += (b->wou->woufs[i].pload_size_rx + WOUF_HDR_SIZE + 1/*TID*/ + CRC_SIZE);
                DP ("rx_req_size(%d)\n", *rx_req_size);
                *Sn += 1;

            }
            if (*Sn == (NR_OF_CLK & 0xFF)) {
                *Sn = 0;
                for (i=0; i<=*Sm; i++) {
                    if (b->wou->woufs[i].use == 0) {
                        break;
                    }
                    buf_src = b->wou->woufs[i].buf;
                    memcpy (buf_tx + *tx_size, buf_src, b->wou->woufs[i].fsize);  
#if TX_ERR_TEST
                if(count_tx > WOU_BREAK_COUNT) {
                	fprintf(stderr,"break tid(%d)\n",(buf_tx+*tx_size)[4]);
					(buf_tx+*tx_size)[3] = 0x00;
					count_tx = 0;
				}
                count_tx ++;
#endif
                    *tx_size += b->wou->woufs[i].fsize;
                    *rx_req_size += (b->wou->woufs[i].pload_size_rx + WOUF_HDR_SIZE + 1/*TID*/ + CRC_SIZE);
                    DP ("rx_req_size(%d)\n", *rx_req_size);
                    *Sn += 1;

                }
            }

        }
    }
       
    if (*tx_size >= NR_OF_WIN*(WOUF_HDR_SIZE+2+MAX_PSIZE+CRC_SIZE)) {
        ERRP ("Sm(%d) Sn(%d) Sb(%d) Sn.use(%d) clock(%d)\n", 
              *Sm, *Sn, b->wou->Sb, b->wou->woufs[*Sn].use, b->wou->clock);
        ERRP ("tx_size(%d)\n", *tx_size);
    }
    assert (*tx_size < NR_OF_WIN*(WOUF_HDR_SIZE+2+MAX_PSIZE+CRC_SIZE));
    
//async write:
    if (b->io.usb.tx_tc) {
        // there's previous pending async write
        if (b->io.usb.tx_tc->transfer) {
            const struct timeval poll_timeout = {0,0};
            if (libusb_handle_events_timeout(ftdic->usb_ctx, &poll_timeout) < 0) {
                ERRP("libusb_handle_events_timeout() (%s)\n", ftdi_get_error_string(ftdic));
            }
        }
        if (b->io.usb.tx_tc->completed) {
            dwBytesWritten = ftdi_transfer_data_done (b->io.usb.tx_tc);
            if (dwBytesWritten < 0) {
                ERRP("dwBytesWritten(%d) (%s)\n", dwBytesWritten, ftdi_get_error_string(ftdic));
                dwBytesWritten = 0;  // to issue another ftdi_write_data_submit()
            } 
            b->io.usb.tx_tc = NULL;
        } else {
           return;
        }
    } else {
        dwBytesWritten = 0;
    }

    assert(b->io.usb.tx_tc == NULL);
    
    if (dwBytesWritten) {
        clock_gettime(CLOCK_REALTIME, &time2);
        dt = diff(time_send_begin,time2);
        DP ("tx_size(%d), dwBytesWritten(%d,0x%08X), dt.sec(%lu), dt.nsec(%lu)\n", 
             *tx_size, dwBytesWritten, dwBytesWritten, dt.tv_sec, dt.tv_nsec);
        DP ("bitrate(%f Mbps)\n", 
             8.0*dwBytesWritten/(1000000.0*dt.tv_sec+dt.tv_nsec/1000.0));
        assert (dwBytesWritten <= *tx_size);
        b->wr_dsize += dwBytesWritten;
        *tx_size -= dwBytesWritten;
        memmove(buf_tx, buf_tx+dwBytesWritten, *tx_size);
    }
    
    if (*tx_size < TX_BURST_MIN) {
        DP ("skip wou_send(), tx_size(%d)\n", *tx_size);
        return;
    }

    clock_gettime(CLOCK_REALTIME, &time_send_begin);
    // issue async_write ...
    if ((b->io.usb.tx_tc = ftdi_write_data_submit (
                            ftdic, 
                            buf_tx, 
                            MIN(*tx_size, TX_BURST_MAX))) 
                            == NULL) {
        ERRP("ftdi_write_data_submit(): %s\n", 
             ftdi_get_error_string (ftdic));
        assert(0);
    }
    // request for rx
    *rx_req += *rx_req_size;    
    DP ("dwBytesWritten(%d) tx_size(%d) rx_req(%d), rx_req_size(%d)\n", 
         dwBytesWritten, *tx_size, *rx_req, *rx_req_size);
    *rx_req_size = 0;

    DP("tx_tc.completed(%d)\n", b->io.usb.tx_tc->completed);
    DP("tx_tc->transfer->status(%d)\n", b->io.usb.tx_tc->transfer->status);
    DP("tx_tc->transfer->actual_length(%d)\n", b->io.usb.tx_tc->transfer->actual_length);

#if(TRACE)
    DP ("buf_tx: tx_size(%d), sent(%d)", *tx_size, MIN(*tx_size, TX_BURST_MAX));
    // for (i=0; ((i<*tx_size) && (i<50)) ; i++)
    for (i=0; i<MIN(*tx_size, TX_BURST_MAX) ; i++) {
      DPS ("<%.2X>", buf_tx[i]);
    }
    DPS ("\n");
#endif
    
    return;
}

int wou_eof (board_t* b)
{
    // took from vip/ftdi/generator.cpp::send_frame()
    int         cur_clock;
    wouf_t      *wou_frame_;
    uint16_t    crc16;

    cur_clock = (int) b->wou->clock;
    wou_frame_ = &(b->wou->woufs[cur_clock]);

    assert ((wou_frame_->fsize - WOUF_HDR_SIZE) <= MAX_PSIZE);
    // update PAYLOAD size TX/RX of WOU_FRAME 
    // PLOAD_SIZE_TX is part of the header
    wou_frame_->buf[3] = 0xFF & (wou_frame_->fsize - WOUF_HDR_SIZE);
    wou_frame_->buf[4] = b->wou->tid;
    wou_frame_->buf[5] = 0xFF & (wou_frame_->pload_size_rx);
    
    // calc CRC for {PLOAD_SIZE_TX, PLOAD_SIZE_RX, TID, WOU_PACKETS}
    crcInit();
    crc16 = crcFast(wou_frame_->buf + (WOUF_HDR_SIZE - 1), 
                    wou_frame_->fsize - (WOUF_HDR_SIZE - 1)); 
    memcpy (wou_frame_->buf + wou_frame_->fsize, &crc16, CRC_SIZE);
    wou_frame_->fsize += CRC_SIZE;

    // set use flag for CLOCK algorithm
    wou_frame_->use = 1;    

    // update the clock pointer
    b->wou->clock += 1;
    if (b->wou->clock == NR_OF_CLK) {
        b->wou->clock = 0;  // clock: 0 ~ (NR_OF_CLK-1)
    }

    // flush pending [wou] packets
    wou_frame_ = &(b->wou->woufs[b->wou->clock]);
    do {
        wou_send(b);
        wou_recv(b);    // update GBN pointer if receiving Rn
        if (wou_frame_->use) {
            const struct timespec   req = {0,300000};   // 0.3ms
            nanosleep(&req, NULL);  // sleep for 0.3ms to avoid busy loop
        }
    } while (wou_frame_->use);
    
    // init the wouf buffer and tid
    b->wou->tid += 1;   // tid: 0 ~ 255
    wouf_init (b);

    return 0;
}

void wouf_init (board_t* b)
{
    // took from vip/ftdi/generator.cpp::init_frame()
    int         cur_clock;
    wouf_t      *wou_frame_;

    cur_clock = (int) b->wou->clock;
    wou_frame_ = &(b->wou->woufs[cur_clock]);

    wou_frame_->buf[0]          = WOUF_PREAMBLE;
    wou_frame_->buf[1]          = WOUF_PREAMBLE;
    wou_frame_->buf[2]          = WOUF_SOFD;    // Start of Frame Delimiter
    wou_frame_->buf[3]          = 0xFF;         // PLOAD_SIZE_TX
    wou_frame_->buf[4]          = 0xFF;         // TID
    wou_frame_->buf[5]          = 0xFF;         // PLOAD_SIZE_RX
    wou_frame_->fsize           = 6;
    wou_frame_->pload_size_rx   = 1;            // there would be no PLOAD_SIZE_RX in response WOU_FRAME
    wou_frame_->use             = 0;

    return ;
}


void wou_append (board_t* b, const uint8_t func, const uint16_t wb_addr, 
                 const uint16_t dsize, const uint8_t* buf)
{
    int         cur_clock;
    wouf_t      *wou_frame_;
    uint16_t    i;

    cur_clock = (int) b->wou->clock;
    wou_frame_ = &(b->wou->woufs[cur_clock]);

    // avoid exceeding WOUF_PAYLOAD limit
    if (func == WB_WR_CMD) {
        if ((wou_frame_->fsize - WOUF_HDR_SIZE + WOU_HDR_SIZE + dsize) 
            > MAX_PSIZE) 
        {
            // CRC_SIZE is not counted in PLOAD_SIZE_TX
            wou_eof(b);
        }
    } else if (func == WB_RD_CMD) {
        if (((wou_frame_->fsize - WOUF_HDR_SIZE + WOU_HDR_SIZE) > MAX_PSIZE) 
            || 
            ((wou_frame_->pload_size_rx + WOU_HDR_SIZE + dsize) > MAX_PSIZE))
        {
            // CRC_SIZE is not counted in PLOAD_SIZE_TX
            wou_eof(b);
        }
    } else {
        assert (0); // not a valid func
    }

    cur_clock = (int) b->wou->clock;
    wou_frame_ = &(b->wou->woufs[cur_clock]);
        
    // DP ("func(0x%02X) dsize(0x%02X) wb_addr(0x%04X)\n", 
    //      func, dsize, wb_addr);
    
    // code took from vip/ftdi/generator.cpp:
    i = wou_frame_->fsize;
    wou_frame_->buf[i] = 0xFF & (func | (0x7F & dsize));
    i++;
    memcpy (wou_frame_->buf + i, &wb_addr, WB_ADDR_SIZE);
    i+= WB_ADDR_SIZE;
    if (func == WB_WR_CMD) {
        // if (wb_addr == JCMD_SYNC_CMD) {
        //     fprintf  ... debug SYNC_CMD only
        // }
        memcpy (wou_frame_->buf + i, buf, dsize);
        wou_frame_->fsize = i + dsize;
    } else  if (func == WB_RD_CMD) {
        wou_frame_->fsize = i;
        wou_frame_->pload_size_rx += (WOU_HDR_SIZE + dsize);
    }
    return;    
}


static void m7i43u_reconfig (board_t* board)
{
    struct timespec  time1, time2, dt;
    uint8_t cBufWrite;
    int     i;
#ifdef HAVE_LIBFTD2XX
    FT_STATUS   ftStatus;
    DWORD       r, t, e;
#else
#ifdef HAVE_LIBFTDI
    int ret;
    // unsigned int tx_chunksize;
    struct ftdi_context *ftdic;
    ftdic = &(board->io.usb.ftdic);
#endif
#endif
        
    DP ("Park 7i43u in RECONFIG mode\n");
    
    DP ("readbuffer_remaining(%u)\n",
        board->io.usb.ftdic.readbuffer_remaining);
    
    // to clear tx and rx queue
    if ((ret = ftdi_usb_purge_buffers (ftdic)) < 0)
    {
        ERRP ("ftdi_usb_purge_buffers() failed: %d", ret);
        return;
    }
    // to flush rx queue
    while (ret = ftdi_read_data (ftdic, &cBufWrite, 1) > 0) { 
        printf ("flush %d byte\n", ret);
        if (ftdic->readbuffer_remaining) {
            printf ("flush %u byte\n", ftdic->readbuffer_remaining);
            ftdi_read_data (ftdic, 
                            board->wou->buf_tx,
                            ftdic->readbuffer_remaining);
        }
    }

//error:    // to lower the writebuffer_chunksize is mandatory for RECONFIG
//error:    // otherwise we won't get a chance to update tidR from buf_rx[]
//error:    tx_chunksize = board->io.usb.ftdic.writebuffer_chunksize;
//error:    // board->io.usb.ftdic.writebuffer_chunksize = ftdic->max_packet_size;
    printf("ftdic->max_packet_size(%d)\n", ftdic->max_packet_size);
    
    printf("1: tidSb(%02X), Sb(%02X), Sn(%02X), Sm(%02X) clock(%02X)\n",
             board->wou->tidSb, board->wou->Sb, board->wou->Sn, 
             board->wou->Sm, board->wou->clock);
    wouf_init(board);
    cBufWrite = 0; // GPIO_SYSTEM = 0x00 (do nothing)
    for (i=0; i<2; i++) {
        do {
            wou_append (board, WB_WR_CMD, GPIO_BASE + GPIO_SYSTEM, 1, &cBufWrite);
            wou_append (board, WB_WR_CMD, GPIO_BASE + GPIO_SYSTEM, 1, &cBufWrite);
            wou_eof(board);
            DP("tx_size(%d)\n", board->wou->tx_size);
        } while ((board->wou->tx_size) < TX_BURST_MIN);
        // while ((board->wou->tx_size) < BURST_MIN);
        // wait until data are wrote to usb
        // while ((board->wou->tx_size >= BURST_MIN) && board->io.usb.tx_tc) {};
        
        // to get TID, and flush buf_rx[]
        printf ("rd_dsize(%lu)\n", board->rd_dsize);
        clock_gettime(CLOCK_REALTIME, &time1);
        do {
            wou_recv(board);
            // wou_send(board);    // to toggle the libusb_handle_events_timeout()
            clock_gettime(CLOCK_REALTIME, &time2);
            dt = diff(time1,time2); 
        } while (dt.tv_sec < 1);
        printf ("rd_dsize(%lu)\n", board->rd_dsize);
    }
    
    // reset clock buffer
    // board->wou->tx_size = 0;
    // board->wou->rx_req_size = 0;
    // board->wou->rx_req = 0;
    // board->wou->rx_size = 0;
    // board->wou->rx_state = SYNC;
    board->wou->tid = board->wou->tidSb;
    board->wou->clock = board->wou->Sb;
    board->wou->Sn = board->wou->Sb;
    for (i=0; i<NR_OF_CLK; i++) {
        board->wou->woufs[i].use = 0;
    }
    wouf_init(board);
    printf("2: tidSb(%02X), Sb(%02X), Sn(%02X), Sm(%02X) clock(%02X)\n",
             board->wou->tidSb, board->wou->Sb, board->wou->Sn, 
             board->wou->Sm, board->wou->clock);

    // fill buf_tx whit NOP until approatching BURST_MIN
    cBufWrite = 0; // GPIO_SYSTEM = 0x00 (do nothing)
    // reserve 12 bytes for WOUF(GPIO_RECONFIG) 
    do {
        // wou_append (board, WB_WR_CMD, GPIO_BASE + GPIO_SYSTEM, 1, &cBufWrite);
        // adopt WB_RD_CMD to fetch more data to RX-buffer
        wou_append (board, WB_RD_CMD, SSIF_BASE + SSIF_PULSE_POS, 16, NULL);
        wou_eof(board);
        DP("tx_size(%d)\n", board->wou->tx_size);
    } while ((board->wou->tx_size + 12) < TX_BURST_MIN);
    
    DP("tx_size(%d)\n", board->wou->tx_size);
    cBufWrite = GPIO_RECONFIG;
    wou_append (board, WB_WR_CMD, GPIO_BASE + GPIO_SYSTEM, 1, &cBufWrite);
    wou_eof (board);
    DP("tx_size(%d)\n", board->wou->tx_size);
    
    if (board->io.usb.tx_tc) {
        ret = ftdi_transfer_data_done (board->io.usb.tx_tc);
        if (ret < 0) {
            ERRP("ret (%d) (%s)\n", ret , ftdi_get_error_string(ftdic));
        } 
        board->io.usb.tx_tc = NULL;
    }
    
    DP ("rd_dsize(%lu)\n", board->rd_dsize);
    if (board->rd_dsize > 50) {
        DP ("if (rd_dsize > 50), that means the FPGA is alive.\n");
        DP ("we can only release the rx_tc(transfer for RX) when FPGA is alive.\n");
        DP ("Otherwise, the system will be locked.\n");
        DP ("this workaround is ugly; thought I can't come out other solution yet. -ysli\n");
        if (board->io.usb.rx_tc) {
            ret = ftdi_transfer_data_done (board->io.usb.rx_tc);
            if (ret < 0) {
                ERRP("ret (%d) (%s)\n", ret , ftdi_get_error_string(ftdic));
            } 
            board->io.usb.rx_tc = NULL;
        }
    }

    // to flush rx queue
    while (ret = ftdi_read_data (ftdic, &cBufWrite, 1) > 0) { 
        printf ("flush %d byte\n", ret);
        if (ftdic->readbuffer_remaining) {
            printf ("flush %u byte\n", ftdic->readbuffer_remaining);
            ftdi_read_data (ftdic, 
                            board->wou->buf_tx,
                            ftdic->readbuffer_remaining);
        }
    }
    
    return;
}

// for 7i43 USB version
static int m7i43u_program_fpga(struct board *board, 
                               struct bitfile_chunk *ch) 
{
    // 
    // reset the FPGA, then send appropriate firmware
    //
    printf ("begin: m7i43u_program_fpga()\n");
    
    // if (board_reset(board)) {
    //     return EC_HDW;  // FTDI reset fail
    // }
    
    printf("DEBUG: about to m7i43u_reconfig\n");
    m7i43u_reconfig (board);
    printf("DEBUG: after m7i43u_reconfig...\n");
    

    printf("about to m7i43u_cpld_reset\n");
    if (!m7i43u_cpld_reset(board)) {
        printf("error resetting FPGA, aborting load\n");
        return -1;
    }
    
    printf("about to m7i43u_cpld_send_firmware\n");
    if (!m7i43u_cpld_send_firmware(board, ch)) {
        printf("ERROR: sending FPGA firmware\n");
        return -1;
    }
    
    // in Linux, there are 519 bytes show up on the RxQueue after
    // programming. TODO: where does it come from?
    struct timespec time;
    time.tv_sec = 0;
    time.tv_nsec = 500000000;   // 500ms
    nanosleep(&time, NULL);
    
    //obsolete: board_reset (board);  
    printf ("end: m7i43u_program_fpga()\n");

    return 0;
}

static void diff_time(struct timespec *start, struct timespec *end,
		      struct timespec *diff)
{
    struct timespec temp;
    if ((end->tv_nsec - start->tv_nsec) < 0) {
	diff->tv_sec = end->tv_sec - start->tv_sec - 1;
	diff->tv_nsec = 1000000000 + end->tv_nsec - start->tv_nsec;
    } else {
	diff->tv_sec = end->tv_sec - start->tv_sec;
	diff->tv_nsec = end->tv_nsec - start->tv_nsec;
    }
    return;
}

static void dsize_to_str(char *buf, uint64_t dsize)
{
    if ((dsize >> 10) > 0) {	// KB?
	dsize >>= 10;
	if ((dsize >> 10) > 0) {	// MB?
	    dsize >>= 10;
	    if ((dsize >> 10) > 0) {	// GB?
		dsize >>= 10;
		sprintf(buf, "%llu GB\0", dsize);
	    } else {
		sprintf(buf, "%llu MB\0", dsize);
	    }
	} else {
	    sprintf(buf, "%llu KB\0", dsize);
	}
    } else {
	sprintf(buf, "%llu Bytes\0", dsize);
    }
    return;
}

/**
 * TODO: update the results of FT_GetStatus into board data structure 
 **/
int board_status (struct board *board)
{
    struct timespec time2, dt;
    int ss, mm, hh;
    char tx_str[80], rx_str[80];
    double data_rate;

    clock_gettime(CLOCK_REALTIME, &time2);

    diff_time(&time_begin, &time2, &dt);

    ss = dt.tv_sec % 60;	// seconds
    
    // update for every seconds only
    if ((ss > prev_ss) || ((ss == 0) && (prev_ss == 59))) {

        //TODO: FT_STATUS s;
        //TODO: DWORD r, t, e;
        //TODO: if (!FT_SUCCESS(s = FT_GetStatus(board->io.usb.ftHandle, &r, &t, &e))) {
        //TODO:     ERRP("TODO: ERRP()\n");
        //TODO:     return -1;
        //TODO: }
        //TODO: printf ("FT_GetStatus: dwRxBytes(%lu) dwTxBytes(%lu) dwEventDWord(%lu)\n",
        //TODO:         r, t, e);

        dsize_to_str(tx_str, board->wr_dsize);
        dsize_to_str(rx_str, board->rd_dsize);

        if (dt.tv_sec > 0) {
            data_rate =
                (double) ((board->wr_dsize + board->rd_dsize) >> 10) 
                          * 8.0 / dt.tv_sec;
        } else {
            data_rate = 0.0;
        }

        prev_ss = ss;
        dt.tv_sec /= 60;
        mm = dt.tv_sec % 60;	// minutes
        hh = dt.tv_sec / 60;	// hr

        // IN(0x%04X), switch_in
        printf
            ("[%02d:%02d:%02d] tx(%s) rx(%s) (%.2f Kbps)\n",
             hh, mm, ss, tx_str, rx_str, data_rate);
    }

    // okay: printf ("debug: board(%p)\n", board);
    return 0;
}

// vim:sw=4:sts=4:et:
