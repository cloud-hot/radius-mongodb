/*
 * rlm_mongo.c
 *
 * Version:	$Id$
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Copyright 2000,2006  The FreeRADIUS server project
 * Copyright 2010 Guillaume Rose <guillaume.rose@gmail.com>
 */

#include <freeradius-devel/ident.h>
RCSID("$Id$")

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/modules.h>

#include "mongo.h"

#define MONGO_STRING_LENGTH 8196

typedef struct rlm_mongo_t {
	char	*ip;
	int		port;
	
	char	*base;
	char	*search_field;
	char	*username_field;
	char	*password_field;
	char	*mac_field;
	char	*enable_field;
} rlm_mongo_t;

static const CONF_PARSER module_config[] = {
  { "port", PW_TYPE_INTEGER,    offsetof(rlm_mongo_t,port), NULL,   "27017" },
  { "ip",  PW_TYPE_STRING_PTR, offsetof(rlm_mongo_t,ip), NULL,  "127.0.0.1"},

  { "base",  PW_TYPE_STRING_PTR, offsetof(rlm_mongo_t,base), NULL,  ""},
  { "search_field",  PW_TYPE_STRING_PTR, offsetof(rlm_mongo_t,search_field), NULL,  ""},
  { "username_field",  PW_TYPE_STRING_PTR, offsetof(rlm_mongo_t,username_field), NULL,  ""},
  { "password_field",  PW_TYPE_STRING_PTR, offsetof(rlm_mongo_t,password_field), NULL,  ""},
  { "mac_field",  PW_TYPE_STRING_PTR, offsetof(rlm_mongo_t,mac_field), NULL,  ""},
  { "enable_field",  PW_TYPE_STRING_PTR, offsetof(rlm_mongo_t,enable_field), NULL,  ""},
  
  { NULL, -1, 0, NULL, NULL }		/* end the list */
};

mongo_connection conn[1];
mongo_connection_options opts;

int mongo_start(rlm_mongo_t *data)
{
	strncpy(opts.host, data->ip, 255);
	opts.host[254] = '\0';
	opts.port = data->port;

	if (mongo_connect(conn, &opts)){
	printf("Failed to connect\n");
	return 0;
	}

	printf("Connected to MongoDB\n");
	return 1;
}

void find_in_array(bson_iterator *it, char *key_ref, char *value_ref, char *key_needed, char *value_needed) 
{
	char value_ref_found[MONGO_STRING_LENGTH];
	char value_needed_found[MONGO_STRING_LENGTH];

	bson_iterator i;
	
	while(bson_iterator_next(it)) {
		switch(bson_iterator_type(it)){
			case bson_string:
				if (strcmp(bson_iterator_key(it), key_ref) == 0)
					strcpy(value_ref_found, bson_iterator_string(it));
				if (strcmp(bson_iterator_key(it), key_needed) == 0)
					strcpy(value_needed_found, bson_iterator_string(it));
				break;
			case bson_object:
			case bson_array:
				bson_iterator_init(&i, bson_iterator_value(it));
				find_in_array(&i, key_ref, value_ref, key_needed, value_needed);
				break;
			default:
				break;
		}
	}
	
	if (strcmp(value_ref_found, value_ref) == 0)
		strcpy(value_needed, value_needed_found);
}

int find_radius_options(rlm_mongo_t *data, char *username, char *mac, char *password) 
{
	bson_buffer bb;
	
	bson query;
	bson field;
	bson result;
	
	bson_buffer_init(&bb);
	
	bson_append_string(&bb, data->search_field, username);
	
	if (strcmp(data->mac_field, "") != 0) {
		bson_append_string(&bb, data->mac_field, mac);
	}
	
	if (strcmp(data->enable_field, "") != 0) {
		bson_append_int(&bb, data->enable_field, 1);
	}	
	
	bson_from_buffer(&query, &bb);

	bson_empty(&field);

	bson_empty(&result);

	MONGO_TRY{
		if (mongo_find_one(conn, data->base, &query, &field, &result) == 0) {
			return 0;
		}
	}MONGO_CATCH{
		mongo_start(data);
		return 0;
	}
	
	bson_iterator it;
	bson_iterator_init(&it, result.data);
	
	find_in_array(&it, data->username_field, username, data->password_field, password);
	return 1;
}

static int mongo_instantiate(CONF_SECTION *conf, void **instance)
{
	rlm_mongo_t *data;

	data = rad_malloc(sizeof(*data));
	if (!data) {
		return -1;
	}
	memset(data, 0, sizeof(*data));

	if (cf_section_parse(conf, data, module_config) < 0) {
		free(data);
		return -1;
	}
	
	mongo_start(data);

	*instance = data;

	return 0;
}

static int mongo_authorize(void *instance, REQUEST *request)
{
	rlm_mongo_t *data = (rlm_mongo_t *) instance;
	
	char mac[MONGO_STRING_LENGTH] = "";
	
	if (strcmp(data->mac_field, "") != 0) {
		radius_xlat(mac, MONGO_STRING_LENGTH, "%{Calling-Station-Id}", request, NULL);
  }
  
	char password[MONGO_STRING_LENGTH] = "";
	find_radius_options(data, request->username->vp_strvalue, mac, password);
	
	printf("\nAutorisation request by username : \"%s\"\n", request->username->vp_strvalue);
	printf("Password found in MongoDB-> \"%s\"\n\n", password);
	
	VALUE_PAIR *vp;

	/* quiet the compiler */
	instance = instance;
	request = request;

 	vp = pairmake("Cleartext-Password", password, T_OP_SET);
 	if (!vp) return RLM_MODULE_FAIL;
 	
	pairmove(&request->config_items, &vp);
	pairfree(&vp);

	return RLM_MODULE_OK;
}

static int mongo_detach(void *instance)
{
	free(instance);
	return 0;
}

module_t rlm_mongo = {
	RLM_MODULE_INIT,
	"mongo",
	RLM_TYPE_THREAD_SAFE,		/* type */
	mongo_instantiate,		/* instantiation */
	mongo_detach,			/* detach */
	{
		NULL,			/* authentication */
		mongo_authorize,	/* authorization */
		NULL,			/* preaccounting */
		NULL,			/* accounting */
		NULL,			/* checksimul */
		NULL,			/* pre-proxy */
		NULL,			/* post-proxy */
		NULL			/* post-auth */
	},
};


