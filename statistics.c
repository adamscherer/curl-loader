/*
 *     statistics.c
 *
 * 2006 Copyright (c)
 * Robert Iakobashvili, <coroberti@gmail.com>
 * Michael Moser,  <moser.michael@gmail.com>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

// must be first include
#include "fdsetsize.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "json.h"

#include "batch.h"
#include "client.h"
#include "loader.h"
#include "conf.h"

#include "statistics.h"
#include "screen.h"

#define UNSECURE_APPL_STR "H/F   "
#define SECURE_APPL_STR "H/F/S "


static void
dump_snapshot_interval_and_advance_total_statistics (batch_context* bctx,
                                                     unsigned long now_time,
                                                     int clients_total_num);

static void dump_statistics (unsigned long period,
                             stat_point *http,
                             stat_point *https);

static void print_statistics_footer_to_file (FILE* file);

static void print_statistics_data_to_file (FILE* file,
                                           unsigned long timestamp,
                                           char* prot,
                                           long clients_num,
                                           stat_point *sd,
                                           unsigned long period);

static void print_operational_statistics (FILE *opstats_file,
                                          op_stat_point*const osp_curr,
                                          op_stat_point*const osp_total,
                                          url_context* url_arr);

static void dump_stat_to_screen (char* protocol,
                                 stat_point* sd,
                                 unsigned long period);

static void dump_clients (client_context* cctx_array);

static void store_json_data (batch_context* bctx,
                             unsigned long now,
                             int clients_total_num,
                             op_stat_point*const osp_total,
                             stat_point *http,
                             stat_point *https);

/****************************************************************************************
* Function name - stat_point_add
*
* Description - Adds counters of one stat_point object to another
* Input -       *left  -pointer to the stat_point, where counter will be added
*               *right -pointer to the stat_point, which counter will be
*                       added to the <left>
* Return Code/Output - None
****************************************************************************************/
void stat_point_add (stat_point* left, stat_point* right)
{
        if (!left || !right)
                return;

        left->data_in += right->data_in;
        left->data_out += right->data_out;

        left->requests += right->requests;

        left->resp_1xx += right->resp_1xx;
        left->resp_2xx += right->resp_2xx;
        left->resp_3xx += right->resp_3xx;
        left->resp_4xx += right->resp_4xx;
        left->resp_5xx += right->resp_5xx;
        left->other_errs += right->other_errs;
        left->url_timeout_errs += right->url_timeout_errs;

        const int total_points = left->appl_delay_points + right->appl_delay_points;

        if (total_points > 0)
        {
                left->appl_delay = (left->appl_delay * left->appl_delay_points  +
                                    right->appl_delay * right->appl_delay_points) / total_points;

                left->appl_delay_points = total_points;
        }
        else
        {
                left->appl_delay = 0;
        }

        const int total_points_2xx = left->appl_delay_2xx_points + right->appl_delay_2xx_points;

        if (total_points_2xx > 0)
        {
                left->appl_delay_2xx = (left->appl_delay_2xx * left->appl_delay_2xx_points  +
                                        right->appl_delay_2xx * right->appl_delay_2xx_points) / total_points_2xx;
                left->appl_delay_2xx_points = total_points_2xx;
        }
        else
        {
                left->appl_delay_2xx = 0;
        }

}

/****************************************************************************************
* Function name - stat_point_reset
*
* Description - Nulls counters of a stat_point structure
*
* Input -       *point -  pointer to the stat_point
* Return Code/Output - None
****************************************************************************************/
void stat_point_reset (stat_point* p)
{
        if (!p)
                return;

        p->data_in = p->data_out = 0;
        p->requests = p->resp_1xx = p->resp_2xx = p->resp_3xx = p->resp_4xx =
                                                                        p->resp_5xx = p->other_errs = p->url_timeout_errs =0;

        p->appl_delay_points = p->appl_delay_2xx_points = 0;
        p->appl_delay = p->appl_delay_2xx = 0;

}

