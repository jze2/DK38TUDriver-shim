#define _GNU_SOURCE
#include <dlfcn.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

typedef long RESPONSECODE;
typedef unsigned long DWORD;
typedef unsigned long *PDWORD;
typedef unsigned long *LPDWORD;
typedef unsigned char UCHAR;
typedef unsigned char *PUCHAR;
typedef const char *LPCSTR;

/* IFD Handler 3.0 return codes */
#define IFD_SUCCESS              0
#define IFD_COMMUNICATION_ERROR  612

/* Logging — syslog works under both foreground and systemd, unlike stderr */
#define SHIM_LOG(fmt, ...) syslog(LOG_DAEMON | LOG_INFO, "shim: " fmt, ##__VA_ARGS__)
#define SHIM_ERR(fmt, ...) syslog(LOG_DAEMON | LOG_ERR,  "shim: " fmt, ##__VA_ARGS__)

/* Embedded build timestamp so journalctl output can confirm which binary is
 * actually loaded (distinguishes stale installs from freshly built ones). */
#define SHIM_BUILD_STAMP "DK38TUDriver shim built " __DATE__ " " __TIME__

typedef struct {
    DWORD Protocol;
    DWORD Length;
} SCARD_IO_HEADER, *PSCARD_IO_HEADER;

/* Handle to real driver */
static void *real_handle = NULL;

/* Function pointers */
static RESPONSECODE (*real_IFDHCreateChannel)(DWORD, DWORD) = NULL;
static RESPONSECODE (*real_IFDHCloseChannel)(DWORD) = NULL;
static RESPONSECODE (*real_IFDHGetCapabilities)(DWORD, DWORD, PDWORD, PUCHAR) = NULL;
static RESPONSECODE (*real_IFDHSetCapabilities)(DWORD, DWORD, DWORD, PUCHAR) = NULL;
static RESPONSECODE (*real_IFDHSetProtocolParameters)(DWORD, DWORD, UCHAR, UCHAR, UCHAR, UCHAR) = NULL;
static RESPONSECODE (*real_IFDHPowerICC)(DWORD, DWORD, PUCHAR, PDWORD) = NULL;
static RESPONSECODE (*real_IFDHTransmitToICC)(DWORD, SCARD_IO_HEADER, PUCHAR, DWORD, PUCHAR, PDWORD, PSCARD_IO_HEADER) = NULL;
static RESPONSECODE (*real_IFDHICCPresence)(DWORD) = NULL;

