/**
 * Copyright Soramitsu Co., Ltd. 2017 All Rights Reserved.
 * http://soramitsu.co.jp
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ametsuchi/ametsuchi.h>
#include <ametsuchi/comparator.h>
#include <ametsuchi/exception.h>
#include <ametsuchi/generated/asset_generated.h>
#include <ametsuchi/generated/transaction_generated.h>
#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>

static auto console = spdlog::stdout_color_mt("ametsuchi");

#define AMETSUCHI_CRITICAL(res, err)                                      \
  if (res == err) {                                                       \
    console->critical("{}", mdb_strerror(res));                           \
    console->critical("err in {} at #{} in file {}", __PRETTY_FUNCTION__, \
                      __LINE__, __FILE__);                                \
    throw exception::InternalError::FATAL;                                \
  }

#define AMETSUCHI_TREES_TOTAL 8
#define AMETSUCHI_MAX_DB_SIZE (8L * 1024 * 1024 * 1024)


namespace ametsuchi {


Ametsuchi::Ametsuchi(const std::string &db_folder)
    : path_(db_folder), tx_store_total(0) {
  // initialize database:
  // create folder, create all handles and btrees
  // in case of any errors prints error to stdout and exits
  init();
}

void Ametsuchi::append(const ByteArray &blob) {
  // TODO calculate the latest merkle root and return it

  auto tx = flatbuffers::GetRoot<iroha::Transaction>(blob.data());

  MDB_val c_key, c_val;
  int res;

  // 1. append TX in the end of TX STORE
  {
    c_key.mv_data = &(++tx_store_total);
    c_key.mv_size = sizeof(tx_store_total);
    c_val.mv_data = (void *)blob.data();
    c_val.mv_size = blob.size();

    if ((res = mdb_cursor_put(trees_.at("tx_store").second, &c_key, &c_val,
                              MDB_NOOVERWRITE | MDB_APPEND))) {
      AMETSUCHI_CRITICAL(res, MDB_KEYEXIST);
      AMETSUCHI_CRITICAL(res, MDB_MAP_FULL);
      AMETSUCHI_CRITICAL(res, MDB_TXN_FULL);
      AMETSUCHI_CRITICAL(res, EACCES);
      AMETSUCHI_CRITICAL(res, EINVAL);
    }
  }

  // 2. insert record into index_add_creator
  {
    auto creator = tx->creatorPubKey();
    c_key.mv_data = (void *)(creator->data());
    c_key.mv_size = creator->size();
    c_val.mv_data = &tx_store_total;
    c_val.mv_size = sizeof(tx_store_total);

    if ((res = mdb_cursor_put(trees_.at("index_add_creator").second, &c_key,
                              &c_val, 0))) {
      AMETSUCHI_CRITICAL(res, MDB_MAP_FULL);
      AMETSUCHI_CRITICAL(res, MDB_TXN_FULL);
      AMETSUCHI_CRITICAL(res, EACCES);
      AMETSUCHI_CRITICAL(res, EINVAL);
    }
  }

  // 3. insert record into index_transfer_sender and index_transfer_receiver
  if (tx->command_type() == iroha::Command::AssetTransfer) {
    // update index_transfer_sender
    auto cmd = tx->command_as_AssetTransfer();
    c_key.mv_data = (void *)(cmd->sender()->data());
    c_key.mv_size = cmd->sender()->size();

    if ((res = mdb_cursor_put(trees_.at("index_transfer_sender").second, &c_key,
                              &c_val, 0))) {
      AMETSUCHI_CRITICAL(res, MDB_MAP_FULL);
      AMETSUCHI_CRITICAL(res, MDB_TXN_FULL);
      AMETSUCHI_CRITICAL(res, EACCES);
      AMETSUCHI_CRITICAL(res, EINVAL);
    }

    // update index_transfer_receiver
    c_key.mv_data = (void *)(cmd->receiver()->data());
    c_key.mv_size = cmd->receiver()->size();

    if ((res = mdb_cursor_put(trees_.at("index_transfer_receiver").second,
                              &c_key, &c_val, 0))) {
      AMETSUCHI_CRITICAL(res, MDB_MAP_FULL);
      AMETSUCHI_CRITICAL(res, MDB_TXN_FULL);
      AMETSUCHI_CRITICAL(res, EACCES);
      AMETSUCHI_CRITICAL(res, EINVAL);
    }
  }

  // 4. update WSV
  {
    switch (tx->command_type()) {
      case iroha::Command::AssetCreate: {
        asset_create(tx->command_as_AssetCreate());
        break;
      }
      case iroha::Command::AssetAdd: {
        asset_add(tx->command_as_AssetAdd());
        break;
      }
      case iroha::Command::AssetRemove: {
        asset_remove(tx->command_as_AssetRemove());
        break;
      }
      case iroha::Command::AssetTransfer: {
        asset_transfer(tx->command_as_AssetTransfer());
        break;
      }
      case iroha::Command::AccountAdd: {
        account_add(tx->command_as_AccountAdd());
        break;
      }
      case iroha::Command::AccountRemove: {
        account_remove(tx->command_as_AccountRemove());
        break;
      }
      case iroha::Command::PeerAdd: {
        peer_add(tx->command_as_PeerAdd());
        break;
      }
      case iroha::Command::PeerRemove: {
        peer_remove(tx->command_as_PeerRemove());
        break;
      }
      default: {
        console->critical("Not implemented. Yet.");
        throw exception::InternalError::NOT_IMPLEMENTED;
      }
    }
  }
}

void Ametsuchi::append(const std::vector<ByteArray> &batch) {
  // TODO calculate the latest merkle root and return it
  for (auto &&tx : batch) {
    append(tx);
  }
}


void Ametsuchi::commit() {
  // commit old transaction
  for (auto &&e : trees_) {
    MDB_cursor *cursor = e.second.second;

    mdb_cursor_close(cursor);
  }
  mdb_txn_commit(append_tx_);
  mdb_env_stat(env, &mst);

  // create new append transaction
  init_append_tx();
}


void Ametsuchi::rollback() {
  abort_append_tx();
  init_append_tx();
}

// std::vector<const MDB_val> Ametsuchi::getAddTxByCreator(const std::string
// &pubKey) {
// TODO
/*
int res;

std::vector<ByteArray> ret;

MDB_val c_key, c_val;
MDB_txn *read_tx;
MDB_dbi read_dbi;
MDB_cursor *cursor;

c_key.mv_size = pubKey.size();
c_key.mv_data = (void *)pubKey.c_str();

if ((res = mdb_txn_begin(env, NULL, MDB_RDONLY, &read_tx))) {
  if (res == MDB_PANIC) console->critical("getAddTxByCreator: MDB_PANIC");
  if (res == MDB_MAP_RESIZED)
    console->critical("getAddTxByCreator: MDB_MAP_RESIZED ");
  if (res == MDB_READERS_FULL)
    console->critical("getAddTxByCreator: MDB_READERS_FULL");
  if (res == ENOMEM) console->critical("getAddTxByCreator: ENOMEM");
  exit(res);
}
mdb_dbi_open(read_tx, "add_creator", MDB_DUPSORT | MDB_DUPFIXED, &read_dbi);
mdb_cursor_open(read_tx, read_dbi, &cursor);


if ((res = mdb_cursor_get(cursor, &c_key, &c_val, MDB_SET)) ==
    MDB_NOTFOUND) {                 // no items with given key
  return std::vector<ByteArray>{};  // return empty vector
}

do {
  size_t index = *static_cast<size_t *>(c_val.mv_data);
  ret.push_back(tx_->get(index));

  if ((res = mdb_cursor_get(cursor, &c_key, &c_val, MDB_NEXT_DUP)) ==
      MDB_NOTFOUND) {  // no more items with given key
    break;
  }
} while (!res);

return ret;
 */
