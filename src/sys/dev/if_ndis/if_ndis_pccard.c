/*-
 * Copyright (c) 2003
 *	Bill Paul <wpaul@windriver.com>.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by Bill Paul.
 * 4. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY Bill Paul AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL Bill Paul OR THE VOICES IN HIS HEAD
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/socket.h>

#include <net/if.h>
#include <net/if_media.h>

#include <machine/bus.h>
#include <machine/resource.h>
#include <sys/bus.h>
#include <sys/rman.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>

#include "pe_var.h"
#include "resource_var.h"
#include "ntoskrnl_var.h"
#include "ndis_var.h"
#include "if_ndisvar.h"

#include <dev/pccard/pccardvar.h>
#include "card_if.h"

MODULE_DEPEND(ndis, pccard, 1, 1, 1);

static int	ndis_alloc_amem(struct ndis_softc *);
static int	ndis_attach_pccard(device_t);
static int	ndis_devcompare_pccard(enum ndis_bus_type,
		    struct ndis_device_type *, device_t);
static int	ndis_probe_pccard(device_t);
static struct resource_list *ndis_get_resource_list(device_t, device_t);

static device_method_t ndis_pccard_methods[] = {
	DEVMETHOD(device_probe,		ndis_probe_pccard),
	DEVMETHOD(device_attach,	ndis_attach_pccard),
	DEVMETHOD(device_detach,	ndis_detach),
	DEVMETHOD(device_shutdown,	ndis_shutdown),
	DEVMETHOD(device_suspend,	ndis_suspend),
	DEVMETHOD(device_resume,	ndis_resume),
	/*
	 * This is an awful kludge, but we need it becase pccard
	 * does not implement a bus_get_resource_list() method.
	 */
	DEVMETHOD(bus_get_resource_list, ndis_get_resource_list),
	DEVMETHOD_END
};

driver_t ndis_pccard_driver = {
	"ndis",
	ndis_pccard_methods,
	sizeof(struct ndis_softc)
};

DRIVER_MODULE(ndis, pccard, ndis_pccard_driver, ndis_devclass, ndisdrv_modevent, 0);

static int
ndis_devcompare_pccard(enum ndis_bus_type bustype,
    struct ndis_device_type *t, device_t dev)
{
	uint32_t product, vendor;

	if (bustype != NDIS_PCMCIABUS ||
	    pccard_get_product(dev, &product) ||
	    pccard_get_vendor(dev, &vendor))
		return (FALSE);

	for (; t->name != NULL; t++) {
		if ((vendor == t->vendor) &&
		    (product == t->device)) {
			device_set_desc(dev, t->name);
			return (TRUE);
		}
	}

	return (FALSE);
}

static int
ndis_probe_pccard(device_t dev)
{
	struct drvdb_ent *db;
	struct driver_object *drv;

	drv = windrv_lookup(0, "PCCARD Bus");
	if (drv == NULL)
		return (ENXIO);

	db = windrv_match((matchfuncptr)ndis_devcompare_pccard, dev);
	if (db == NULL)
		return (ENXIO);
	return (windrv_create_pdo(drv, dev));
}

static int
ndis_attach_pccard(device_t dev)
{
	struct ndis_softc *sc;
	struct ndis_device_type *t;
	struct drvdb_ent *db;
	uint32_t product, vendor;
	int devidx = 0, error = 0, rid = 0;

	sc = device_get_softc(dev);
	sc->ndis_dev = dev;

	db = windrv_match((matchfuncptr)ndis_devcompare_pccard, dev);
	if (db == NULL)
		return (ENXIO);
	sc->ndis_dobj = db->windrv_object;
	sc->ndis_regvals = db->windrv_regvals;
	resource_list_init(&sc->ndis_rl);

	sc->ndis_io_rid = 0;
	sc->ndis_res_io = bus_alloc_resource_any(dev, SYS_RES_IOPORT,
	    &sc->ndis_io_rid, RF_ACTIVE);
	if (sc->ndis_res_io == NULL) {
		device_printf(dev, "couldn't map iospace\n");
		return (ENXIO);
	}
	sc->ndis_rescnt++;
	resource_list_add(&sc->ndis_rl, SYS_RES_IOPORT, sc->ndis_io_rid,
	    rman_get_start(sc->ndis_res_io), rman_get_end(sc->ndis_res_io),
	    rman_get_size(sc->ndis_res_io));

	sc->ndis_irq = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid,
	    RF_SHAREABLE | RF_ACTIVE);
	if (sc->ndis_irq == NULL) {
		device_printf(dev, "couldn't map interrupt\n");
		return (ENXIO);
	}
	sc->ndis_rescnt++;
	resource_list_add(&sc->ndis_rl, SYS_RES_IRQ, rid,
	    rman_get_start(sc->ndis_irq), rman_get_start(sc->ndis_irq), 1);
	sc->ndis_bus_type = NDIS_PCMCIABUS;

	error = pccard_get_product(dev, &product);
	if (error)
		return (error);
	error = pccard_get_vendor(dev, &vendor);
	if (error)
		return (error);
	/* Figure out exactly which device we matched. */
	for (t = db->windrv_devlist; t->name != NULL; t++, devidx++) {
		if ((vendor == t->vendor) &&
		    (product == t->device))
			break;
	}
	sc->ndis_devidx = devidx;

	error = ndis_alloc_amem(sc);
	if (error) {
		ndis_free_amem(sc);
		return (error);
	}
	return (ndis_attach(dev));
}

static struct resource_list *
ndis_get_resource_list(device_t dev, device_t child)
{
	struct ndis_softc *sc;

	sc = device_get_softc(dev);
	return (&sc->ndis_rl);
}

#define	NDIS_AM_RID 3

static int
ndis_alloc_amem(struct ndis_softc *sc)
{
	int error, rid = NDIS_AM_RID;

	sc->ndis_res_am = bus_alloc_resource(sc->ndis_dev, SYS_RES_MEMORY,
	    &rid, 0UL, ~0UL, 0x1000, RF_ACTIVE);
	if (sc->ndis_res_am == NULL) {
		device_printf(sc->ndis_dev,
		    "failed to allocate attribute memory\n");
		return (ENXIO);
	}
	sc->ndis_rescnt++;
	resource_list_add(&sc->ndis_rl, SYS_RES_MEMORY, rid,
	    rman_get_start(sc->ndis_res_am), rman_get_end(sc->ndis_res_am),
	    rman_get_size(sc->ndis_res_am));

	error = CARD_SET_MEMORY_OFFSET(device_get_parent(sc->ndis_dev),
	    sc->ndis_dev, rid, 0, NULL);
	if (error) {
		device_printf(sc->ndis_dev,
		    "CARD_SET_MEMORY_OFFSET() returned 0x%x\n", error);
		return (error);
	}

	error = CARD_SET_RES_FLAGS(device_get_parent(sc->ndis_dev),
	    sc->ndis_dev, SYS_RES_MEMORY, rid, PCCARD_A_MEM_ATTR);
	if (error) {
		device_printf(sc->ndis_dev,
		    "CARD_SET_RES_FLAGS() returned 0x%x\n", error);
		return (error);
	}

	sc->ndis_am_rid = rid;

	return (0);
}

void
ndis_free_amem(void *arg)
{
	struct ndis_softc *sc = arg;

	if (sc->ndis_res_am != NULL)
		bus_release_resource(sc->ndis_dev, SYS_RES_MEMORY,
		    sc->ndis_am_rid, sc->ndis_res_am);
	resource_list_free(&sc->ndis_rl);
}
