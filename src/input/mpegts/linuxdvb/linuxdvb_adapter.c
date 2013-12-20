/*
 *  Tvheadend - Linux DVB input system
 *
 *  Copyright (C) 2013 Adam Sutton
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "tvheadend.h"
#include "input.h"
#include "linuxdvb_private.h"
#include "queue.h"

#include <sys/types.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <fcntl.h>

#define FE_PATH  "/dev/dvb/adapter%d/frontend%d"
#define DVR_PATH "/dev/dvb/adapter%d/dvr%d"
#define DMX_PATH "/dev/dvb/adapter%d/demux%d"

/* ***************************************************************************
 * DVB Adapter
 * **************************************************************************/

static void
linuxdvb_adapter_class_save ( idnode_t *in )
{
  linuxdvb_adapter_t *la = (linuxdvb_adapter_t*)in;
  linuxdvb_device_save(la->la_device);
}

static idnode_set_t *
linuxdvb_adapter_class_get_childs ( idnode_t *in )
{
  linuxdvb_frontend_t *lfe;
  linuxdvb_adapter_t *la = (linuxdvb_adapter_t*)in;
  idnode_set_t *is = idnode_set_create();
  LIST_FOREACH(lfe, &la->la_frontends, lfe_link)
    idnode_set_add(is, &lfe->ti_id, NULL);
  return is;
}

static const char *
linuxdvb_adapter_class_get_title ( idnode_t *in )
{
  linuxdvb_adapter_t *la = (linuxdvb_adapter_t*)in;
  return la->la_name ?: la->la_rootpath;
}

const idclass_t linuxdvb_adapter_class =
{
  .ic_class      = "linuxdvb_adapter",
  .ic_caption    = "LinuxDVB Adapter",
  .ic_save       = linuxdvb_adapter_class_save,
  .ic_get_childs = linuxdvb_adapter_class_get_childs,
  .ic_get_title  = linuxdvb_adapter_class_get_title,
  .ic_properties = (const property_t[]){
    {
      .type     = PT_STR,
      .id       = "rootpath",
      .name     = "Device Path",
      .opts     = PO_RDONLY,
      .off      = offsetof(linuxdvb_adapter_t, la_rootpath),
    },
    {}
  }
};

/*
 * Save data
 */
void
linuxdvb_adapter_save ( linuxdvb_adapter_t *la, htsmsg_t *m )
{
  htsmsg_t *l;
  linuxdvb_frontend_t *lfe;

  idnode_save(&la->la_id, m);
  htsmsg_add_u32(m, "number", la->la_number);

  /* Frontends */
  l = htsmsg_create_map();
  LIST_FOREACH(lfe, &la->la_frontends, lfe_link) {
    htsmsg_t *e = htsmsg_create_map();
    linuxdvb_frontend_save(lfe, e);
    htsmsg_add_msg(l, idnode_uuid_as_str(&lfe->ti_id), e);
  }
  htsmsg_add_msg(m, "frontends", l);
}

/*
 * Check if enabled
 */
static int
linuxdvb_adapter_is_enabled ( linuxdvb_adapter_t *la )
{
  linuxdvb_frontend_t *lfe;
  LIST_FOREACH(lfe, &la->la_frontends, lfe_link) {
    if (lfe->mi_is_enabled((mpegts_input_t*)lfe))
      return 1;
  }
  return 0;
}

/*
 * Create
 */
linuxdvb_adapter_t *
linuxdvb_adapter_create0 
  ( linuxdvb_device_t *ld, const char *uuid, htsmsg_t *conf )
{
  uint32_t u32;
  htsmsg_t *e;
  htsmsg_field_t *f;
  linuxdvb_adapter_t *la;

  la = calloc(1, sizeof(linuxdvb_adapter_t));
  if (idnode_insert(&la->la_id, uuid, &linuxdvb_adapter_class)) {
    free(la);
    return NULL;
  }

  LIST_INSERT_HEAD(&ld->ld_adapters, la, la_link);
  la->la_device     = ld;
  la->la_dvb_number = -1;
  la->la_is_enabled = linuxdvb_adapter_is_enabled;

  /* No conf */
  if (!conf)
    return la;

  idnode_load(&la->la_id, conf);
  if (!htsmsg_get_u32(conf, "number", &u32))
    la->la_number = u32;

  /* Frontends */
  if ((conf = htsmsg_get_map(conf, "frontends"))) {
    HTSMSG_FOREACH(f, conf) {
      if (!(e = htsmsg_get_map_by_field(f))) continue;
      (void)linuxdvb_frontend_create0(la, f->hmf_name, e, 0);
    }
  }

  return la;
}

