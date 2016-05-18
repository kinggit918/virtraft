/**
 * Copyright (c) 2015, Willem-Hendrik Thiart
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <fcntl.h>

#include "raft.h"
#include "linked_list_queue.h"

#include "usage.c"

#define VERSION "0.1.0"
#define RAFT_BUFLEN 512
#define len(x) (sizeof((x)) / sizeof((x)[0]))

enum {
    NODE_DISCONNECTED,
    NODE_CONNECTED,
    NODE_CONNECTING,
    NODE_DISCONNECTING
};

/** Message types used for peer to peer traffic
 * These values are used to identify message types during deserialization */
typedef enum
{
    MSG_REQUESTVOTE,
    MSG_REQUESTVOTE_RESPONSE,
    MSG_APPENDENTRIES,
    MSG_APPENDENTRIES_RESPONSE,
    /* the node wants to leave */
    MSG_LEAVE,
    MSG_ENTRIES,
    MSG_ENTRIES_RESPONSE,
} peer_message_type_e;

typedef struct
{
    int type;
    int len;
    void* data;

    /* node ID of sender */
    int sender;
} msg_t;

typedef struct
{
    /* the server's node ID */
    int node_id;

    raft_server_t* raft;

    /* messages we want to receive */
    void* inbox;

    /* whether or not this node can communicate with other servers */
    int partitioned;

    int connected;

    int total_offer_count;
} server_t;

typedef struct {
    int node_id;
} entry_cfg_change_t;

typedef struct
{
    server_t* servers;
    int n_servers;
    int n_entries;

    raft_node_t* leader;

    /* stat: max number of entries spotted in an appendentries message */
    int max_entries_in_ae;

    /* stat: number of leadership changes */
    int leadership_changes;

    int log_pops;

    int client_rate;

    int membership_rate;
} system_t;

system_t sys;

options_t opts;

static void __print_tsv()
{
    int i;

    printf("node\t");
    printf("state\t");
    printf("current_idx\t");
    printf("last_log_term\t");
    printf("current_term\t");
    printf("commit_idx\t");
    printf("last_applied_idx\t");
    printf("log_count\t");
    printf("peers\t");
    printf("total_offer_count\t");
    printf("connected\t");
    printf("\n");

    for (i = 0; i < sys.n_servers; i++)
    {
        server_t* sv = &sys.servers[i];
        raft_server_t* r = sv->raft;

        printf("%d:%0.10d\t", i, raft_get_nodeid(r));
        printf("%s\t",
           raft_get_state(r) == RAFT_STATE_LEADER ? "leader\t" :
           raft_get_state(r) == RAFT_STATE_CANDIDATE ? "candidate" :
           "follower");
        printf("%d\t", raft_get_current_idx(r));
        printf("%d\t", raft_get_last_log_term(r));
        printf("%d\t", raft_get_current_term(r));
        printf("%d\t", raft_get_commit_idx(r));
        printf("%d\t", raft_get_last_applied_idx(r));
        printf("%d\t", raft_get_log_count(r));
        printf("%d\t", raft_get_num_nodes(r));
        printf("%d\t", sv->total_offer_count);
        printf("%d\t", sv->connected);
        printf("\n");
    }
}

static void __print_stats()
{
    int i;

    for (i = 0; i < sys.n_servers; i++)
    {
        server_t* sv = &sys.servers[i];
        raft_server_t* r = sys.servers[i].raft;

        printf("node %d:%0.10d\t", i, raft_get_nodeid(r));
        printf("state %s\n",
               raft_get_state(r) == RAFT_STATE_LEADER ? "leader" :
               raft_get_state(r) == RAFT_STATE_CANDIDATE ? "candidate" :
               "follower");
        printf("current_idx %d\n", raft_get_current_idx(r));
        printf("last_log_term %d\n", raft_get_last_log_term(r));
        printf("current_term %d\n", raft_get_current_term(r));
        printf("commit_idx %d\n", raft_get_commit_idx(r));
        printf("last_applied_idx %d\n", raft_get_last_applied_idx(r));
        printf("log_count %d\n", raft_get_log_count(r));
        printf("connected %d\n", sv->connected);
        printf("\n");
    }
}

