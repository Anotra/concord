#define _GNU_SOURCE /* asprintf() */
#include <string.h>

#include "reddit.h"
#include "reddit-internal.h"

ORCAcode
reddit_comment(
  struct reddit *client,
  struct reddit_comment_params *params,
  struct sized_buffer *p_json)
{
  if (!params) {
    log_error("Missing 'params'");
    return ORCA_MISSING_PARAMETER;
  }

  char payload[2048];
  size_t ret = reddit_comment_params_to_json(payload, sizeof(payload), params);
  struct sized_buffer req_body = { payload, ret };
  ERR("%.*s", (int)ret, payload);

  return reddit_adapter_run(
    &client->adapter,
    p_json,
    &req_body,
    HTTP_POST, "/api/comment");
}