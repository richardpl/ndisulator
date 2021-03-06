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

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>
#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>

#include "pe_var.h"
#include "resource_var.h"
#include "ntoskrnl_var.h"
#include "ndis_var.h"
#include "if_ndisvar.h"

MODULE_DEPEND(ndis, pci, 1, 1, 1);

static int	ndis_attach_pci(device_t);
static int	ndis_devcompare_pci(enum ndis_bus_type,
		    struct ndis_device_type *, device_t);
static int	ndis_probe_pci(device_t);
static struct resource_list *ndis_get_resource_list(device_t, device_t);

static device_method_t ndis_pci_methods[] = {
	DEVMETHOD(device_probe,		ndis_probe_pci),
	DEVMETHOD(device_attach,	ndis_attach_pci),
	DEVMETHOD(device_detach,	ndis_detach),
	DEVMETHOD(device_shutdown,	ndis_shutdown),
	DEVMETHOD(device_suspend,	ndis_suspend),
	DEVMETHOD(device_resume,	ndis_resume),
	DEVMETHOD(bus_get_resource_list, ndis_get_resource_list),
	DEVMETHOD_END
};

driver_t ndis_pci_driver = {
	"ndis",
	ndis_pci_methods,
	sizeof(struct ndis_softc)
};

DRIVER_MODULE(ndis, pci, ndis_pci_driver, ndis_devclass, ndisdrv_modevent, 0);

static int
ndis_devcompare_pci(enum ndis_bus_type bustype,
    struct ndis_device_type *t, device_t dev)
{

	if (bustype != NDIS_PCIBUS)
		return (FALSE);

	for (; t->name != NULL; t++) {
		if ((pci_get_vendor(dev) == t->vendor) &&
		    (pci_get_device(dev) == t->device) &&
		    ((pci_read_config(dev, PCIR_SUBVEND_0, 4) ==
		    t->subsys) || t->subsys == 0)) {
			device_set_desc(dev, t->name);
			return (TRUE);
		}
	}

	return (FALSE);
}

static int
ndis_probe_pci(device_t dev)
{
	struct drvdb_ent *db;
	struct driver_object *drv;

	drv = windrv_lookup(0, "PCI Bus");
	if (drv == NULL)
		return (ENXIO);

	db = windrv_match((matchfuncptr)ndis_devcompare_pci, dev);
	if (db == NULL)
		return (ENXIO);
	return (windrv_create_pdo(drv, dev));
}