/****************************************************************************************
* Function name - op_stat_point_add
*
* Description - Adds counters of one op_stat_point object to another
* Input -       *left  -  pointer to the op_stat_point, where counter will be added
*               *right -  pointer to the op_stat_point, which counter will be added
*                         to the <left>
* Return Code/Output - None
****************************************************************************************/
void op_stat_point_add (op_stat_point* left, op_stat_point* right)
{
        size_t i;

        if (!left || !right)
                return;

        if (left->url_num != right->url_num)
                return;

        for ( i = 0; i < left->url_num; i++)
        {
                left->url_ok[i] += right->url_ok[i];
                left->url_failed[i] += right->url_failed[i];
                left->url_timeouted[i] += right->url_timeouted[i];
        }

        left->call_init_count += right->call_init_count;
}

/****************************************************************************************
* Function name - op_stat_point_reset
*
* Description - Nulls counters of an op_stat_point structure
* Input -       *point -  pointer to the op_stat_point
* Return Code/Output - None
****************************************************************************************/
void op_stat_point_reset (op_stat_point* point)
{
        if (!point)
                return;

        if (point->url_num)
        {
                size_t i;
                for ( i = 0; i < point->url_num; i++)
                {
                        point->url_ok[i] = point->url_failed[i] = point->url_timeouted[i] = 0;
                }
        }

        /* Don't null point->url_num ! */
        point->call_init_count = 0;
}

/****************************************************************************************
* Function name -  url_stat_point_release
*
* Description - Releases memory allocated by url_stat_point_init ()
* Input -       *point -  pointer to the stat_point, where counter will be added
* Return Code/Output - None
****************************************************************************************/
void url_stat_point_release (stat_point* point)
{
        free (point);
        point = NULL;

        memset (point, 0, sizeof (stat_point));
}

/****************************************************************************************
* Function name -  op_stat_point_release
*
* Description - Releases memory allocated by op_stat_point_init ()
* Input -       *point -  pointer to the op_stat_point, where counter will be added
* Return Code/Output - None
****************************************************************************************/
void op_stat_point_release (op_stat_point* point)
{
        if (point->url_ok)
        {
                free (point->url_ok);
                point->url_ok = NULL;
        }

        if (point->url_failed)
        {
                free (point->url_failed);
                point->url_failed = NULL;
        }

        if (point->url_timeouted)
        {
                free (point->url_timeouted);
                point->url_timeouted = NULL;
        }

        memset (point, 0, sizeof (op_stat_point));
}

/****************************************************************************************
* Function name - op_stat_point_init
*
* Description - Initializes an allocated op_stat_point by allocating relevant pointer
*     fields for counters
*
* Input -       *point  - pointer to the op_stat_point, where counter will be added
*               url_num - number of urls
*
* Return Code/Output - None
****************************************************************************************/
int op_stat_point_init (op_stat_point* point, size_t url_num)
{
        if (!point)
                return -1;

        if (url_num)
        {
                if (!(point->url_ok = calloc (url_num, sizeof (unsigned long))) ||
                    !(point->url_failed = calloc (url_num, sizeof (unsigned long))) ||
                    !(point->url_timeouted = calloc (url_num, sizeof (unsigned long)))
                    )
                {
                        goto allocation_failed;
                }
                else
                {
                        point->url_num = url_num;
                }
        }

        point->call_init_count = 0;

        return 0;

allocation_failed:
        fprintf(stderr, "%s - calloc () failed with errno %d.\n",
                __func__, errno);

        return -1;
}

/****************************************************************************************
* Function name -  op_stat_update
*
* Description - Updates operation statistics using information from client context
*
* Input -       *point           - pointer to the op_stat_point, where counters to be updated
*               current_state    - current state of a client
*               prev_state       - previous state of a client
*               current_url_index- current url index of a the client
*               prev_url_index   - previous url index of a the client
*
* Return Code/Output - None
****************************************************************************************/
void op_stat_update (op_stat_point* op_stat,
                     int current_state,
                     int prev_state,
                     size_t current_url_index,
                     size_t prev_url_index)
{
        (void) current_url_index;

        if (!op_stat)
                return;

        if (prev_state == CSTATE_URLS)
        {
                (current_state == CSTATE_ERROR) ? op_stat->url_failed[prev_url_index]++ :
                op_stat->url_ok[prev_url_index]++;
        }

        return;
}

