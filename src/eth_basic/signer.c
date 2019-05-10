#include "signer.h"
#include "../eth_nano/serialize.h"
#include "filter.h"
#include <client/client.h>
#include <client/context.h>
#include <client/keys.h>
#include <crypto/ecdsa.h>
#include <crypto/secp256k1.h>
#include <inttypes.h>
#include <stdio.h>
#include <util/data.h>

static inline bytes_t get(d_token_t* t, uint16_t key) {
  return d_to_bytes(d_get(t, key));
}

/**
 * return data from the client.
 * 
 * In case of an error report this tol parent and return an empty bytes
 */
static bytes_t get_from_nodes(in3_ctx_t* parent, char* method, char* params, bytes32_t dst) {
  in3_ctx_t* ctx = in3_client_rpc_ctx(parent->client, method, params);
  bytes_t    b   = bytes(NULL, 0);
  int        res = 0;
  if (ctx->error)
    res = ctx_set_error(parent, ctx->error, -1);
  else {
    d_token_t* result = d_get(ctx->responses[0], K_RESULT);
    if (!result)
      res = ctx_set_error(parent, "No result found when fetching data for tx", -1);
    else {
      b = d_to_bytes(result);
      if (b.len)
        memcpy(dst, b.data, b.len);
      b.data = dst;
    }
  }
  free_ctx(ctx);
  return res < 0 ? bytes(NULL, 0) : b;
}

/** signs the given data */
int eth_sign(void* pk, d_signature_type_t type, bytes_t message, bytes_t account, uint8_t* dst) {
  UNUSED_VAR(account); // at least for now
  switch (type) {
    case SIGN_EC_RAW:
      if (ecdsa_sign_digest(&secp256k1, pk, message.data, dst, dst + 64, NULL) < 0)
        return -1;
      break;
    case SIGN_EC_HASH:
      if (ecdsa_sign(&secp256k1, HASHER_SHA3K, pk, message.data, message.len, dst, dst + 64, NULL) < 0)
        return -1;
      break;

    default:
      return -2;
  }
  return 65;
}

/** sets the signer and a pk to the client*/
int eth_set_pk_signer(in3_t* in3, bytes32_t pk) {
  if (in3->signer) _free(in3->signer);
  in3->signer         = _malloc(sizeof(in3_signer_t));
  in3->signer->sign   = eth_sign;
  in3->signer->wallet = pk;
  return 0;
}

bytes_t sign_tx(d_token_t* tx, in3_ctx_t* ctx) {
  address_t from;
  bytes32_t nonce_data, gas_price_data;
  bytes_t   tmp;
  uint8_t   sig[65];

  // get the from-address
  if ((tmp = d_to_bytes(d_get(tx, K_FROM))).len == 0) {
    if (!d_get(tx, K_NONCE)) {
      // Derive the from-address from pk if no nonce is given.
      // Note: This works because the signer->wallet points to the pk in the current signer implementation
      // (see eth_set_pk_signer()), and may change in the future.
      // Also, other wallet implementations may differ - hence the check.
      if (ctx->client->signer->sign != eth_sign) {
        ctx_set_error(ctx, "you need to specify the from-address in the tx!", -1);
        return bytes(NULL, 0);
      }

      uint8_t public_key[65], sdata[32];
      bytes_t pubkey_bytes = {.data = public_key + 1, .len = 64};
      ecdsa_get_public_key65(&secp256k1, ctx->client->signer->wallet, public_key);
      sha3_to(&pubkey_bytes, sdata);
      memcpy(from, sdata + 12, 20);
    } else
      memset(from, 0, 20);
  } else
    memcpy(from, tmp.data, 20);

  // build nonce-params
  tmp      = bytes(from, 20);
  sb_t* sb = sb_new("[");
  sb_add_bytes(sb, "", &tmp, 1, false);
  sb_add_chars(sb, ",\"latest\"]");

  // read the values
  bytes_t nonce     = d_get(tx, K_NONCE) ? get(tx, K_NONCE) : get_from_nodes(ctx, "eth_getTransactionCount", sb->data, nonce_data),
          gas_price = d_get(tx, K_GAS_PRICE) ? get(tx, K_GAS_PRICE) : get_from_nodes(ctx, "eth_gasPrice", "[]", gas_price_data),
          gas_limit = d_get(tx, K_GAS_LIMIT) ? get(tx, K_GAS_LIMIT) : bytes((uint8_t*) "\x52\x08", 2),
          to        = get(tx, K_TO),
          value     = get(tx, K_VALUE),
          data      = get(tx, K_DATA);

  // create raw without signature
  bytes_t* raw = serialize_tx_raw(nonce, gas_price, gas_limit, to, value, data, ctx->requests_configs->chainId, bytes(NULL, 0), bytes(NULL, 0));

  // sign the raw message
  int res = ctx->client->signer->sign(ctx->client->signer->wallet, SIGN_EC_HASH, *raw, bytes(NULL, 0), sig);

  // free temp resources
  b_free(raw);
  sb_free(sb);
  if (res < 0) return bytes(NULL, 0);

  // create raw transaction with signature
  raw            = serialize_tx_raw(nonce, gas_price, gas_limit, to, value, data, 27 + sig[64] + ctx->requests_configs->chainId * 2 + 8, bytes(sig, 32), bytes(sig + 32, 32));
  bytes_t raw_tx = bytes(raw->data, raw->len);
  _free(raw); // we only free the struct, not the data!

  return raw_tx;
}