//}


void Ametsuchi::abort_append_tx() {
  for (auto &&e : trees_) {
    MDB_cursor *cursor = e.second.second;
    if (cursor) mdb_cursor_close(cursor);
  }
  if (append_tx_) mdb_txn_abort(append_tx_);
}


void Ametsuchi::init() {
  int res;

  // create database directory
  if ((res = mkdir(path_.c_str(), 0700))) {
    if (res == EEXIST) {
      console->debug("folder with database exists");
    } else {
      AMETSUCHI_CRITICAL(res, EACCES);
      AMETSUCHI_CRITICAL(res, ELOOP);
      AMETSUCHI_CRITICAL(res, EMLINK);
      AMETSUCHI_CRITICAL(res, ENAMETOOLONG);
      AMETSUCHI_CRITICAL(res, ENOENT);
      AMETSUCHI_CRITICAL(res, ENOSPC);
      AMETSUCHI_CRITICAL(res, ENOTDIR);
      AMETSUCHI_CRITICAL(res, EROFS);
    }
  }

  // create environment
  if ((res = mdb_env_create(&env))) {
    AMETSUCHI_CRITICAL(res, MDB_VERSION_MISMATCH);
    AMETSUCHI_CRITICAL(res, MDB_INVALID);
    AMETSUCHI_CRITICAL(res, ENOENT);
    AMETSUCHI_CRITICAL(res, EACCES);
    AMETSUCHI_CRITICAL(res, EAGAIN);
  }

  // set maximum mmap size. Must be multiple of OS page size (4 KB).
  // max size of the database (!!!)
  if ((res = mdb_env_set_mapsize(env, AMETSUCHI_MAX_DB_SIZE))) {
    AMETSUCHI_CRITICAL(res, EINVAL);
  }

  // set number of databases in single file
  if ((res = mdb_env_set_maxdbs(env, AMETSUCHI_TREES_TOTAL))) {
    AMETSUCHI_CRITICAL(res, EINVAL);
  }

  // create database environment
  if ((res = mdb_env_open(env, path_.c_str(), MDB_FIXEDMAP, 0700))) {
    AMETSUCHI_CRITICAL(res, MDB_VERSION_MISMATCH);
    AMETSUCHI_CRITICAL(res, MDB_INVALID);
    AMETSUCHI_CRITICAL(res, ENOENT);
    AMETSUCHI_CRITICAL(res, EACCES);
    AMETSUCHI_CRITICAL(res, EAGAIN);
  }

  // stats about db
  mdb_env_stat(env, &mst);

  // initialize
  init_append_tx();

  tx_store_total = tx_store_size();  // last index in db as "total entries"

  // we should know created assets, so read entire table in memory
  read_created_assets();
}


