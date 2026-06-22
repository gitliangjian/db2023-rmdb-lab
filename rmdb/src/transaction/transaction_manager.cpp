/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction_manager.h"
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

namespace {
void clear_write_set(Transaction *txn) {
    auto write_set = txn->get_write_set();
    for (auto *write_record : *write_set) {
        delete write_record;
    }
    write_set->clear();
}
}

/**
 * @description: 事务的开始方法
 * @return {Transaction*} 开始事务的指针
 * @param {Transaction*} txn 事务指针，空指针代表需要创建新事务，否则开始已有事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
Transaction * TransactionManager::begin(Transaction* txn, LogManager* log_manager) {
    if (txn == nullptr) {
        txn = new Transaction(next_txn_id_++);
    }
    txn->set_state(TransactionState::GROWING);
    txn->set_start_ts(next_timestamp_++);
    if (log_manager != nullptr) {
        BeginLogRecord log_record(txn->get_transaction_id(), txn->get_prev_lsn());
        lsn_t lsn = log_manager->add_log_to_buffer(&log_record);
        txn->set_prev_lsn(lsn);
    }
    std::scoped_lock lock{latch_};
    txn_map[txn->get_transaction_id()] = txn;
    return txn;
}

/**
 * @description: 事务的提交方法
 * @param {Transaction*} txn 需要提交的事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    if (txn == nullptr || txn->get_state() == TransactionState::COMMITTED) {
        return;
    }
    if (log_manager != nullptr) {
        CommitLogRecord log_record(txn->get_transaction_id(), txn->get_prev_lsn());
        lsn_t lsn = log_manager->add_log_to_buffer(&log_record);
        txn->set_prev_lsn(lsn);
        log_manager->flush_log_to_disk();
    }
    txn->set_state(TransactionState::COMMITTED);
    clear_write_set(txn);
    auto lock_set = *txn->get_lock_set();
    for (auto &lock_data_id : lock_set) {
        lock_manager_->unlock(txn, lock_data_id);
    }
}

/**
 * @description: 事务的终止（回滚）方法
 * @param {Transaction *} txn 需要回滚的事务
 * @param {LogManager} *log_manager 日志管理器指针
 */
void TransactionManager::abort(Transaction * txn, LogManager *log_manager) {
    if (txn == nullptr || txn->get_state() == TransactionState::ABORTED) {
        return;
    }
    auto write_set = txn->get_write_set();
    for (auto it = write_set->rbegin(); it != write_set->rend(); ++it) {
        WriteRecord *write_record = *it;
        auto fh = sm_manager_->fhs_.at(write_record->GetTableName()).get();
        TabMeta &tab = sm_manager_->db_.get_table(write_record->GetTableName());
        if (write_record->GetWriteType() == WType::INSERT_TUPLE) {
            auto &new_keys = write_record->GetNewIndexKeys();
            for (size_t i = 0; i < tab.indexes.size() && i < new_keys.size(); ++i) {
                auto &index = tab.indexes[i];
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab.name, index.cols)).get();
                ih->delete_entry(new_keys[i].data(), txn);
            }
            fh->delete_record(write_record->GetRid(), nullptr);
        } else if (write_record->GetWriteType() == WType::DELETE_TUPLE) {
            fh->insert_record(write_record->GetRid(), write_record->GetRecord().data);
            auto &old_keys = write_record->GetOldIndexKeys();
            for (size_t i = 0; i < tab.indexes.size() && i < old_keys.size(); ++i) {
                auto &index = tab.indexes[i];
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab.name, index.cols)).get();
                ih->insert_entry(old_keys[i].data(), write_record->GetRid(), txn);
            }
        } else if (write_record->GetWriteType() == WType::UPDATE_TUPLE) {
            auto &new_keys = write_record->GetNewIndexKeys();
            for (size_t i = 0; i < tab.indexes.size() && i < new_keys.size(); ++i) {
                auto &index = tab.indexes[i];
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab.name, index.cols)).get();
                ih->delete_entry(new_keys[i].data(), txn);
            }
            fh->update_record(write_record->GetRid(), write_record->GetRecord().data, nullptr);
            auto &old_keys = write_record->GetOldIndexKeys();
            for (size_t i = 0; i < tab.indexes.size() && i < old_keys.size(); ++i) {
                auto &index = tab.indexes[i];
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab.name, index.cols)).get();
                ih->insert_entry(old_keys[i].data(), write_record->GetRid(), txn);
            }
        }
    }
    clear_write_set(txn);
    auto lock_set = *txn->get_lock_set();
    for (auto &lock_data_id : lock_set) {
        lock_manager_->unlock(txn, lock_data_id);
    }
    if (log_manager != nullptr) {
        AbortLogRecord log_record(txn->get_transaction_id(), txn->get_prev_lsn());
        lsn_t lsn = log_manager->add_log_to_buffer(&log_record);
        txn->set_prev_lsn(lsn);
        log_manager->flush_log_to_disk();
    }
    txn->set_state(TransactionState::ABORTED);
}
