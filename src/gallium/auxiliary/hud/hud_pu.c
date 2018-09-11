/**************************************************************************
 *
 * Copyright 2018 Pengutronix, Philipp Zabel
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

/* This file contains code for reading processing unit (GPU, VPU, etc.) load
 * for displaying on the HUD.
 */

#include "hud/hud_private.h"
#include "util/os_time.h"
#include "os/os_thread.h"
#include "util/u_memory.h"
#include "util/u_queue.h"
#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>

static boolean
get_pu_active(const char *filename, uint64_t *busy_time)
{
   char line[1024];
   FILE *f;

   f = fopen(filename, "r");
   if (!f)
      return FALSE;

   if (!feof(f) && fgets(line, sizeof(line), f)) {
      uint64_t active_us;
      int num;

      num = sscanf(line, "%"PRIu64"", &active_us);
      if (num < 1) {
         return FALSE;
      }

      *busy_time = active_us;

      fclose(f);
      return TRUE;
   }

   fclose(f);
   return FALSE;
}

struct pu_info {
   char filename[PATH_MAX];
   uint64_t last_busy, last_time;
};

static void
query_pu_load(struct hud_graph *gr, struct pipe_context *pipe)
{
   struct pu_info *info = gr->query_data;
   uint64_t now = os_time_get();

   if (info->last_time) {
      if (info->last_time + gr->pane->period <= now) {
         uint64_t pu_busy;
         double pu_load;

         get_pu_active(info->filename, &pu_busy);

         pu_load = (pu_busy - info->last_busy) * 100 /
                    (double)(now - info->last_time);

         hud_graph_add_value(gr, pu_load);

         info->last_busy = pu_busy;
         info->last_time = now;
      }
   }
   else {
      /* initialize */
      info->last_time = now;
      get_pu_active(info->filename, &info->last_busy);
   }
}

static void
free_query_data(void *p, struct pipe_context *pipe)
{
   FREE(p);
}

static void
__hud_pu_graph_install(struct hud_pane *pane, const char *prefix,
                       const char *pu_name, const char *path)
{
   struct hud_graph *gr;
   struct pu_info *info;
   char filename[PATH_MAX];
   uint64_t busy;

   snprintf(filename, sizeof(filename), "%s/%s/active_us", path, pu_name);
   if (!get_pu_active(filename, &busy))
      return;

   gr = CALLOC_STRUCT(hud_graph);
   if (!gr)
      return;

   snprintf(gr->name, sizeof(gr->name), "%s-%s", prefix, pu_name);

   gr->query_data = CALLOC_STRUCT(pu_info);
   if (!gr->query_data) {
      FREE(gr);
      return;
   }

   gr->query_new_value = query_pu_load;

   /* Don't use free() as our callback as that messes up Gallium's
    * memory debugger.  Use simple free_query_data() wrapper.
    */
   gr->free_query_data = free_query_data;

   info = gr->query_data;
   strncpy(info->filename, filename, sizeof(info->filename) - 1);

   hud_pane_add_graph(pane, gr);
   hud_pane_set_max_value(pane, 100);
}

void
hud_gpu_graph_install(struct hud_pane *pane, const char *gpu_name)
{
   __hud_pu_graph_install(pane, "gpu", gpu_name,
                          "/sys/class/drm/scheduler/rings");
}

void
hud_v4l2_graph_install(struct hud_pane *pane, const char *v4l2_name)
{
   __hud_pu_graph_install(pane, "v4l2", v4l2_name,
                          "/sys/class/video4linux/stats");
}

static int
__hud_get_num_pus(bool displayhelp, const char *prefix, const char *path)
{
   struct dirent *dent;
   DIR *ring_dir;
   char filename[PATH_MAX];
   uint64_t busy;
   int i = 0;

   ring_dir = opendir(path);
   if (!ring_dir)
      return 0;

   while ((dent = readdir(ring_dir)) != NULL) {
      if (dent->d_type != DT_DIR)
         continue;

      if (!strncmp(dent->d_name, ".", 1))
         continue;

      snprintf(filename, sizeof(filename), "%s/%s/active_us", path,
               dent->d_name);
      if (!get_pu_active(filename, &busy))
         continue;

      if (displayhelp)
         printf("    %s-%s\n", prefix, dent->d_name);

      i++;
   }

   closedir(ring_dir);

   return i;
}

int
hud_get_num_gpus(bool displayhelp)
{
   return __hud_get_num_pus(displayhelp, "gpu",
                            "/sys/class/drm/scheduler/rings");
}

int
hud_get_num_v4l2(bool displayhelp)
{
   return __hud_get_num_pus(displayhelp, "v4l2",
                            "/sys/class/video4linux/stats");
}
