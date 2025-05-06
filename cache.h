#include <stdio.h>
#include "csapp.h"

typedef struct cache_entry{
    char uri[MAXLINE];
    char *object;
    int size;
    struct cache_entry *next;
    struct cache_entry *prev;
}cache_entry;

typedef struct {
    cache_entry *head;
    cache_entry *tail;
    int total_size;
    pthread_rwlock_t lock;
}cache_list;

cache_list cache;

void init_cache();
void move_cache_to_end(cache_list *cache, cache_entry *entry);
void remove_cache(cache_list *cache, int size_needed);
void insert_cache(cache_list *cache, const char *uri, const char *object, int size);
int find_cache(cache_list *cache, const char *uri, char *object_buf, int *size_buf);
