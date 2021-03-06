/*
 * Copyright (c) 2014 Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/****************************************************************************
 * drivers/usbdev/apb-es1.c
 *
 * Author: Alexandre Bailon <abailon@baylibre.com>
 * Based on pl2303 usb device driver
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <semaphore.h>
#include <string.h>
#include <errno.h>
#include <queue.h>
#include <debug.h>
#include <fcntl.h>

#include <nuttx/list.h>
#include <nuttx/kmalloc.h>
#include <nuttx/arch.h>
#include <nuttx/serial/serial.h>
#include <nuttx/usb_device.h>
#include <nuttx/usb/usb.h>
#include <nuttx/usb/usbdev.h>
#include <nuttx/usb/usbdev_trace.h>
#include <nuttx/logbuffer.h>
#include <nuttx/gpio.h>
#include <nuttx/greybus/greybus.h>
#include <nuttx/unipro/unipro.h>
#include <arch/byteorder.h>
#include <arch/board/common_gadget.h>
#include <arch/board/apbridgea_gadget.h>
#include <nuttx/wdog.h>
#include <nuttx/greybus/greybus_timestamp.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Configuration ************************************************************/

/* Logical endpoint numbers / max packet sizes */

#define CONFIG_APBRIDGE_EPBULKOUT 2

#define CONFIG_APBRIDGE_EPBULKIN 1

/* Packet and request buffer sizes */

#define CONFIG_APBRIDGE_EP0MAXPACKET 64

/* Vendor and product IDs and strings */

#define CONFIG_APBRIDGE_VENDORSTR  "Toshiba"

#define CONFIG_APBRIDGE_PRODUCTSTR "APBridge"

#undef CONFIG_APBRIDGE_SERIALSTR
#define CONFIG_APBRIDGE_SERIALSTR "0"

#undef CONFIG_APBRIDGE_CONFIGSTR
#define CONFIG_APBRIDGE_CONFIGSTR "Bulk"

/* Descriptors ****************************************************************/

/* These settings are not modifiable via the NuttX configuration */

#define APBRIDGE_VERSIONNO           (0x0001)   /* Device version number */
#define APBRIDGE_CONFIGIDNONE        (0)        /* Config ID means to return to address mode */
#define APBRIDGE_CONFIGID            (1)        /* The only supported configuration ID */
#define APBRIDGE_NCONFIGS            (1)        /* Number of configurations supported */
#if defined(CONFIG_TSB_CHIP_REV_ES2)
#define APBRIDGE_NBULKS              (7)
#endif

#define APBRIDGE_INTERFACEID         (0)
#define APBRIDGE_ALTINTERFACEID      (0)
#define APBRIDGE_NINTERFACES         (1)        /* Number of interfaces in the configuration */
/* Number of endpoints in the interface  */
#define APBRIDGE_NENDPOINTS          (APBRIDGE_NBULKS << 1)

#define BULKEP_TO_N(ep) \
  ((USB_EPNO(ep->eplog) - CONFIG_APBRIDGE_EPBULKOUT) >> 1)

#define APBRIDGE_NREQS               (1)
#define APBRIDGE_REQ_SIZE            (2048)

#define APBRIDGE_CONFIG_ATTR \
  USB_CONFIG_ATTR_ONE | \
  USB_CONFIG_ATTR_SELFPOWER | \
  USB_CONFIG_ATTR_WAKEUP

/* Endpoint configuration */

#define APBRIDGE_EPOUTBULK_ADDR      (CONFIG_APBRIDGE_EPBULKOUT)
#define APBRIDGE_EPOUTBULK_ATTR      (USB_EP_ATTR_XFER_BULK)

#define APBRIDGE_EPINBULK_ADDR       (USB_DIR_IN|CONFIG_APBRIDGE_EPBULKIN)
#define APBRIDGE_EPINBULK_ATTR       (USB_EP_ATTR_XFER_BULK)

#define APBRIDGE_BULK_MXPACKET       (512)

/* String language */

#define APBRIDGE_STR_LANGUAGE        (0x0409)   /* en-us */

/* Descriptor strings */

#define APBRIDGE_MANUFACTURERSTRID   (1)
#define APBRIDGE_PRODUCTSTRID        (2)
#define APBRIDGE_SERIALSTRID         (3)
#define APBRIDGE_CONFIGSTRID         (4)

/* Buffer big enough for any of our descriptors */

#define APBRIDGE_MXDESCLEN           (64)

/* Vender specific control requests *******************************************/

#define APBRIDGE_RWREQUEST_LOG                  (0x02)
#define APBRIDGE_RWREQUEST_EP_MAPPING           (0x03)
#define APBRIDGE_ROREQUEST_CPORT_COUNT          (0x04)
#define APBRIDGE_WOREQUEST_CPORT_RESET          (0x05)
#define APBRIDGE_ROREQUEST_LATENCY_TAG_EN       (0x06)
#define APBRIDGE_ROREQUEST_LATENCY_TAG_DIS      (0x07)

#define TIMEOUT_IN_MS           300
#define ONE_SEC_IN_MSEC         1000
#define RESET_TIMEOUT_DELAY (TIMEOUT_IN_MS * CLOCKS_PER_SEC) / ONE_SEC_IN_MSEC

/* Misc Macros ****************************************************************/

/* min/max macros */

#ifndef min
#define min(a,b) ((a)<(b)?(a):(b))
#endif

#ifndef max
#define max(a,b) ((a)>(b)?(a):(b))
#endif

/* Total number of endpoints (included setup endpoint) */
#define APBRIDGE_MAX_ENDPOINTS (APBRIDGE_NENDPOINTS + 1)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct apbridge_msg_s {
    struct list_head list;
    struct usbdev_ep_s *ep;
    const void *buf;
    size_t len;
};

/* This structure describes the internal state of the driver */

struct apbridge_dev_s {
    struct usbdev_s *usbdev;    /* usbdev driver pointer */
    struct usbdevclass_driver_s drvr;   /* gadget callback */

    uint8_t config;             /* Configuration number */
    sem_t config_sem;

    struct usbdev_ep_s *ep[APBRIDGE_MAX_ENDPOINTS];

    struct list_head msg_queue;

    int *cport_to_epin_n;
    struct gb_timestamp *ts;
    int epout_to_cport_n[APBRIDGE_NBULKS];

    struct apbridge_usb_driver *driver;
};

typedef uint16_t __le16;
typedef uint8_t __u8;

