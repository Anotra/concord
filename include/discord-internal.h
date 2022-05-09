/**
 * @file discord-internal.h
 * @ingroup DiscordInternal
 * @author Cogmasters
 * @brief Internal functions and datatypes
 */

#ifndef DISCORD_INTERNAL_H
#define DISCORD_INTERNAL_H

#include <pthread.h>

#define JSONB_HEADER
#include "json-build.h"
#define JSMN_STRICT
#define JSMN_HEADER
#include "jsmn.h"
#include "jsmn-find.h"

#include "logconf.h" /* struct logconf */
#include "user-agent.h"
#include "websockets.h"
#include "work.h"
#include "cog-utils.h"
#include "io_poller.h"

#include "queue.h"
#include "priority_queue.h"

/** @brief Return 1 if string isn't considered empty */
#define NOT_EMPTY_STR(str) ((str) && *(str))
/**
 * @brief Get container `type` from a field `ptr`
 *
 * @param ptr the field contained in `type`
 * @param type the container datatype
 * @param path the path to the field from the container POV
 */
#define CONTAINEROF(ptr, type, path)                                          \
    ((type *)((char *)(ptr)-offsetof(type, path)))

/** @defgroup DiscordInternal Internal implementation details
 * @brief Documentation useful when developing or debugging Concord itself
 *  @{ */

/** @brief Get client from its nested field */
#define CLIENT(ptr, path) CONTAINEROF(ptr, struct discord, path)

/**
 * @brief log and return `code` if `expect` condition is false
 *
 * @param expect the expected outcome
 * @param client the discord client
 * @param error return CCORDcode error
 * @param reason for return
 */
