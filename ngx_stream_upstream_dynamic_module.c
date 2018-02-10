// Copyright (C) liwq


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_stream.h>


typedef struct {
    ngx_stream_upstream_init_pt         original_init_upstream;
    ngx_stream_upstream_init_peer_pt    original_init_peer;
} ngx_stream_upstream_dynamic_srv_conf_t;


typedef struct {
    ngx_stream_upstream_srv_conf_t     *uscf;
    in_port_t                           port;
} ngx_stream_upstream_resolover_ctx_t;


static char *ngx_stream_upstream_server_resolver(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static void *ngx_stream_upstream_dynamic_create_srv_conf(ngx_conf_t *cf);


static ngx_command_t  ngx_stream_upstream_dynamic_commands[] = {

    { ngx_string("server_resolver"),
      NGX_STREAM_UPS_CONF|NGX_CONF_NOARGS,
      ngx_stream_upstream_server_resolver,
      NGX_STREAM_SRV_CONF_OFFSET,
      0,
      NULL },

      ngx_null_command
};


static ngx_stream_module_t  ngx_stream_upstream_dynamic_module_ctx = {
    NULL,                                          /* preconfiguration */
    NULL,                                          /* postconfiguration */

    NULL,                                          /* create main configuration */
    NULL,                                          /* init main configuration */

    ngx_stream_upstream_dynamic_create_srv_conf,   /* create server configuration */
    NULL                                           /* merge server configuration */
};


ngx_module_t  ngx_stream_upstream_dynamic_module = {
    NGX_MODULE_V1,
    &ngx_stream_upstream_dynamic_module_ctx,  /* module context */
    ngx_stream_upstream_dynamic_commands,     /* module directives */
    NGX_STREAM_MODULE,                        /* module type */
    NULL,                                     /* init master */
    NULL,                                     /* init module */
    NULL,                                     /* init process */
    NULL,                                     /* init thread */
    NULL,                                     /* exit thread */
    NULL,                                     /* exit process */
    NULL,                                     /* exit master */
    NGX_MODULE_V1_PADDING
};


static void *
ngx_stream_upstream_dynamic_create_srv_conf(ngx_conf_t *cf)
{
    ngx_stream_upstream_dynamic_srv_conf_t    *conf;
    conf = ngx_pcalloc(cf->pool, sizeof(ngx_stream_upstream_dynamic_srv_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    return conf;
}



static ngx_stream_upstream_rr_peer_t *
ngx_stream_upstream_zone_copy_peer(ngx_stream_upstream_rr_peers_t *peers, ngx_str_t *server,
    ngx_str_t *host, in_port_t port, struct sockaddr *sockaddr, socklen_t socklen)
{
    size_t                          plen;
    ngx_slab_pool_t                *pool;
    ngx_stream_upstream_rr_peer_t  *dst;

    pool = peers->shpool;
    if (pool == NULL) return NULL;

    ngx_shmtx_lock(&pool->mutex);
    dst = ngx_slab_calloc_locked(pool, sizeof(ngx_stream_upstream_rr_peer_t));
    if (dst == NULL) {
        ngx_shmtx_unlock(&pool->mutex);
        return NULL;
    }

    dst->socklen  = socklen;
    dst->sockaddr = NULL;
    dst->name.data = NULL;
    dst->server.data = NULL;

    if (server == NULL) {
        if (port > 1 && port < 10) {
            plen = 1;
        } else if (port < 100) {
            plen = 2;
        } else if (port < 1000) {
            plen = 3;
        } else if (port < 10000) {
            plen = 4;
        } else {
            plen = 5;
        }
        dst->server.len = host->len + 1 + plen;

    } else {
        dst->server.len = server->len;
    }

    dst->sockaddr = ngx_slab_calloc_locked(pool, sizeof(ngx_sockaddr_t));
    if (dst->sockaddr == NULL) {
        goto failed;
    }

    dst->name.data = ngx_slab_calloc_locked(pool, NGX_SOCKADDR_STRLEN);
    if (dst->name.data == NULL) {
        goto failed;
    }


    ngx_memcpy(dst->sockaddr, sockaddr, socklen);
    ngx_inet_set_port(dst->sockaddr, port);
    dst->name.len = ngx_sock_ntop(dst->sockaddr, socklen, dst->name.data, NGX_SOCKADDR_STRLEN, 1);

    dst->server.data = ngx_slab_alloc_locked(pool, dst->server.len);
    if (dst->server.data == NULL) {
        goto failed;
    }

    if (server == NULL) {
        ngx_memcpy(dst->server.data, host->data, host->len);
        ngx_sprintf(dst->server.data + host->len, ":%d", port);
    } else {
        ngx_memcpy(dst->server.data, server->data, server->len);
    }

    ngx_shmtx_unlock(&pool->mutex);
    return dst;

failed:

    if (dst->server.data) {
        ngx_slab_free_locked(pool, dst->server.data);
    }

    if (dst->name.data) {
        ngx_slab_free_locked(pool, dst->name.data);
    }

    if (dst->sockaddr) {
        ngx_slab_free_locked(pool, dst->sockaddr);
    }

    ngx_slab_free_locked(pool, dst);
    ngx_shmtx_unlock(&pool->mutex);

    return NULL;
}


static void
ngx_stream_upstream_zone_free_peer(ngx_stream_upstream_rr_peers_t *peers,
    ngx_stream_upstream_rr_peer_t *dst)
{
    ngx_slab_pool_t              *pool;

    if (dst == NULL) return;

    pool = peers->shpool;
    if (pool == NULL) return;

    ngx_shmtx_lock(&pool->mutex);

    if (dst->server.data) {
        ngx_slab_free_locked(pool, dst->server.data);
    }

    if (dst->name.data) {
        ngx_slab_free_locked(pool, dst->name.data);
    }

    if (dst->sockaddr) {
        ngx_slab_free_locked(pool, dst->sockaddr);
    }

    ngx_slab_free_locked(pool, dst);
    ngx_shmtx_unlock(&pool->mutex);

    return;
}


static void
ngx_stream_upstream_resolve_handler(ngx_resolver_ctx_t *ctx) {
    time_t                                fail_timeout;
    ngx_int_t                             weight, max_fails;
    struct sockaddr_in                   *sin, *peer_sin;
    u_char                               *p;
    in_port_t                             port;
    ngx_uint_t                            i;
    ngx_str_t                             name;
    ngx_stream_upstream_rr_peer_t        *peer, *nxt, **ups_nxt;
    ngx_stream_upstream_rr_peers_t       *peers;
    ngx_stream_upstream_resolover_ctx_t  *urctx = ctx->data;

    #if (nginx_version >= 1011005)
    ngx_int_t                           max_conns;
    #endif

    peers = urctx->uscf->peer.data;
    port  = urctx->port;
    name  = ctx->name;

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ngx_cycle->log, 0,
        "resolver handler name: \"%V\" state: %i", &name, ctx->state);

    if (NGX_AGAIN == ctx->state) {
        return;
    }

    if (ctx->state) {
        ngx_shmtx_lock(&peers->shpool->mutex);
        ngx_slab_free_locked(peers->shpool, name.data);
        ngx_shmtx_unlock(&peers->shpool->mutex);

        ngx_free(urctx);
        ctx->data = NULL;
        ngx_resolve_name_done(ctx);

        return;
    }

#if (NGX_DEBUG)
    {
    u_char      text[NGX_SOCKADDR_STRLEN];
    ngx_str_t   addr;
    ngx_uint_t  i;

    addr.data = text;

    for (i = 0; i < ctx->naddrs; i++) {
        addr.len = ngx_sock_ntop(ctx->addrs[i].sockaddr, ctx->addrs[i].socklen,
                                 text, NGX_SOCKADDR_STRLEN, 0);
        ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ngx_cycle->log, 0,
                       "resolver handler name: \"%V\" was resolver to: %V", &name, &addr);
    }
    }
#endif

    fail_timeout = 10;
    weight = 1;
    #if (nginx_version >= 1011005)
    max_conns = 0;
    #endif
    max_fails = 1;

    ngx_stream_upstream_rr_peers_wlock(peers);
    for (peer = peers->peer, ups_nxt = &peers->peer; peer; peer = nxt) {

        nxt = peer->next;
        p = ngx_strlchr(peer->server.data, peer->server.data + peer->server.len, ':');
        if ((p != NULL && (size_t)(p - peer->server.data) != name.len) ||
            (p == NULL && peer->server.len != name.len) ||
            ngx_strncmp(peer->server.data, name.data, name.len) != 0)
        {
            ups_nxt = &peer->next;
            continue;
        }

        fail_timeout = peer->fail_timeout;
        #if (nginx_version >= 1011005)
        max_conns    = peer->max_conns;
        #endif
        max_fails    = peer->max_fails;
        weight       = peer->weight;

        // TODO:
        peer_sin = (struct sockaddr_in *)peer->sockaddr;
        for (i = 0; i < ctx->naddrs; ++i) {
            sin = (struct sockaddr_in *)ctx->addrs[i].sockaddr;
            // The IP does not change. keep this peer.
            if (peer_sin->sin_addr.s_addr == sin->sin_addr.s_addr) {
                ups_nxt = &peer->next;
                goto skip_del;
            }
        }

        // The IP is not exists, down or free this peer.
        if (peer->conns > 0) {
            ups_nxt = &peer->next;
            peer->down |= 0x2;
            continue;
        }

        peers->number--;
        peers->total_weight -= weight;
        *ups_nxt = nxt;
        ngx_stream_upstream_zone_free_peer(peers, peer);

    skip_del:
        continue;
    }

    for (i = 0; i < ctx->naddrs; ++i) {
        // TODO:
        sin = (struct sockaddr_in *)ctx->addrs[i].sockaddr;
        for (peer = peers->peer; peer; peer = peer->next) {
            peer_sin = (struct sockaddr_in *)peer->sockaddr;
            // The IP have exists. update the expire.
            if (peer_sin->sin_addr.s_addr == sin->sin_addr.s_addr) {
                #if (NGX_COMPAT)
                peer->spare[0] = ctx->valid;
                #endif
                goto skip_add;
            }
        }

        peer = ngx_stream_upstream_zone_copy_peer(peers, NULL, &name, port, ctx->addrs[i].sockaddr, ctx->addrs[i].socklen);
        if (peer == NULL) {
            continue;
        }
        peer->fail_timeout = fail_timeout;
        #if (nginx_version >= 1011005)
        peer->max_conns = max_conns;
        #endif
        peer->max_fails = max_fails;
        peer->weight = weight;
        peer->effective_weight = weight;
        peer->current_weight   = 0;
        #if (NGX_COMPAT)
        peer->spare[0] = ctx->valid;
        #endif

        peer->next = peers->peer;
        peers->peer = peer;
        peers->number++;
        peers->total_weight += weight;

    skip_add:
        continue;
    }

    peers->single = (peers->number == 1);

    ngx_shmtx_lock(&peers->shpool->mutex);
    ngx_slab_free_locked(peers->shpool, name.data);
    ngx_shmtx_unlock(&peers->shpool->mutex);

    ngx_stream_upstream_rr_peers_unlock(peers);

    ngx_free(urctx);
    ctx->data = NULL;
    ngx_resolve_name_done(ctx);

    return;
}


static ngx_int_t
ngx_stream_upstream_init_resolver_peer(ngx_stream_session_t *s,
    ngx_stream_upstream_srv_conf_t *us)
{
    time_t                                     expire;
    ngx_str_t                                  host;
    ngx_int_t                                  rc;
    ngx_url_t                                  url;
    ngx_resolver_ctx_t                        *ctx, temp;
    ngx_stream_core_srv_conf_t                *cscf;
    ngx_stream_upstream_rr_peer_t             *peer, *nxt, **ups_nxt;
    ngx_stream_upstream_rr_peers_t            *peers;
    ngx_stream_upstream_dynamic_srv_conf_t    *dcf;
    ngx_stream_upstream_resolover_ctx_t       *urctx;

    dcf = ngx_stream_conf_upstream_srv_conf(us,
                                            ngx_stream_upstream_dynamic_module);
    rc = dcf->original_init_peer(s, us);
    if (rc != NGX_OK) {
        return rc;
    }

    peers = us->peer.data;
    ngx_stream_upstream_rr_peers_wlock(peers);
    for (peer = peers->peer, ups_nxt = &peers->peer; peer != NULL; peer = nxt) {
        nxt = peer->next;

        if (peer->down & 0x2 && peer->conns == 0) {
            // Free the down peer.
            *ups_nxt = nxt;
            peers->number--;
            peers->total_weight -= peer->weight;
            ngx_stream_upstream_zone_free_peer(peers, peer);
            continue;
        }

        ngx_memzero(&url, sizeof(ngx_url_t));
        url.url.len = peer->server.len;
        url.url.data = peer->server.data;
        url.no_resolve = 1;
        if (ngx_parse_url(s->connection->pool, &url) != NGX_OK || url.host.len == 0 || url.naddrs > 0) {
            goto next;
        }

        expire = ngx_time();
        #if (NGX_COMPAT)
        expire = peer->spare[0]? peer->spare[0]: 1;
        ngx_log_debug2(NGX_LOG_DEBUG_STREAM, s->connection->log, 0, "compat check resolver \"%V\" expire: %l", &url.host, expire);
        #endif

        if (
        #if (NGX_DEBUG)
            1 ||
        #endif
            expire < ngx_time()
            || peer->fails > peer->max_fails
            )
        {
            break;
        }

        next:
        ups_nxt = &peer->next;
        // not resolver this peer
        url.naddrs = 1;
    }

    if (url.naddrs != 0) {
        // no need resolver domain
        ngx_stream_upstream_rr_peers_unlock(peers);
        return NGX_OK;
    }

    ngx_shmtx_lock(&peers->shpool->mutex);
    host.data = ngx_slab_alloc_locked(peers->shpool, url.host.len);
    ngx_shmtx_unlock(&peers->shpool->mutex);

    if (host.data == NULL) {
        ngx_stream_upstream_rr_peers_unlock(peers);
        return NGX_OK;
    }

    ngx_memcpy(host.data, url.host.data, url.host.len);
    host.len = url.host.len;

    #if (NGX_COMPAT)
    ngx_stream_upstream_rr_peer_lock(peers, peer);
    peer->spare[0] += 10;
    ngx_stream_upstream_rr_peer_unlock(peers, peer);
    #endif

    ngx_stream_upstream_rr_peers_unlock(peers);


    urctx = NULL;

    cscf = ngx_stream_get_module_srv_conf(s, ngx_stream_core_module);
    if (cscf == NULL) {
        goto failed;
    }

    urctx = ngx_alloc(sizeof(ngx_stream_upstream_resolover_ctx_t), cscf->resolver->log);
    if (urctx == NULL) {
        ngx_log_error(NGX_LOG_WARN, s->connection->log, 0, "alloc ctx null. size: %uz", sizeof(ngx_stream_upstream_resolover_ctx_t));
        goto failed;
    }

    temp.name = host;
    ctx = ngx_resolve_start(cscf->resolver, &temp);
    if (ctx == NULL) {
        goto failed;
    }
    if (ctx == NGX_NO_RESOLVER) {
        ngx_log_error(NGX_LOG_WARN, s->connection->log, 0,
                          "no resolver defined to resolve %V", &host);
        goto failed;
    }

    urctx->port = url.port;
    urctx->uscf = us;

    ctx->data = urctx;
    ctx->name = host;
    ctx->handler = ngx_stream_upstream_resolve_handler;
    ctx->timeout = cscf->resolver_timeout;

    ngx_log_debug1(NGX_LOG_DEBUG_STREAM, s->connection->log, 0, "start resolver \"%V\"", &host);

    if (ngx_resolve_name(ctx) != NGX_OK) {
        goto failed;
    }

    return NGX_OK;


failed:

    ngx_shmtx_lock(&peers->shpool->mutex);
    ngx_slab_free_locked(peers->shpool, host.data);
    ngx_shmtx_unlock(&peers->shpool->mutex);
    if (NULL != urctx) {
        ngx_free(urctx);
    }

    return NGX_OK;
}


static ngx_int_t
ngx_stream_upstream_init_resolver(ngx_conf_t *cf,
    ngx_stream_upstream_srv_conf_t *us)
{
    ngx_int_t                                  rc;
    ngx_stream_upstream_dynamic_srv_conf_t    *dcf;

    dcf = ngx_stream_conf_upstream_srv_conf(us,
                                            ngx_stream_upstream_dynamic_module);
    if (NULL == dcf) {
        return NGX_ERROR;
    }

    rc = dcf->original_init_upstream(cf, us);
    if (rc != NGX_OK) {
        return rc;
    }

    dcf->original_init_peer = us->peer.init;
    us->peer.init = ngx_stream_upstream_init_resolver_peer;

    return NGX_OK;
}


static char *
ngx_stream_upstream_server_resolver(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_stream_upstream_srv_conf_t            *uscf;
    ngx_stream_upstream_dynamic_srv_conf_t    *dcf = conf;

    uscf = ngx_stream_conf_get_module_srv_conf(cf, ngx_stream_upstream_module);

    if (NULL != dcf->original_init_upstream) {
        return "is duplicate";
    }

#if (NGX_STREAM_UPSTREAM_ZONE)
    if (uscf->shm_zone == NULL) {
        return "must reside in the shared memory";
    }
#else
    return "must enables upstream zone";
#endif

    dcf->original_init_upstream = uscf->peer.init_upstream ? uscf->peer.init_upstream:
                                  ngx_stream_upstream_init_round_robin;

    uscf->peer.init_upstream = ngx_stream_upstream_init_resolver;

    return NGX_CONF_OK;
}