struct cport_to_ep {
    __le16 cport_id;
    __u8 endpoint_in;
    __u8 endpoint_out;
};

enum ctrlreq_state {
    USB_REQ,
    GREYBUS_LOG,
    GREYBUS_EP_MAPPING,
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* Configuration ***********************************************************/

static int usbclass_mkstrdesc(uint8_t id, struct usb_strdesc_s *strdesc);
static void usbclass_mkepdesc(const struct usb_epdesc_s *indesc,
                              uint16_t mxpacket, struct usb_epdesc_s *outdesc);
static int16_t usbclass_mkcfgdesc(uint8_t * buf, uint8_t speed, uint8_t type);
static void usbclass_resetconfig(struct apbridge_dev_s *priv);
static int usbclass_setconfig(struct apbridge_dev_s *priv, uint8_t config);

/* Completion event handlers ***********************************************/

static void usbclass_ep0incomplete(struct usbdev_ep_s *ep,
                                   struct usbdev_req_s *req);
static void usbclass_rdcomplete(struct usbdev_ep_s *ep,
                                struct usbdev_req_s *req);
static void usbclass_wrcomplete(struct usbdev_ep_s *ep,
                                struct usbdev_req_s *req);

/* USB class device ********************************************************/

static int usbclass_bind(struct usbdevclass_driver_s *driver,
                         struct usbdev_s *dev);
static void usbclass_unbind(struct usbdevclass_driver_s *driver,
                            struct usbdev_s *dev);
static int usbclass_setup(struct usbdevclass_driver_s *driver,
                          struct usbdev_s *dev,
                          const struct usb_ctrlreq_s *ctrl,
                          uint8_t * dataout, size_t outlen);
static void usbclass_disconnect(struct usbdevclass_driver_s *driver,
                                struct usbdev_s *dev);

/****************************************************************************
 * Private Variables
 ****************************************************************************/

/* USB class device ********************************************************/

static const struct usbdevclass_driverops_s g_driverops = {
    usbclass_bind,              /* bind */
    usbclass_unbind,            /* unbind */
    usbclass_setup,             /* setup */
    usbclass_disconnect,        /* disconnect */
    NULL,                       /* suspend */
    NULL,                       /* resume */
};

/* USB descriptor templates these will be copied and modified **************/

static const struct usb_devdesc_s g_devdesc = {
    USB_SIZEOF_DEVDESC,         /* len */
    USB_DESC_TYPE_DEVICE,       /* type */
    {LSBYTE(0x0200), MSBYTE(0x0200)},   /* usb */
    USB_CLASS_PER_INTERFACE,    /* classid */
    0,                          /* subclass */
    0,                          /* protocol */
    CONFIG_APBRIDGE_EP0MAXPACKET,       /* maxpacketsize */
    {LSBYTE(CONFIG_APBRIDGE_VENDORID),  /* vendor */
     MSBYTE(CONFIG_APBRIDGE_VENDORID)},
    {LSBYTE(CONFIG_APBRIDGE_PRODUCTID), /* product */
     MSBYTE(CONFIG_APBRIDGE_PRODUCTID)},
    {LSBYTE(APBRIDGE_VERSIONNO),        /* device */
     MSBYTE(APBRIDGE_VERSIONNO)},
    APBRIDGE_MANUFACTURERSTRID, /* imfgr */
    APBRIDGE_PRODUCTSTRID,      /* iproduct */
    APBRIDGE_SERIALSTRID,       /* serno */
    APBRIDGE_NCONFIGS           /* nconfigs */
};

static const struct usb_cfgdesc_s g_cfgdesc = {
    USB_SIZEOF_CFGDESC,         /* len */
    USB_DESC_TYPE_CONFIG,       /* type */
    {0, 0},                     /* totallen -- to be provided */
    APBRIDGE_NINTERFACES,       /* ninterfaces */
    APBRIDGE_CONFIGID,          /* cfgvalue */
    APBRIDGE_CONFIGSTRID,       /* icfg */
    APBRIDGE_CONFIG_ATTR,       /* attr */
    0                           /* mxpower */
};

static const struct usb_ifdesc_s g_ifdesc = {
    USB_SIZEOF_IFDESC,          /* len */
    USB_DESC_TYPE_INTERFACE,    /* type */
    0,                          /* ifno */
    0,                          /* alt */
    APBRIDGE_NENDPOINTS,        /* neps */
    USB_CLASS_VENDOR_SPEC,      /* classid */
    0,                          /* subclass */
    0,                          /* protocol */
    APBRIDGE_CONFIGSTRID        /* iif */
};

static const struct usb_epdesc_s g_epbulkoutdesc = {
    USB_SIZEOF_EPDESC,          /* len */
    USB_DESC_TYPE_ENDPOINT,     /* type */
    APBRIDGE_EPOUTBULK_ADDR,    /* addr */
    APBRIDGE_EPOUTBULK_ATTR,    /* attr */
    {LSBYTE(APBRIDGE_BULK_MXPACKET),
     MSBYTE(APBRIDGE_BULK_MXPACKET)},   /* maxpacket -- might change to 512 */
    0                           /* interval */
};

static const struct usb_epdesc_s g_epbulkindesc = {
    USB_SIZEOF_EPDESC,          /* len */
    USB_DESC_TYPE_ENDPOINT,     /* type */
    APBRIDGE_EPINBULK_ADDR,     /* addr */
    APBRIDGE_EPINBULK_ATTR,     /* attr */
    {LSBYTE(APBRIDGE_BULK_MXPACKET),
     MSBYTE(APBRIDGE_BULK_MXPACKET)},   /* maxpacket -- might change to 512 */
    0                           /* interval */
};

static const struct usb_epdesc_s *g_usbdesc[APBRIDGE_MAX_ENDPOINTS] = {
    NULL,               /* No descriptor for ep0 */
    &g_epbulkindesc,
    &g_epbulkoutdesc,
};

static const struct usb_qualdesc_s g_qualdesc = {
    USB_SIZEOF_QUALDESC,        /* len */
    USB_DESC_TYPE_DEVICEQUALIFIER,      /* type */
    {LSBYTE(0x0200), MSBYTE(0x0200)},   /* USB */
    USB_CLASS_PER_INTERFACE,    /* classid */
    0,                          /* subclass */
    0,                          /* protocol */
    CONFIG_APBRIDGE_EP0MAXPACKET,       /* mxpacketsize */
    APBRIDGE_NCONFIGS,          /* nconfigs */
    0,                          /* reserved */
};

static inline
struct apbridge_dev_s *driver_to_apbridge(struct usbdevclass_driver_s *drv)
{
    return CONTAINER_OF(drv, struct apbridge_dev_s, drvr);
}

static inline
struct apbridge_dev_s *usbdev_to_apbridge(struct usbdev_s *dev)
{
    return dev->ep0->priv;
}

static inline struct apbridge_dev_s *ep_to_apbridge(struct usbdev_ep_s *ep)
{
    return (struct apbridge_dev_s *)ep->priv;
}

/**
 * @brief Wait until usb connection has been established
 * USB driver is not fully initialized until enumeration is done.
 * This method will block caller USB is working.
 * @param priv usb device.
 */

void usb_wait(struct apbridge_dev_s *priv)
{
    sem_wait(&priv->config_sem);
}

static unsigned int get_cportid(const struct gb_operation_hdr *hdr)
{
    return hdr->pad[0];
}

static int apbridge_queue(struct apbridge_dev_s *priv, struct usbdev_ep_s *ep,
                          const void *payload, size_t len)
{
    irqstate_t flags;
    struct apbridge_msg_s *info;

    info = malloc(sizeof(*info));
    if (!info) {
        return -ENOMEM;
    }

    info->ep = ep;
    info->buf = payload;
    info->len = len;

    flags = irqsave();
    list_add(&priv->msg_queue, &info->list);
    irqrestore(flags);

    return OK;
}

static struct apbridge_msg_s *apbridge_dequeue(struct apbridge_dev_s *priv)
{
    irqstate_t flags;
    struct list_head *list;

