#pragma once
#include <stddef.h>
#include <stdint.h>

#define FLIPPY_IPC_MAJORVER = 1;
#define FLIPPY_IPC_MINORVER = 3;

#define GCN_ALIGNED(type) type __attribute__((aligned(32)))

// Check if static_assert is supported
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
    #include <assert.h>
    // C11 or later: use static_assert
    #define ASSERT_SIZE_MULTIPLE_OF_32(T) static_assert(sizeof(T) % 32 == 0, "Size of " #T " is not a multiple of 32.")
#else
    // Pre-C11: define a fallback mechanism
    #define ASSERT_SIZE_MULTIPLE_OF_32(T) 
#endif


//MUST be a multiple of 32 for DMA reasons in the cube
#define MAX_FILE_NAME 256
#define FD_IPC_MAXRESP 1024*16

#define FD_BYPASS_EXIT_MAGIC0 0xE3F72BAB
#define FD_BYPASS_EXIT_MAGIC1 0X72648977

#define IPC_MAGIC 0xAA55F641
#pragma pack(push,1)

#define IPC_READ_STATUS_RESPONSE_LEN    sizeof(file_status_t)
#define IPC_FS_INFO_RESPONSE_LEN        sizeof(fs_info_t)
#define IPC_FILE_READ_RESPONSE_LEN      read_len
#define IPC_FILE_WRITE_RESPONSE_LEN     0
#define IPC_FILE_OPEN_RESPONSE_LEN      0
#define IPC_FILE_CLOSE_RESPONSE_LEN     0
#define IPC_FILE_STAT_RESPONSE_LEN      static_assert(0, "STAT format not yet defined");
#define IPC_FILE_SEEK_RESPONSE_LEN      0
#define IPC_FILE_UNLINK_RESPONSE_LEN    0
#define IPC_FILE_MKDIR_RESPONSE_LEN     0
#define IPC_FILE_RENAME_RESPONSE_LEN    0
#define IPC_FILE_READDIR_RESPONSE_LEN   sizeof(file_entry_t)
#define IPC_NET_STATUS_RESPONSE_LEN     sizeof(flippydrive_net_status_t)
#define IPC_NET_CONFIGURE_RESPONSE_LEN  0
#define IPC_NET_PRESENCE_RESPONSE_LEN   0

#define IPC_WRITE_PAYLOAD_MAX_LEN       FD_IPC_MAXRESP-32

#define IPC_RESERVED0_SIZE 224

typedef enum {
    IPC_READ_STATUS        = 0x00,
    IPC_SET_DEFAULT_FD     = 0x01, //Purely 2040
    IPC_RELOAD_CONFIG      = 0x02, //Purely 2040
    IPC_NET_STATUS         = 0x03,

    IPC_RESET              = 0x05, //Purely 2040
    IPC_FS_INFO            = 0x06,
    IPC_FILE_MKDIR         = 0x07,
    IPC_FILE_READ          = 0x08,
    IPC_FILE_WRITE         = 0x09,
    IPC_FILE_OPEN          = 0x0A,
    IPC_FILE_CLOSE         = 0x0B,
    IPC_FILE_STAT          = 0x0C,
    IPC_FILE_SEEK          = 0x0D,
    IPC_FILE_UNLINK        = 0x0E,
    IPC_FILE_READDIR       = 0x0F,

    IPC_NET_CONFIGURE      = 0x10,
    IPC_FILE_OPEN_FLASH    = 0x11, //Purely 2040
    IPC_FILE_UNLINK_FLASH  = 0x12, //Purely 2040
    IPC_FILE_RENAME        = 0x13,

    IPC_NET_PRESENCE       = 0x14,

    IPC_CMD_MAX            = 0x1F
} ipc_command_type_t;

typedef struct
{
    uint32_t result;
    uint64_t fsize;
    uint8_t fd; //Valid after open
    uint8_t flags;
    uint8_t pad[18];
} file_status_t;

ASSERT_SIZE_MULTIPLE_OF_32(file_status_t);

typedef struct
{
    uint32_t result;
    uint64_t free;
    uint64_t total;
    uint8_t pad[12];
} fs_info_t;

ASSERT_SIZE_MULTIPLE_OF_32(fs_info_t);

typedef struct
{
    uint32_t bits;
    uint8_t pad[28];
} config_ini_t;

ASSERT_SIZE_MULTIPLE_OF_32(config_ini_t);

enum {
    IPC_FILE_FLAG_NONE            = 0x00,
    IPC_FILE_FLAG_DISABLECACHE    = 0x01,
    IPC_FILE_FLAG_DISABLEFASTSEEK = 0x02,
    IPC_FILE_FLAG_DISABLESPEEDEMU = 0x04,
    IPC_FILE_FLAG_WRITE           = 0x08,
};

enum {
    FILE_ENTRY_TYPE_FILE = 0,
    FILE_ENTRY_TYPE_DIR = 1,

    FILE_ENTRY_TYPE_BAD = 0xFF,
    FILE_ENTRY_TYPE_MAX = 0xFF
};