static int
ndis_attach_pci(device_t dev)
{
	struct ndis_softc *sc;
	struct ndis_device_type *t;
	struct resource_list *rl;
	struct resource_list_entry *rle;
	struct drvdb_ent *db;
	uint32_t devidx = 0, defidx = 0;
	int error = 0, rid;

	sc = device_get_softc(dev);
	sc->ndis_dev = dev;

	db = windrv_match((matchfuncptr)ndis_devcompare_pci, dev);
	if (db == NULL)
		return (ENXIO);
	sc->ndis_dobj = db->windrv_object;
	sc->ndis_regvals = db->windrv_regvals;

	/*
	 * Map control/status registers.
	 */
	pci_enable_busmaster(dev);
	rl = BUS_GET_RESOURCE_LIST(device_get_parent(dev), dev);
	if (rl == NULL)
		return (ENXIO);

	STAILQ_FOREACH(rle, rl, link) {
		switch (rle->type) {
		case SYS_RES_IOPORT:
			sc->ndis_io_rid = rle->rid;
			sc->ndis_res_io = bus_alloc_resource_any(dev,
			    SYS_RES_IOPORT, &sc->ndis_io_rid, RF_ACTIVE);
			if (sc->ndis_res_io == NULL) {
				device_printf(dev, "no ioport\n");
				error = ENXIO;
				goto fail;
			}
			break;
		case SYS_RES_MEMORY:
			if (sc->ndis_res_altmem != NULL &&
			    sc->ndis_res_mem != NULL) {
				device_printf(dev, "too many mem\n");
				error = ENXIO;
				goto fail;
			}
			if (sc->ndis_res_mem) {
				sc->ndis_altmem_rid = rle->rid;
				sc->ndis_res_altmem = bus_alloc_resource_any(
				    dev, SYS_RES_MEMORY, &sc->ndis_altmem_rid,
				    RF_ACTIVE);
				if (sc->ndis_res_altmem == NULL) {
					device_printf(dev, "no map alt\n");
					error = ENXIO;
					goto fail;
				}
			} else {
				sc->ndis_mem_rid = rle->rid;
				sc->ndis_res_mem = bus_alloc_resource_any(dev,
				    SYS_RES_MEMORY, &sc->ndis_mem_rid,
				    RF_ACTIVE);
				if (sc->ndis_res_mem == NULL) {
					device_printf(dev, "no map\n");
					error = ENXIO;
					goto fail;
				}
			}
			break;
		case SYS_RES_IRQ:
			rid = rle->rid;
			sc->ndis_irq = bus_alloc_resource_any(dev, SYS_RES_IRQ,
			    &rid, RF_SHAREABLE | RF_ACTIVE);
			if (sc->ndis_irq == NULL) {
				device_printf(dev, "no irq\n");
				error = ENXIO;
				goto fail;
			}
			break;
		default:
			break;
		}
		sc->ndis_rescnt++;
	}

	/*
	 * If the BIOS did not set up an interrupt for this device,
	 * the resource traversal code above will fail to set up
	 * an IRQ resource. This is usually a bad thing, so try to
	 * force the allocation of an interrupt here. If one was
	 * not assigned to us by the BIOS, bus_alloc_resource_any()
	 * should route one for us.
	 */
	if (sc->ndis_irq == NULL) {
		rid = 0;
		sc->ndis_irq = bus_alloc_resource_any(dev, SYS_RES_IRQ,
		    &rid, RF_SHAREABLE | RF_ACTIVE);
		if (sc->ndis_irq == NULL) {
			device_printf(dev, "couldn't route interrupt\n");
			error = ENXIO;
			goto fail;
		}
		sc->ndis_rescnt++;
	}

	/*
	 * Allocate the parent bus DMA tag appropriate for PCI.
	 */
#define	NDIS_NSEG_NEW 32
	error = bus_dma_tag_create(bus_get_dma_tag(dev),/* PCI parent */
			1, 0,			/* alignment, boundary */
			BUS_SPACE_MAXADDR_32BIT,/* lowaddr */
			BUS_SPACE_MAXADDR,	/* highaddr */
			NULL, NULL,		/* filter, filterarg */
			MAXBSIZE, NDIS_NSEG_NEW,/* maxsize, nsegments */
			BUS_SPACE_MAXSIZE_32BIT,/* maxsegsize */
			0,			/* flags */
			NULL, NULL,		/* lockfunc, lockarg */
			&sc->ndis_parent_tag);
	if (error)
		goto fail;

	sc->ndis_bus_type = NDIS_PCIBUS;

	/* Figure out exactly which device we matched. */
	for (t = db->windrv_devlist; t->name != NULL; t++, devidx++) {
		if ((pci_get_vendor(dev) == t->vendor) &&
		    (pci_get_device(dev) == t->device)) {
			if (t->subsys == 0)
				defidx = devidx;
			else {
				if (t->subsys ==
				    pci_read_config(dev, PCIR_SUBVEND_0, 4))
					break;
			}
		}
	}
	if (t->name == NULL)
		sc->ndis_devidx = defidx;
	else
		sc->ndis_devidx = devidx;

	error = ndis_attach(dev);
fail:
	return (error);
}

static struct resource_list *
ndis_get_resource_list(device_t dev, device_t child)
{
	struct ndis_softc *sc;

	sc = device_get_softc(dev);

	return (BUS_GET_RESOURCE_LIST(device_get_parent(sc->ndis_dev), dev));
}