static server_t* __get_server_from_nodeid(system_t* sys, int node_id)
{
    int i;
    for (i=0; i<sys->n_servers; i++)
        if (sys->servers[i].node_id == node_id)
            return &sys->servers[i];

    return NULL;
}

static void __int_handler(int dummy)
{
    __print_stats();
}

static server_t* __get_leader(system_t* sys)
{
    int i;
    /* for (i=0; i<sys->n_servers; i++) */
    for (i=sys->n_servers-1; 0<=i; i--)
        if (sys->servers[i].connected == NODE_CONNECTED)
            if (raft_is_leader(sys->servers[i].raft))
                return &sys->servers[i];
    return NULL;
}

/** Raft callback for applying an entry to the finite state machine */
static int __raft_applylog(
    raft_server_t* raft,
    void *udata,
    raft_entry_t *ety
    )
{
    int i, last_applied_idx = raft_get_last_applied_idx(raft);

    assert(last_applied_idx <= raft_get_current_idx(raft));

    system_t* sys = udata;

    switch (ety->type)
    {
        case RAFT_LOGTYPE_REMOVE_NODE:
            {
                entry_cfg_change_t *chg = (void*)ety->data.buf;
                int is_self = chg->node_id == raft_get_nodeid(raft);

                printf("COMMIT: %d finished removing %0.10d from %0.10d\n",
                        last_applied_idx, chg->node_id, raft_get_nodeid(raft));

                if (is_self)
                    return RAFT_ERR_SHUTDOWN;

                /* raft_node_t* to_remove = raft_get_node(raft, chg->node_id); */
                /* if (to_remove) */
                /*     raft_remove_node(raft, to_remove); */

                /* #<{(| send leave message to our peers |)}># */
                /* msg_entry_t* entries = calloc(1, sizeof(msg_entry_t) * msg->n_entries); */
                /* memcpy(entries, msg->entries, sizeof(msg_entry_t) * msg->n_entries); */
                /* msg->entries = entries; */
                /*  */
                /* #<{(| collect stats |)}># */
                /* if (sys.max_entries_in_ae < msg->n_entries) */
                /*     sys.max_entries_in_ae = msg->n_entries; */
                /*  */
                /* return __append_msg(udata, msg, MSG_APPENDENTRIES, sizeof(*msg), raft_node_get_id(node), raft); */
                /*  */
            }
            break;

        case RAFT_LOGTYPE_ADD_NONVOTING_NODE:
            break;

        case RAFT_LOGTYPE_ADD_NODE:
            {
                entry_cfg_change_t *chg = (void*)ety->data.buf;

                if (chg->node_id == raft_get_nodeid(raft))
                {
                    server_t* sv = __get_server_from_nodeid(sys, raft_get_nodeid(raft));
                    sv->connected = NODE_CONNECTED;
                    printf("ADD NODE: %d\n", raft_get_nodeid(raft));
                }
            }
            break;

        default:
            break;
    }

    /* State Machine Safety */
    for (i = 0; i < sys->n_servers; i++)
    {
        raft_server_t* other = sys->servers[i].raft;

        if (other == raft || other == NULL)
            continue;

        raft_entry_t* other_ety = raft_get_entry_from_idx(other, last_applied_idx);
        if (other_ety && last_applied_idx <= raft_get_commit_idx(other))
        {
            if (ety->term != other_ety->term || ety->id != other_ety->id)
            {
                printf("node applied bad ety idx:%d (ie. t: %d vs %d) %lx %lx id: %d vs %d\n",
                       last_applied_idx,
                       ety->term,
                       other_ety ? other_ety->term : -999,
                       (unsigned long)raft,
                       (unsigned long)other,
                       ety->id,
                       other_ety ? other_ety->id : -999);
                __print_stats();
                abort();
            }
        }
    }

    return 0;
}

// TODO
/** Raft callback for saving term field to disk.
 * This only returns when change has been made to disk. */
