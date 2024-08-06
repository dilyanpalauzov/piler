/*
 * import.c, SJ
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include <locale.h>
#include <syslog.h>
#include <piler.h>


int import_message(struct session_data *sdata, struct data *data, struct counters *counters, struct config *cfg){
   int rc=ERR;
   char *rule;
   struct stat st;
   struct parser_state state;

   if(data->import->delay > 0){
      struct timespec req;

      req.tv_sec = 0;
      req.tv_nsec = 1000000 * data->import->delay;

      nanosleep(&req, NULL);
   }
   if (data->import->la_limit > 0) {
     double loadavg[3];
     while(1) {
       if (getloadavg(loadavg, 3) == -1 || loadavg[0] < data->import->la_limit) break;
       sleep(1);
     }
   }

   init_session_data(sdata, cfg);

   if(cfg->verbosity > 1) printf("processing: %s\n", data->import->filename);

   if(strcmp(data->import->filename, "-") == 0){

      if(read_from_stdin(sdata) == ERR){
         printf("ERROR: error reading from stdin\n");
         return rc;
      }

      snprintf(sdata->filename, SMALLBUFSIZE-1, "%s", sdata->ttmpfile);

   }
   else {

      if(stat(data->import->filename, &st) != 0){
         printf("ERROR: cannot stat() %s\n", data->import->filename);
         return rc;
      }

      if(S_ISREG(st.st_mode) == 0){
         printf("ERROR: %s is not a file\n", data->import->filename);
         return rc;
      }

      snprintf(sdata->filename, SMALLBUFSIZE-1, "%s", data->import->filename);

      sdata->tot_len = st.st_size;
   }


   if(sdata->tot_len < cfg->min_message_size){
      printf("%s is too short: %d bytes\n", sdata->filename, sdata->tot_len);
      return rc;
   }

   data->import->total_size += sdata->tot_len;


   sdata->delivered = 0;

   sdata->import = 1;

   state = parse_message(sdata, 1, data, cfg);
   post_parse(sdata, &state, cfg);
   rule = check_against_ruleset(data->archiving_rules, &state, sdata->tot_len, sdata->spam_message);

   if(rule){
      if(data->quiet == 0) printf("discarding %s by archiving policy: %s\n", data->import->filename, rule);
      rc = OK;
   }
   else {
      make_digests(sdata, cfg);

      if(sdata->hdr_len < 10){
         printf("%s: invalid message, hdr_len: %d\n", data->import->filename, sdata->hdr_len);
         rc = ERR;
      }
      else if(data->import->after > 0 && sdata->sent < data->import->after){
         if(cfg->verbosity > 1) printf("discarding older email: %s (%ld/%ld)\n", sdata->filename, sdata->sent, data->import->after);
         rc = ERR_DISCARDED;
      }
      else if(data->import->before > 0 && sdata->sent > data->import->before){
         if(cfg->verbosity > 1) printf("discarding newer email: %s (%ld/%ld)\n", sdata->filename, sdata->sent, data->import->before);
         rc = ERR_DISCARDED;
      }
      else {
         // When importing emails, we should add the retention value (later) to the original sent value
         sdata->retained = sdata->sent;

         // backup original value of data->folder
         int folder = data->folder;
         rc = process_message(sdata, &state, data, cfg);
         data->folder = folder;
         unlink(state.message_id_hash);
      }
   }

   unlink(sdata->tmpframe);

   remove_stripped_attachments(&state);

   if(strcmp(data->import->filename, "-") == 0) unlink(sdata->ttmpfile);


   switch(rc) {
      case OK:
                        counters->c_rcvd++;
                        counters->c_size += sdata->tot_len;
                        counters->c_stored_size += sdata->stored_len;

                        break;

      case ERR_EXISTS:
                        rc = OK;

                        counters->c_duplicate++;

                        if(data->quiet == 0) printf("duplicate: %s (duplicate id: %llu)\n", data->import->filename, sdata->duplicate_id);
                        break;

      case ERR_DISCARDED:
                        rc = OK;

                        counters->c_ignore++;

                        break;

      default:
                        printf("failed to import: %s (id: %s)\n", data->import->filename, sdata->ttmpfile);
                        break;
   }

   if(rc != OK && data->import->failed_folder){
      char *p = strrchr(data->import->filename, '/');
      if(p)
         p++;
      else
         p = data->import->filename;

      char newpath[SMALLBUFSIZE];
      snprintf(newpath, sizeof(newpath)-2, "%s/%s", data->import->failed_folder, p);

      if(rename(data->import->filename, newpath))
         printf("cannot move %s to %s\n", data->import->filename, newpath);
   }

   return rc;
}


int update_import_table(struct session_data *sdata, struct data *data) {
   int ret=ERR, status=2, started=1;
   struct sql sql;

   if(prepare_sql_statement(sdata, &sql, SQL_PREPARED_STMT_UPDATE_IMPORT_TABLE) == ERR) return ret;

   p_bind_init(&sql);

   sql.sql[sql.pos] = (char *)&(started); sql.type[sql.pos] = TYPE_LONG; sql.pos++;
   sql.sql[sql.pos] = (char *)&(status); sql.type[sql.pos] = TYPE_LONG; sql.pos++;
   sql.sql[sql.pos] = (char *)&(data->import->tot_msgs); sql.type[sql.pos] = TYPE_LONG; sql.pos++;
   sql.sql[sql.pos] = (char *)&(data->import->table_id); sql.type[sql.pos] = TYPE_LONG; sql.pos++;

   if(p_exec_stmt(sdata, &sql) == OK) ret = OK;

   close_prepared_statement(&sql);

   return ret;
}


int get_folder_id(struct session_data *sdata, char *foldername, int parent_id){
   int id=ERR_FOLDER;
   struct sql sql;

   if(prepare_sql_statement(sdata, &sql, SQL_PREPARED_STMT_GET_FOLDER_ID) == ERR) return id;

   p_bind_init(&sql);

   sql.sql[sql.pos] = foldername; sql.type[sql.pos] = TYPE_STRING; sql.pos++;
   sql.sql[sql.pos] = (char *)&parent_id; sql.type[sql.pos] = TYPE_LONG; sql.pos++;

   if(p_exec_stmt(sdata, &sql) == OK){

      p_bind_init(&sql);

      sql.sql[sql.pos] = (char *)&id; sql.type[sql.pos] = TYPE_LONG; sql.len[sql.pos] = sizeof(unsigned long); sql.pos++;

      p_store_results(&sql);
      p_fetch_results(&sql);
      p_free_results(&sql);
   }

   close_prepared_statement(&sql);

   return id;
}


int add_new_folder(struct session_data *sdata, char *foldername, int parent_id){
   int id=ERR_FOLDER;
   struct sql sql;

   if(foldername == NULL) return id;

   if(prepare_sql_statement(sdata, &sql, SQL_PREPARED_STMT_INSERT_INTO_FOLDER_TABLE) == ERR) return id;

   p_bind_init(&sql);

   sql.sql[sql.pos] = foldername; sql.type[sql.pos] = TYPE_STRING; sql.pos++;
   sql.sql[sql.pos] = (char *)&parent_id; sql.type[sql.pos] = TYPE_LONG; sql.pos++;

   if(p_exec_stmt(sdata, &sql) == OK){
      id = p_get_insert_id(&sql);
   }

   close_prepared_statement(&sql);

   return id;
}
