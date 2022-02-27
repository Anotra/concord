#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h> /* isgraph() */
#include <errno.h>

#include "discord.h"
#include "discord-internal.h"
#include "cog-utils.h"

static void
_discord_init(struct discord *new_client)
{
    ccord_global_init();

    new_client->io_poller = io_poller_create();
    discord_adapter_init(&new_client->adapter, &new_client->conf,
                         &new_client->token);
#ifdef HAS_DISCORD_VOICE
    discord_voice_connections_init(new_client);
#endif /* HAS_DISCORD_VOICE */

    /* fetch the client user structure */
    if (new_client->token.size) {
        struct discord_ret_user ret = { 0 };
        CCORDcode code;

        ret.sync = &new_client->self;
        code = discord_get_current_user(new_client, &ret);
        ASSERT_S(CCORD_OK == code, "Couldn't fetch client's user object");
    }

    new_client->is_original = true;
}

struct discord *
discord_init(const char token[])
{
    struct discord *new_client;

    new_client = calloc(1, sizeof *new_client);
    logconf_setup(&new_client->conf, "DISCORD", NULL);
    /* silence terminal input by default */
    logconf_set_quiet(&new_client->conf, true);

    new_client->token.start = (char *)token;
    new_client->token.size = token ? cog_str_bounds_check(token, 128) : 0;

    _discord_init(new_client);

    return new_client;
}

struct discord *
discord_config_init(const char config_file[])
{
    struct discord *new_client;
    char *path[] = { "discord", "token" };
    FILE *fp;

    fp = fopen(config_file, "rb");
    VASSERT_S(fp != NULL, "Couldn't open '%s': %s", config_file,
              strerror(errno));

    new_client = calloc(1, sizeof *new_client);
    logconf_setup(&new_client->conf, "DISCORD", fp);

    fclose(fp);

    new_client->token = logconf_get_field(&new_client->conf, path,
                                          sizeof(path) / sizeof *path);
    if (!strncmp("YOUR-BOT-TOKEN", new_client->token.start,
                 new_client->token.size))
    {
        memset(&new_client->token, 0, sizeof(new_client->token));
    }

    _discord_init(new_client);

    return new_client;
}

struct discord *
discord_clone(const struct discord *orig_client)
{
    struct discord *clone_client = malloc(sizeof(struct discord));

    memcpy(clone_client, orig_client, sizeof(struct discord));
    clone_client->is_original = false;

    return clone_client;
}

void
discord_cleanup(struct discord *client)
{
    if (client->is_original) {
        logconf_cleanup(&client->conf);
        discord_adapter_cleanup(&client->adapter);
        client->shards.controller.destroy(client);
        discord_user_cleanup(&client->self);
        io_poller_destroy(client->io_poller);
#ifdef HAS_DISCORD_VOICE
        discord_voice_connections_cleanup(client);
#endif /* HAS_DISCORD_VOICE */
    }
    free(client);
}

const char *
discord_strerror(CCORDcode code, struct discord *client)
{
    switch (code) {
    default:
        return ccord_strerror(code);
    case CCORD_DISCORD_JSON_CODE:
        return client ? client->adapter.errbuf
                      : "Discord JSON Error Code: Failed request";
    case CCORD_DISCORD_BAD_AUTH:
        return "Discord Bad Authentication: Bad authentication token";
    case CCORD_DISCORD_RATELIMIT:
        return "Discord Ratelimit: You are being ratelimited";
    case CCORD_DISCORD_CONNECTION:
        return "Discord Connection: Couldn't establish a connection to "
               "discord";
    }
}

int
concord_return_error(const char *error, int32_t error_code)
{
  if (error_code < 0 || error_code > 2) {
    return 1;
  }
  
  log_info("%s", error);
  
  return error_code;
}
void *
discord_set_data(struct discord *client, void *data)
{
    return client->data = data;
}

void *
discord_get_data(struct discord *client)
{
    return client->data;
}

void
discord_add_intents(struct discord *client, uint64_t code)
{
    client->intents |= code;
}

void
discord_remove_intents(struct discord *client, uint64_t code)
{
    client->intents &= ~code;
}

void
discord_set_prefix(struct discord *client, char *prefix)
{
    if (!prefix || !*prefix) return;

    if (client->cmds.prefix.start) free(client->cmds.prefix.start);

    client->cmds.prefix.size =
        cog_strndup(prefix, strlen(prefix), &client->cmds.prefix.start);
}

const struct discord_user *
discord_get_self(struct discord *client)
{
    return &client->self;
}