void Ametsuchi::init_btree(const std::string &name, uint32_t flags,
                           MDB_cmp_func *dupsort) {
  int res;
  MDB_dbi dbi;
  MDB_cursor *cursor;
  if ((res = mdb_dbi_open(append_tx_, name.c_str(), flags, &dbi))) {
    AMETSUCHI_CRITICAL(res, MDB_NOTFOUND);
    AMETSUCHI_CRITICAL(res, MDB_DBS_FULL);
  }

  if ((res = mdb_cursor_open(append_tx_, dbi, &cursor))) {
    AMETSUCHI_CRITICAL(res, EINVAL);
  }

  // set comparator for dupsort keys in btree
  if (dupsort) {
    if ((res = mdb_set_dupsort(append_tx_, dbi, dupsort))) {
      AMETSUCHI_CRITICAL(res, EINVAL);
    }
  }

  // TODO: potential memory leak: close previous cursor if it is not NULL

  trees_[name] = {dbi, cursor};
}

size_t Ametsuchi::tx_store_size() {
  MDB_val c_key, c_val;
  int res;

  MDB_cursor *cursor = trees_.at("tx_store").second;

  if ((res = mdb_cursor_get(cursor, &c_key, &c_val, MDB_LAST))) {
    if (res == MDB_NOTFOUND) return 0u;
    AMETSUCHI_CRITICAL(res, EINVAL);
  }

  return *reinterpret_cast<size_t *>(c_key.mv_data);
}

void Ametsuchi::init_append_tx() {
  int res;

  // begin "append" transaction
  if ((res = mdb_txn_begin(env, NULL, 0, &append_tx_))) {
    AMETSUCHI_CRITICAL(res, MDB_PANIC);
    AMETSUCHI_CRITICAL(res, MDB_MAP_RESIZED);
    AMETSUCHI_CRITICAL(res, MDB_READERS_FULL);
    AMETSUCHI_CRITICAL(res, ENOMEM);
  }

  // create database instances for each tree, open cursors for each tree, save
  // them in map this.trees_: [name] => std::pair<dbi, cursor>

  // [autoincrement_key] => tx (NODUP)
  init_btree("tx_store", MDB_CREATE | MDB_INTEGERKEY);

  // [pubkey] => autoincrement_key (DUP)
  init_btree("index_add_creator", MDB_DUPSORT | MDB_DUPFIXED | MDB_CREATE);

  // [pubkey] => autoincrement_key (DUP)
  init_btree("index_transfer_sender", MDB_DUPSORT | MDB_DUPFIXED | MDB_CREATE);

  // [pubkey] => autoincrement_key (DUP)
  init_btree("index_transfer_receiver",
             MDB_DUPSORT | MDB_DUPFIXED | MDB_CREATE);

  // [pubkey] => assets (DUP)
  init_btree("wsv_pubkey_assets", MDB_DUPSORT | MDB_DUPFIXED | MDB_CREATE,
             comparator::cmp_assets);

  // [pubkey] => account (NODUP)
  init_btree("wsv_pubkey_account", MDB_CREATE);

  // [ledger_name+domain_name+asset_name] => creator public key (NODUP)
  init_btree("wsv_assetid_asset", MDB_CREATE);

  // [ip] => peer (NODUP)
  init_btree("wsv_ip_peer", MDB_CREATE);

  assert(AMETSUCHI_TREES_TOTAL == trees_.size());

  // stats about db
  mdb_env_stat(env, &mst);
}