    flags = irqsave();
    if (list_is_empty(&priv->msg_queue)) {
        irqrestore(flags);
        return NULL;
    }

    list = priv->msg_queue.next;
    list_del(list);
    irqrestore(flags);
    return list_entry(list, struct apbridge_msg_s, list);
}

static int _to_usb_submit(struct usbdev_ep_s *ep, struct usbdev_req_s *req,
                          const void *payload, size_t len)
{
    struct apbridge_dev_s *priv;
    int ret;
    unsigned int cportid;

    priv = ep->priv;
    req->len = len;
    req->flags = USBDEV_REQFLAGS_NULLPKT;

    cportid = get_cportid(payload);

    memcpy(req->buf, payload, len);
    gb_timestamp_tag_exit_time(&priv->ts[cportid], cportid);
    gb_timestamp_log(&priv->ts[cportid], cportid, req->buf, len,
                          GREYBUS_FW_TIMESTAMP_APBRIDGE);

    unipro_unpause_rx(cportid);

    /* Then submit the request to the endpoint */

    ret = EP_SUBMIT(ep, req);
    if (ret != OK) {
        usbtrace(TRACE_CLSERROR(USBSER_TRACEERR_SUBMITFAIL), (uint16_t) - ret);
        return ret;
    }

    return 0;
}

/**
 * @brief Send incoming data from unipro to AP module
 * priv usb device.
 * param payload data to send from SVC
 * size of data to send on unipro
 * @return 0 in success or -EINVAL if len is too big
 */

int unipro_to_usb(struct apbridge_dev_s *priv, unsigned int cportid,
                  const void *payload, size_t len)
{
    uint8_t epno;
    struct usbdev_ep_s *ep;
    struct usbdev_req_s *req;
    struct gb_operation_hdr *hdr = (void *)payload;

    if (len > APBRIDGE_REQ_SIZE)
        return -EINVAL;

    /* Store the cport id in the header pad bytes. */
    hdr->pad[0] = cportid & 0xff;

    epno = priv->cport_to_epin_n[cportid];
    ep = priv->ep[epno & USB_EPNO_MASK];
    req = get_request(ep, usbclass_wrcomplete, APBRIDGE_REQ_SIZE, NULL);
    if (!req) {
        return apbridge_queue(priv, ep, payload, len);
    }

    return _to_usb_submit(ep, req, payload, len);
}

int usb_release_buffer(struct apbridge_dev_s *priv, const void *buf)
{
    struct usbdev_ep_s *ep;
    struct usbdev_req_s *req;
    int ret = 0;


    req = find_request_by_priv(buf);
    if (!req) {
        return -EINVAL;
    }

    ep = request_to_ep(req);
    ret = EP_SUBMIT(ep, req);
    if (ret != OK) {
        usbtrace(TRACE_CLSERROR(USBSER_TRACEERR_RDSUBMIT),
                 (uint16_t) -ret);
    }
    return ret;
}

void map_cport_to_ep(struct apbridge_dev_s *priv,
                     struct cport_to_ep *cport_to_ep)
{
    unsigned int cportid = le16_to_cpu(cport_to_ep->cport_id);
    uint8_t ep_out = cport_to_ep->endpoint_out - CONFIG_APBRIDGE_EPBULKOUT;

    priv->cport_to_epin_n[cportid] = cport_to_ep->endpoint_in;
    priv->epout_to_cport_n[ep_out >> 1] = cportid;
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: usbclass_mkstrdesc
 *
 * Description:
 *   Construct a string descriptor
 *
 ****************************************************************************/

static int usbclass_mkstrdesc(uint8_t id, struct usb_strdesc_s *strdesc)
{
    const char *str;
    int len;
    int ndata;
    int i;

    switch (id) {
    case 0:
        {
            /* Descriptor 0 is the language id */

            strdesc->len = 4;
            strdesc->type = USB_DESC_TYPE_STRING;
            strdesc->data[0] = LSBYTE(APBRIDGE_STR_LANGUAGE);
            strdesc->data[1] = MSBYTE(APBRIDGE_STR_LANGUAGE);
            return 4;
        }

    case APBRIDGE_MANUFACTURERSTRID:
        str = CONFIG_APBRIDGE_VENDORSTR;
        break;

    case APBRIDGE_PRODUCTSTRID:
        str = CONFIG_APBRIDGE_PRODUCTSTR;
        break;

    case APBRIDGE_SERIALSTRID:
        str = CONFIG_APBRIDGE_SERIALSTR;
        break;

    case APBRIDGE_CONFIGSTRID:
        str = CONFIG_APBRIDGE_CONFIGSTR;
        break;

    default:
        return -EINVAL;
    }

    /* The string is utf16-le.  The poor man's utf-8 to utf16-le
     * conversion below will only handle 7-bit en-us ascii
     */

    len = strlen(str);
    for (i = 0, ndata = 0; i < len; i++, ndata += 2) {
        strdesc->data[ndata] = str[i];
        strdesc->data[ndata + 1] = 0;
    }

    strdesc->len = ndata + 2;
    strdesc->type = USB_DESC_TYPE_STRING;
    return strdesc->len;
}

/****************************************************************************
 * Name: usbclass_mkepdesc
 *
 * Description:
 *   Construct the endpoint descriptor
 *
 ****************************************************************************/

static inline void usbclass_mkepdesc(const struct usb_epdesc_s *indesc,
                                     uint16_t mxpacket,
                                     struct usb_epdesc_s *outdesc)
{
    /* Copy the canned descriptor */