void
discord_set_on_command(struct discord *client,
                       char *command,
                       discord_ev_message callback)
{
    /**
     * default command callback if prefix is detected, but command isn't
     *  specified
     */
    if (client->cmds.prefix.size && (!command || !*command)) {
        client->cmds.on_default.cb = callback;
        return; /* EARLY RETURN */
    }
    size_t index = 0;
    const size_t command_len = strlen(command);
    for (; index < client->cmds.amt; index++)
        if (command_len == client->cmds.pool[index].size
            && 0 == strcmp(command, client->cmds.pool[index].start))
                goto modify;
    if (index == client->cmds.cap) {
        size_t cap = 8;
        while (cap <= index) cap <<= 1;
        
        void *tmp =
            realloc(client->cmds.pool, cap * sizeof(*client->cmds.pool));
        if (tmp) {
            client->cmds.pool = tmp;
            client->cmds.cap = cap;
        } else
            return;
    }
    ++client->cmds.amt;
    client->cmds.pool[index].start = strdup(command);
    client->cmds.pool[index].size = command_len;
modify:
    client->cmds.pool[index].cb = callback;

    discord_add_intents(client, DISCORD_GATEWAY_GUILD_MESSAGES
                              | DISCORD_GATEWAY_DIRECT_MESSAGES);
}

void
discord_set_on_commands(struct discord *client,
                        discord_ev_message callback,
                        ...)
{
    char *command = NULL;
    va_list commands;

    va_start(commands, callback);

    command = va_arg(commands, char *);
    while (command != NULL) {
        discord_set_on_command(client, command, callback);
        command = va_arg(commands, char *);
    }

    va_end(commands);
}

void
discord_set_event_scheduler(struct discord *client,
                            discord_ev_scheduler callback)
{
    client->cmds.scheduler = callback;
}


void
discord_set_next_wakeup(struct discord *client, int64_t delay)
{
    if (delay == -1) {
        client->wakeup_timer.next = -1;
    } else if (delay >= 0) {
        client->wakeup_timer.next = cog_timestamp_ms() + delay;
    }
}

void
discord_set_on_wakeup(struct discord *client, discord_ev_idle callback)
{
    client->wakeup_timer.cb = callback;
    client->wakeup_timer.next = -1;
}

void
discord_set_on_idle(struct discord *client, discord_ev_idle callback)
{
    client->on_idle = callback;
}

void
discord_set_on_cycle(struct discord *client, discord_ev_idle callback)
{
    client->on_cycle = callback;
}

void
discord_set_on_ready(struct discord *client, discord_ev_idle callback)
{
    client->cmds.cbs.on_ready = callback;
}

CCORDcode
discord_run(struct discord *client)
{
    int64_t next_gateway_run, now;
    CCORDcode code;

    if (!client->shards.controller.init)
        discord_sharding_use_none(client);
    client->shards.controller.init(client);

    next_gateway_run = cog_timestamp_ms();
    while (1) {
        now = cog_timestamp_ms();
        int poll_time = 0;
        if (!client->on_idle) {
            poll_time = now < next_gateway_run ? next_gateway_run - now : 0;
            if (-1 != client->wakeup_timer.next)
                if (client->wakeup_timer.next <= now + poll_time)
                    poll_time = client->wakeup_timer.next - now;
        }
        
        int poll_result = io_poller_poll(client->io_poller, poll_time);
        if (-1 == poll_result) {
            //TODO: handle poll error here
        } else if (0 == poll_result) {
            if (client->on_idle)
                client->on_idle(client);
        }
        
        if (client->on_cycle)
            client->on_cycle(client);

        if (CCORD_OK != (code = io_poller_perform(client->io_poller)))
            break;

        now = cog_timestamp_ms();
        if (client->wakeup_timer.next != -1) {
            if (now >= client->wakeup_timer.next) {
                client->wakeup_timer.next = -1;
                if (client->wakeup_timer.cb)
                    client->wakeup_timer.cb(client);
            }
        }
        if (next_gateway_run <= now) {
            if (CCORD_OK != (code = client->shards.controller.perform(client)))
                break;
                
            if (CCORD_OK != (code = discord_adapter_perform(&client->adapter)))
                break;
            next_gateway_run = now + 1000;
        }
    }
    discord_adapter_stop_all(&client->adapter);

    return code;
}

void
discord_shutdown(struct discord *client)
{
    client->shards.controller.shutdown(client);
}

void
discord_reconnect(struct discord *client, bool resume)
{
    client->shards.controller.reconnect(client, resume);
}

