/* $Id$ */
/*
 * cleanup.c - Enforces metric/host delete time. Helps keep
 *    memory usage trim and fit by deleting expired metrics from hash.
 *
 * ChangeLog:
 *
 * 25sept2002 - Federico Sacerdoti <fds@sdsc.edu>
 *    Original version.
 *
 * 16feb2003 - Federico Sacerdoti <fds@sdsc.edu>
 *    Made more efficient: O(n) from O(n^2) in
 *    worst case. Fixed bug in hash_foreach() that caused
 *    immortal metrics.
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>

#include "lib/ganglia.h"
#include "lib/hash.h"
#include "lib/barrier.h"
#include "lib/debug_msg.h"

#include "conf.h"
#include "cmdline.h"
#include "key_metrics.h"
#include "metric_typedefs.h"
#include "node_data_t.h"

#ifdef AIX
extern void *h_errno_which(void);
#define h_errno   (*(int *)h_errno_which())
#else
extern int h_errno;
#endif

extern metric_t metric[];

extern int msg_out_socket;
extern pthread_mutex_t msg_out_socket_mutex;

/* The root hash table pointer. */
extern hash_t *cluster;


struct cleanup_arg {
   struct timeval *tv;
   datum_t *key;
   datum_t *val;
   size_t hashval;
};


static int
cleanup_metric ( datum_t *key, datum_t *val, void *arg )
{
   struct cleanup_arg *cleanup = (struct cleanup_arg *) arg;
   struct timeval *tv = cleanup->tv;
   metric_data_t * metric = (metric_data_t *) val->data;
   unsigned int born;

   born = metric->timestamp.tv_sec;

   /* Never delete a metric if its DMAX=0 */
   if (metric->dmax && ((tv->tv_sec - born) > metric->dmax) ) 
      {
         cleanup->key = key;
         return 1;
      }
   return 0;
}


static int
cleanup_node ( datum_t *key, datum_t *val, void *arg )
{
   struct cleanup_arg *nodecleanup = (struct cleanup_arg *) arg;
   struct cleanup_arg cleanup;
   struct timeval *tv = (struct timeval *) nodecleanup->tv;
   node_data_t * node = (node_data_t *) val->data;
   datum_t *rv;
   unsigned int born;

   born = node->timestamp.tv_sec;

   /* Never delete a node if its DMAX=0 */
   if (node->dmax && ((tv->tv_sec - born) > node->dmax) ) {
      /* Host is older than dmax. Delete. */
      nodecleanup->key = key;
      nodecleanup->val = val;
      return 1;
   }

   cleanup.tv = tv;

   cleanup.key = 0;
   cleanup.hashval = 0;
   while (hash_walkfrom(node->hashp, cleanup.hashval, cleanup_metric, (void*)&cleanup)) {

      if (cleanup.key) {
         cleanup.hashval = hashval(cleanup.key, node->hashp);
         rv=hash_delete(cleanup.key, node->hashp);
         if (rv) datum_free(rv);
         cleanup.key=0;
      }
      else break;
   }

   cleanup.key = 0;
   cleanup.hashval = 0;
   while (hash_walkfrom(node->user_hashp, cleanup.hashval, cleanup_metric, (void*)&cleanup)) {

      if (cleanup.key) {
         debug_msg("Cleanup deleting user metric \"%s\" on host \"%s\"", 
            (char*) cleanup.key->data,
            (char*) key->data);
         cleanup.hashval = hashval(cleanup.key, node->user_hashp);
         rv=hash_delete(cleanup.key, node->user_hashp);
         if (rv) datum_free(rv);
         cleanup.key=0;
      }
      else break;
   }

   /* We have not deleted this node */
   return 0;
}


void *
cleanup_thread(void *arg)
{
   struct timeval tv;
   struct cleanup_arg cleanup;
   node_data_t * node;
   datum_t *rv;

   for (;;) {
      /* Cleanup every 3 minutes. */
      sleep(180);

      debug_msg("Cleanup thread running...");

      // report_stats();
      gettimeofday(&tv, NULL);

      cleanup.tv = &tv;
      cleanup.key = 0;
      cleanup.val = 0;
      cleanup.hashval = 0;
      while (hash_walkfrom(cluster, cleanup.hashval, cleanup_node, (void*)&cleanup)) {

         if (cleanup.key) {
            debug_msg("Cleanup deleting host \"%s\"", (char*) cleanup.key->data);
            node = (node_data_t *) cleanup.val->data;
            hash_destroy(node->hashp);
            hash_destroy(node->user_hashp);

            /* No need to WRITE_UNLOCK since there is only one cleanup thread? */
            cleanup.hashval = hashval(cleanup.key, cluster);
            rv=hash_delete(cleanup.key, cluster);
            /* Don't use 'node' pointer after this call. */
            if (rv) datum_free(rv);
            cleanup.key = 0;
         }
         else break;
      }

   } /* for (;;) */

}

