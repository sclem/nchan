#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

//with the declarations
typedef struct {
	ngx_int_t                       index;
	time_t                          buffer_timeout;
	ngx_int_t                       max_message_queue_size;
} ngx_http_push_loc_conf_t;

#define NGX_HTTP_PUSH_DEFAULT_SHM_SIZE 3145728 //3 megs
#define NGX_HTTP_PUSH_DEFAULT_BUFFER_TIMEOUT 3600
#define NGX_HTTP_PUSH_DEFAULT_MESSAGE_QUEUE_SIZE 5

typedef struct {
	size_t                          shm_size;
} ngx_http_push_main_conf_t;

//message queue
typedef struct {
    ngx_queue_t                     queue;
	ngx_str_t                       content_type;
	ngx_str_t                       charset;
	ngx_buf_t                      *buf;
	time_t                          expires;
	time_t                          message_time; //tag message by time
	ngx_int_t                       message_tag; //used in conjunction with message_time if more than one message have the same time.
} ngx_http_push_msg_t;

typedef struct ngx_http_push_listener_s ngx_http_push_listener_t;
typedef struct ngx_http_push_node_s ngx_http_push_node_t;

//listener request queue
struct ngx_http_push_listener_s {
    ngx_queue_t                        queue;
	ngx_http_request_t                *request;
};

//our typecast-friendly rbtree node
struct ngx_http_push_node_s {
	ngx_rbtree_node_t               node;
	ngx_str_t                       id;
    ngx_http_push_msg_t            *message_queue;
	ngx_uint_t                      message_queue_size;
	ngx_http_push_listener_t       *listener_queue;
	ngx_uint_t                      listener_queue_size;
	time_t                          last_seen;
}; 

//sender stuff
static char *       ngx_http_push_sender(ngx_conf_t *cf, ngx_command_t *cmd, void *conf); //push_sender hook
static ngx_int_t    ngx_http_push_sender_handler(ngx_http_request_t * r);
static void         ngx_http_push_sender_body_handler(ngx_http_request_t * r);
static ngx_int_t    ngx_http_push_node_info(ngx_http_request_t *r, ngx_uint_t message_queue_size, ngx_uint_t listener_queue_size, time_t last_seen);

//listener stuff
static char *       ngx_http_push_listener(ngx_conf_t *cf, ngx_command_t *cmd, void *conf); //push_listener hook
static ngx_int_t    ngx_http_push_listener_handler(ngx_http_request_t * r);

//response generating stuff
static ngx_int_t    ngx_http_push_set_listener_header(ngx_http_request_t *r, ngx_http_push_msg_t *msg);
static ngx_chain_t *ngx_http_push_create_output_chain(ngx_http_request_t *r, ngx_buf_t *buf, ngx_slab_pool_t *shpool);
static void         ngx_http_push_copy_preallocated_buffer(ngx_buf_t *buf, ngx_buf_t *cbuf);
static ngx_int_t    ngx_http_push_set_listener_body(ngx_http_request_t *r, ngx_chain_t *out);

//misc stuff
ngx_shm_zone_t *    ngx_http_push_shm_zone = NULL;
static char *       ngx_http_push_setup_handler(ngx_conf_t *cf, void * conf, ngx_int_t (*handler)(ngx_http_request_t *));
static void *       ngx_http_push_create_main_conf(ngx_conf_t *cf);
static void *       ngx_http_push_create_loc_conf(ngx_conf_t *cf);
static char *       ngx_http_push_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);
static ngx_int_t    ngx_http_push_set_up_shm(ngx_conf_t *cf, size_t shm_size);
static ngx_int_t    ngx_http_push_init_shm_zone(ngx_shm_zone_t * shm_zone, void * data);
static ngx_int_t    ngx_http_push_postconfig(ngx_conf_t *cf);

static ngx_http_push_msg_t * ngx_http_push_dequeue_message(ngx_http_push_node_t * node); // doesn't free associated memory
static ngx_http_push_msg_t * ngx_http_push_find_message(ngx_http_push_node_t * node, ngx_http_request_t *r, ngx_int_t *status);
static ngx_http_push_listener_t * ngx_http_push_dequeue_listener(ngx_http_push_node_t * node); //doesn't free associated memory
static ngx_inline ngx_http_push_listener_t *ngx_http_push_queue_listener_request(ngx_http_push_node_t * node, ngx_http_request_t *r, ngx_slab_pool_t *shpool);
static ngx_inline ngx_http_push_listener_t *ngx_http_push_queue_listener_request_locked(ngx_http_push_node_t * node, ngx_http_request_t *r, ngx_slab_pool_t *shpool);
//message stuff
static ngx_http_push_msg_t *ngx_http_push_get_last_message(ngx_http_push_node_t * node);
static ngx_http_push_msg_t *ngx_http_push_get_oldest_message(ngx_http_push_node_t * node);
static void ngx_http_push_delete_oldest_message_locked(ngx_slab_pool_t *shpool, ngx_http_push_node_t *node);
static ngx_inline void ngx_http_push_delete_message(ngx_slab_pool_t *shpool, ngx_http_push_node_t *node, ngx_http_push_msg_t *msg);
static ngx_inline void ngx_http_push_delete_message_locked(ngx_slab_pool_t *shpool, ngx_http_push_node_t *node, ngx_http_push_msg_t *msg);

//missing in nginx < 0.7.?
#ifndef ngx_queue_insert_tail
#define ngx_queue_insert_tail(h, x)                                           \
    (x)->prev = (h)->prev;                                                    \
    (x)->prev->next = x;                                                      \
    (x)->next = h;                                                            \
    (h)->prev = x
#endif