static int __raft_persist_term(
    raft_server_t* raft,
    void *udata,
    const int current_term
    )
{
    return 0;
}

// TODO
/** Raft callback for saving voted_for field to disk.
 * This only returns when change has been made to disk. */
static int __raft_persist_vote(
    raft_server_t* raft,
    void *udata,
    const int voted_for
    )
{
    return 0;
}

/** Raft callback for displaying debugging information */
void __raft_log(raft_server_t* raft, raft_node_t* node, void *udata,
                const char *buf)
{
    /* system_t* sys = udata; */

    if (node)
        /* printf("> %lx, %lx %s\n", */
        /*        (unsigned long)raft, */
        /*        (unsigned long)sys->servers[raft_node_get_id(node)].raft, */
        printf("> %0.10d, %0.10d %s\n",
               raft_get_nodeid(raft),
               raft_node_get_id(node),
               buf);
    else
        printf("> %0.10d               %s\n",
               raft_get_nodeid(raft),
               buf);
}

/** Raft callback for appending an item to the log */
static int __raft_logentry_offer(
    raft_server_t* r,
    void *udata,
    raft_entry_t *ety,
    int ety_idx
    )
{
    system_t* sys = udata;
    server_t* server = __get_server_from_nodeid(sys, raft_get_nodeid(r));

    if (server)
        server->total_offer_count += 1;

    if (!raft_entry_is_cfg_change(ety))
        return 0;

    entry_cfg_change_t *chg = (void*)ety->data.buf;
    raft_node_t* node = raft_get_node(r, chg->node_id);
    int is_self = chg->node_id == raft_get_nodeid(r);

    switch (ety->type)
    {
        case RAFT_LOGTYPE_ADD_NONVOTING_NODE:
            printf("ADD %0.10d to %0.10d\n", chg->node_id, raft_get_nodeid(r));
            if (!is_self)
            {
                raft_node_t* node = raft_add_non_voting_node(r, NULL, chg->node_id, is_self);
                assert(node);
            }
            break;

        case RAFT_LOGTYPE_ADD_NODE:
            printf("ADDV voting %0.10d to %0.10d\n", chg->node_id, raft_get_nodeid(r));
            node = raft_add_node(r, NULL, chg->node_id, is_self);
            assert(node);
            assert(raft_node_is_voting(node));
            break;

        case RAFT_LOGTYPE_REMOVE_NODE:
            printf("REM %0.10d from %0.10d\n", chg->node_id, raft_get_nodeid(r));
            if (node)
            {
                printf("REM OK\n");
                raft_remove_node(r, node);
            }
            break;

        default:
            assert(0);
    }

    return 0;
}

// TODO
/** Raft callback for removing the first entry from the log
 * @note this is provided to support log compaction in the future */
static int __raft_logentry_poll(
    raft_server_t* raft,
    void *udata,
    raft_entry_t *entry,
    int ety_idx
    )
{
    return 0;
}

// TODO
/** Raft callback for deleting the most recent entry from the log.
 * This happens when an invalid leader finds a valid leader and has to delete
 * superseded log entries. */
static int __raft_logentry_pop(
    raft_server_t* raft,
    void *udata,
    raft_entry_t *ety,
    int ety_idx
    )
{
    entry_cfg_change_t *chg = (void*)ety->data.buf;

    if (!raft_entry_is_cfg_change(ety))
        return 0;

    printf("POPPING %0.10d type: %d from %0.10d\n",
        chg->node_id, ety->type, raft_get_nodeid(raft));

    system_t* sys = udata;

    server_t* sv = __get_server_from_nodeid(sys, chg->node_id);

    sys->log_pops += 1;

    switch (ety->type)
    {
        case RAFT_LOGTYPE_REMOVE_NODE:
            {
            int is_self = chg->node_id == raft_get_nodeid(raft);
            raft_node_t* node = raft_add_node(raft, NULL, chg->node_id, is_self);
            assert(node);
            assert(raft_node_is_voting(node));
            if (is_self)
                sv->connected = NODE_CONNECTED;
            }
            break;

        case RAFT_LOGTYPE_ADD_NONVOTING_NODE:
            {
            int is_self = chg->node_id == raft_get_nodeid(raft);
            raft_node_t* node = raft_get_node(raft, chg->node_id);
            raft_remove_node(raft, node);
            if (is_self)
            {
                assert(0);
                sv->connected = NODE_DISCONNECTED;
            }
            }
            break;

        case RAFT_LOGTYPE_ADD_NODE:
            {
            int is_self = chg->node_id == raft_get_nodeid(raft);
            raft_node_t* node = raft_get_node(raft, chg->node_id);
            raft_node_set_voting(node, 0);
            if (is_self)
            {
                assert(sv->connected == NODE_CONNECTING);
                sv->connected = NODE_CONNECTING;
            }
            }
            break;

        default:
            assert(0);
            break;
    }

    return 0;
}