void
discord_set_on_guild_role_create(struct discord *client,
                                 discord_ev_guild_role callback)
{
    client->cmds.cbs.on_guild_role_create = callback;
    discord_add_intents(client, DISCORD_GATEWAY_GUILDS);
}

void
discord_set_on_guild_role_update(struct discord *client,
                                 discord_ev_guild_role callback)
{
    client->cmds.cbs.on_guild_role_update = callback;
    discord_add_intents(client, DISCORD_GATEWAY_GUILDS);
}

void
discord_set_on_guild_role_delete(struct discord *client,
                                 discord_ev_guild_role_delete callback)
{
    client->cmds.cbs.on_guild_role_delete = callback;
    discord_add_intents(client, DISCORD_GATEWAY_GUILDS);
}

void
discord_set_on_guild_member_add(struct discord *client,
                                discord_ev_guild_member callback)
{
    client->cmds.cbs.on_guild_member_add = callback;
    discord_add_intents(client, DISCORD_GATEWAY_GUILD_MEMBERS);
}

void
discord_set_on_guild_member_update(struct discord *client,
                                   discord_ev_guild_member callback)
{
    client->cmds.cbs.on_guild_member_update = callback;
    discord_add_intents(client, DISCORD_GATEWAY_GUILD_MEMBERS);
}

void
discord_set_on_guild_member_remove(struct discord *client,
                                   discord_ev_guild_member_remove callback)
{
    client->cmds.cbs.on_guild_member_remove = callback;
    discord_add_intents(client, DISCORD_GATEWAY_GUILD_MEMBERS);
}

void
discord_set_on_guild_ban_add(struct discord *client,
                             discord_ev_guild_ban callback)
{
    client->cmds.cbs.on_guild_ban_add = callback;
    discord_add_intents(client, DISCORD_GATEWAY_GUILD_BANS);
}

void
discord_set_on_guild_ban_remove(struct discord *client,
                                discord_ev_guild_ban callback)
{
    client->cmds.cbs.on_guild_ban_remove = callback;
    discord_add_intents(client, DISCORD_GATEWAY_GUILD_BANS);
}

void
discord_set_on_application_command_create(
    struct discord *client, discord_ev_application_command callback)
{
    client->cmds.cbs.on_application_command_create = callback;
}

void
discord_set_on_application_command_update(
    struct discord *client, discord_ev_application_command callback)
{
    client->cmds.cbs.on_application_command_update = callback;
}

void
discord_set_on_application_command_delete(
    struct discord *client, discord_ev_application_command callback)
{
    client->cmds.cbs.on_application_command_delete = callback;
}

void
discord_set_on_channel_create(struct discord *client,
                              discord_ev_channel callback)
{
    client->cmds.cbs.on_channel_create = callback;
    discord_add_intents(client, DISCORD_GATEWAY_GUILDS);
}

void
discord_set_on_channel_update(struct discord *client,
                              discord_ev_channel callback)
{
    client->cmds.cbs.on_channel_update = callback;
    discord_add_intents(client, DISCORD_GATEWAY_GUILDS);
}

void
discord_set_on_channel_delete(struct discord *client,
                              discord_ev_channel callback)
{
    client->cmds.cbs.on_channel_delete = callback;
    discord_add_intents(client, DISCORD_GATEWAY_GUILDS);
}

void
discord_set_on_channel_pins_update(struct discord *client,
                                   discord_ev_channel_pins_update callback)
{
    client->cmds.cbs.on_channel_pins_update = callback;
    discord_add_intents(client, DISCORD_GATEWAY_GUILDS);
}

void
discord_set_on_thread_create(struct discord *client,
                             discord_ev_channel callback)
{
    client->cmds.cbs.on_thread_create = callback;
    discord_add_intents(client, DISCORD_GATEWAY_GUILDS);
}

void
discord_set_on_thread_update(struct discord *client,
                             discord_ev_channel callback)
{
    client->cmds.cbs.on_thread_update = callback;
    discord_add_intents(client, DISCORD_GATEWAY_GUILDS);
}

void
discord_set_on_thread_delete(struct discord *client,
                             discord_ev_channel callback)
{
    client->cmds.cbs.on_thread_delete = callback;
    discord_add_intents(client, DISCORD_GATEWAY_GUILDS);
}

void
discord_set_on_guild_create(struct discord *client, discord_ev_guild callback)
{
    client->cmds.cbs.on_guild_create = callback;
    discord_add_intents(client, DISCORD_GATEWAY_GUILDS);
}