/*
 * Find existing adapter/device entry
 */
static linuxdvb_adapter_t *
linuxdvb_adapter_find_by_number ( int adapter )
{
  int a;
  char buf[1024];
  linuxdvb_device_t *ld;
  linuxdvb_adapter_t *la;

  /* Find device */
  if (!(ld = linuxdvb_device_find_by_adapter(adapter)))
    return NULL;

  /* Find existing adapter */ 
  a = adapter - ld->ld_devid.di_min_adapter;
  LIST_FOREACH(la, &ld->ld_adapters, la_link) {
    if (la->la_number == a)
      break;
  }

  /* Create */
  if (!la) {
    if (!(la = linuxdvb_adapter_create0(ld, NULL, NULL)))
      return NULL;
  }

  /* Update */
  la->la_number = a;
  snprintf(buf, sizeof(buf), "/dev/dvb/adapter%d", adapter);
  tvh_str_update(&la->la_rootpath, buf);

  return la;
}

/*
 * Load an adapter
 */
void
linuxdvb_adapter_added ( int adapter )
{
  int i, r, fd, save = 0;
  char fe_path[512], dmx_path[512], dvr_path[512];
  linuxdvb_adapter_t *la = NULL;
  struct dvb_frontend_info dfi;

  /* Process each frontend */
  for (i = 0; i < 32; i++) {
    snprintf(fe_path, sizeof(fe_path), FE_PATH, adapter, i);

    /* No access */
    if (access(fe_path, R_OK | W_OK)) continue;

    /* Get frontend info */
    fd = tvh_open(fe_path, O_RDONLY | O_NONBLOCK, 0);
    if (fd == -1) {
      tvhlog(LOG_ERR, "linuxdvb", "unable to open %s", fe_path);
      continue;
    }
    r = ioctl(fd, FE_GET_INFO, &dfi);
    close(fd);
    if(r) {
      tvhlog(LOG_ERR, "linuxdvb", "unable to query %s", fe_path);
      continue;
    }

    /* DVR/DMX (bit of a guess) */
    snprintf(dmx_path, sizeof(dmx_path), DMX_PATH, adapter, i);
    if (access(dmx_path, R_OK | W_OK)) {
      snprintf(dmx_path, sizeof(dmx_path), DMX_PATH, adapter, 0);
      if (access(dmx_path, R_OK | W_OK)) continue;
    }

    snprintf(dvr_path, sizeof(dvr_path), DVR_PATH, adapter, i);
    if (access(dvr_path, R_OK | W_OK)) {
      snprintf(dvr_path, sizeof(dvr_path), DVR_PATH, adapter, 0);
      if (access(dvr_path, R_OK | W_OK)) continue;
    }

    /* Create/Find adapter */
    if (!la) {
      if (!(la = linuxdvb_adapter_find_by_number(adapter))) {
        tvhlog(LOG_ERR, "linuxdvb", "failed to find/create adapter%d", adapter);
        return;
      }
      la->la_dvb_number = adapter;
      if (!la->la_name) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s #%d", dfi.name, la->la_number);
        la->la_name = strdup(buf);
      }
    }

    /* Create frontend */
    tvhlog(LOG_DEBUG, "linuxdvb", "fe_create(%p, %s, %s, %s)",
           la, fe_path, dmx_path, dvr_path);
    save |= linuxdvb_frontend_added(la, i, fe_path, dmx_path, dvr_path, &dfi);
  }

  if (save)
    linuxdvb_device_save(la->la_device);
}
