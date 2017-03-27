
/*
 *
 * Author: Thomas Pasquier <tfjmp@g.harvard.edu>
 *
 * Copyright (C) 2017 Harvard University
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2, as
 * published by the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version.
 *
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <netdb.h>
#include <pthread.h>

#include "provenancelib.h"
#include "provenanceutils.h"
#include "provenancePovJSON.h"
#include "libut.h"

#define	LOG_FILE "/tmp/audit.log"
#define gettid() syscall(SYS_gettid)
#define WIN_SIZE 100

static pthread_mutex_t l_log =  PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static pthread_mutex_t c_lock = PTHREAD_MUTEX_INITIALIZER;

FILE *fp=NULL;

int counter = 0;

void _init_logs( void ){
 int n;
 fp = fopen(LOG_FILE, "a+");
 if(!fp){
   printf("Cannot open file\n");
   exit(-1);
 }
 n = fprintf(fp, "Starting audit service...\n");
 printf("%d\n", n);

 provenance_opaque_file(LOG_FILE, true);
}

static inline void print(char* str){
    pthread_mutex_lock(&l_log);
    fprintf(fp, str);
    fprintf(fp, "\n");
    fflush(fp);
    pthread_mutex_unlock(&l_log);
}

// per thread init
void init( void ){
 pid_t tid = gettid();
 pthread_mutex_lock(&l_log);
 fprintf(fp, "audit writer thread, tid:%ld\n", tid);
 pthread_mutex_unlock(&l_log);
}

struct hashable_node {
  prov_entry_t msg;
  struct node_identifier key;
  UT_hash_handle hh;
};

struct hashable_edge {
  prov_entry_t msg;
  struct hashable_edge *next;
};

int edge_compare(struct hashable_edge* he1, struct hashable_edge* he2) {
  uint32_t he1_id = he1->msg.relation_info.identifier.relation_id.id;
  uint32_t he2_id = he2->msg.relation_info.identifier.relation_id.id;
  if (he1_id > he2_id) return 1;
  else if (he1_id == he2_id) return 0;
  else return -1;
}

static struct hashable_node *node_hash_table = NULL;
static struct hashable_edge *edge_hash_head = NULL;

void process(struct hashable_node* nodes, struct hashable_edge* head_edge) {
  struct hashable_edge *elt, *tmp;
  LL_FOREACH_SAFE(head_edge, elt, tmp) {
    //Find nodes of the edge
    struct node_identifier from = elt->msg.relation_info.snd.node_id;
    struct node_identifier to = elt->msg.relation_info.rcv.node_id;
    struct hashable_node *from_node, *to_node;
    HASH_FIND(hh, node_hash_table, &from, sizeof(struct node_identifier), from_node);
    HASH_FIND(hh, node_hash_table, &to, sizeof(struct node_identifier), to_node);
    if (from_node && to_node) {
      //do something with the nodes if both nodes are found
      //TODO: write code here
      //garbage collect the edge
      LL_DELETE(head_edge, elt);
      free(elt);
      counter--;
    }
  }
}

bool filter(prov_entry_t* msg){
  prov_entry_t* elt = malloc(sizeof(prov_entry_t));
  memcpy(elt, msg, sizeof(prov_entry_t));

  if(prov_is_relation(msg)) {
    struct hashable_edge *edge;
    edge = (struct hashable_edge*) malloc(sizeof(struct hashable_edge));
    memset(edge, 0, sizeof(struct hashable_edge));
    edge->msg = *msg;
    pthread_mutex_lock(&c_lock);
    LL_APPEND(edge_hash_head, edge);
    counter++;
    pthread_mutex_unlock(&c_lock);
  } else{
    struct hashable_node *node;
    node = (struct hashable_node*) malloc(sizeof(struct hashable_node));
    memset(node, 0, sizeof(struct hashable_node));
    node->msg = *msg;
    node->key = msg->node_info.identifier.node_id;
    pthread_mutex_lock(&c_lock);
    HASH_ADD(hh, node_hash_table, key, sizeof(struct node_identifier), node);
    pthread_mutex_unlock(&c_lock);
  }

  pthread_mutex_lock(&c_lock);
  if (counter >= WIN_SIZE) {
    LL_SORT(edge_hash_head, edge_compare);
    process(node_hash_table, edge_hash_head);
    pthread_mutex_unlock(&c_lock);
  } else pthread_mutex_unlock(&c_lock);

  print("Received an entry!");
  return false;
}

void log_error(char* err_msg){
  print(err_msg);
}

struct provenance_ops ops = {
 .init=&init,
 .filter=&filter,
 .log_error=&log_error
};

int main(void){
 int rc;
 char json[4096];
	_init_logs();
 fprintf(fp, "Runtime query service pid: %ld\n", getpid());
 rc = provenance_register(&ops);
 if(rc<0){
   fprintf(fp, "Failed registering audit operation (%d).\n", rc);
   exit(rc);
 }
 fprintf(fp, machine_description_json(json));
 fprintf(fp, "\n");
 fflush(fp);

 while(1){
   sleep(1);
 }
 provenance_stop();
 return 0;
}
