/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2015 - Daniel De Matteis
 *  Copyright (C) 2013-2015 - Jason Fetters
 * 
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "database_info.h"
#include "hash.h"
#include "file_ops.h"
#include <file/file_extract.h>
#include "general.h"
#include "runloop.h"
#include <file/file_path.h>
#include "file_ext.h"
#include <file/dir_list.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

int database_open_cursor(libretrodb_t *db,
      libretrodb_cursor_t *cur, const char *query)
{
   const char *error     = NULL;
   libretrodb_query_t *q = NULL;

   if (query) 
      q = (libretrodb_query_t*)libretrodb_query_compile(db, query,
      strlen(query), &error);
    
   if (error)
      return -1;
   if ((libretrodb_cursor_open(db, cur, q)) != 0)
      return -1;

   return 0;
}

#ifdef HAVE_ZLIB
static int zlib_compare_crc32(const char *name, const char *valid_exts,
      const uint8_t *cdata, unsigned cmode, uint32_t csize, uint32_t size,
      uint32_t crc32, void *userdata)
{
   RARCH_LOG("CRC32: 0x%x\n", crc32);

   return 1;
}
#endif

database_info_handle_t *database_info_init(const char *dir, enum database_type type)
{
   const char *exts                = "";
   global_t                *global = global_get_ptr();
   database_info_handle_t     *db  = (database_info_handle_t*)calloc(1, sizeof(*db));

   if (!db)
      return NULL;

   if (global->core_info)
      exts = core_info_list_get_all_extensions(global->core_info);
   
   db->list      = dir_list_new(dir, exts, false);

   if (!db->list)
      goto error;

   db->list_ptr       = 0;
   db->status         = DATABASE_STATUS_ITERATE;
   db->type           = type;

   return db;

error:
   if (db)
      free(db);
   return NULL;
}

void database_info_free(database_info_handle_t *db)
{
   if (!db)
      return;

   string_list_free(db->list);
   free(db);
}

static int database_info_iterate_rdl_write(
      database_info_handle_t *db, const char *name)
{
   char parent_dir[PATH_MAX_LENGTH];
   bool to_continue = (db->list_ptr < db->list->size);

   if (!to_continue)
   {
      rarch_main_msg_queue_push("Scanning of directory finished.\n", 1, 180, true);
      db->status = DATABASE_STATUS_FREE;
      return -1;
   }

   path_parent_dir(parent_dir);

   if (!strcmp(path_get_extension(name), "zip"))
   {
#ifdef HAVE_ZLIB
      RARCH_LOG("[ZIP]: name: %s\n", name);

      if (!zlib_parse_file(name, NULL, zlib_compare_crc32,
               (void*)parent_dir))
         RARCH_LOG("Could not process ZIP file.\n");
#endif
   }
   else
   {
      char msg[PATH_MAX_LENGTH];
      ssize_t ret;
      uint32_t crc, target_crc = 0;
      uint8_t *ret_buf         = NULL;
      int read_from            = read_file(name, (void**)&ret_buf, &ret);

      (void)target_crc;

      if (read_from != 1)
         return 0;
      if (ret <= 0)
         return 0;

      snprintf(msg, sizeof(msg), "%zu/%zu: Scanning %s...\n",
            db->list_ptr, db->list->size, name);

      rarch_main_msg_queue_push(msg, 1, 180, true);

#ifdef HAVE_ZLIB
      crc = zlib_crc32_calculate(ret_buf, ret);

      RARCH_LOG("CRC32: 0x%x .\n", (unsigned)crc);
#endif

      if (ret_buf)
         free(ret_buf);
   }

   db->list_ptr++;

   return 0;
}

int database_info_iterate(database_info_handle_t *db)
{
   const char *name = NULL;

   if (!db || !db->list)
      return -1;

   name = db->list->elems[db->list_ptr].data;

   if (!name)
      return 0;

   switch (db->type)
   {
      case DATABASE_TYPE_NONE:
         break;
      case DATABASE_TYPE_RDL_WRITE:
         if (database_info_iterate_rdl_write(db, name) != 0)
            return -1;
         break;
   }

   return 0;
}

static char *bin_to_hex_alloc(const uint8_t *data, size_t len)
{
   size_t i;
   char *ret = (char*)malloc(len * 2 + 1);

   if (len && !ret)
      return NULL;
   
   for (i = 0; i < len; i++)
      snprintf(ret+i*2, 3, "%02X", data[i]);
   return ret;
}