    memcpy(outdesc, indesc, USB_SIZEOF_EPDESC);

    /* Then add the correct max packet size */

    outdesc->mxpacketsize[0] = LSBYTE(mxpacket);
    outdesc->mxpacketsize[1] = MSBYTE(mxpacket);
}

/****************************************************************************
 * Name: usbclass_mkcfgdesc
 *
 * Description:
 *   Construct the configuration descriptor
 *
 ****************************************************************************/

static int16_t usbclass_mkcfgdesc(uint8_t * buf, uint8_t speed, uint8_t type)
{
    int i;
    struct usb_cfgdesc_s *cfgdesc = (struct usb_cfgdesc_s *)buf;
    bool hispeed = (speed == USB_SPEED_HIGH);
    uint16_t mxpacket;
    uint16_t totallen;

    /* This is the total length of the configuration (not necessarily the
     * size that we will be sending now.
     */

    totallen =
        USB_SIZEOF_CFGDESC + USB_SIZEOF_IFDESC +
        APBRIDGE_NENDPOINTS * USB_SIZEOF_EPDESC;

    /* Configuration descriptor -- Copy the canned descriptor and fill in the
     * type (we'll also need to update the size below
     */

    memcpy(cfgdesc, &g_cfgdesc, USB_SIZEOF_CFGDESC);
    buf += USB_SIZEOF_CFGDESC;

    /*  Copy the canned interface descriptor */

    memcpy(buf, &g_ifdesc, USB_SIZEOF_IFDESC);
    buf += USB_SIZEOF_IFDESC;

    /* Make the three endpoint configurations.  First, check for switches
     * between high and full speed
     */

    if (type == USB_DESC_TYPE_OTHERSPEEDCONFIG) {
        hispeed = !hispeed;
    }

    for (i = 1; i < APBRIDGE_MAX_ENDPOINTS; i++) {
        if (hispeed) {
            mxpacket = GETUINT16(g_usbdesc[i]->mxpacketsize);
        } else {
            mxpacket = 64;
        }
        usbclass_mkepdesc(g_usbdesc[i],
                          mxpacket, (struct usb_epdesc_s *)buf);
        buf += USB_SIZEOF_EPDESC;
    }

    /* Finally, fill in the total size of the configuration descriptor */

    cfgdesc->totallen[0] = LSBYTE(totallen);
    cfgdesc->totallen[1] = MSBYTE(totallen);
    return totallen;
}

/****************************************************************************
 * Name: usbclass_resetconfig
 *
 * Description:
 *   Mark the device as not configured and disable all endpoints.
 *
 ****************************************************************************/

static void usbclass_resetconfig(struct apbridge_dev_s *priv)
{
    int i;

    /* Are we configured? */

    if (priv->config != APBRIDGE_CONFIGIDNONE) {
        /* Yes.. but not anymore */

        priv->config = APBRIDGE_CONFIGIDNONE;

        /* Disable endpoints.  This should force completion of all pending
         * transfers.
         */

        for (i = 1; i < APBRIDGE_MAX_ENDPOINTS; i++)
            EP_DISABLE(priv->ep[i]);
    }
}

/****************************************************************************
 * Name: usbclass_setconfig
 *
 * Description:
 *   Set the device configuration by allocating and configuring endpoints and
 *   by allocating and queue read and write requests.
 *
 ****************************************************************************/

static int usbclass_setconfig(struct apbridge_dev_s *priv, uint8_t config)
{
    struct usbdev_req_s *req;
    struct usb_epdesc_s epdesc;
    uint16_t mxpacket;
    int i, j;
    int ret = 0;

#if CONFIG_DEBUG
    if (priv == NULL) {
        usbtrace(TRACE_CLSERROR(USBSER_TRACEERR_INVALIDARG), 0);
        return -EIO;
    }
#endif

    if (config == priv->config) {
        /* Already configured -- Do nothing */

        usbtrace(TRACE_CLSERROR(USBSER_TRACEERR_ALREADYCONFIGURED), 0);
        return 0;
    }

    /* Discard the previous configuration data */

    usbclass_resetconfig(priv);

    /* Was this a request to simply discard the current configuration? */

    if (config == APBRIDGE_CONFIGIDNONE) {
        usbtrace(TRACE_CLSERROR(USBSER_TRACEERR_CONFIGNONE), 0);
        return 0;
    }

    /* We only accept one configuration */

    if (config != APBRIDGE_CONFIGID) {
        usbtrace(TRACE_CLSERROR(USBSER_TRACEERR_CONFIGIDBAD), 0);
        return -EINVAL;
    }

    for (i = 1; i < APBRIDGE_MAX_ENDPOINTS; i++) {
        if (priv->usbdev->speed == USB_SPEED_HIGH) {
            mxpacket = GETUINT16(g_usbdesc[i]->mxpacketsize);
        } else {
            mxpacket = 64;
        }
        usbclass_mkepdesc(g_usbdesc[i], mxpacket, &epdesc);
        ret = EP_CONFIGURE(priv->ep[i], &epdesc, false);
        if (ret < 0) {
            usbtrace(TRACE_CLSERROR(USBSER_TRACEERR_EPINTINCONFIGFAIL), 0);
            goto errout;
        }
    }

    /* Queue read requests in the bulk OUT endpoint */
    for (i = 0; i < APBRIDGE_NBULKS; i++) {
        for (j = 0; j < APBRIDGE_NREQS; j++) {
            struct usbdev_ep_s *ep;

            ep = priv->ep[CONFIG_APBRIDGE_EPBULKOUT + i * 2];
            req = get_request(ep, usbclass_rdcomplete,
                              APBRIDGE_REQ_SIZE, NULL);
            request_set_priv(req, req->buf);
            ret = EP_SUBMIT(ep, req);

            if (ret != OK) {
                usbtrace(TRACE_CLSERROR(USBSER_TRACEERR_RDSUBMIT), (uint16_t) - ret);
                goto errout;
            }
        }
    }

    /* We are successfully configured */

    priv->config = config;
    sem_post(&priv->config_sem);

    return OK;

 errout:
    usbclass_resetconfig(priv);
    return ret;
}

/****************************************************************************
 * Name: usbclass_ep0incomplete
 *
 * Description:
 *   Handle completion of EP0 control operations
 *
 ****************************************************************************/

static void usbclass_ep0incomplete(struct usbdev_ep_s *ep,
                                   struct usbdev_req_s *req)
{
    struct apbridge_dev_s *priv;
    int *req_priv;