enum {
    FILE_ATTRIB_FLAG_FILE   = 0x00,
    FILE_ATTRIB_FLAG_RDO    = 0x01, // AM_RDO
    FILE_ATTRIB_FLAG_HIDDEN = 0x02, // AM_HID
    FILE_ATTRIB_FLAG_SYSTEM = 0x04, // AM_SYS
    FILE_ATTRIB_FLAG_DIR    = 0x10, // AM_DIR
    FILE_ATTRIB_FLAG_ARC    = 0x20, // AM_ARC
};

typedef struct {
    char name[MAX_FILE_NAME];
    uint8_t type;
    uint8_t flags;
    uint64_t size;
    uint32_t date;
    uint32_t time;
    uint32_t last_status;
    uint8_t attrib;
    uint8_t pad[9];
} file_entry_t;

ASSERT_SIZE_MULTIPLE_OF_32(file_entry_t);

#define FLIPPYDRIVE_NET_STATE_MASK 0x7
#define FLIPPYDRIVE_NET_FAIL_MASK  0x8

#define FLIPPYDRIVE_NET_STATE_NONE 0
#define FLIPPYDRIVE_NET_STATE_LINK 1
#define FLIPPYDRIVE_NET_STATE_IP   2
#define FLIPPYDRIVE_NET_STATE_CONNECT 3

typedef struct
{
    uint8_t net_flags;
    uint8_t net_status;
    char ssid[33];
    char addrstr[40];
    uint8_t nm;
    uint8_t gw[16];
    uint8_t dns0[16];
    uint8_t dns1[16];

    uint8_t server[16];

    uint8_t pad[20];
} flippydrive_net_status_t;

ASSERT_SIZE_MULTIPLE_OF_32(flippydrive_net_status_t);

typedef struct
{
    uint8_t presence; //Bit 0 - Online/Active
    char status[40];
    char sub_status[40];
    char sys_state[40];

    uint8_t pad[135];
} flippydrive_net_presence_t;

ASSERT_SIZE_MULTIPLE_OF_32(flippydrive_net_presence_t);

typedef struct {
    uint32_t magic;
    uint8_t ipc_command_type;
    uint8_t fd;
    uint8_t pad;
    uint8_t subcmd;
    union
    {
        uint8_t shortpayload[24];
        __attribute__((packed)) struct
        {
            uint32_t offset;
            uint32_t length;
        };
    };
} ipc_req_header_t;

ASSERT_SIZE_MULTIPLE_OF_32(ipc_req_header_t);

typedef struct
{
    ipc_req_header_t hdr;
    flippydrive_net_presence_t presence;
} ipc_req_net_presence_t;

ASSERT_SIZE_MULTIPLE_OF_32(ipc_req_net_presence_t);

typedef struct {
    ipc_req_header_t hdr;
    file_entry_t file;
} ipc_req_open_t;

ASSERT_SIZE_MULTIPLE_OF_32(ipc_req_open_t);

typedef struct {
    //Setup alignment such that the payload is 32-byte aligned for the GCN's DMA
    ipc_req_header_t hdr;
    uint8_t payload[IPC_WRITE_PAYLOAD_MAX_LEN];
} ipc_req_write_t;

ASSERT_SIZE_MULTIPLE_OF_32(ipc_req_write_t);

typedef struct
{
    ipc_req_header_t hdr;
    file_entry_t file;
} ipc_req_unlink_t;

ASSERT_SIZE_MULTIPLE_OF_32(ipc_req_unlink_t);

typedef struct
{
    ipc_req_header_t hdr;
    file_entry_t file;
} ipc_req_mkdir_t;

ASSERT_SIZE_MULTIPLE_OF_32(ipc_req_mkdir_t);

typedef struct
{
    ipc_req_header_t hdr;
    file_entry_t oldfile;
    file_entry_t newfile;
} ipc_req_rename_t;

ASSERT_SIZE_MULTIPLE_OF_32(ipc_req_rename_t);

#pragma pack(pop)

static const size_t ipc_payloadlen[IPC_CMD_MAX] = {
    0,                    // Read status
    0,                    // Set default fd
    0,                    // reload config
    0,                    // RESERVED1
    0,
    0,                    // RESET
    0,                    // FS_INFO
    sizeof(file_entry_t), // FILE_MKDIR
    0,                    // FILE_READ
    0,                    // FILE_WRITE
    sizeof(file_entry_t), // FILE_OPEN
    0,                    // FILE_CLOSE
    0,                    // FILE_STAT
    0,                    // FILE_SEEK
    sizeof(file_entry_t), // FILE_UNLINK
    0,                    // READDIR

    IPC_RESERVED0_SIZE,   // RESERVED0
    0,                    // FILE_OPEN_FLASH is purely internal to RP2040 and has no meaning over IPC
    0,                    // FILE_UNLINK_FLASH is purely internal
    sizeof(file_entry_t)*2, // FILE_RENAME

    sizeof(flippydrive_net_presence_t), //IPC_NET_PRESENCE

};