database_info_list_t *database_info_list_new(const char *rdb_path, const char *query)
{
   libretrodb_t db;
   libretrodb_cursor_t cur;
   struct rmsgpack_dom_value item;
   size_t j;
   unsigned k                               = 0;
   database_info_t *database_info           = NULL;
   database_info_list_t *database_info_list = NULL;

   if ((libretrodb_open(rdb_path, &db)) != 0)
      return NULL;
   if ((database_open_cursor(&db, &cur, query) != 0))
      return NULL;

   database_info_list = (database_info_list_t*)calloc(1, sizeof(*database_info_list));
   if (!database_info_list)
      goto error;

   while (libretrodb_cursor_read_item(&cur, &item) == 0)
   {
      database_info_t *db_info = NULL;
      if (item.type != RDT_MAP)
         continue;

      database_info = (database_info_t*)realloc(database_info, (k+1) * sizeof(database_info_t));

      if (!database_info)
         goto error;

      db_info = &database_info[k];

      if (!db_info)
         continue;

      db_info->name                   = NULL;
      db_info->description            = NULL;
      db_info->publisher              = NULL;
      db_info->developer              = NULL;
      db_info->origin                 = NULL;
      db_info->franchise              = NULL;
      db_info->bbfc_rating            = NULL;
      db_info->elspa_rating           = NULL;
      db_info->esrb_rating            = NULL;
      db_info->pegi_rating            = NULL;
      db_info->cero_rating            = NULL;
      db_info->edge_magazine_review   = NULL;
      db_info->enhancement_hw         = NULL;
      db_info->crc32                  = NULL;
      db_info->sha1                   = NULL;
      db_info->md5                    = NULL;
      db_info->famitsu_magazine_rating= 0;
      db_info->edge_magazine_rating   = 0;
      db_info->edge_magazine_issue    = 0;
      db_info->max_users              = 0;
      db_info->releasemonth           = 0;
      db_info->releaseyear            = 0;
      db_info->analog_supported       = -1;
      db_info->rumble_supported       = -1;

      for (j = 0; j < item.map.len; j++)
      {
         struct rmsgpack_dom_value *key = &item.map.items[j].key;
         struct rmsgpack_dom_value *val = &item.map.items[j].value;

         if (!strcmp(key->string.buff, "name"))
            db_info->name = strdup(val->string.buff);

         if (!strcmp(key->string.buff, "description"))
            db_info->description = strdup(val->string.buff);

         if (!strcmp(key->string.buff, "publisher"))
            db_info->publisher = strdup(val->string.buff);

         if (!strcmp(key->string.buff, "developer"))
            db_info->developer = strdup(val->string.buff);

         if (!strcmp(key->string.buff, "origin"))
            db_info->origin = strdup(val->string.buff);

         if (!strcmp(key->string.buff, "franchise"))
            db_info->franchise = strdup(val->string.buff);

         if (!strcmp(key->string.buff, "bbfc_rating"))
            db_info->bbfc_rating = strdup(val->string.buff);

         if (!strcmp(key->string.buff, "esrb_rating"))
            db_info->esrb_rating = strdup(val->string.buff);

         if (!strcmp(key->string.buff, "elspa_rating"))
            db_info->elspa_rating = strdup(val->string.buff);

         if (!strcmp(key->string.buff, "cero_rating"))
            db_info->cero_rating = strdup(val->string.buff);

         if (!strcmp(key->string.buff, "pegi_rating"))
            db_info->pegi_rating = strdup(val->string.buff);

         if (!strcmp(key->string.buff, "enhancement_hw"))
            db_info->enhancement_hw = strdup(val->string.buff);

         if (!strcmp(key->string.buff, "edge_review"))
            db_info->edge_magazine_review = strdup(val->string.buff);

         if (!strcmp(key->string.buff, "edge_rating"))
            db_info->edge_magazine_rating = val->uint_;

         if (!strcmp(key->string.buff, "edge_issue"))
            db_info->edge_magazine_issue = val->uint_;

         if (!strcmp(key->string.buff, "famitsu_rating"))
            db_info->famitsu_magazine_rating = val->uint_;

         if (!strcmp(key->string.buff, "users"))
            db_info->max_users = val->uint_;

         if (!strcmp(key->string.buff, "releasemonth"))
            db_info->releasemonth = val->uint_;

         if (!strcmp(key->string.buff, "releaseyear"))
            db_info->releaseyear = val->uint_;

         if (!strcmp(key->string.buff, "rumble"))
            db_info->rumble_supported = val->uint_;

         if (!strcmp(key->string.buff, "analog"))
            db_info->analog_supported = val->uint_;

         if (!strcmp(key->string.buff, "crc"))
            db_info->crc32 = bin_to_hex_alloc((uint8_t*)val->binary.buff, val->binary.len);
         if (!strcmp(key->string.buff, "sha1"))
            db_info->sha1 = bin_to_hex_alloc((uint8_t*)val->binary.buff, val->binary.len);
         if (!strcmp(key->string.buff, "md5"))
            db_info->md5 = bin_to_hex_alloc((uint8_t*)val->binary.buff, val->binary.len);
      }
      k++;
   }

   database_info_list->list  = database_info;
   database_info_list->count = k;

   return database_info_list;

error:
   libretrodb_cursor_close(&cur);
   libretrodb_close(&db);
   database_info_list_free(database_info_list);
   return NULL;
}

void database_info_list_free(database_info_list_t *database_info_list)
{
   size_t i;

   if (!database_info_list)
      return;

   for (i = 0; i < database_info_list->count; i++)
   {
      database_info_t *info = &database_info_list->list[i];

      if (!info)
         continue;

      if (info->name)
         free(info->name);
      if (info->description)
         free(info->description);
      if (info->publisher)
         free(info->publisher);
      if (info->developer)
         free(info->developer);
      if (info->origin)
         free(info->origin);
      if (info->franchise)
         free(info->franchise);
      if (info->edge_magazine_review)
         free(info->edge_magazine_review);

      if (info->cero_rating)
         free(info->cero_rating);
      if (info->pegi_rating)
         free(info->pegi_rating);
      if (info->enhancement_hw)
         free(info->enhancement_hw);
      if (info->elspa_rating)
         free(info->elspa_rating);
      if (info->esrb_rating)
         free(info->esrb_rating);
      if (info->bbfc_rating)
         free(info->bbfc_rating);
      if (info->crc32)
         free(info->crc32);
      if (info->sha1)
         free(info->sha1);
      if (info->md5)
         free(info->md5);
   }

   free(database_info_list->list);
   free(database_info_list);
}