void op_stat_timeouted (op_stat_point* op_stat, size_t url_index)
{
        if (!op_stat)
                return;

        op_stat->url_timeouted[url_index]++;
}

void op_stat_call_init_count_inc (op_stat_point* op_stat)
{
        op_stat->call_init_count++;
}

/****************************************************************************************
* Function name - get_tick_count
*
* Description - Delivers timestamp in milliseconds.
*
* Return Code/Output - timestamp in milliseconds or -1 on errors
****************************************************************************************/
unsigned long get_tick_count ()
{
        struct timeval tval;

        if (gettimeofday (&tval, NULL) == -1)
        {
                fprintf(stderr, "%s - gettimeofday () failed with errno %d.\n",
                        __func__, errno);
                exit (1);
        }

        return tval.tv_sec * 1000 + (tval.tv_usec / 1000);
}

/****************************************************************************************
* Function name - dump_final_statistics
*
* Description - Dumps final statistics counters to stdout and statistics file using
*               print_snapshot_interval_statistics and print_statistics_* functions.
*               At the end calls dump_clients () to dump the clients table.
*
* Input -       *cctx - pointer to client context, where the decision to
*                       complete loading (and dump) has been made.
* Return Code/Output - None
****************************************************************************************/
void dump_final_statistics (client_context* cctx)
{
        int i;
        batch_context* bctx = cctx->bctx;
        unsigned long now = get_tick_count();

        for (i = 0; i <= threads_subbatches_num; i++)
        {
                if (i)
                {
                        stat_point_add (&bctx->http_delta, &(bctx + i)->http_delta);
                        stat_point_add (&bctx->https_delta, &(bctx + i)->https_delta);

                        /* Other threads statistics - reset just after collecting */
                        stat_point_reset (&(bctx + i)->http_delta);
                        stat_point_reset (&(bctx + i)->https_delta);
                }
        }

        print_snapshot_interval_statistics (now - bctx->last_measure,
                                            &bctx->http_delta,
                                            &bctx->https_delta);

        stat_point_add (&bctx->http_total, &bctx->http_delta);
        stat_point_add (&bctx->https_total, &bctx->https_delta);

        fprintf(stderr,"\n==================================================="
                "====================================\n");
        fprintf(stderr,"End of the test for batch: %-10.10s\n", bctx->batch_name);
        fprintf(stderr,"======================================================"
                "=================================\n\n");

        now = get_tick_count();

        const int seconds_run = (int)(now - bctx->start_time)/ 1000;
        if (!seconds_run)
                return;

        fprintf(stderr,"\nTest total duration was %d seconds and CAPS average %ld:\n",
                seconds_run, bctx->op_total.call_init_count / seconds_run);

        dump_statistics (seconds_run,
                         &bctx->http_total,
                         &bctx->https_total);

        for (i = 0; i <= threads_subbatches_num; i++)
        {
                if (i)
                {
                        op_stat_point_add (&bctx->op_delta, &(bctx + i)->op_delta );

                        /* Other threads operational statistics - reset just after collecting */
                        op_stat_point_reset (&(bctx + i)->op_delta);
                }
        }

        op_stat_point_add (&bctx->op_total, &bctx->op_delta);

        print_operational_statistics (bctx->opstats_file,
                                      &bctx->op_delta,
                                      &bctx->op_total,
                                      bctx->url_ctx_array);

        store_json_data(bctx, now, 0, &bctx->op_total, &bctx->http_total, &bctx->https_total);

        if (bctx->statistics_file)
        {
                print_statistics_footer_to_file (bctx->statistics_file);
                print_statistics_header (bctx->statistics_file);

                const unsigned long loading_t = now - bctx->start_time;
                const unsigned long loading_time = loading_t ? loading_t : 1;

                print_statistics_data_to_file (bctx->statistics_file,
                                               loading_time/1000,
                                               UNSECURE_APPL_STR,
                                               pending_active_and_waiting_clients_num_stat (bctx),
                                               &bctx->http_total,
                                               loading_time);

                print_statistics_data_to_file (bctx->statistics_file,
                                               loading_time/1000,
                                               SECURE_APPL_STR,
                                               pending_active_and_waiting_clients_num_stat (bctx),
                                               &bctx->https_total,
                                               loading_time);
        }

        dump_clients (cctx);
        (void)fprintf (stderr, "\nExited. For details look in the files:\n"
                       "- %s.log for errors and traces;\n"
                       "- %s.txt for loading statistics;\n"
                       "- %s.ctx for virtual client based statistics.\n",
                       bctx->batch_name, bctx->batch_name, bctx->batch_name);
        if (bctx->dump_opstats)
                (void)fprintf (stderr,"- %s.ops for operational statistics.\n",
                               bctx->batch_name);
        (void)fprintf (stderr,
                       "Add -v and -u options to the command line for "
                       "verbose output to %s.log file.\n",bctx->batch_name);
}