/* Load real driver */
static void load_real(void)
{
    if (!real_handle) {
        /* Locate the real driver in the same directory as this shim.
         * dlopen("name.so") without a path searches LD_LIBRARY_PATH and
         * the linker cache — it does NOT search the bundle directory.
         * dladdr() gives us our own filesystem path so we can build an
         * absolute path to DK38TUDriver.real.so next to us. */
        Dl_info info;
        char real_path[PATH_MAX];

        if (dladdr((void *)load_real, &info) && info.dli_fname) {
            char tmp[PATH_MAX];
            strncpy(tmp, info.dli_fname, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = '\0';
            snprintf(real_path, sizeof(real_path),
                     "%s/DK38TUDriver.real.so", dirname(tmp));
        } else {
            strncpy(real_path, "DK38TUDriver.real.so", sizeof(real_path) - 1);
            real_path[sizeof(real_path) - 1] = '\0';
        }

        SHIM_LOG("loading real driver from %s", real_path);
        real_handle = dlopen(real_path, RTLD_NOW);
        if (!real_handle) {
            SHIM_ERR("failed to load real driver: %s", dlerror());
            return;
        }
        SHIM_LOG("real driver loaded successfully");

        real_IFDHCreateChannel =
            dlsym(real_handle, "IFDHCreateChannel");
        real_IFDHCloseChannel =
            dlsym(real_handle, "IFDHCloseChannel");
        real_IFDHGetCapabilities =
            dlsym(real_handle, "IFDHGetCapabilities");
        real_IFDHSetCapabilities =
            dlsym(real_handle, "IFDHSetCapabilities");
        real_IFDHSetProtocolParameters =
            dlsym(real_handle, "IFDHSetProtocolParameters");
        real_IFDHPowerICC =
            dlsym(real_handle, "IFDHPowerICC");
        real_IFDHTransmitToICC =
            dlsym(real_handle, "IFDHTransmitToICC");
        real_IFDHICCPresence =
            dlsym(real_handle, "IFDHICCPresence");

#define CHECK(fn) if (!real_##fn) SHIM_ERR("missing symbol: " #fn)
        CHECK(IFDHCreateChannel);
        CHECK(IFDHCloseChannel);
        CHECK(IFDHGetCapabilities);
        CHECK(IFDHSetCapabilities);
        CHECK(IFDHSetProtocolParameters);
        CHECK(IFDHPowerICC);
        CHECK(IFDHTransmitToICC);
        CHECK(IFDHICCPresence);
#undef CHECK
    }
}

/* Runs when pcscd dlopen()s this shim — logs build identity immediately
 * so journalctl output confirms which binary is actually installed. */
static void __attribute__((constructor)) shim_init(void)
{
    openlog("DK38TUDriver-shim", LOG_PID, LOG_DAEMON);
    SHIM_LOG(SHIM_BUILD_STAMP);

    Dl_info info;
    if (dladdr((void *)shim_init, &info) && info.dli_fname)
        SHIM_LOG("loaded from %s", info.dli_fname);
}

/*
 * Parse a libudev device name of the form:
 *   usb:VID/PID:libudev:N:/dev/bus/usb/BUS/DEV
 * and return the channel number the old IFD 2.0 driver expects:
 *   0x200000 | (bus << 8) | dev
 * Returns 0 on parse failure (caller treats as unknown port).
 */
static DWORD channel_from_device_name(LPCSTR deviceName)
{
    unsigned int bus = 0, dev = 0;

    if (!deviceName)
        return 0;

    /* Walk past the last two path components: .../BUS/DEV */
    const char *p = strrchr(deviceName, '/');
    if (!p || p == deviceName)
        return 0;
    dev = (unsigned int)strtoul(p + 1, NULL, 10);

    /* Step back one more slash to find BUS */
    const char *q = p - 1;
    while (q > deviceName && *q != '/')
        q--;
    if (*q != '/')
        return 0;
    bus = (unsigned int)strtoul(q + 1, NULL, 10);

    return (DWORD)(0x200000u | (bus << 8) | dev);
}

/* Shimmed function: maps new-API CreateChannelByName -> old-API CreateChannel */
RESPONSECODE IFDHCreateChannelByName(DWORD Lun, LPCSTR deviceName)
{
    load_real();

    DWORD channel = channel_from_device_name(deviceName);
    SHIM_LOG("CreateChannelByName(%s) -> CreateChannel(0x%lX)",
             deviceName ? deviceName : "(null)", channel);

    if (!real_IFDHCreateChannel)
        return -1;

    return real_IFDHCreateChannel(Lun, channel);
}

/*
 * IFDHControl — IFD Handler 3.0 mandatory export.
 * The old driver has no equivalent; return IFD_COMMUNICATION_ERROR so
 * pcscd disables SCardControl for this reader rather than refusing to
 * bind the driver entirely.  *pdwBytesReturned must be zeroed on error
 * per the IFD 3.0 specification.
 */
RESPONSECODE IFDHControl(DWORD Lun, DWORD dwControlCode,
                         PUCHAR TxBuffer, DWORD TxLength,
                         PUCHAR RxBuffer, DWORD RxLength,
                         LPDWORD pdwBytesReturned)
{
    (void)Lun; (void)dwControlCode;
    (void)TxBuffer; (void)TxLength;
    (void)RxBuffer; (void)RxLength;

    if (pdwBytesReturned)
        *pdwBytesReturned = 0;

    return IFD_COMMUNICATION_ERROR;
}

/* Pass-through for required symbols */
RESPONSECODE IFDHCreateChannel(DWORD Lun, DWORD Channel)
{
    load_real();
    if (!real_IFDHCreateChannel)
        return -1;
    return real_IFDHCreateChannel(Lun, Channel);
}

RESPONSECODE IFDHCloseChannel(DWORD Lun)
{
    load_real();
    if (!real_IFDHCloseChannel)
        return -1;
    return real_IFDHCloseChannel(Lun);
}

RESPONSECODE IFDHGetCapabilities(DWORD Lun, DWORD Tag, PDWORD Length, PUCHAR Value)
{
    load_real();
    if (!real_IFDHGetCapabilities)
        return -1;
    return real_IFDHGetCapabilities(Lun, Tag, Length, Value);
}

RESPONSECODE IFDHSetCapabilities(DWORD Lun, DWORD Tag, DWORD Length, PUCHAR Value)
{
    load_real();
    if (!real_IFDHSetCapabilities)
        return -1;
    return real_IFDHSetCapabilities(Lun, Tag, Length, Value);
}

RESPONSECODE IFDHSetProtocolParameters(DWORD Lun, DWORD Protocol,
    UCHAR Flags, UCHAR PTS1, UCHAR PTS2, UCHAR PTS3)
{
    load_real();
    if (!real_IFDHSetProtocolParameters)
        return -1;
    return real_IFDHSetProtocolParameters(Lun, Protocol, Flags, PTS1, PTS2, PTS3);
}

RESPONSECODE IFDHPowerICC(DWORD Lun, DWORD Action, PUCHAR Atr, PDWORD AtrLength)
{
    load_real();
    if (!real_IFDHPowerICC)
        return -1;
    return real_IFDHPowerICC(Lun, Action, Atr, AtrLength);
}

RESPONSECODE IFDHTransmitToICC(DWORD Lun, SCARD_IO_HEADER SendPci,
    PUCHAR TxBuffer, DWORD TxLength,
    PUCHAR RxBuffer, PDWORD RxLength,
    PSCARD_IO_HEADER RecvPci)
{
    load_real();
    if (!real_IFDHTransmitToICC)
        return -1;
    return real_IFDHTransmitToICC(Lun, SendPci, TxBuffer, TxLength,
                                   RxBuffer, RxLength, RecvPci);
}

RESPONSECODE IFDHICCPresence(DWORD Lun)
{
    load_real();
    if (!real_IFDHICCPresence)
        return -1;
    return real_IFDHICCPresence(Lun);
}