static int __raft_logentry_get_node_id(
    raft_server_t* raft,
    void *udata,
    raft_entry_t *ety,
    int ety_idx
    )
{
    entry_cfg_change_t *chg = (void*)ety->data.buf;
    return chg->node_id;
}

/**
 * @param sys The udata of the raft server sending this
 * @param dst_node_id The sending raft server's node it is sending to
 * @param raft The raft server sending this
 */
static int __append_msg(
    system_t* sys,
    void* data,
    int type,
    int len,
    int dst_node_id,
    raft_server_t* raft
    )
{
    /* drop rate */
    if (random() % 100 < atoi(opts.drop_rate))
        return 0;

    server_t* sv = __get_server_from_nodeid(sys, dst_node_id);
    if (!sv)
        return 0;

    if (sv->partitioned)
        return 0;

    /* put inside peer's inbox */
    do
    {
        msg_t* m = calloc(1, sizeof(msg_t));
        m->type = type;
        m->len = len;
        m->sender = raft_get_nodeid(raft);
        m->data = malloc(len);
        memcpy(m->data, data, len);
        llqueue_offer(sv->inbox, m);
    }
    while (random() % 100 < atoi(opts.dupe_rate));

    return 1;
}

int __raft_send_requestvote(raft_server_t* raft,
                            void* udata,
                            raft_node_t* node,
                            msg_requestvote_t* msg)
{
    return __append_msg(udata, msg, MSG_REQUESTVOTE, sizeof(*msg), raft_node_get_id(node), raft);
}

int sender_requestvote_response(raft_server_t* raft,
                                void* udata,
                                raft_node_t* node,
                                msg_requestvote_response_t* msg)
{
    return __append_msg(udata, msg, MSG_REQUESTVOTE_RESPONSE, sizeof(*msg), raft_node_get_id(node), raft);
}

int __raft_send_appendentries(raft_server_t* raft,
                              void* udata,
                              raft_node_t* node,
                              msg_appendentries_t* msg)
{
    msg_entry_t* entries = calloc(1, sizeof(msg_entry_t) * msg->n_entries);
    memcpy(entries, msg->entries, sizeof(msg_entry_t) * msg->n_entries);
    msg->entries = entries;

    /* collect stats */
    if (sys.max_entries_in_ae < msg->n_entries)
        sys.max_entries_in_ae = msg->n_entries;

    return __append_msg(udata, msg, MSG_APPENDENTRIES, sizeof(*msg), raft_node_get_id(node), raft);
}

int __raft_send_entries(
    raft_server_t* raft,
    void *udata,
    raft_node_t* node,
    msg_entries_t* msg
    )
{
    msg_entries_t* out = calloc(1, sizeof(msg_entries_t));
    printf("sending entries %d\n", raft_get_nodeid(raft));
    memcpy(out, msg, sizeof(msg_entries_t));
    return __append_msg(udata, out, MSG_ENTRIES, sizeof(*out), raft_node_get_id(node), raft);
}

/** Non-voting node now has enough logs to be able to vote.
 * Append a finalization cfg log entry. */