    priv = ep_to_apbridge(ep);

    if (req->result || req->xfrd != req->len) {
        usbtrace(TRACE_CLSERROR(USBSER_TRACEERR_REQRESULT),
                 (uint16_t) - req->result);
    }

    req_priv = (int *)request_get_priv(req);
    if (req_priv) {
        if (*req_priv == GREYBUS_EP_MAPPING)
            map_cport_to_ep(priv, (struct cport_to_ep *)req->buf);
        kmm_free(req_priv);
    }
    put_request(req);
}

static void usbdclass_log_rx_time(struct apbridge_dev_s *priv, unsigned int cportid)
{
    gb_timestamp_tag_entry_time(&priv->ts[cportid], cportid);
}

/****************************************************************************
 * Name: usbclass_rdcomplete
 *
 * Description:
 *   Handle completion of read request on the bulk OUT endpoint.  This
 *   is handled like the receipt of serial data on the "UART"
 *
 ****************************************************************************/

static void usbclass_rdcomplete(struct usbdev_ep_s *ep,
                                struct usbdev_req_s *req)
{
    struct apbridge_dev_s *priv;
    struct apbridge_usb_driver *drv;
    struct gb_operation_hdr *hdr;
    int ep_n;
    unsigned int cportid;

    /* Sanity check */

#ifdef CONFIG_DEBUG
    if (!ep || !ep->priv || !req) {
        usbtrace(TRACE_CLSERROR(USBSER_TRACEERR_INVALIDARG), 0);
        return;
    }
#endif

    /* Extract references to private data */

    priv = ep_to_apbridge(ep);
    drv = priv->driver;

    /* Process the received data unless this is some unusual condition */

    switch (req->result) {
    case OK:                    /* Normal completion */
        usbtrace(TRACE_CLASSRDCOMPLETE, 0);
        ep_n = BULKEP_TO_N(ep);
        hdr = (struct gb_operation_hdr *)req->buf;
        /* Legacy ep: copy from payload cportid */

        if (ep_n != 0) {
            cportid = priv->epout_to_cport_n[ep_n];
            hdr->pad[0] = cportid & 0xff;
        }

        /*
         * Retreive and clear the cport id stored in the header pad bytes.
         */
        cportid = hdr->pad[0];
        hdr->pad[0] = 0;

        usbdclass_log_rx_time(priv, cportid);
        drv->usb_to_unipro(priv, cportid, req->buf , req->xfrd);
        break;

    case -ESHUTDOWN:           /* Disconnection */
        usbtrace(TRACE_CLSERROR(USBSER_TRACEERR_RDSHUTDOWN), 0);
        return;

    default:                   /* Some other error occurred */
        usbtrace(TRACE_CLSERROR(USBSER_TRACEERR_RDUNEXPECTED),
                 (uint16_t) - req->result);
        break;
    };
}

static void usbclass_wrcomplete(struct usbdev_ep_s *ep,
                              struct usbdev_req_s *req)
{
    struct apbridge_msg_s *info;
    struct apbridge_dev_s *priv;

    /* Sanity check */
#ifdef CONFIG_DEBUG
    if (!ep || !ep->priv || !req || !req->priv) {
        usbtrace(TRACE_CLSERROR(USBSER_TRACEERR_INVALIDARG), 0);
        return;
    }
#endif

    priv = ep_to_apbridge(ep);
    info = apbridge_dequeue(priv);
    if (info) {
        _to_usb_submit(info->ep, req, info->buf, info->len);
        free(info);
    } else {
        put_request(req);
    }

    switch (req->result) {
    case OK:                   /* Normal completion */
        usbtrace(TRACE_CLASSWRCOMPLETE, 0);
        break;

    case -ESHUTDOWN:           /* Disconnection */
        usbtrace(TRACE_CLSERROR(USBSER_TRACEERR_WRSHUTDOWN), 0);
        break;

    default:                   /* Some other error occurred */
        usbtrace(TRACE_CLSERROR(USBSER_TRACEERR_WRUNEXPECTED),
                 (uint16_t) - req->result);
        break;
    }
}

/****************************************************************************
 * USB Class Driver Methods
****************************************************************************/
typedef void (*usb_callback)(FAR struct usbdev_ep_s *ep, FAR
                             struct usbdev_req_s *req);

static int allocep(struct apbridge_dev_s *priv,
                    const struct usb_epdesc_s *desc)
{
    int in;
    uint8_t epno;
    struct usbdev_ep_s *ep;

    if (desc) {
        in = desc->addr & USB_DIR_IN ? 1 : 0;
        epno = desc->addr & USB_EPNO_MASK;
        DEBUGASSERT(epno < APBRIDGE_MAX_ENDPOINTS);

        ep = DEV_ALLOCEP(priv->usbdev, epno, in, desc->type);
        if (!ep) {
            usbtrace(TRACE_CLSERROR(USBSER_TRACEERR_EPINTINALLOCFAIL), 0);
            return -ENODEV;
        }
        priv->ep[epno] = ep;
    } else {
        ep = priv->usbdev->ep0;
        priv->ep[0] = ep;
    }
    ep->priv = priv;

    return OK;
}

static void freeep(struct apbridge_dev_s *priv, uint8_t epno)
{
    DEBUGASSERT(epno < APBRIDGE_MAX_ENDPOINTS);
    if (epno == 0 || !priv->ep[epno])
        return;

    DEV_FREEEP(priv->usbdev, priv->ep[epno]);
    priv->ep[epno] = NULL;
}

static void g_usbdesc_init(void) {
    int i;
    struct usb_epdesc_s *usbdesc;

    for (i = 0; i < APBRIDGE_NBULKS; i++) {
        usbdesc = malloc(sizeof(struct usb_epdesc_s));
        memcpy(usbdesc, &g_epbulkoutdesc, sizeof(struct usb_epdesc_s));
        usbdesc->addr += i * 2;
        g_usbdesc[i * 2 + CONFIG_APBRIDGE_EPBULKOUT] = usbdesc;

        usbdesc = malloc(sizeof(struct usb_epdesc_s));
        memcpy(usbdesc, &g_epbulkindesc, sizeof(struct usb_epdesc_s));
        usbdesc->addr += i * 2;
        g_usbdesc[i * 2 + CONFIG_APBRIDGE_EPBULKIN] = usbdesc;
    }
}

/****************************************************************************
 * Name: usbclass_bind
 *
 * Description:
 *   Invoked when the driver is bound to a USB device driver
 *
 ****************************************************************************/

static int usbclass_bind(struct usbdevclass_driver_s *driver,
                         struct usbdev_s *dev)
{
    struct apbridge_dev_s *priv = driver_to_apbridge(driver);
    int ret;
    int i;