void Ametsuchi::account_add(const iroha::AccountAdd *command) {
  MDB_val c_key, c_val;
  int res;

  auto account =
      flatbuffers::GetRoot<iroha::Account>(command->account()->data());
  auto pubkey = account->pubKey();

  c_key.mv_data = (void *)(pubkey->data());
  c_key.mv_size = pubkey->size();
  c_val.mv_data = (void *)command->account()->data();
  c_val.mv_size = command->account()->size();

  if ((res = mdb_cursor_put(trees_.at("wsv_pubkey_account").second, &c_key,
                            &c_val, 0))) {
    // account with this public key exists
    if (res == MDB_KEYEXIST) {
      throw exception::InvalidTransaction::ACCOUNT_EXISTS;
    }
    AMETSUCHI_CRITICAL(res, MDB_MAP_FULL);
    AMETSUCHI_CRITICAL(res, MDB_TXN_FULL);
    AMETSUCHI_CRITICAL(res, EACCES);
    AMETSUCHI_CRITICAL(res, EINVAL);
  }
}


void Ametsuchi::peer_add(const iroha::PeerAdd *command) {
  MDB_cursor *cursor = trees_.at("wsv_ip_peer").second;
  MDB_val c_key, c_val;
  int res;

  auto peer = flatbuffers::GetRoot<iroha::Peer>(command->peer()->data());
  auto ip = peer->ip();

  flatbuffers::GetRoot<iroha::Peer>(peer);

  c_key.mv_data = (void *)(ip->data());
  c_key.mv_size = ip->size();
  c_val.mv_data = (void *)command->peer()->data();
  c_val.mv_size = command->peer()->size();

  if ((res = mdb_cursor_put(cursor, &c_key, &c_val, 0))) {
    // account with this public key exists
    if (res == MDB_KEYEXIST) {
      throw exception::InvalidTransaction::PEER_EXISTS;
    }
    AMETSUCHI_CRITICAL(res, MDB_MAP_FULL);
    AMETSUCHI_CRITICAL(res, MDB_TXN_FULL);
    AMETSUCHI_CRITICAL(res, EACCES);
    AMETSUCHI_CRITICAL(res, EINVAL);
  }
}


void Ametsuchi::asset_create(const iroha::AssetCreate *command) {
  MDB_val c_key, c_val;
  int res;

  auto ln = command->ledger_name();
  auto dn = command->domain_name();
  auto an = command->asset_name();

  // in this order: ledger+domain+asset
  std::string pk;
  pk += ln->data();
  pk += dn->data();
  pk += an->data();

  // create Asset
  flatbuffers::FlatBufferBuilder fbb;
  auto asset =
      iroha::CreateCurrencyDirect(fbb, an->data(), dn->data(), ln->data());
  fbb.Finish(asset);

  auto ptr = fbb.GetBufferPointer();

  c_key.mv_data = (void *)(pk.c_str());
  c_key.mv_size = reinterpret_cast<size_t>(pk.size());
  c_val.mv_data = (void *)ptr;
  c_val.mv_size = fbb.GetSize();

  if ((res = mdb_cursor_put(trees_.at("wsv_assetid_asset").second, &c_key,
                            &c_val, 0))) {
    if (res == MDB_KEYEXIST) {
      throw exception::InvalidTransaction::ASSET_EXISTS;
    }
    AMETSUCHI_CRITICAL(res, MDB_MAP_FULL);
    AMETSUCHI_CRITICAL(res, MDB_TXN_FULL);
    AMETSUCHI_CRITICAL(res, EACCES);
    AMETSUCHI_CRITICAL(res, EINVAL);
  }

  created_assets_[pk] = std::vector<uint8_t>{ptr, ptr + fbb.GetSize()};
}