void
discord_set_on_guild_update(struct discord *client, discord_ev_guild callback)
{
    client->cmds.cbs.on_guild_update = callback;
    discord_add_intents(client, DISCORD_GATEWAY_GUILDS);
}

void
discord_set_on_guild_delete(struct discord *client,
                            discord_ev_guild_delete callback)
{
    client->cmds.cbs.on_guild_delete = callback;
    discord_add_intents(client, DISCORD_GATEWAY_GUILDS);
}

void
discord_set_on_message_create(struct discord *client,
                              discord_ev_message callback)
{
    client->cmds.cbs.on_message_create = callback;
    discord_add_intents(client, DISCORD_GATEWAY_GUILD_MESSAGES
                                    | DISCORD_GATEWAY_DIRECT_MESSAGES);
}

void
discord_set_on_message_update(struct discord *client,
                              discord_ev_message callback)
{
    client->cmds.cbs.on_message_update = callback;
    discord_add_intents(client, DISCORD_GATEWAY_GUILD_MESSAGES
                                    | DISCORD_GATEWAY_DIRECT_MESSAGES);
}

void
discord_set_on_message_delete(struct discord *client,
                              discord_ev_message_delete callback)
{
    client->cmds.cbs.on_message_delete = callback;
    discord_add_intents(client, DISCORD_GATEWAY_GUILD_MESSAGES
                                    | DISCORD_GATEWAY_DIRECT_MESSAGES);
}

void
discord_set_on_message_delete_bulk(struct discord *client,
                                   discord_ev_message_delete_bulk callback)
{
    client->cmds.cbs.on_message_delete_bulk = callback;
    discord_add_intents(client, DISCORD_GATEWAY_GUILD_MESSAGES
                                    | DISCORD_GATEWAY_DIRECT_MESSAGES);
}

void
discord_set_on_message_reaction_add(struct discord *client,
                                    discord_ev_message_reaction_add callback)
{
    client->cmds.cbs.on_message_reaction_add = callback;
    discord_add_intents(client,
                        DISCORD_GATEWAY_GUILD_MESSAGE_REACTIONS
                            | DISCORD_GATEWAY_DIRECT_MESSAGE_REACTIONS);
}

void
discord_set_on_message_reaction_remove(
    struct discord *client, discord_ev_message_reaction_remove callback)
{
    client->cmds.cbs.on_message_reaction_remove = callback;
    discord_add_intents(client,
                        DISCORD_GATEWAY_GUILD_MESSAGE_REACTIONS
                            | DISCORD_GATEWAY_DIRECT_MESSAGE_REACTIONS);
}

void
discord_set_on_message_reaction_remove_all(
    struct discord *client, discord_ev_message_reaction_remove_all callback)
{
    client->cmds.cbs.on_message_reaction_remove_all = callback;
    discord_add_intents(client,
                        DISCORD_GATEWAY_GUILD_MESSAGE_REACTIONS
                            | DISCORD_GATEWAY_DIRECT_MESSAGE_REACTIONS);
}

void
discord_set_on_message_reaction_remove_emoji(
    struct discord *client, discord_ev_message_reaction_remove_emoji callback)
{
    client->cmds.cbs.on_message_reaction_remove_emoji = callback;
    discord_add_intents(client,
                        DISCORD_GATEWAY_GUILD_MESSAGE_REACTIONS
                            | DISCORD_GATEWAY_DIRECT_MESSAGE_REACTIONS);
}

void
discord_set_on_interaction_create(struct discord *client,
                                  discord_ev_interaction callback)
{
    client->cmds.cbs.on_interaction_create = callback;
}

void
discord_set_on_voice_state_update(struct discord *client,
                                  discord_ev_voice_state_update callback)
{
    client->cmds.cbs.on_voice_state_update = callback;
    discord_add_intents(client, DISCORD_GATEWAY_GUILD_VOICE_STATES);
}

void
discord_set_on_voice_server_update(struct discord *client,
                                   discord_ev_voice_server_update callback)
{
    client->cmds.cbs.on_voice_server_update = callback;
    discord_add_intents(client, DISCORD_GATEWAY_GUILD_VOICE_STATES);
}

void
discord_set_presence(struct discord *client,
                     struct discord_presence_update *presence)
{
    client->shards.controller.set_presence(client, presence);
}

int
discord_get_ping(struct discord *client)
{
    return client->shards.controller.get_ping(client);
}

uint64_t
discord_timestamp(struct discord *client)
{
    (void)client;
    return cog_timestamp_ms();
}

struct logconf *
discord_get_logconf(struct discord *client)
{
    return &client->conf;
}