    usbtrace(TRACE_CLASSBIND, 0);

    /* Bind the structures */

    priv->usbdev = dev;

     g_usbdesc_init();

    /* Allocate endpoints */

    for (i = 0; i < APBRIDGE_MAX_ENDPOINTS; i++) {
        ret = allocep(priv, g_usbdesc[i]);
        if (ret < 0)
            goto error;
    }

    /* Pre-allocate all endpoints... the endpoints will not be functional
     * until the SET CONFIGURATION request is processed in usbclass_setconfig.
     * This is done here because there may be calls to kmm_malloc and the SET
     * CONFIGURATION processing probably occurrs within interrupt handling
     * logic where kmm_malloc calls will fail.
     */

    request_pool_prealloc(priv->ep[0], APBRIDGE_MXDESCLEN, 1);
    request_pool_prealloc(priv->ep[CONFIG_APBRIDGE_EPBULKOUT],
                          APBRIDGE_REQ_SIZE, APBRIDGE_NREQS * APBRIDGE_NBULKS);
    request_pool_prealloc(priv->ep[CONFIG_APBRIDGE_EPBULKIN],
                          APBRIDGE_REQ_SIZE, APBRIDGE_NREQS * APBRIDGE_NBULKS);

    /* TODO test result of prealloc */

    /* Report if we are selfpowered */

    DEV_SETSELFPOWERED(dev);

    /* And pull-up the data line for the soft connect function */

    DEV_CONNECT(dev);

    return OK;

error:
    usbclass_unbind(driver, dev);
    return ret;
}

/****************************************************************************
 * Name: usbclass_unbind
 *
 * Description:
 *    Invoked when the driver is unbound from a USB device driver
 *
 ****************************************************************************/

static void usbclass_unbind(struct usbdevclass_driver_s *driver,
                            struct usbdev_s *dev)
{
    int i;
    struct apbridge_dev_s *priv;

    usbtrace(TRACE_CLASSUNBIND, 0);

#ifdef CONFIG_DEBUG
    if (!driver || !dev || !dev->ep0) {
        usbtrace(TRACE_CLSERROR(USBSER_TRACEERR_INVALIDARG), 0);
        return;
    }
#endif

    /* Extract reference to private data */

    priv = driver_to_apbridge(driver);

#ifdef CONFIG_DEBUG
    if (!priv) {
        usbtrace(TRACE_CLSERROR(USBSER_TRACEERR_EP0NOTBOUND), 0);
        return;
    }
#endif

    /* Make sure that we are not already unbound */

    if (priv != NULL) {
        /* Make sure that the endpoints have been unconfigured.  If
         * we were terminated gracefully, then the configuration should
         * already have been reset.  If not, then calling usbclass_resetconfig
         * should cause the endpoints to immediately terminate all
         * transfers and return the requests to us (with result == -ESHUTDOWN)
         */

        usbclass_resetconfig(priv);
        up_mdelay(50);

        /* Free the pre-allocated request */

        request_pool_freeall();

        for (i = 0; i < APBRIDGE_MAX_ENDPOINTS; i++) {
            freeep(priv, i);
        }

    }
}

#if defined(CONFIG_APB_USB_LOG)
static struct log_buffer *g_lb;

static ssize_t usb_log_read(struct file *filep, char *buffer, size_t buflen)
{
    return 0;
}

static ssize_t usb_log_write(struct file *filep, const char *buffer,
                                 size_t buflen)
{
    return log_buffer_write(g_lb, buffer, buflen);
}

static const struct file_operations usb_log_ops = {
    .read = usb_log_read,
    .write = usb_log_write,
};

void usb_log_init(void)
{
    g_lb = log_buffer_alloc(CONFIG_USB_LOG_BUFFER_SIZE);
    if (g_lb)
        register_driver("/dev/console", &usb_log_ops,
                        CONFIG_USB_LOG_BUFFER_SIZE, NULL);
}

void usb_putc(int c)
{
    if (g_lb)
        log_buffer_write(g_lb, &c, 1);
}

int usb_get_log(void *buf, int len)
{
    return log_buffer_readlines(g_lb, buf, len);
}
#endif

struct cport_reset_priv {
    struct usbdev_req_s *req;
    struct usbdev_ep_s *ep0;
    struct wdog_s timeout_wd;
};

static void cport_reset_cb(unsigned int cportid, void *data)
{
    struct cport_reset_priv *priv = data;
    int ret;

    priv->req->len = 0;
    priv->req->flags = USBDEV_REQFLAGS_NULLPKT;

    ret = EP_SUBMIT(priv->ep0, priv->req);
    if (ret < 0) {
        usbclass_ep0incomplete(priv->ep0, priv->req);
    }
}

static void cport_reset_timeout(int argc, uint32_t data, ...)
{
    struct cport_reset_priv *priv = (struct cport_reset_priv*) data;

    if (argc != 1)
        return;

    wd_delete(&priv->timeout_wd);
    usbclass_ep0incomplete(priv->ep0, priv->req);

    free(priv);
}

static int reset_cport(unsigned int cportid, struct usbdev_req_s *req,
                       struct usbdev_ep_s *ep0)
{
    struct cport_reset_priv *priv;

    priv = zalloc(sizeof(*priv));
    if (!priv) {
        return -ENOMEM;
    }

    wd_static(&priv->timeout_wd);
    priv->req = req;
    priv->ep0 = ep0;