bool Ametsuchi::asset_add(const iroha::AssetAdd *command) {
  /*
  MDB_cursor *cursor = trees_.at("wsv_pubkey_assets").second;
  MDB_val c_key, c_val;
  int res;

  auto asset = flatbuffers::GetRoot<iroha::Asset>(command->asset());

  if (asset->asset_type() != iroha::AnyAsset::Currency) {
    console->critical("Only Currency assets are supported.");
    throw exception::InternalError::NOT_IMPLEMENTED;
  }

  auto currency = asset->asset_as_Currency();

  // in this order: ledger+domain+asset
  std::string pk;
  pk += currency->ledger_name()->data();
  pk += currency->domain_name()->data();
  pk += currency->currency_name()->data();

  // query asset by public key
  c_key.mv_data = (void *)command->accPubKey()->data();
  c_key.mv_size = command->accPubKey()->size();

  if ((res = mdb_cursor_get(cursor, &c_key, &c_val, MDB_SET)) == MDB_NOTFOUND) {
    // append new value
    c_key.mv_data = (void *)command->asset()->data();
    c_key.mv_size = command->asset()->size();

    if ((res = mdb_cursor_put(cursor, &c_key, &c_val, 0))) {
      AMETSUCHI_CRITICAL(res, MDB_MAP_FULL);
      AMETSUCHI_CRITICAL(res, MDB_TXN_FULL);
      AMETSUCHI_CRITICAL(res, EACCES);
      AMETSUCHI_CRITICAL(res, EINVAL);
    } else {
      // we successfully added new asset to user's account
      return true;
    }
  }

  // account has assets. try to find asset with the same `pk`
  auto a = flatbuffers::GetMutableRoot<iroha::Asset>(c_val.mv_data);
  auto c = a->asset_as_Currency();

  // iterate over account's assets, O(N), where N is number of different assets,
  // which account has
  do {
    // user's current amount
    auto u =
        flatbuffers::GetMutableRoot<iroha::Currency>((void *)c_val.mv_size);

    if (u->ledger_name() == c->ledger_name() &&
        u->domain_name() == c->ledger_name() &&
        u->currency_name() == c->currency_name()) {
      // given asset exists in user's db
      Currency uc(u->amount(), u->precision());
      Currency ac(c->amount(), c->precision());

      Currency new_value = uc + ac;

      // update user's data with new values
      u->mutate_amount(new_value.get_amount());
      u->mutate_precision(new_value.get_precision());
    }

    // move to next asset in user's account
    if ((res = mdb_cursor_get(cursor, &c_key, &c_val, MDB_NEXT_DUP)) ==
        MDB_NOTFOUND) {
      // asset with given PK is not found, append new
      if ((res = mdb_cursor_put(cursor, &c_key, &c_val, 0))) {
        AMETSUCHI_CRITICAL(res, EINVAL);
      } else {
        // we successfully added new asset to user's account
        return true;
      }
    }
  } while (res == 0);
   */
}


void Ametsuchi::peer_remove(const iroha::PeerRemove *command) {
  auto cursor = trees_.at("wsv_ip_peer").second;
  MDB_val c_key, c_val;
  int res;

  auto peer = flatbuffers::GetRoot<iroha::Peer>(command->peer()->data());
  auto ip = peer->ip();

  flatbuffers::GetRoot<iroha::Peer>(peer);

  c_key.mv_data = (void *)(ip->data());
  c_key.mv_size = ip->size();
  c_val.mv_data = (void *)command->peer()->data();
  c_val.mv_size = command->peer()->size();

  if ((res = mdb_cursor_get(cursor, &c_key, &c_val, MDB_SET))) {
    if (res == MDB_NOTFOUND) {
      throw exception::InvalidTransaction::PEER_NOT_FOUND;
    }

    AMETSUCHI_CRITICAL(res, EINVAL);
  }

  if ((res = mdb_cursor_del(cursor, 0))) {
    AMETSUCHI_CRITICAL(res, EACCES);
    AMETSUCHI_CRITICAL(res, EINVAL);
  }
}