// this is called before a request is send
int eth_handle_intern(in3_ctx_t* ctx, in3_response_t** response) {
  if (ctx->len > 1) return 0; // internal handling is only possible for single requests (at least for now)
  d_token_t* req = ctx->requests[0];

  // check method
  if (strcmp(d_get_stringk(req, K_METHOD), "eth_sendTransaction") == 0) {
    // get the transaction-object
    d_token_t* tx_params = d_get(req, K_PARAMS);
    if (!tx_params || d_type(tx_params + 1) != T_OBJECT) return ctx_set_error(ctx, "invalid params", -1);
    if (!ctx->client->signer) return ctx_set_error(ctx, "no signer set", -1);

    // sign it.
    bytes_t raw = sign_tx(tx_params + 1, ctx);
    if (!raw.len) return ctx_set_error(ctx, "error signing the transaction", -1);

    // build the RPC-request
    uint64_t id = d_get_longk(req, K_ID);
    sb_t*    sb = sb_new("{ \"jsonrpc\":\"2.0\", \"method\":\"eth_sendRawTransaction\", \"params\":[");
    sb_add_bytes(sb, "", &raw, 1, false);
    sb_add_chars(sb, "]");
    if (id) {
      char tmp[16];
      sprintf(tmp, ", \"id\":%" PRId64 "", id);
      sb_add_chars(sb, tmp);
    }
    sb_add_chars(sb, "}");

    // now that we included the signature in the rpc-request, we can free it + the old rpc-request.
    _free(raw.data);
    free_json(ctx->request_context);

    // set the new RPC-Request.
    ctx->request_context = parse_json(sb->data);
    ctx->requests[0]     = ctx->request_context->result;

    // we add the request-string to the cache, to make sure the request-string will be cleaned afterwards
    ctx->cache = in3_cache_add_entry(ctx->cache, bytes(NULL, 0), bytes((uint8_t*) sb->data, sb->len));
  } else if (strcmp(d_get_stringk(req, K_METHOD), "eth_newFilter") == 0) {
    int        ret       = 0;
    d_token_t* tx_params = d_get(req, K_PARAMS);
    if (!tx_params || d_type(tx_params + 1) != T_OBJECT)
      return ctx_set_error(ctx, "invalid params", -1);

    char*      from_block;
    d_token_t* frmblk = d_get(tx_params + 1, K_FROM_BLOCK);
    if (!frmblk) {
      from_block = NULL;
    } else if (d_type(frmblk) == T_INTEGER || d_type(frmblk) == T_BYTES) {
      from_block = stru64(d_long(frmblk));
    } else if (d_type(frmblk) == T_STRING && (!strcmp(d_string(frmblk), "latest") || !strcmp(d_string(frmblk), "earliest") || !strcmp(d_string(frmblk), "pending"))) {
      from_block = strdup(d_string(frmblk));
    } else {
      ret = ctx_set_error(ctx, "invalid params (fromblock)", -1);
      goto ERR_FLT;
    }

    char*      to_block;
    d_token_t* toblk = d_get(tx_params + 1, K_TO_BLOCK);
    if (!toblk) {
      to_block = NULL;
    } else if (d_type(toblk) == T_INTEGER || d_type(toblk) == T_BYTES) {
      to_block = stru64(d_long(toblk));
    } else if (d_type(toblk) == T_STRING && (!strcmp(d_string(toblk), "latest") || !strcmp(d_string(toblk), "earliest") || !strcmp(d_string(toblk), "pending"))) {
      to_block = strdup(d_string(toblk));
    } else {
      ret = ctx_set_error(ctx, "invalid params (toblock)", -1);
      goto ERR_FLT1;
    }

    char*      jaddr;
    d_token_t* addrs = d_get(tx_params + 1, K_ADDRESS);
    if (addrs == NULL) {
      jaddr = NULL;
    } else if (filter_valid_addrs(addrs)) {
      jaddr = (d_type(addrs) == T_BYTES && d_len(addrs) == 20) ? stru64(d_long(addrs)) : strdup(d_string(addrs));
      if (jaddr == NULL) {
        ret = ctx_set_error(ctx, "ENOMEM", -1);
        goto ERR_FLT2;
      }
    } else {
      ret = ctx_set_error(ctx, "invalid params (address)", -1);
      goto ERR_FLT2;
    }

    char*      jtopics;
    d_token_t* topics = d_get(tx_params + 1, K_TOPICS);
    if (topics == NULL) {
      jtopics = NULL;
    } else if (filter_valid_topics(topics)) {
      jtopics = (d_type(topics) == T_BYTES && d_len(topics) == 20) ? stru64(d_long(topics)) : strdup(d_string(topics));
      if (jtopics == NULL) {
        ret = ctx_set_error(ctx, "ENOMEM", -1);
        goto ERR_FLT3;
      }
    } else {
      ret = ctx_set_error(ctx, "invalid params (topics)", -1);
      goto ERR_FLT3;
    }

    in3_filter_opt_t* fopt = filter_new_opt();
    if (!fopt) {
      ret = ctx_set_error(ctx, "filter option creation failed", -1);
      goto ERR_FLT4;
    }
    fopt->from_block = from_block;
    fopt->to_block   = to_block;
    fopt->addresses  = jaddr;
    fopt->topics     = jtopics;

    size_t id = filter_add(ctx->client, FILTER_EVENT, fopt);
    if (!id) {
      ret = ctx_set_error(ctx, "filter option creation failed", -1);
      goto ERR_FLT5;
    }

    // prepare response-object
    *response = _malloc(sizeof(in3_response_t));
    sb_init(&response[0]->result);
    sb_init(&response[0]->error);
    sb_add_chars(&response[0]->result, "{ \"id\":1, \"jsonrpc\": \"2.0\", \"result\": \"");
    char* strid = stru64(id);
    sb_add_chars(&response[0]->result, strid);
    free(strid);
    sb_add_chars(&response[0]->result, "\"}");
    return 0;

  ERR_FLT5:
    fopt->release(fopt);
  ERR_FLT4:
    free(jtopics);
  ERR_FLT3:
    free(jaddr);
  ERR_FLT2:
    free(to_block);
  ERR_FLT1:
    free(from_block);
  ERR_FLT:
    return ret;
  } else if (strcmp(d_get_stringk(req, K_METHOD), "eth_newBlockFilter") == 0) {
    size_t id = filter_add(ctx->client, FILTER_BLOCK, NULL);
    if (!id) return ctx_set_error(ctx, "filter creation failed", -1);
    *response = _malloc(sizeof(in3_response_t));
    sb_init(&response[0]->result);
    sb_init(&response[0]->error);
    sb_add_chars(&response[0]->result, "{ \"id\":1, \"jsonrpc\": \"2.0\", \"result\": \"");
    char* strid = stru64(id);
    sb_add_chars(&response[0]->result, strid);
    free(strid);
    sb_add_chars(&response[0]->result, "\"}");
  } else if (strcmp(d_get_stringk(req, K_METHOD), "eth_newPendingTransactionFilter") == 0) {
    return ctx_set_error(ctx, "pending filter not supported", -1);
  } else if (strcmp(d_get_stringk(req, K_METHOD), "eth_uninstallFilter") == 0) {
    d_token_t* tx_params = d_get(req, K_PARAMS);
    if (!tx_params || d_type(tx_params + 1) != T_OBJECT)
      return ctx_set_error(ctx, "invalid params", -1);

    uint64_t id = d_get_longk(tx_params + 1, K_ID);
    if (!id)
      return ctx_set_error(ctx, "invalid params (id)", -1);

    *response = _malloc(sizeof(in3_response_t));
    sb_init(&response[0]->result);
    sb_init(&response[0]->error);
    if (filter_remove(ctx->client, id)) {
      sb_add_chars(&response[0]->result, "{ \"id\":1, \"jsonrpc\": \"2.0\", \"result\": true");
    } else {
      sb_add_chars(&response[0]->result, "{ \"id\":1, \"jsonrpc\": \"2.0\", \"result\": false");
    }
  } else if (strcmp(d_get_stringk(req, K_METHOD), "eth_getFilterChanges") == 0) {
  }
  return 0;
}