    wd_start(&priv->timeout_wd, RESET_TIMEOUT_DELAY, cport_reset_timeout, 1,
             priv);
    return unipro_reset_cport(cportid, cport_reset_cb, priv);
}

/****************************************************************************
 * Name: usbclass_setup
 *
 * Description:
 *   Invoked for ep0 control requests.  This function probably executes
 *   in the context of an interrupt handler.
 *
 ****************************************************************************/

static int usbclass_setup(struct usbdevclass_driver_s *driver,
                          struct usbdev_s *dev,
                          const struct usb_ctrlreq_s *ctrl,
                          uint8_t * dataout, size_t outlen)
{
    struct apbridge_dev_s *priv;
    struct usbdev_req_s *req;
    int *req_priv;

    uint16_t value;
    uint16_t index;
    uint16_t len;
    int ret = -EOPNOTSUPP;
    bool do_not_send_response = false;

#ifdef CONFIG_DEBUG
    if (!driver || !dev || !dev->ep0 || !ctrl) {
        usbtrace(TRACE_CLSERROR(USBSER_TRACEERR_INVALIDARG), 0);
        return -EIO;
    }
#endif

    /* Extract reference to private data */

    usbtrace(TRACE_CLASSSETUP, ctrl->req);
    priv = driver_to_apbridge(driver);

#ifdef CONFIG_DEBUG
    if (!priv) {
        usbtrace(TRACE_CLSERROR(USBSER_TRACEERR_EP0NOTBOUND), 0);
        return -ENODEV;
    }
#endif
    req_priv = kmm_malloc(sizeof(*req_priv));
    if (!req_priv) {
        return -ENOMEM;
    }
    req = get_request(dev->ep0, usbclass_ep0incomplete,
                      APBRIDGE_MXDESCLEN, req_priv);
    if (!req) {
        lowsyslog("%s(): unable to get a request buffer\n", __func__);
        return -ENOMEM;
    }

    *req_priv = USB_REQ;

    /* Extract the little-endian 16-bit values to host order */

    value = GETUINT16(ctrl->value);
    index = GETUINT16(ctrl->index);
    len = GETUINT16(ctrl->len);

    uvdbg("type=%02x req=%02x value=%04x index=%04x len=%04x\n",
          ctrl->type, ctrl->req, value, index, len);

    switch (ctrl->type & USB_REQ_TYPE_MASK) {
     /***********************************************************************
      * Standard Requests
      ***********************************************************************/

    case USB_REQ_TYPE_STANDARD:
        {
            switch (ctrl->req) {
            case USB_REQ_GETDESCRIPTOR:
                {
                    /* The value field specifies the descriptor type in the MS byte and the
                     * descriptor index in the LS byte (order is little endian)
                     */

                    switch (ctrl->value[1]) {
                    case USB_DESC_TYPE_DEVICE:
                        {
                            ret = USB_SIZEOF_DEVDESC;
                            memcpy(req->buf, &g_devdesc, ret);
                        }
                        break;

                    case USB_DESC_TYPE_DEVICEQUALIFIER:
                        {
                            ret = USB_SIZEOF_QUALDESC;
                            memcpy(req->buf, &g_qualdesc, ret);
                        }
                        break;

                    case USB_DESC_TYPE_OTHERSPEEDCONFIG:
                    case USB_DESC_TYPE_CONFIG:
                        {
                            ret =
                                usbclass_mkcfgdesc(req->buf, dev->speed,
                                                   ctrl->req);
                        }
                        break;

                    case USB_DESC_TYPE_STRING:
                        {
                            /* index == language code. */

                            ret =
                                usbclass_mkstrdesc(ctrl->value[0],
                                                   (struct usb_strdesc_s *)
                                                   req->buf);
                        }
                        break;

                    default:
                        {
                            usbtrace(TRACE_CLSERROR
                                     (USBSER_TRACEERR_GETUNKNOWNDESC), value);
                        }
                        break;
                    }
                }
                break;

            case USB_REQ_SETCONFIGURATION:
                {
                    if (ctrl->type == 0) {
                        ret = usbclass_setconfig(priv, value);
                    }
                }
                break;

            case USB_REQ_GETCONFIGURATION:
                {
                    if (ctrl->type == USB_DIR_IN) {
                        *(uint8_t *) req->buf = priv->config;
                        ret = 1;
                    }
                }
                break;

            case USB_REQ_SETINTERFACE:
                {
                    if (ctrl->type == USB_REQ_RECIPIENT_INTERFACE) {
                        if (priv->config == APBRIDGE_CONFIGID &&
                            index == APBRIDGE_INTERFACEID &&
                            value == APBRIDGE_ALTINTERFACEID) {
                            usbclass_resetconfig(priv);
                            usbclass_setconfig(priv, priv->config);
                            ret = 0;
                        }
                    }
                }
                break;

            case USB_REQ_GETINTERFACE:
                {
                    if (ctrl->type ==
                        (USB_DIR_IN | USB_REQ_RECIPIENT_INTERFACE)
                        && priv->config == APBRIDGE_CONFIGIDNONE) {
                        if (index != APBRIDGE_INTERFACEID) {
                            ret = -EDOM;
                        } else {
                            *(uint8_t *) req->buf =
                                APBRIDGE_ALTINTERFACEID;
                            ret = 1;
                        }
                    }
                }
                break;

            default:
                usbtrace(TRACE_CLSERROR(USBSER_TRACEERR_UNSUPPORTEDSTDREQ),
                         ctrl->req);
                break;
            }
        }
        break;

        /* Put here vendor request */
    case USB_REQ_TYPE_VENDOR:
        {
            if ((ctrl->type & USB_REQ_RECIPIENT_MASK) ==
                USB_REQ_RECIPIENT_INTERFACE) {
                if (ctrl->req == APBRIDGE_RWREQUEST_LOG) {
                    if ((ctrl->type & USB_DIR_IN) == 0) {
                    } else {
#if defined(CONFIG_APB_USB_LOG)
                        *req_priv = GREYBUS_LOG;
                        ret = usb_get_log(req->buf, len);
#else
                        ret = 0;
#endif
                    }
                } else if (ctrl->req == APBRIDGE_RWREQUEST_EP_MAPPING) {
                    if ((ctrl->type & USB_DIR_IN) != 0) {
                        *(uint32_t *) req->buf = 0xdeadbeef;
                        ret = 4;
                    } else {
                        *req_priv = GREYBUS_EP_MAPPING;
                        ret = len;
                    }
                } else if (ctrl->req == APBRIDGE_ROREQUEST_CPORT_COUNT) {
                    if ((ctrl->type & USB_DIR_IN) != 0) {
                        *(uint16_t *) req->buf =
                            cpu_to_le16(unipro_cport_count());
                        ret = sizeof(uint16_t);
                    } else {
                        ret = -EINVAL;
                    }
                } else if (ctrl->req == APBRIDGE_WOREQUEST_CPORT_RESET) {
                    if (!(ctrl->type & USB_DIR_IN)) {
                        ret = reset_cport(value, req, dev->ep0);
                        if (!ret)
                            do_not_send_response = true;
                    } else {
                        ret = -EINVAL;
                    }
                } else if (ctrl->req == APBRIDGE_ROREQUEST_LATENCY_TAG_EN) {
                    if ((ctrl->type & USB_DIR_IN) != 0) {
                        ret = -EINVAL;
                    } else {
                        ret = -EINVAL;
                        if (value < unipro_cport_count()) {
                            priv->ts[value].tag = true;
                            ret = 0;
                            lldbg("enable tagging for cportid %d\n", value);
                        }
                    }
                } else if (ctrl->req == APBRIDGE_ROREQUEST_LATENCY_TAG_DIS) {
                    if ((ctrl->type & USB_DIR_IN) != 0) {
                        ret = -EINVAL;
                    } else {
                        ret = -EINVAL;
                        if (value < unipro_cport_count()) {
                            priv->ts[value].tag = false;
                            ret = 0;
                            lldbg("disable tagging for cportid %d\n", value);
                        }
                    }
                } else {
                    usbtrace(TRACE_CLSERROR
                             (USBSER_TRACEERR_UNSUPPORTEDCLASSREQ),
                             ctrl->type);
                }
            }
        }
        break;

    default:
        usbtrace(TRACE_CLSERROR(USBSER_TRACEERR_UNSUPPORTEDTYPE), ctrl->type);
        break;
    }

    /* Respond to the setup command if data was returned.  On an error return
     * value (ret < 0), the USB driver will stall.
     */

    if (ret >= 0 && !do_not_send_response) {
        req->len = min(len, ret);
        req->flags = USBDEV_REQFLAGS_NULLPKT;
        ret = EP_SUBMIT(dev->ep0, req);
        if (ret < 0) {
            usbtrace(TRACE_CLSERROR(USBSER_TRACEERR_EPRESPQ),
                     (uint16_t) - ret);
            req->result = OK;
        }
    }

    if (ret < 0) {
         usbclass_ep0incomplete(dev->ep0, req);
    }

    return ret;
}

/****************************************************************************
 * Name: usbclass_disconnect
 *
 * Description:
 *   Invoked after all transfers have been stopped, when the host is
 *   disconnected.  This function is probably called from the context of an
 *   interrupt handler.
 *
 ****************************************************************************/

static void usbclass_disconnect(struct usbdevclass_driver_s *driver,
                                struct usbdev_s *dev)
{
    struct apbridge_dev_s *priv;
    irqstate_t flags;