static int __raft_node_has_sufficient_logs(
    raft_server_t* raft,
    void *user_data,
    raft_node_t* node)
{
    server_t* leader = __get_leader(&sys);
    entry_cfg_change_t *change = calloc(1, sizeof(*change));
    change->node_id = raft_node_get_id(node);

    if (!leader)
        return -1;

    printf("SUFFXX %d\n", change->node_id);

    msg_entry_t entry = {
        // FIXME: Should be random
        .id = 1,
        .data.buf = (void*)change,
        .data.len = sizeof(*change),
        .type = RAFT_LOGTYPE_ADD_NODE,
    };

    assert(raft_entry_is_cfg_change(&entry));

    msg_entry_response_t r;
    int e = raft_recv_entry(leader->raft, &entry, &r);
    if (0 == e)
    {
        printf("SUFFICIENT: %0.10d\n", change->node_id);
        return 0;
    }

    printf("INSUFFICIENT: %d\n", change->node_id);
    return -1;
}

raft_cbs_t raft_funcs = {
    .send_requestvote            = __raft_send_requestvote,
    .send_appendentries          = __raft_send_appendentries,
    .send_entries                = __raft_send_entries,
    .applylog                    = __raft_applylog,
    .persist_vote                = __raft_persist_vote,
    .persist_term                = __raft_persist_term,
    .log_offer                   = __raft_logentry_offer,
    .log_poll                    = __raft_logentry_poll,
    .log_pop                     = __raft_logentry_pop,
    .log_get_node_id             = __raft_logentry_get_node_id,
    .node_has_sufficient_logs    = __raft_node_has_sufficient_logs,
    .log                         = __raft_log,
};

static void __create_node(server_t* sv, int id, system_t* sys)
{
    sv->raft = raft_new();
    raft_set_callbacks(sv->raft, &raft_funcs, sys);
    raft_set_election_timeout(sv->raft, 500);
    sv->inbox = llqueue_new();
    sv->connected = NODE_DISCONNECTED;
    sv->node_id = id;
}

static void __shutdown_server(server_t* sv)
{
    printf("shutting down: %0.10d\n", raft_get_nodeid(sv->raft));
    raft_clear(sv->raft);
    sv->connected = NODE_DISCONNECTED;
    while (llqueue_poll(sv->inbox));
    assert(llqueue_count(sv->inbox) == 0);
}

static void __server_poll_messages(server_t* me, system_t* sys)
{
    msg_t* m;

    /* if (random() < 0.5) */
    /*     return; */

    assert(me->connected != NODE_DISCONNECTED);

    while ((m = llqueue_poll(me->inbox)))
    {
        raft_node_t* n = raft_get_node(me->raft, m->sender);
        switch (m->type)
        {
        case MSG_APPENDENTRIES:
        {
            msg_appendentries_response_t response;
            int e = raft_recv_appendentries(me->raft, n, m->data, &response);

            if (RAFT_ERR_SHUTDOWN == e)
                __shutdown_server(me);

            __append_msg(sys,
                &response,
                MSG_APPENDENTRIES_RESPONSE,
                sizeof(response),
                m->sender,
                me->raft);
        }
            break;

        case MSG_APPENDENTRIES_RESPONSE:
            raft_recv_appendentries_response(me->raft, n, m->data);
            break;

        case MSG_ENTRIES:
        {
            msg_entries_response_t response;
            /* printf("ENTRIZ to %0.10d from %0.10d\n", raft_get_nodeid(me->raft), m->sender); */
            raft_recv_entries(me->raft, n, m->data, &response);
            __append_msg(sys,
                &response,
                MSG_ENTRIES_RESPONSE,
                sizeof(response),
                m->sender,
                me->raft);
        }
            break;

        case MSG_ENTRIES_RESPONSE:
        {
            /* printf("ENTRIZRESP to %0.10d from %0.10d\n", raft_get_nodeid(me->raft), m->sender); */
            int e = raft_recv_entries_response(me->raft, n, m->data);
            if (RAFT_ERR_SHUTDOWN == e)
                __shutdown_server(me);
        }
            break;

        case MSG_REQUESTVOTE:
        {
            msg_requestvote_response_t response;
            raft_recv_requestvote(me->raft, n, m->data, &response);
            __append_msg(sys,
                &response,
                MSG_REQUESTVOTE_RESPONSE,
                sizeof(response),
                m->sender,
                me->raft);
        }
            break;

        case MSG_REQUESTVOTE_RESPONSE:
            {
            int e = raft_recv_requestvote_response(me->raft, n, m->data);
            if (RAFT_ERR_SHUTDOWN == e)
                __shutdown_server(me);
            }
            break;

        case MSG_LEAVE:
            {
            raft_node_t* to_remove = raft_get_node(me->raft, m->sender);
            if (to_remove)
                raft_remove_node(me->raft, to_remove);
            }
            break;
        }
    }
}