bool Ametsuchi::asset_remove(const iroha::AssetRemove *command) {
  // TODO: review and check

  /*
  // TODO: test is needed
  MDB_val c_key, c_val;
  MDB_cursor *cursor = trees_.at("wsv_pubkey_assets").second;
  int res;

  auto asset = flatbuffers::GetRoot<iroha::Asset>(command->asset());

  if (asset->asset_type() != iroha::AnyAsset::Currency) {
    console->critical("Only Currency assets are supported.");
    throw FATAL_ERROR;
  }

  auto currency = asset->asset_as_Currency();

  // in this order: ledger+domain+asset
  std::string pk;
  pk += currency->ledger_name()->data();
  pk += currency->domain_name()->data();
  pk += currency->currency_name()->data();

  // query asset by public key
  c_key.mv_data = (void *)command->accPubKey()->data();
  c_key.mv_size = command->accPubKey()->size();

  if ((res = mdb_cursor_get(cursor, &c_key, &c_val, MDB_SET)) == MDB_NOTFOUND) {
    throw INCORRECT_TRANSACTION;
  }

  // account has assets. try to find asset with the same `pk`
  auto a = flatbuffers::GetMutableRoot<iroha::Asset>(c_val.mv_data);
  auto c = a->asset_as_Currency();

  // iterate over account's assets, O(N), where N is number of different assets,
  // which account has
  do {
    // user's current amount
    auto u =
        flatbuffers::GetMutableRoot<iroha::Currency>((void *)c_val.mv_size);

    if (u->ledger_name() == c->ledger_name() &&
        u->domain_name() == c->ledger_name() &&
        u->currency_name() == c->currency_name()) {
      // given asset exists in user's db
      Currency uc(u->amount(), u->precision());
      Currency ac(c->amount(), c->precision());

      // throws exception if uc < ac
      Currency new_value = uc - ac;

      // update user's data with new values
      u->mutate_amount(new_value.get_amount());
      u->mutate_precision(new_value.get_precision());

      return true;
    }

    // move to next asset in user's account
    if ((res = mdb_cursor_get(cursor, &c_key, &c_val, MDB_NEXT_DUP)) ==
        MDB_NOTFOUND) {
      // asset with given PK is not found, then current TX is incorrect
      throw INCORRECT_TRANSACTION;
    }
  } while (res == 0);

  return true;
   */
}

void Ametsuchi::account_add_currency(const flatbuffers::String *acc_pub_key,
                                     const iroha::Currency *c, size_t c_size) {
  int res;
  MDB_val c_key, c_val;
  auto cursor = trees_.at("wsv_pubkey_assets").second;
  std::vector<uint8_t> copy;

  try {
    // may throw ASSET_NOT_FOUND
    MDB_val account_asset = this->accountGetAsset(
        acc_pub_key, c->ledger_name(), c->domain_name(), c->currency_name());

    assert(c_size == account_asset.mv_size);

    // asset exists, change it:

    copy = {account_asset.mv_data,
            account_asset.mv_data + account_asset.mv_size};

    // update the copy
    auto copy_fb = flatbuffers::GetMutableRoot<iroha::Currency>(copy.data());

    Currency current(copy_fb->amount(), copy_fb->precision());
    Currency delta(c->amount(), c->precision());
    Currency new_state = current + delta;

    copy_fb->mutate_amount(new_state.get_amount());
    copy_fb->mutate_precision(new_state.get_precision());

  } catch (exception::InvalidTransaction e) {
    if (e == exception::InvalidTransaction::ASSET_NOT_FOUND) {
      copy = {c, c + c_size};
    } else {
      throw;
    }
  }

  // write to tree
  c_key.mv_data = (void *)acc_pub_key->data();
  c_key.mv_size = acc_pub_key->size();
  c_val.mv_data = (void *)copy.data();
  c_val.mv_size = copy.size();

  // cursor is at the correct asset, just replace with a copy of FB and flag
  // MDB_CURRENT
  if ((res = mdb_cursor_put(cursor, &c_key, &c_val, MDB_CURRENT))) {
    AMETSUCHI_CRITICAL(res, MDB_KEYEXIST);
    AMETSUCHI_CRITICAL(res, MDB_MAP_FULL);
    AMETSUCHI_CRITICAL(res, MDB_TXN_FULL);
    AMETSUCHI_CRITICAL(res, EACCES);
    AMETSUCHI_CRITICAL(res, EINVAL);
  }
}