#define CCORD_EXPECT(client, expect, code, reason)                            \
    do {                                                                      \
        if (!(expect)) {                                                      \
            logconf_error(&(client)->conf, "Expected: " #expect ": " reason); \
            return code;                                                      \
        }                                                                     \
    } while (0)

/**
 * @brief Shortcut for checking OOB-write attempts
 * @note unsigned values are expected
 *
 * @param nbytes amount of bytes to be written
 * @param destsz size of dest in bytes
 */
#define ASSERT_NOT_OOB(nbytes, destsz)                                        \
    ASSERT_S((size_t)nbytes < (size_t)destsz, "Out of bounds write attempt");

/** @defgroup DiscordInternalAdapter REST API
 * @brief Wrapper to the Discord REST API
 *  @{ */

/** @brief Request's return context */
struct discord_ret_generic {
    /** `true` if may receive a datatype from response*/
    bool has_type;

    /** optional callback to be executed on a successful request */
    union {
        void (*typed)(struct discord *client, void *data, const void *ret);
        void (*typeless)(struct discord *client, void *data);
    } done;

    DISCORDT_RET_DEFAULT_FIELDS;

    /** if an address is provided, then request will block the thread and
     * perform on-spot. On success the response object will be written to
     * the address. */
    void *sync;
};

/** @brief Attributes of response datatype */
struct discord_generic {
    /** pointer to the datatype in memory */
    void *data;
    /** size of datatype in bytes */
    size_t size;
    /** initializer function for datatype fields */
    void (*init)(void *data);
    /** populate datatype with JSON values */
    size_t (*from_json)(const char *json, size_t len, void *data);
    /** cleanup function for datatype */
    void (*cleanup)(void *data);
};

/** @brief Behavior of request return struct */
struct discord_request {
    /** request response's return datatype attributes */
    struct discord_generic gnrc;
    /** request attributes set by client */
    struct discord_ret_generic ret;
    /** in case of HTTP_MIMEPOST, provide attachments */
    struct discord_attachments attachments;
};

/** URL endpoint threshold length */
#define DISCORD_ENDPT_LEN 512
/** Route's unique key threshold length */
#define DISCORD_ROUTE_LEN 256

/**
 * @brief Context of individual requests that are scheduled to run
 *        asynchronously
 */
struct discord_context {
    /** request return struct attributes */
    struct discord_request req;

    /** the request's bucket */
    struct discord_bucket *b;

    /** request body handle @note buffer is kept and recycled */
    struct {
        /** the request body contents */
        struct sized_buffer buf;
        /** the real size occupied in memory by `buf.start` */
        size_t memsize;
    } body;

    /** the request's http method */
    enum http_method method;
    /** the request's endpoint */
    char endpoint[DISCORD_ENDPT_LEN];
    /** the request bucket's key */
    char key[DISCORD_ROUTE_LEN];
    /** the connection handler assigned */
    struct ua_conn *conn;
    /** the request bucket's queue entry */
    QUEUE entry;

    /** current retry attempt (stop at adapter->retry_limit) */
    int retry_attempt;
};

/** @brief The handle used for performing HTTP Requests */
struct discord_adapter {
    /** DISCORD_HTTP or DISCORD_WEBHOOK logging module */
    struct logconf conf;
    /** the user agent handle for performing requests */
    struct user_agent *ua;
    /** curl_multi handle for performing non-blocking requests */
    CURLM *mhandle;

    /** buckets discovered (declared at discord-adapter_ratelimit.c) */
    struct discord_ratelimiter *ratelimiter;

    /** idle request handles */
    QUEUE(struct discord_context) * idleq;

    /** max amount of retries before a failed request gives up */
    int retry_limit;
};

/**
 * @brief Initialize the fields of a Discord Adapter handle
 *
 * @param adapter the adapter handle to be initialized
 * @param conf optional pointer to a parent logconf
 * @param token the bot token
 */
void discord_adapter_init(struct discord_adapter *adapter,
                          struct logconf *conf,
                          struct sized_buffer *token);

/**
 * @brief Free a Discord Adapter handle
 *
 * @param adapter the handle initialized with discord_adapter_init()
 */
void discord_adapter_cleanup(struct discord_adapter *adapter);

/**
 * @brief Perform a request to Discord
 *
 * This functions is a selector over discord_adapter_run() or
 *        discord_adapter_run_async()
 * @param adapter the handle initialized with discord_adapter_init()
 * @param req return object of request
 * @param body the body sent for methods that require (ex: post), leave as
 *        null if unecessary
 * @param method the method in opcode format of the request being sent
 * @param endpoint_fmt the printf-like endpoint formatting string
 * @CCORD_return
 * @note if sync is set then this function will block the thread and perform it
 *              immediately
 */
CCORDcode discord_adapter_run(struct discord_adapter *adapter,
                              struct discord_request *req,
                              struct sized_buffer *body,
                              enum http_method method,
                              char endpoint_fmt[],
                              ...);

/**
 * @brief Check and manage on-going, pending and timed-out requests
 *
 * @param adapter the handle initialized with discord_adapter_init()
 * @CCORD_return
 */
CCORDcode discord_adapter_perform(struct discord_adapter *adapter);

/**
 * @brief Stop all bucket's on-going, pending and timed-out requests
 *
 * The requests will be moved over to client's 'idleq' queue
 * @param adapter the handle initialized with discord_adapter_init()
 */
void discord_adapter_stop_buckets(struct discord_adapter *adapter);

/** @defgroup DiscordInternalAdapterRatelimit Ratelimiting
 * @brief Enforce ratelimiting per the official Discord Documentation
 *  @{ */

#define DISCORD_BUCKET_TIMEOUT (void *)(0xf)

/** @brief The Discord bucket for handling per-group ratelimits */
struct discord_bucket {
    /** the hash associated with the bucket's ratelimiting group */
    char hash[64];
    /** maximum connections this bucket can handle before ratelimit */
    long limit;
    /** connections this bucket can do before waiting for cooldown */
    long remaining;
    /** timestamp of when cooldown timer resets */
    u64unix_ms reset_tstamp;
    /** synchronize ratelimiting between threads */
    pthread_mutex_t lock;
    /** pending requests */
    QUEUE(struct discord_context) waitq;
    /**
     * pointer to currently performing busy request (if any)
     * @note `NULL` if free or @ref DISCORD_BUCKET_TIMEOUT if being ratelimited
     */
    struct discord_context *busy;
};

/**
 * @brief Return bucket timeout timestamp
 *
 * @param rl the handle initialized with discord_ratelimiter_init()
 * @param bucket the bucket to be checked for time out
 * @return the timeout timestamp
 */
u64unix_ms discord_bucket_get_timeout(struct discord_ratelimiter *rl,
                                      struct discord_bucket *bucket);

/**
 * @brief Try to sleep bucket for pending cooldown time
 * @note this is used for `sync` mode and **WILL** block the bucket's
 *      execution thread
 *
 * @param rl the handle initialized with discord_ratelimiter_init()
 * @param bucket the bucket to wait on cooldown
 */
void discord_bucket_try_sleep(struct discord_ratelimiter *rl,
                              struct discord_bucket *bucket);

/**
 * @brief Try to timeout bucket for pending cooldown time
 *
 * @param client the client initialized with discord_init()
 * @param bucket the bucket to wait on cooldown
 */
void discord_bucket_try_timeout(struct discord *client,
                                struct discord_bucket *b);

/**
 * @brief Get a `struct discord_bucket` assigned to `key`
 *
 * @param rl the handle initialized with discord_ratelimiter_init()
 * @param key obtained from discord_ratelimiter_build_key()
 * @return bucket matched to `key`
 */
struct discord_bucket *discord_bucket_get(struct discord_ratelimiter *rl,
                                          const char key[]);

/** @brief The ratelimiter struct for handling ratelimiting */
struct discord_ratelimiter {
    /** DISCORD_RATELIMIT logging module */
    struct logconf conf;
    /** amount of bucket's routes discovered */
    int length;
    /** route's cap before increase */
    int capacity;
    /**
     * routes matched to individual buckets
     * @note datatype declared at discord-adapter_ratelimit.c
     */
    struct _discord_route *routes;
    /** singleton bucket for requests that haven't been matched to a
     *      known or new bucket (i.e first time running the request) */
    struct discord_bucket *null;
    /** singleton bucket for requests that are not part of any known
     *      ratelimiting group */
    struct discord_bucket *miss;

    /* client-wide ratelimiting timeout */
    struct {
        /** global ratelimit */
        u64unix_ms wait_ms;
        /** global rwlock  */
        pthread_rwlock_t rwlock;
        /** global lock */
        pthread_mutex_t lock;
    } global;
};

/**
 * @brief Initialize ratelimiter handle
 *
 * A hashtable shall be used for storage and retrieval of discovered buckets
 * @param conf optional pointer to a parent logconf
 * @return the ratelimiter handle
 */
struct discord_ratelimiter *discord_ratelimiter_init(struct logconf *conf);

/**
 * @brief Cleanup all buckets that have been discovered
 *
 * @note pending requests will be moved to `adapter.idleq`
 * @param rl the handle initialized with discord_ratelimiter_init()
 */
void discord_ratelimiter_cleanup(struct discord_ratelimiter *rl);

/**
 * @brief Iterate known buckets
 *
 * @param rl the handle initialized with discord_ratelimiter_init()
 * @param adapter the handle initialized with discord_adapter_init()
 * @param iter the user callback to be called per bucket
 */
void discord_ratelimiter_foreach(struct discord_ratelimiter *rl,
                                 struct discord_adapter *adapter,
                                 void (*iter)(struct discord_adapter *adapter,
                                              struct discord_bucket *b));

/**
 * @brief Build unique key formed from the HTTP method and endpoint
 * @see https://discord.com/developers/docs/topics/rate-limits
 *
 * @param[in] method the request method
 * @param[out] key unique key for matching to buckets
 * @param[in] endpoint_fmt the printf-like endpoint formatting string
 * @param[in] args variadic arguments matched to `endpoint_fmt`
 */
void discord_ratelimiter_build_key(enum http_method method,
                                   char key[DISCORD_ROUTE_LEN],
                                   const char endpoint_fmt[],
                                   va_list args);

/**
 * @brief Get global timeout timestamp
 *
 * @param rl the handle initialized with discord_ratelimiter_init()
 * @return the most recent global timeout timestamp
 */
u64unix_ms discord_ratelimiter_get_global_wait(struct discord_ratelimiter *rl);

/**
 * @brief Update the bucket with response header data
 *
 * @param rl the handle initialized with discord_ratelimiter_init()
 * @param bucket NULL when bucket is first discovered
 * @param key obtained from discord_ratelimiter_build_key()
 * @param info informational struct containing details on the current transfer
 * @note If the bucket was just discovered it will be created here.
 */
void discord_ratelimiter_build(struct discord_ratelimiter *rl,
                               struct discord_bucket *bucket,
                               const char key[],
                               struct ua_info *info);

/** @} DiscordInternalAdapterRatelimit */

/** @} DiscordInternalAdapter */

/** @defgroup DiscordInternalGateway WebSockets API
 * @brief Wrapper to the Discord Gateway API
 *  @{ */

/** Generic event callback */
typedef void (*discord_ev)(struct discord *client, void *event);

/** @defgroup DiscordInternalGatewaySessionStatus Client's session status
 * @brief Client's session status
 *  @{ */
/** client is currently offline */
#define DISCORD_SESSION_OFFLINE 0u
/** client will attempt to resume session after reconnect */
#define DISCORD_SESSION_RESUMABLE 1u << 0
/** client in the process of being shutdown */
#define DISCORD_SESSION_SHUTDOWN 1u << 1
/** @} DiscordInternalGatewaySessionStatus */

/** @brief The handle for storing the Discord response payload */
struct discord_gateway_payload {
    /** current iteration JSON string data */
    char *json;
    /** current iteration JSON string data length */
    size_t length;
    /** field 'op' */
    enum discord_gateway_opcodes opcode;
    /** field 's' */
    int seq;
    /** field 't' */
    char name[32];
    /** field 'd' */
    jsmnf_pair *data;
};

/** @defgroup DiscordInternalGatewayMessageCommands Message Commands API
 * @brief The Message Commands API for registering and parsing user commands
 *  @{ */

/**
 * @brief The handle for storing user's message commands
 * @see discord_set_on_command()
 */
struct discord_message_commands {
    /** DISCORD_MESSAGE_COMMANDS logging module */
    struct logconf conf;
    /** the prefix expected for every command */
    struct sized_buffer prefix;
    /** fallback message command @see discord_set_on_command() */
    discord_ev_message fallback;
    /** amount of message commands created */
    int length;
    /** message commands cap before increase */
    int capacity;
    /**
     * message command entries
     * @note datatype declared at discord-gateway_command.c
     */
    struct _discord_message_commands_entry *entries;
};

/**
 * @brief Initialize the fields of the Message Commands handle
 *
 * @param conf optional pointer to a parent logconf
 * @return the message commands handle
 */
struct discord_message_commands *discord_message_commands_init(
    struct logconf *conf);

/**
 * @brief Free Message Commands handle
 *
 * @param cmds the handle initialized with discord_message_commands_init()
 */
void discord_message_commands_cleanup(struct discord_message_commands *cmds);

/**
 * @brief Search for a callback matching the command
 *
 * @param cmds the handle initialized with discord_message_commands_init()
 * @param command the command to be searched for
 * @param length the command length
 * @return the callback match, `NULL` in case there wasn't a match
 */
discord_ev_message discord_message_commands_find(
    struct discord_message_commands *cmds,
    const char command[],
    size_t length);

/**
 * @brief Add a new command/callback pair, or update an existing command
 *
 * @param cmds the handle initialized with discord_message_commands_init()
 * @param command the message command to be matched with callback
 * @param length the command length
 * @param callback the callback to be triggered when the command is sent
 */
void discord_message_commands_append(struct discord_message_commands *cmds,
                                     const char command[],
                                     size_t length,
                                     discord_ev_message callback);

/**
 * @brief Set a mandatory prefix before commands
 * @see discord_set_on_command()
 *
 * Example: If @a 'help' is a command and @a '!' prefix is set, the command
 *       will only be validated if @a '!help' is sent
 * @param cmds the handle initialized with discord_message_commands_init()
 * @param prefix the mandatory command prefix
 * @param length the prefix length
 */
void discord_message_commands_set_prefix(struct discord_message_commands *cmds,
                                         const char prefix[],
                                         size_t length);

/**
 * @brief Read the current `MESSAGE_CREATE` payload and attempt to perform its
 *      matching callback
 *
 * @param gw the handle initialized with discord_gateway_init()
 * @param payload the event payload to read from
 *      (assumes its from `MESSAGE_CREATE`)
 * @param client the handle initialized with discord_init()
 *      @note used for its @ref discord_refcounter and passing as a callback
 *      parameter
 * @return `true` if the callback has been performed
 */
bool discord_message_commands_try_perform(
    struct discord_message_commands *cmds,
    struct discord_gateway_payload *payload,
    struct discord *client);

/** @} DiscordInternalGatewayMessageCommands */

/** @brief The handle used for establishing a WebSockets connection */
struct discord_gateway {
    /** DISCORD_GATEWAY logging module */
    struct logconf conf;
    /** the websockets handle that connects to Discord */
    struct websockets *ws;
    /** curl_multi handle for non-blocking transfer over websockets */
    CURLM *mhandle;

    /** timers kept for synchronization */
    struct {
        /** fixed interval between heartbeats */
        u64unix_ms interval;
        /** last heartbeat pulse timestamp */
        u64unix_ms hbeat;
        /** Gateway's concept of "now" */
        u64unix_ms now;
        /** timestamp of last succesful identify request */
        u64unix_ms identify;
        /** timestamp of last succesful event timestamp in ms
         *      (resets every 60s) */
        u64unix_ms event;
        /** latency obtained from HEARTBEAT and HEARTBEAT_ACK interval */
        int ping_ms;
        /** ping rwlock  */
        pthread_rwlock_t rwlock;
    } * timer;

    /** the identify structure for client authentication */
    struct discord_identify id;

    /** on-going session structure */
    struct {
        /** whether client is ready to start sending/receiving events */
        bool is_ready;
        /** session id for resuming lost connections */
        char id[64];
        /** amount of shards being used by this session */
        int shards;
        /** session limits */
        struct discord_session_start_limit start_limit;
        /** active concurrent sessions */
        int concurrent;
        /** event counter to avoid reaching limit of 120 events per 60 sec */
        int event_count;
        /** @ref DiscordInternalGatewaySessionStatus */
        unsigned status;

        /** retry connection structure */
        struct {
            /** will attempt reconnecting if true */
            bool enable;
            /** current retry attempt (resets to 0 when succesful) */
            int attempt;
            /** max amount of retries before giving up */
            int limit;
        } retry;
    } * session;

    /** parse JSON tokens into a `jsmnf_pairs` key/value pairs hashtable */
    struct {
        /** current iteration JSON key/value pairs */
        jsmnf_pair *pairs;
        /** current iteration number of JSON key/value pairs */
        unsigned npairs;
        /** current iteration JSON tokens (fed to `jsmnf_pair`) */
        jsmntok_t *tokens;
        /** current iteration number of JSON tokens */
        unsigned ntokens;
    } parse;

    /** response-payload structure */
    struct discord_gateway_payload payload;
    /** the user's message commands @see discord_set_on_command() */
    struct discord_message_commands *commands;
    /** the user's callbacks for Discord events */
    discord_ev cbs[DISCORD_EV_MAX];
    /** the event scheduler callback */
    discord_ev_scheduler scheduler;
};

/**
 * @brief Initialize the fields of Discord Gateway handle
 *
 * @param gw the gateway handle to be initialized
 * @param conf optional pointer to a parent logconf
 * @param token the bot token
 */
void discord_gateway_init(struct discord_gateway *gw,
                          struct logconf *conf,
                          struct sized_buffer *token);

/**
 * @brief Free a Discord Gateway handle
 *
 * @param gw the handle initialized with discord_gateway_init()
 */
void discord_gateway_cleanup(struct discord_gateway *gw);

/**
 * @brief Initialize handle with the new session primitives
 *
 * @param gw the handle initialized with discord_gateway_init()
 * @CCORD_return
 */
CCORDcode discord_gateway_start(struct discord_gateway *gw);

/**
 * @brief Cleanup and reset `gw` session primitives
 *
 * @param ws the WebSockets handle created with ws_init()
 * @return `true` if session is over, `false` if session can be retried for
 *      reconnection
 */
bool discord_gateway_end(struct discord_gateway *gw);

/**
 * @brief Check and manage on-going Gateway session
 *
 * @param req the request handler
 * @CCORD_return
 */
CCORDcode discord_gateway_perform(struct discord_gateway *gw);

/**
 * @brief Gracefully shutdown a ongoing Discord connection over WebSockets
 *
 * @param gw the handle initialized with discord_gateway_init()
 */
void discord_gateway_shutdown(struct discord_gateway *gw);

/**
 * @brief Gracefully reconnect a ongoing Discord connection over WebSockets
 *
 * @param gw the handle initialized with discord_gateway_init()
 * @param resume true to attempt to resume to previous session,
 *        false restart a fresh session
 */
void discord_gateway_reconnect(struct discord_gateway *gw, bool resume);

/**
 * @brief Trigger the initial handshake with the gateway
 *
 * @param gw the handle initialized with discord_gateway_init()
 * @param event provide client identification information
 */
void discord_gateway_send_identify(struct discord_gateway *gw,
                                   struct discord_identify *event);

/**
 * @brief Replay missed events when a disconnected client resumes
 *
 * @param gw the handle initialized with discord_gateway_init()
 * @param event session resume information
 */
void discord_gateway_send_resume(struct discord_gateway *gw,
                                 struct discord_resume *event);

/**
 * @brief Maintain an active gateway connection
 *
 * @param gw the handle initialized with discord_gateway_init()
 * @param seq the last session sequence number
 */
void discord_gateway_send_heartbeat(struct discord_gateway *gw, int seq);

/**
 * @brief Request all members for a guild or a list of guilds.
 *
 * @param gw the handle initialized with discord_gateway_init()
 * @param event request guild members information
 */
void discord_gateway_send_request_guild_members(
    struct discord_gateway *gw, struct discord_request_guild_members *event);

/**
 * @brief Sent when a client wants to join, move or disconnect from a voice
 *      channel
 *
 * @param gw the handle initialized with discord_gateway_init()
 * @param event request guild members information
 */
void discord_gateway_send_update_voice_state(
    struct discord_gateway *gw, struct discord_update_voice_state *event);

/**
 * @brief Send client's presence status update payload
 *
 * @param gw the handle initialized with discord_gateway_init()
 * @param event the presence to be set
 */
void discord_gateway_send_presence_update(
    struct discord_gateway *gw, struct discord_presence_update *event);

/**
 * @brief Dispatch user callback matched to event
 *
 * @param gw the handle initialized with discord_gateway_init()
 * @param event the Discord event to be executed
 */
void discord_gateway_dispatch(struct discord_gateway *gw,
                              enum discord_gateway_events event);

/** @} DiscordInternalGateway */

/** @defgroup DiscordInternalTimer Timer API
 * @brief Callback scheduling API
 *  @{ */

struct discord_timers {
    priority_queue *q;
    struct {
        struct discord_timer *timer;
        bool skip_update_phase;
    } active;
};

/**
 * @brief Prepare timers for usage
 *
 * @param client the client created with discord_init()
 */
void discord_timers_init(struct discord *client);

/**
 * @brief Cleanup timers and call cancel any running ones
 *
 * @param client the client created with discord_init()
 */
void discord_timers_cleanup(struct discord *client);

/**
 * @brief Run all timers that are due
 *
 * @param client the client created with discord_init()
 * @param timers the timers to run
 */
void discord_timers_run(struct discord *client, struct discord_timers *timers);

/**
 * @brief Modifies or creates a timer
 *
 * @param client the client created with discord_init()
 * @param timers the timer group to perform this operation on
 * @param timer the timer that should be modified
 * @return the id of the timer
 */
unsigned _discord_timer_ctl(struct discord *client,
                            struct discord_timers *timers,
                            struct discord_timer *timer);

/**
 * @brief Modifies or creates a timer
 *
 * @param client the client created with discord_init()
 * @param timer the timer that should be modified
 * @return the id of the timer
 */
unsigned discord_internal_timer_ctl(struct discord *client,
                                    struct discord_timer *timer);

/**
 * @brief Creates a one shot timer that automatically deletes itself upon
 *      completion
 *
 * @param client the client created with discord_init()
 * @param cb the callback that should be called when timer triggers
 * @param data user data
 * @param delay delay before timer should start in milliseconds
 * @return the id of the timer
 */
unsigned discord_internal_timer(struct discord *client,
                                discord_ev_timer cb,
                                void *data,
                                int64_t delay);

/** @} DiscordInternalTimer */

/** @defgroup DiscordInternalRefcount Reference counter
 * @brief Handle automatic cleanup of user's data
 *  @{ */

/**
 * @brief Automatically cleanup user data
 *
 * Automatically cleanup user data that is passed around Discord event's
 *      callbacks once its reference counter reaches 0, meaning there are no
 *      more callbacks expecting the data
 */
struct discord_refcounter {
    /** DISCORD_REFCOUNT logging module */
    struct logconf conf;
    /** amount of individual user's data held for automatic cleanup */
    int length;
    /** cap before increase */
    int capacity;
    /**
     * individual user's data held for automatic cleanup
     * @note datatype declared at discord-adapter_refcount.c
     */
    struct _discord_ref *refs;
};

/**
 * @brief Initialize reference counter handle
 *
 * A hashtable shall be used for storage and retrieval of user data
 * @param conf optional pointer to a parent logconf
 * @return the reference counter handle
 */
struct discord_refcounter *discord_refcounter_init(struct logconf *conf);

/**
 * @brief Cleanup refcounter and all user data currently held
 *
 * @param rc the handle initialized with discord_refcounter_init()
 */
void discord_refcounter_cleanup(struct discord_refcounter *rc);

/**
 * @brief Increment the reference counter for `ret->data`
 *
 * @param rc the handle initialized with discord_refcounter_init()
 * @param data the user arbitrary data to have its reference counter
 * @param cleanup user-defined function for cleaning `data` resources once its
 *      no longer referenced
 * @param should_free whether `data` cleanup should be followed by a free()
 */
void discord_refcounter_incr(struct discord_refcounter *rc,
                             void *data,
                             void (*cleanup)(void *data),
                             bool should_free);

/**
 * @brief Decrement the reference counter for `data`
 *
 * If the count reaches zero then `data` shall be cleanup up with its
 *      user-defined cleanup function
 * @param rc the handle initialized with discord_refcounter_init()
 * @param data the user arbitrary data to have its reference counter
 *      decremented
 */
void discord_refcounter_decr(struct discord_refcounter *rc, void *data);

/** @} DiscordInternalRefcount */

/**
 * @brief The Discord client handler
 *
 * Used to access/perform public functions from discord.h
 * @see discord_init(), discord_config_init(), discord_cleanup()
 */
struct discord {
    /** DISCORD logging module */
    struct logconf conf;
    /** whether this is the original client or a clone */
    bool is_original;
    /** the bot token */
    struct sized_buffer token;
    /** the io poller for listening to file descriptors */
    struct io_poller *io_poller;
    /** the handle for interfacing with Discord's REST API */
    struct discord_adapter adapter;
    /** the handle for interfacing with Discord's Gateway API */
    struct discord_gateway gw;
    /** user's data reference counter for automatic cleanup */
    struct discord_refcounter *refcounter;
    /** the client's user structure */
    struct discord_user self;

    struct {
        struct discord_timers internal;
        struct discord_timers user;
    } timers;

    /** wakeup timer handle */
    struct {
        /** callback to be triggered on timer's timeout */
        discord_ev_idle cb;
        /** the id of the wake timer */
        unsigned id;
    } wakeup_timer;

    /** triggers when idle.  */
    discord_ev_idle on_idle;
    /** triggers once per loop cycle */
    discord_ev_idle on_cycle;

    /** space for user arbitrary data */
    void *data;

#ifdef CCORD_VOICE
    struct discord_voice vcs[DISCORD_MAX_VCS];
    struct discord_voice_evcallbacks voice_cbs;
#endif /* CCORD_VOICE */
};

/** @} DiscordInternal */

#endif /* DISCORD_INTERNAL_H */
