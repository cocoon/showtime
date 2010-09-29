/*
 *  Services
 *  Copyright (C) 2010 Andreas Öman
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

#include <stdio.h>
#include <string.h>

#include "showtime.h"
#include "service.h"
#include "misc/strtab.h"
#include "prop/prop_nodefilter.h"
#include "backend/backend.h"

static prop_t *all_services;


struct svc_type_meta {
  const char *name;
};



static struct svc_type_meta svc_types[SVC_num];

struct service_list services;
hts_mutex_t service_mutex;
static hts_cond_t service_cond;



/**
 *
 */
static struct strtab status_tab[] = {
  {"ok",        SVC_STATUS_OK},
  {"auth",      SVC_STATUS_AUTH_NEEDED},
  {"nohandler", SVC_STATUS_NO_HANDLER},
  {"fail",      SVC_STATUS_FAIL},
  {"scanning",  SVC_STATUS_SCANNING},
};


static void *service_probe_loop(void *aux);

/**
 *
 */
static void
add_service_type(prop_t *root, service_type_t type, const char *name)
{
  struct prop_nf *pnf;

  svc_types[type].name = name;

  pnf = prop_nf_create(prop_create(prop_create(root, name), "nodes"),
		       all_services, NULL, "node.title");

  prop_nf_pred_str_add(pnf, "node.type",
		       PROP_NF_CMP_NEQ, name, NULL, 
		       PROP_NF_MODE_EXCLUDE);
}


/**
 *
 */
void
service_init(void)
{
  hts_mutex_init(&service_mutex);
  hts_cond_init(&service_cond);

  hts_thread_create_detached("service probe", service_probe_loop, NULL);

  all_services = prop_create(NULL, NULL);

  prop_nf_create(prop_create(prop_get_global(), "sources"),
		 all_services, NULL, "node.title");

  prop_t *p = prop_create(prop_get_global(), "services");

  add_service_type(p, SVC_TYPE_MUSIC, "music");
  add_service_type(p, SVC_TYPE_IMAGE, "image");
  add_service_type(p, SVC_TYPE_VIDEO, "video");
  add_service_type(p, SVC_TYPE_TV,    "tv");
  add_service_type(p, SVC_TYPE_OTHER, "other");
}


/**
 *
 */
void 
service_destroy(service_t *s)
{
  prop_destroy(s->s_root);
  free(s->s_url);
  
  hts_mutex_lock(&service_mutex);
  LIST_REMOVE(s, s_link);
  s->s_zombie = 1;
  if(--s->s_ref == 0)
    free(s);
  hts_mutex_unlock(&service_mutex);
}


/**
 *
 */
static void
service_reprobe(service_t *s)
{
  if(!s->s_do_probe)
    return;
  s->s_need_probe = 1;
  hts_cond_signal(&service_cond);
}


/**
 *
 */
static void
seturl(service_t *s, const char *url)
{
  char urlbuf[URL_MAX];
  backend_t *be;

  be = backend_canhandle(url);
  if(be != NULL && be->be_normalize != NULL &&
     !be->be_normalize(be, url, urlbuf, sizeof(urlbuf)))
    mystrset(&s->s_url, urlbuf);
  else
    mystrset(&s->s_url, url);
}


/**
 *
 */
service_t *
service_create(const char *title,
	       const char *url,
	       service_type_t type,
	       const char *icon,
	       int probe)
{
  service_t *s = calloc(1, sizeof(service_t));
  prop_t *p;
  s->s_ref = 1;
  s->s_type = type;

  p = s->s_root = prop_create(NULL, NULL);
  seturl(s, url);

  prop_set_string(prop_create(p, "title"), title);
  prop_set_string(prop_create(p, "icon"), icon);
  prop_set_string(prop_create(p, "url"), url);

  s->s_prop_type = prop_create(p, "type");
  prop_set_string(s->s_prop_type, svc_types[type].name);

  s->s_prop_status = prop_create(p, "status");
  prop_set_string(s->s_prop_status, "ok");

  s->s_prop_status_txt = prop_create(p, "statustxt");

  if(prop_set_parent(s->s_root, all_services))
    abort();
  
  hts_mutex_lock(&service_mutex);
  LIST_INSERT_HEAD(&services, s, s_link);
  s->s_need_probe = s->s_do_probe = probe;
  hts_cond_signal(&service_cond);
  hts_mutex_unlock(&service_mutex);
  return s;
}


/**
 *
 */
prop_t *
service_get_status_prop(service_t *s)
{
  return s->s_prop_status;
}


/**
 *
 */
prop_t *
service_get_statustxt_prop(service_t *s)
{
  return s->s_prop_status_txt;
}


/**
 *
 */
void 
service_set_type(service_t *s, service_type_t type)
{
  prop_set_string(s->s_prop_type, svc_types[type].name);
}


/**
 *
 */
void
service_set_title(service_t *s, const char *title)
{
  prop_set_string(prop_create(s->s_root, "title"), title);
}


/**
 *
 */
void
service_set_icon(service_t *s, const char *icon)
{
  prop_set_string(prop_create(s->s_root, "icon"), icon);
}


/**
 *
 */
void
service_set_url(service_t *s, const char *url)
{
  prop_set_string(prop_create(s->s_root, "url"), url);

  hts_mutex_lock(&service_mutex);
  seturl(s, url);
  service_reprobe(s);
  hts_mutex_unlock(&service_mutex);
}


/**
 *
 */
void
service_set_status(service_t *s, service_status_t status)
{

}


/**
 *
 */
static void *
service_probe_loop(void *aux)
{
  service_t *s;
  char *url;
  service_status_t st;
  char txt[256];

  hts_mutex_lock(&service_mutex);

  while(1) {
    
    LIST_FOREACH(s, &services, s_link) {
      if(s->s_need_probe)
	break;
    }

    if(s == NULL) {
      hts_cond_wait(&service_cond, &service_mutex);
      continue;
    }
    s->s_need_probe = 0;
    // Will release lock, so reference and copy URL
    s->s_ref++;

    prop_set_string(s->s_prop_status, val2str(SVC_STATUS_SCANNING, status_tab));

    if(s->s_url == NULL) {
      st = SVC_STATUS_FAIL;
    } else {
      url = strdup(s->s_url);

      hts_mutex_unlock(&service_mutex);
      st = backend_probe(url, txt, sizeof(txt)); // Can take a lot of time
      free(url);
      hts_mutex_lock(&service_mutex);
    }

    if(!s->s_zombie) {
      prop_set_string(s->s_prop_status, val2str(st, status_tab));
      if(st != SVC_STATUS_OK)
	prop_set_string(s->s_prop_status_txt, txt);
      else
	prop_set_void(s->s_prop_status_txt);
    }
    if(--s->s_ref == 0) {
      printf("free %p\n", s);
      free(s);
    }
  }
  return NULL;
}


/**
 *
 */
static struct strtab strtype_tab[] = {
  { "music",    SVC_TYPE_MUSIC },
  { "video",    SVC_TYPE_VIDEO },
  { "tv",       SVC_TYPE_TV },
  { "image",    SVC_TYPE_IMAGE },
  { "other",    SVC_TYPE_OTHER },
};

/**
 *
 */
service_type_t
service_str2type(const char *str)
{
  return str ? str2val(str, strtype_tab) : SVC_TYPE_OTHER;
}


/**
 *
 */
const char *
service_type2str(service_type_t type)
{
  return val2str(type, strtype_tab);
}