/**************************************************
 * Function name - ascii_time
 *
 * Description - evaluate current time in ascii
 *
 * Input -       *tbuf - pointer to time buffer
 * Return -      tbuf filled with time
 ***************************************************/

char *ascii_time (char *tbuf)
{
        time_t timeb;

        (void)time(&timeb);
        return ctime_r(&timeb,tbuf);
}

/****************************************************************************************
* Function name - dump_snapshot_interval
*
* Description - Dumps summary statistics since the start of load
* Input -       *bctx - pointer to batch structure
*               now   - current time in msec since epoch
* Return Code/Output - None
****************************************************************************************/
void dump_snapshot_interval (batch_context* bctx, unsigned long now)
{
        if (!stop_loading)
        {
                fprintf(stderr, "\033[2J");
        }

        int i;
        int total_current_clients = 0;

        for (i = 0; i <= threads_subbatches_num; i++)
        {
                total_current_clients +=
                        pending_active_and_waiting_clients_num_stat (bctx + i);
        }

        dump_snapshot_interval_and_advance_total_statistics (bctx,
                                                             now,
                                                             total_current_clients);

        int seconds_run = (int)(now - bctx->start_time)/ 1000;
        if (!seconds_run)
        {
                seconds_run = 1;
        }

        fprintf(stderr,"--------------------------------------------------------------------------------\n");

        fprintf(stderr,"Summary stats (runs:%d secs, CAPS-average:%ld):\n",
                seconds_run, bctx->op_total.call_init_count / seconds_run);

        dump_statistics (seconds_run,
                         &bctx->http_total,
                         &bctx->https_total);

        fprintf(stderr,"============================================================"
                "=====================\n");

        long total_clients_rampup_inc = 0;
        int total_client_num_max = 0;

        for (i = 0; i <= threads_subbatches_num; i++)
        {
                total_clients_rampup_inc += (bctx + i)->clients_rampup_inc;
                total_client_num_max += (bctx + i)->client_num_max;
        }

        if (bctx->do_client_num_gradual_increase &&
            (bctx->stop_client_num_gradual_increase == 0))
        {
                fprintf(stderr," Automatic: adding %ld clients/sec. Stop inc and manual [M].\n",
                        total_clients_rampup_inc);
        }
        else
        {
                const int current_clients =
                        pending_active_and_waiting_clients_num_stat (bctx);

                fprintf(stderr," Manual: clients:max[%d],curr[%d]. Inc num: [+|*].",
                        total_client_num_max, total_current_clients);

                if (bctx->stop_client_num_gradual_increase &&
                    bctx->clients_rampup_inc &&
                    current_clients < bctx->client_num_max)
                {
                        fprintf(stderr," Automatic: [A].\n");
                }
                else
                {
                        fprintf(stderr,"\n");
                }
        }

        fprintf(stderr,"============================================================"
                "=====================\n");
        fflush (stdout);
}