    usbtrace(TRACE_CLASSDISCONNECT, 0);

#ifdef CONFIG_DEBUG
    if (!driver || !dev || !dev->ep0) {
        usbtrace(TRACE_CLSERROR(USBSER_TRACEERR_INVALIDARG), 0);
        return;
    }
#endif

    /* Extract reference to private data */

    priv = driver_to_apbridge(driver);

#ifdef CONFIG_DEBUG
    if (!priv) {
        usbtrace(TRACE_CLSERROR(USBSER_TRACEERR_EP0NOTBOUND), 0);
        return;
    }
#endif

    /* Inform the "upper half serial driver that we have lost the USB serial
     * connection.
     */

    flags = irqsave();

    /* Reset the configuration */

    usbclass_resetconfig(priv);

    /* Clear out all outgoing data in the circular buffer */

    irqrestore(flags);

    /* Perform the soft connect function so that we will we can be
     * re-enumerated.
     */

    DEV_CONNECT(dev);
}

int usbdev_apbinitialize(struct device *dev,
                         struct apbridge_usb_driver *driver)
{
    struct apbridge_dev_s *priv;
    struct usbdevclass_driver_s *drvr;
    int ret;
    unsigned int i;
    unsigned int cport_count = unipro_cport_count();

    /* Allocate the structures needed */

    priv = (struct apbridge_dev_s *)
        kmm_malloc(sizeof(struct apbridge_dev_s));
    if (!priv) {
        usbtrace(TRACE_CLSERROR(USBSER_TRACEERR_ALLOCDEVSTRUCT), 0);
        return -ENOMEM;
    }

    /* Convenience pointers into the allocated blob */

    drvr = &priv->drvr;

    /* Initialize the USB driver structure */

    memset(priv, 0, sizeof(struct apbridge_dev_s));
    priv->driver = driver;

    priv->cport_to_epin_n = kmm_malloc(sizeof(int) * unipro_cport_count());
    if (!priv->cport_to_epin_n) {
        ret = -ENOMEM;
        goto errout_with_alloc;
    }
    priv->ts = kmm_malloc(sizeof(struct gb_timestamp) * unipro_cport_count());
    if (!priv->ts) {
        ret = -ENOMEM;
        goto errout_with_alloc_ts;
    }

    for (i = 0; i < cport_count; i++) {
        priv->cport_to_epin_n[i] = CONFIG_APBRIDGE_EPBULKIN;
        priv->ts[i].tag = false;
    }
    sem_init(&priv->config_sem, 0, 0);
    list_init(&priv->msg_queue);
    gb_timestamp_init();

    /* Initialize the USB class driver structure */

    drvr->speed = USB_SPEED_HIGH;
    drvr->ops = &g_driverops;

    /* Register the USB serial class driver */

    ret = device_usbdev_register_gadget(dev, drvr);
    if (ret) {
        usbtrace(TRACE_CLSERROR(USBSER_TRACEERR_DEVREGISTER),
                 (uint16_t) - ret);
        goto errout_cport_table;
    }
    ret = priv->driver->init(priv);
    if (ret)
        goto errout_with_init;

    /* Register the single port supported by this implementation */
    return OK;

 errout_with_init:
    device_usbdev_unregister_gadget(dev, drvr);
errout_cport_table:
    kmm_free(priv->ts);
errout_with_alloc_ts:
    kmm_free(priv->cport_to_epin_n);
 errout_with_alloc:
    kmm_free(priv);
    return ret;
}
