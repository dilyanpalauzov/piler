/*
 * stats.c, SJ
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <locale.h>
#include <syslog.h>
#include <getopt.h>
#include <piler.h>


extern char *optarg;
extern int optind;


int query_counters(struct session_data *sdata, uint64 *rcvd, uint64 *size, uint64 *ssize){
   int rc=ERR;
   struct sql sql;

   if(prepare_sql_statement(sdata, &sql, "select rcvd, size, stored_size from counter") == ERR) return rc;

   p_bind_init(&sql);

   if(p_exec_stmt(sdata, &sql) == OK){
      p_bind_init(&sql);

      sql.sql[sql.pos] = (char *)rcvd; sql.type[sql.pos] = TYPE_LONGLONG; sql.len[sql.pos] = sizeof(uint64); sql.pos++;
      sql.sql[sql.pos] = (char *)size; sql.type[sql.pos] = TYPE_LONGLONG; sql.len[sql.pos] = sizeof(uint64); sql.pos++;
      sql.sql[sql.pos] = (char *)ssize; sql.type[sql.pos] = TYPE_LONGLONG; sql.len[sql.pos] = sizeof(uint64); sql.pos++;

      p_store_results(&sql);

      if(p_fetch_results(&sql) == OK) rc = OK;

      p_free_results(&sql);
   }

   close_prepared_statement(&sql);

   return rc;
}


uint64 query_sphinx(struct session_data *sdata){
   uint64 sphx=0;
   MYSQL_RES *result;
   MYSQL_ROW row;

   p_query(sdata, "SHOW STATUS LIKE 'queries'");

   result = mysql_store_result(&(sdata->mysql));
   if(result){
      row = mysql_fetch_row(result);

      if(row){
         if(mysql_num_fields(result) == 2){
            sphx = strtoull(row[1], NULL, 10);
         }
      }

      mysql_free_result(result);
   }

   return sphx;
}


int main(int argc, char **argv){
   uint64 rcvd=0, size=0, ssize=0, sphx=0;
   struct session_data sdata;
   struct config cfg;
   char *configfile=CONFIG_FILE;

   srand(getpid());

   (void) openlog("pilerstat", LOG_PID, LOG_MAIL);

   cfg = read_config(configfile);

   if(open_database(&sdata, &cfg) == ERR) return 0;

   setlocale(LC_CTYPE, cfg.locale);

   query_counters(&sdata, &rcvd, &size, &ssize);

   close_database(&sdata);


   cfg.mysqlsocket[0] = '\0';
   snprintf(cfg.mysqlhost, MAXVAL-2, "127.0.0.1");
   cfg.mysqlport = 9306;

   if(open_database(&sdata, &cfg) == ERR) return 0;

   sphx = query_sphinx(&sdata);
   
   close_database(&sdata);

   printf("{\n\t\"rcvd\": %llu,\n\t\"size\": %llu,\n\t\"ssize\": %llu,\n\t\"sphx\": %llu\n}\n", rcvd, size, ssize, sphx);

   return 0;
}