/****************************************************************************************
* Function name - print_snapshot_interval_statistics
*
* Description - Outputs latest snapshot interval statistics.
*
* Input -       period - latest time period in milliseconds
*               *http  - pointer to the HTTP collected statistics to output
*               *https - pointer to the HTTPS collected statistics to output
* Return Code/Output - None
****************************************************************************************/
void print_snapshot_interval_statistics (unsigned long period,
                                         stat_point *http,
                                         stat_point *https)
{
        period /= 1000;
        if (period == 0)
        {
                period = 1;
        }

        dump_stat_to_screen (UNSECURE_APPL_STR, http, period);
        dump_stat_to_screen (SECURE_APPL_STR, https, period);
}


/****************************************************************************************
* Function name - dump_snapshot_interval_and_advance_total_statistics
*
* Description - Dumps snapshot_interval statistics for the latest loading time
*               period and adds this statistics to the total loading counters.
*
* Input -       *bctx    - pointer to batch context
*               now_time - current time in msec since the epoch
*
* Return Code/Output - None
****************************************************************************************/
void dump_snapshot_interval_and_advance_total_statistics (batch_context* bctx,
                                                          unsigned long now_time,
                                                          int clients_total_num)
{

        int i;
        const unsigned long delta_t = now_time - bctx->last_measure;
        const unsigned long delta_time = delta_t ? delta_t : 1;

        if (stop_loading)
        {
                dump_final_statistics (bctx->cctx_array);
                screen_release ();
                exit (1);
        }

        fprintf(stderr,"============  loading batch is: %-10.10s ===================="
                "==================\n",
                bctx->batch_name);

        /*Collect the operational statistics*/

        for (i = 0; i <= threads_subbatches_num; i++)
        {
                if (i)
                {
                        op_stat_point_add (&bctx->op_delta, &(bctx + i)->op_delta );

                        /* Other threads operational statistics - reset just after collecting */
                        op_stat_point_reset (&(bctx + i)->op_delta);
                }
        }

        op_stat_point_add (&bctx->op_total, &bctx->op_delta );

        print_operational_statistics (bctx->opstats_file,
                                      &bctx->op_delta,
                                      &bctx->op_total,
                                      bctx->url_ctx_array);


        fprintf(stderr,"--------------------------------------------------------------------------------\n");

        fprintf(stderr,"Interval stats (latest:%ld sec, clients:%d, CAPS-curr:%ld):\n",
                (unsigned long ) delta_time/1000, clients_total_num,
                bctx->op_delta.call_init_count* 1000/delta_time);

        op_stat_point_reset (&bctx->op_delta);


        for (i = 0; i <= threads_subbatches_num; i++)
        {
                if (i)
                {
                        stat_point_add (&bctx->http_delta, &(bctx + i)->http_delta);
                        stat_point_add (&bctx->https_delta, &(bctx + i)->https_delta);

                        /* Other threads statistics - reset just after collecting */
                        stat_point_reset (&(bctx + i)->http_delta);
                        stat_point_reset (&(bctx + i)->https_delta);
                }
        }

        stat_point_add (&bctx->http_total, &bctx->http_delta);
        stat_point_add (&bctx->https_total, &bctx->https_delta);

        print_snapshot_interval_statistics(delta_time,
                                           &bctx->http_delta,
                                           &bctx->https_delta);

        store_json_data(bctx, now_time, clients_total_num, &bctx->op_total, &bctx->http_total, &bctx->https_total);

        if (bctx->statistics_file)
        {
                const unsigned long timestamp_sec =  (now_time - bctx->start_time) / 1000;

                print_statistics_data_to_file (bctx->statistics_file,
                                               timestamp_sec,
                                               UNSECURE_APPL_STR,
                                               clients_total_num,
                                               &bctx->http_delta,
                                               delta_time);

                print_statistics_data_to_file (bctx->statistics_file,
                                               timestamp_sec,
                                               SECURE_APPL_STR,
                                               clients_total_num,
                                               &bctx->https_delta,
                                               delta_time);
        }

        stat_point_reset (&bctx->http_delta);
        stat_point_reset (&bctx->https_delta);

        bctx->last_measure = now_time;
}