void Ametsuchi::account_remove_currency(const flatbuffers::String *acc_pub_key,
                                        const iroha::Currency *c) {
  int res;
  MDB_val c_key, c_val;
  auto cursor = trees_.at("wsv_pubkey_assets").second;
  std::vector<uint8_t> copy;

  // may throw ASSET_NOT_FOUND
  MDB_val account_asset = this->accountGetAsset(
      acc_pub_key, c->ledger_name(), c->domain_name(), c->currency_name());

  // asset exists, change it:

  copy = {account_asset.mv_data, account_asset.mv_data + account_asset.mv_size};

  // update the copy
  auto copy_fb = flatbuffers::GetMutableRoot<iroha::Currency>(copy.data());

  Currency current(copy_fb->amount(), copy_fb->precision());
  Currency delta(c->amount(), c->precision());
  if (current < delta) throw exception::InvalidTransaction::NOT_ENOUGH_ASSETS;
  Currency new_state = current - delta;

  copy_fb->mutate_amount(new_state.get_amount());
  copy_fb->mutate_precision(new_state.get_precision());

  // write to tree
  c_key.mv_data = (void *)acc_pub_key->data();
  c_key.mv_size = acc_pub_key->size();
  c_val.mv_data = (void *)copy.data();
  c_val.mv_size = copy.size();

  // cursor is at the correct asset, just replace with a copy of FB and flag
  // MDB_CURRENT
  if ((res = mdb_cursor_put(cursor, &c_key, &c_val, MDB_CURRENT))) {
    AMETSUCHI_CRITICAL(res, MDB_KEYEXIST);
    AMETSUCHI_CRITICAL(res, MDB_MAP_FULL);
    AMETSUCHI_CRITICAL(res, MDB_TXN_FULL);
    AMETSUCHI_CRITICAL(res, EACCES);
    AMETSUCHI_CRITICAL(res, EINVAL);
  }
}


void Ametsuchi::asset_transfer(const iroha::AssetTransfer *command) {
  auto asset_fb = flatbuffers::GetRoot<iroha::Asset>(command->asset());

  if (asset_fb->asset_type() != iroha::AnyAsset::Currency)
    throw exception::InternalError::NOT_IMPLEMENTED;

  auto currency = asset_fb->asset_as_Currency();

  // sender should have enough assets:
  MDB_val sender_asset =
      this->accountGetAsset(command->sender(), currency->ledger_name(),
                            currency->domain_name(), currency->currency_name());
  auto sender_cur = flatbuffers::GetRoot<iroha::Asset>(sender_asset.mv_data)
                        ->asset_as_Currency();
  Currency csender(sender_cur->amount(), sender_cur->precision());
  Currency in_tx(currency->amount(), currency->precision());

  if (csender < in_tx) throw exception::InvalidTransaction::NOT_ENOUGH_ASSETS;

  Currency new_sender_cur = csender - in_tx;

  // cursor is at required position, just replace flatbuffer with a copy

  int res;
  MDB_val c_key, c_val;

  std::vector<uint8_t> copy{
      command->asset()->data(),
      command->asset()->data() + command->asset()->size()};

  auto new_sender_asset =
      (iroha::Currency *)(flatbuffers::GetMutableRoot<iroha::Asset>(
          copy.data()));
  // ????????????? WHY CONST ???????????????????
  // TODO

  new_sender_asset->mutate_amount(new_sender_cur.get_amount());

  c_key.mv_data = (void *)command->sender()->data();
  c_key.mv_size = command->receiver()->size();
  c_val.mv_data = (void *)command->asset()->data();
  c_val.mv_size = command->asset()->size();

  // cursor is at the correct asset, just replace with a copy of FB and flag
  // MDB_CURRENT
  // TODO

  if ((res = mdb_cursor_put(trees_.at("wsv_pubkey_assets").second, &c_key,
                            &c_val, MDB_CURRENT))) {
    AMETSUCHI_CRITICAL(res, MDB_KEYEXIST);
    AMETSUCHI_CRITICAL(res, MDB_MAP_FULL);
    AMETSUCHI_CRITICAL(res, MDB_TXN_FULL);
    AMETSUCHI_CRITICAL(res, EACCES);
    AMETSUCHI_CRITICAL(res, EINVAL);
  }


  try {
    MDB_val receiver_asset = this->accountGetAsset(
        command->receiver(), currency->ledger_name(), currency->domain_name(),
        currency->currency_name());

    // cursor is at the correct asset, just replace with a copy of FB and flag
    // MDB_CURRENT
    // TODO

  } catch (exception::InvalidTransaction e) {
  }
}