static void __server_drop_messages(server_t* me, system_t* sys)
{
    msg_t* m;

    /* while ((m = llqueue_poll(me->inbox))) */

    /* Drop one message */
    if ((m = llqueue_poll(me->inbox)))
        free(m);
}

// FIXME: this is O(n^2)
/** Election Safety
 * At most one leader can be elected in a given term. */
static void __ensure_election_safety(system_t* sys)
{
    int i;

    for (i = 0; i < sys->n_servers; i++)
    {
        raft_server_t* r = sys->servers[i].raft;

        if (!raft_is_leader(r))
            continue;

        int j;
        for (j = i + 1; j < sys->n_servers; j++)
        {
            raft_server_t* r2 = sys->servers[j].raft;
            if (raft_is_leader(r2) &&
                raft_get_current_term(r) == raft_get_current_term(r2))
            {
                printf("election safety invalidated %lx %lx\n",
                       (unsigned long)r,
                       (unsigned long)r2);
                __int_handler(0);
                abort();
            }
        }
    }
}

// TODO:
/* Log Matching
 * If two logs contain an entry with the same index and term, then the
 * logs are identical in all entries up through the given index. */
static void __ensure_log_matching(system_t* sys)
{

}

void strrnd(char* s, size_t len)
{
    int i;
    for (i = 0; i < len; i++)
        s[i] = 'a' + random() % 64;
}

/** Add an entry to the leader */
static void __push_entry(system_t* sys)
{
    int i;

    for (i = 0; i < sys->n_servers; i++)
    {
        raft_server_t* r = sys->servers[i].raft;
        if (!raft_is_leader(r))
            continue;

        raft_entry_t* ety = calloc(1, sizeof(raft_entry_t));
        ety->id = sys->n_entries++;
        ety->data.buf = malloc(16);
        ety->data.len = 16;
        strrnd(ety->data.buf, 16);
        msg_entry_response_t response;
        raft_recv_entry(r, ety, &response);
    }
}

static int __voting_servers(system_t* sys)
{
    int i, servers = 0;
    for (i = 0; i < sys->n_servers; i++)
        if (sys->servers[i].connected == NODE_CONNECTED)
            servers += 1;
    return servers;
}

// FIXME: this is O(n^2)
/** Leader Completeness
 * If a log entry is committed in a given term, then that entry will be present
 * in the logs of the leaders for all higher-numbered terms. §3.6 */
static void __ensure_leader_completeness(system_t* sys)
{
    int i;

    for (i = 0; i < sys->n_servers; i++)
    {
        raft_server_t* r = sys->servers[i].raft;

        if (sys->servers[i].connected != NODE_CONNECTED)
            continue;

        int j;
        int comi = raft_get_commit_idx(r);

        /* we need to confirm that at least half of the servers have this */
        int have = 1;

        if (comi == 0)
            continue;

        raft_entry_t* ety = raft_get_entry_from_idx(r, comi);

        for (j=0; j < sys->n_servers; j++)
        {
            raft_server_t* r2 = sys->servers[j].raft;

            if (sys->servers[j].connected != NODE_CONNECTED)
                continue;

            if (r == r2)
                continue;

            /* all the other servers should not conflict with this */

            raft_entry_t* oety = raft_get_entry_from_idx(r2, comi);

            if (!oety)
                continue;

            if (!(oety->term != ety->term || oety->id != ety->id))
            {
                have += 1;
            }
        }

        /* a majority don't have it */
        if (!(__voting_servers(sys) / 2 <= have))
        {
            printf("leaders completeness failed: %d/%d %d commit_idx: %d t: %d id: %d\n",
                   have,
                   __voting_servers(sys),
                   raft_get_nodeid(r),
                   raft_get_commit_idx(r),
                   ety->term,
                   ety->id
                   );
            __int_handler(0);
            abort();
        }
    }
}