/****************************************************************************************
* Function name - dump_statistics
*
* Description - Dumps statistics to screen
*
* Input -       period - time interval of the statistics collection in msecs.
*               *http  - pointer to stat_point structure with HTTP/FTP counters collection
*               *https - pointer to stat_point structure with HTTPS/FTPS counters collection
*
* Return Code/Output - None
****************************************************************************************/
static void dump_statistics (unsigned long period,
                             stat_point *http,
                             stat_point *https)
{
        if (period == 0)
        {
                fprintf(stderr,
                        "%s - less than 1 second duration test without statistics.\n",
                        __func__);
                return;
        }

        dump_stat_to_screen (UNSECURE_APPL_STR, http, period);
        dump_stat_to_screen (SECURE_APPL_STR, https, period);
}


/****************************************************************************************
* Function name - dump_stat_to_screen
*
* Description - Dumps statistics to screen
*
* Input -       *protocol - name of the application/protocol
*               *sd       - pointer to statistics data with statistics counters collection
*               period    - time interval of the statistics collection in msecs.
*
* Return Code/Output - None
****************************************************************************************/
static void dump_stat_to_screen (char* protocol,
                                 stat_point* sd,
                                 unsigned long period)
{
        fprintf(stderr, "%sReq:%ld,1xx:%ld,2xx:%ld,3xx:%ld,4xx:%ld,5xx:%ld,Err:%ld,T-Err:%ld,"
                "D:%ldms,D-2xx:%ldms,Ti:%lldB/s,To:%lldB/s\n",
                protocol, sd->requests, sd->resp_1xx, sd->resp_2xx, sd->resp_3xx,
                sd->resp_4xx, sd->resp_5xx, sd->other_errs, sd->url_timeout_errs, sd->appl_delay,
                sd->appl_delay_2xx, sd->data_in/period, sd->data_out/period);

}

/****************************************************************************************
* Function name - print_statistics_header
*
* Description - Prints to a file header for statistics numbers, describing counters
* Input -       *file - open file pointer
* Return Code/Output - None
****************************************************************************************/
void print_statistics_header (FILE* file)
{
        fprintf (file,
                 "RunTime(sec),Appl,Clients,Req,1xx,2xx,3xx,4xx,5xx,Err,T-Err,D,D-2xx,Ti,To\n");
        fflush (file);
}

/****************************************************************************************
* Function name - print_statistics_footer_to_file
*
* Description - Prints to a file separation string between the snapshot_interval statistics and
*               the final statistics number for the total loading process
*
* Input -       *file - open file pointer
* Return Code/Output - None
****************************************************************************************/
static void print_statistics_footer_to_file (FILE* file)
{
        fprintf (file, "*, *, *, *, *, *, *, *, *, *, *, *, *, *\n");
        fflush (file);
}

/****************************************************************************************
* Function name - print_statistics_data_to_file
*
* Description - Prints to a file batch statistics. At run time the interval statistics
*               is printed and at the end of a load - summary statistics.
*
* Input -       *file       - open file pointer
*               timestamp   - time in seconds since the load started
*               *protocol   - name of the applications/protocols
*               clients_num - number of active (running + waiting) clients
*               *sd         - pointer to statistics data with statistics counters collection
*               period      - time interval of the statistics collection in msecs.
*
* Return Code/Output - None
****************************************************************************************/
static void print_statistics_data_to_file (FILE* file,
                                           unsigned long timestamp,
                                           char* prot,
                                           long clients_num,
                                           stat_point *sd,
                                           unsigned long period)
{
        period /= 1000;
        if (period == 0)
        {
                period = 1;
        }

        fprintf (file, "%ld, %s, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %ld, %lld, %lld\n",
                 timestamp, prot, clients_num, sd->requests, sd->resp_1xx, sd->resp_2xx,
                 sd->resp_3xx, sd->resp_4xx, sd->resp_5xx,
                 sd->other_errs, sd->url_timeout_errs, sd->appl_delay, sd->appl_delay_2xx,
                 sd->data_in/period, sd->data_out/period);
        fflush (file);
}