std::vector<MDB_val> Ametsuchi::accountGetAssets(
    const flatbuffers::String *pubKey) {
  MDB_cursor *cursor = trees_.at("wsv_pubkey_assets").second;
  MDB_val c_key, c_val;
  int res;

  // query asset by public key
  c_key.mv_data = (void *)pubKey->data();
  c_key.mv_size = pubKey->size();

  // if sender has no such asset, then it is incorrect transaction
  if ((res = mdb_cursor_get(cursor, &c_key, &c_val, MDB_SET))) {
    if (res == MDB_NOTFOUND) return std::vector<MDB_val>{};
    AMETSUCHI_CRITICAL(res, EINVAL);
  }

  std::vector<MDB_val> ret(10);  // optimization
  // account has assets. try to find asset with the same `pk`
  // iterate over account's assets, O(N), where N is number of different assets,
  do {
    // user's current amount
    ret.push_back(c_val);

    // move to next asset in user's account
    if ((res = mdb_cursor_get(cursor, &c_key, &c_val, MDB_NEXT_DUP))) {
      if (res == MDB_NOTFOUND) return ret;
      AMETSUCHI_CRITICAL(res, EINVAL);
    }
  } while (res == 0);

  return ret;
}


MDB_val Ametsuchi::accountGetAsset(const flatbuffers::String *pubKey,
                                   const flatbuffers::String *ln,
                                   const flatbuffers::String *dn,
                                   const flatbuffers::String *cn) {
  MDB_cursor *cursor = trees_.at("wsv_pubkey_assets").second;
  MDB_val c_key, c_val;
  int res;

  std::string pk;
  pk += ln->data();
  pk += dn->data();
  pk += cn->data();

  // if given asset exists, then we can get its blob, which consists of ledger
  // name, domain name and asset name to speedup read in DUP btree, because we
  // have custom comparator
  auto blob = created_assets_.find(pk);
  if (blob != created_assets_.end()) {
    // asset found, use asset's flatbuffer to find asset in account faster
    c_val.mv_data = (void *)blob->second.data();
    c_val.mv_size = blob->second.size();
  } else {
    throw exception::InvalidTransaction::ASSET_NOT_FOUND;
  }

  // query asset by public key
  c_key.mv_data = (void *)pubKey->data();
  c_key.mv_size = pubKey->size();

  // if sender has no such asset, then it is incorrect transaction
  if ((res = mdb_cursor_get(cursor, &c_key, &c_val, MDB_GET_BOTH))) {
    if (res == MDB_NOTFOUND)
      throw exception::InvalidTransaction::ASSET_NOT_FOUND;
    AMETSUCHI_CRITICAL(res, EINVAL);
  }

  return c_val;
}


void Ametsuchi::read_created_assets() {
  /*
  created_assets_.clear();

  for (auto &&asset : read_all_records("wsv_assetid_asset")) {
    std::string assetid{asset.first.mv_data,
                        asset.first.mv_data + asset.first.mv_size};

    std::vector<uint8_t> assetfb{asset.second.mv_data,
                                 asset.second.mv_data + asset.second.mv_size};

    created_assets_[assetid] = assetfb;
  }
   */
}


std::vector<std::pair<MDB_val, MDB_val>> Ametsuchi::read_all_records(
    const std::string &tree_name) {
  MDB_cursor *cursor = trees_.at(tree_name).second;
  MDB_val c_key, c_val;
  int res;

  if ((res = mdb_cursor_get(cursor, &c_key, &c_val, MDB_FIRST))) {
    if (res == MDB_NOTFOUND) return std::vector<std::pair<MDB_val, MDB_val>>{};
    AMETSUCHI_CRITICAL(res, EINVAL);
  }

  std::vector<std::pair<MDB_val, MDB_val>> ret(10);  // optimization
  do {
    ret.push_back({c_key, c_val});

    if ((res = mdb_cursor_get(cursor, &c_key, &c_val, MDB_NEXT))) {
      if (res == MDB_NOTFOUND) return ret;
      AMETSUCHI_CRITICAL(res, EINVAL);
    }
  } while (res == 0);

  return ret;
}

Ametsuchi::~Ametsuchi() {
  abort_append_tx();
  for (auto &&it : trees_) {
    auto dbi = it.second.first;
    mdb_dbi_close(env, dbi);
  }
  mdb_env_close(env);
}

bool Ametsuchi::account_remove(const iroha::AccountRemove *command) {
  // TODO
  return false;
}

}  // namespace ametsuchi