static void __poll_messages(system_t* sys)
{
    int i;
    for (i=0; i<sys->n_servers; i++)
        if (sys->servers[i].connected != NODE_DISCONNECTED)
            __server_poll_messages(&sys->servers[i], sys);
}

static void __toggle_membership(server_t* node)
{
    server_t* leader = __get_leader(&sys);
    entry_cfg_change_t *change = calloc(1, sizeof(*change));

    if (!leader)
        return;

    if (leader == node)
        return;

    printf("yyy %0.10d\n", node->node_id);

    if (NODE_DISCONNECTING == node->connected)
        return;

    if (NODE_CONNECTING == node->connected)
        return;

    if (NODE_DISCONNECTED == node->connected)
    {
        /* New servers SHOULD create a new node id for themselves */
        node->node_id = random();

        /* make sure inbox is empty */
        while (llqueue_poll(node->inbox));
    }

    change->node_id = node->node_id;

    msg_entry_t entry = {
        // FIXME: Should be random
        .id = 1,
        .data.buf = (void*)change,
        .data.len = sizeof(*change),
        .type = node->connected == NODE_CONNECTED ?
            RAFT_LOGTYPE_REMOVE_NODE :
            RAFT_LOGTYPE_ADD_NONVOTING_NODE
    };

    assert(raft_entry_is_cfg_change(&entry));

    printf("xxx %0.10d\n", node->node_id);

    msg_entry_response_t r;
    int e = raft_recv_entry(leader->raft, &entry, &r);
    if (0 != e)
        return;

    printf("zzz %0.10d\n", node->node_id);

    if (NODE_DISCONNECTED == node->connected)
    {

        node->connected = NODE_CONNECTING;
        raft_node_t* added_node = raft_add_non_voting_node(node->raft, NULL, node->node_id, 1);

        printf("%d\n", llqueue_count(node->inbox));
        assert(llqueue_count(node->inbox) == 0);
        assert(added_node);
    }
    else if (NODE_CONNECTED == node->connected)
    {
        /* node->connected = NODE_DISCONNECTING; */
    }
}

static void __periodic(system_t* sys)
{
    /* if (opts.debug) */
        /* printf("\n"); */

    if (random() % 100 < sys->client_rate)
        __push_entry(sys);

    __poll_messages(sys);

    int i;
    for (i = 0; i < sys->n_servers; i++)
    {
        server_t* sv = &sys->servers[i];

        if (random() % 100 < sys->membership_rate)
            __toggle_membership(sv);

        if (!opts.no_random_period && sv->connected != NODE_DISCONNECTED)
        {
            int e = raft_periodic(sv->raft, random() % 100);
            if (-1 == e)
            {
                printf("ERROR node %d\n", raft_get_nodeid(sv->raft));
                assert(0);
            }
            else if (RAFT_ERR_SHUTDOWN == e)
                __shutdown_server(sv);
            else
            {
                raft_apply_all(sv->raft);

                int j, vpeers = 0;
                for (j = 0; j < raft_get_num_nodes(sv->raft); j++)
                {
                    raft_node_t* node = raft_get_node_from_idx(sv->raft, j);
                    vpeers += node && raft_node_is_voting(node) ? 1 : 0;
                }

                printf("node %0.10d peers (%d, %d) ci:%d",
                    raft_get_nodeid(sv->raft),
                    raft_get_num_nodes(sv->raft),
                    vpeers,
                    raft_get_current_idx(sv->raft)
                    );

                printf("\t\t\t(");
#if 0
                for (j = 0; j < raft_get_num_nodes(sv->raft); j++)
                {
                    raft_node_t* node = raft_get_node_from_idx(sv->raft, j);
                    if (node)
                        printf("%0.10d ", raft_node_get_id(node));
                }
                printf(") ");
                printf("\t(");
#endif
                for (j = 0; j < raft_get_num_nodes(sv->raft); j++)
                {
                    raft_node_t* node = raft_get_node_from_idx(sv->raft, j);
                    if (node && raft_node_is_voting(node))
                        printf("%0.10d ", raft_node_get_id(node));
                }
                printf(") ");

                printf("%d ", sv->connected);
                if (raft_is_leader(sv->raft))
                    printf("LDR");
                printf("\n");
            }
        }
    }

    __ensure_election_safety(sys);
    __ensure_log_matching(sys);
    __ensure_leader_completeness(sys);
    /* TODO: add deadlock detection */

    /* collect stats */
    if (sys->leader != raft_get_current_leader_node(sys->servers[0].raft))
        sys->leadership_changes += 1;

    sys->leader = raft_get_current_leader_node(sys->servers[0].raft);
}