/****************************************************************************************
* Function name - dump_clients
*
* Description - Prints to the <batch-name>.ctx file debug and statistics information
*               collected by every loading client.
*
* Input -       *cctx_array - array of client contexts
*
* Return Code/Output - None
****************************************************************************************/
static void dump_clients (client_context* cctx_array)
{
        batch_context* bctx = cctx_array->bctx;
        char client_table_filename[BATCH_NAME_SIZE+4];
        FILE* ct_file = NULL;
        int i;

        /*
           Init batch logfile for the batch clients output
         */
        sprintf (client_table_filename, "%s.ctx", bctx->batch_name);

        if (!(ct_file = fopen(client_table_filename, "w")))
        {
                fprintf (stderr,
                         "%s - \"%s\" - failed to open file \"%s\" with errno %d.\n",
                         __func__, bctx->batch_name, client_table_filename, errno);
                return;
        }

        for (i = 0; i < bctx->client_num_max; i++)
        {
                dump_client (ct_file, &cctx_array[i]);
        }

        fclose (ct_file);
}

/***********************************************************************************
 * Function name - print_operational_statistics
 *
 * Description - writes number of login, UAS - for each URL and logoff operations
 *               success and failure numbers
 *
 * Input -       *opstats_file - FILE pointer
 *               *osp_curr - pointer to the current operational statistics point
 *               *osp_total - pointer to the current operational statistics point
 *
 * Return Code/Output - None
 *************************************************************************************/
static void print_operational_statistics (FILE *opstats_file,
                                          op_stat_point*const osp_curr,
                                          op_stat_point*const osp_total,
                                          url_context* url_arr)
{
        if (!osp_curr || !osp_total || !opstats_file)
                return;

        (void)fprintf (opstats_file,
                       " Operations:\t\t Success\t\t Failed\t\t\tTimed out\n");

        if (osp_curr->url_num && (osp_curr->url_num == osp_total->url_num))
        {
                unsigned long i;
                for (i = 0; i < osp_curr->url_num; i++)
                {
                        (void)fprintf (opstats_file,
                                       "URL%ld:%-12.12s\t%-6ld %-8ld\t\t%-6ld %-8ld\t\t%-6ld %-8ld\n",
                                       i, url_arr[i].url_short_name,
                                       osp_curr->url_ok[i], osp_total->url_ok[i],
                                       osp_curr->url_failed[i], osp_total->url_failed[i],
                                       osp_curr->url_timeouted[i], osp_total->url_timeouted[i]);
                }
        }
}

/***********************************************************************************
 * * Function name - store_json_data
 * *
 * * Description - writes number of login, UAS - for each URL and logoff operations
 * *               success and failure numbers
 * *
 * * Input -       *opstats_file - FILE pointer
 * *               *osp_curr - pointer to the current operational statistics point
 * *               *osp_total - pointer to the current operational statistics point
 * *
 * * Return Code/Output - None
 * *************************************************************************************/
static void store_json_data (batch_context* bctx,
                             unsigned long now,
                             int clients_total_num,
                             op_stat_point*const osp_total,
                             stat_point *http,
                             stat_point *https)
{
        int seconds_run = (int)(now - bctx->start_time)/ 1000;
        url_context* url_arr = bctx->url_ctx_array;
        stat_point* url_stats = bctx->url_stats;

        json_object *my_object, *my_array, *stat_object;
        my_object = json_object_new_object();
        stat_object = json_object_new_object();
        json_object_object_add(stat_object, "timestamp", json_object_new_int64(now));
        json_object_object_add(stat_object, "totalClients", json_object_new_int(clients_total_num));
        json_object_object_add(stat_object, "secondsRun", json_object_new_int(seconds_run));
        json_object_object_add(stat_object, "totalRequests", json_object_new_int(http->requests + https->requests));
        json_object_object_add(stat_object, "1xxRequests", json_object_new_int(http->resp_1xx + https->resp_1xx));
        json_object_object_add(stat_object, "2xxRequests", json_object_new_int(http->resp_2xx + https->resp_2xx));
        json_object_object_add(stat_object, "3xxRequests", json_object_new_int(http->resp_3xx + https->resp_3xx));
        json_object_object_add(stat_object, "4xxRequests", json_object_new_int(http->resp_4xx + https->resp_4xx));
        json_object_object_add(stat_object, "5xxRequests", json_object_new_int(http->resp_5xx + https->resp_5xx));
        json_object_object_add(stat_object, "totalDataIn", json_object_new_int64(http->data_in + https->data_in));
        json_object_object_add(stat_object, "totalDataOut", json_object_new_int64(http->data_out + https->data_out));

        int points = http->appl_delay_points + https->appl_delay_points;
        if (points > 0)
        {
                json_object_object_add(stat_object, "avgTime", json_object_new_int(
                                               ((http->appl_delay * http->appl_delay_points) +
                                                (https->appl_delay * https->appl_delay_points))
                                               / points ));
        }
        else
        {
                json_object_object_add(stat_object, "avgTime", json_object_new_int(0));
        }

        int points2xx = http->appl_delay_2xx_points + https->appl_delay_2xx_points;
        if (points2xx > 0)
        {
                json_object_object_add(stat_object, "avgTime2xx", json_object_new_int(
                                               ((http->appl_delay_2xx * http->appl_delay_2xx_points) +
                                                (https->appl_delay_2xx * https->appl_delay_2xx_points))
                                               / points2xx ));
        }
        else
        {
                json_object_object_add(stat_object, "avgTime2xx", json_object_new_int(0));
        }

        my_array = json_object_new_array();
        signed long i;
        for (i = 0; i < bctx->urls_num; i++)
        {
                json_object *my_url_object;
                my_url_object = json_object_new_object();
                json_object_object_add(my_url_object, "url", json_object_new_string(url_arr[i].url_str));
                json_object_object_add(my_url_object, "urlShortName", json_object_new_string(url_arr[i].url_short_name));
                json_object_object_add(my_url_object, "success", json_object_new_int(osp_total->url_ok[i]));
                json_object_object_add(my_url_object, "fail", json_object_new_int(osp_total->url_failed[i]));
                json_object_object_add(my_url_object, "timeout", json_object_new_int(osp_total->url_timeouted[i]));
                json_object_object_add(my_url_object, "min", json_object_new_int(url_stats[i].min_resp));
                json_object_object_add(my_url_object, "max", json_object_new_int(url_stats[i].max_resp));
                json_object_object_add(my_url_object, "last", json_object_new_int(url_stats[i].last_resp));
                json_object_object_add(my_url_object, "avg", json_object_new_int(url_stats[i].appl_delay));
                json_object_object_add(my_url_object, "min2xx", json_object_new_int(url_stats[i].min_resp_2xx));
                json_object_object_add(my_url_object, "max2xx", json_object_new_int(url_stats[i].max_resp_2xx));
                json_object_object_add(my_url_object, "last2xx", json_object_new_int(url_stats[i].last_resp_2xx));
                json_object_object_add(my_url_object, "avg2xx", json_object_new_int(url_stats[i].appl_delay_2xx));
                json_object_object_add(my_url_object, "totalRequests", json_object_new_int(url_stats[i].requests));
                json_object_object_add(my_url_object, "1xxRequests", json_object_new_int(url_stats[i].resp_1xx));
                json_object_object_add(my_url_object, "2xxRequests", json_object_new_int(url_stats[i].resp_2xx));
                json_object_object_add(my_url_object, "3xxRequests", json_object_new_int(url_stats[i].resp_3xx));
                json_object_object_add(my_url_object, "4xxRequests", json_object_new_int(url_stats[i].resp_4xx));
                json_object_object_add(my_url_object, "5xxRequests", json_object_new_int(url_stats[i].resp_5xx));
                json_object_object_add(my_url_object, "totalDataIn", json_object_new_int64(url_stats[i].data_in));
                json_object_object_add(my_url_object, "totalDataOut", json_object_new_int64(url_stats[i].data_out));
                json_object_array_add(my_array, my_url_object);
        }

        json_object_object_add(my_object, "stat", stat_object);
        json_object_object_add(my_object, "urls", my_array);

        fprintf(stdout, json_object_to_json_string(my_object));
        fflush (stdout);
}