#include "command_parser.c"

int main(int argc, char **argv)
{
    int e, i;

    e = parse_options(argc, argv, &opts);
    if (-1 == e)
        exit(-1);
    else if (opts.help)
    {
        show_usage();
        exit(0);
    }
    else if (opts.version)
    {
        fprintf(stdout, "%s\n", VERSION);
        exit(0);
    }

    if (!opts.debug)
        raft_funcs.log = NULL;

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, __int_handler);

    srand(atoi(opts.seed));

    sys.n_servers = atoi(opts.servers);
    sys.servers = calloc(sys.n_servers, sizeof(*sys.servers));

    for (i = 0; i < sys.n_servers; i++)
        __create_node(&sys.servers[i], i, &sys);

    server_t* sv = &sys.servers[0];
    raft_add_non_voting_node(sv->raft, NULL, 0, 1);
    raft_become_leader(sv->raft);
    sv->connected = NODE_CONNECTED;

    /* add configuration change for leader's node */
    entry_cfg_change_t *change = calloc(1, sizeof(*change));
    change->node_id = 0;
    msg_entry_t entry = {
        // FIXME: Should be random
        .id = 1,
        .data.buf = (void*)change,
        .data.len = sizeof(*change),
        .type = RAFT_LOGTYPE_ADD_NODE,
    };
    msg_entry_response_t r;
    e = raft_recv_entry(sv->raft, &entry, &r);
    assert(RAFT_ERR_ONE_VOTING_CHANGE_ONLY != e);
    assert(0 == e);
    raft_set_commit_idx(sv->raft, 0 + 1);
    raft_apply_all(sv->raft);

    sys.client_rate = atoi(opts.client_rate);
    sys.membership_rate = atoi(opts.member_rate);

    /* We're being fed commands via stdin.
     * This is the fuzzer's entry point */
    parse_result_t result;
    if (1 == parse_commands(&sys, &result))
    {
        /* printf("%d ", sys.max_entries_in_ae); */
        /* printf("%d | ", sys.leadership_changes); */
        /* for (i=0; i<sys.n_servers; i++) */
        /* { */
        /*     printf("%d ", raft_get_current_idx(sys.servers[i].raft)); */
        /*     printf("%d, ", raft_get_current_term(sys.servers[i].raft)); */
        /* } */
    }
    else
    {
        int iters, max_iters = atoi(opts.iterations);
        for (iters = 0; iters < max_iters || max_iters == -1; iters++)
        {
            printf("%.5d\n\n", iters);
            __periodic(&sys);
        }
    }

    if (opts.tsv)
        __print_tsv();
    else if (!opts.quiet)
    {
        __print_stats();
        printf("Maximum appendentries size: %d\n", sys.max_entries_in_ae);
        printf("Leadership changes: %d\n", sys.leadership_changes);
        printf("Log pops: %d\n", sys.log_pops);
    }

    return 0;
}
