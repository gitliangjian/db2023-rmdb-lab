/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;

   public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = set_clauses;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        context_ = context;
        if (context_ != nullptr && context_->lock_mgr_ != nullptr) {
            context_->lock_mgr_->lock_exclusive_on_table(context_->txn_, fh_->GetFd());
        }
        conds_ = conds;
        rids_ = rids;
    }
    std::unique_ptr<RmRecord> Next() override {
        for (auto &rid : rids_) {
            auto rec = fh_->get_record(rid, context_);
            std::vector<std::vector<char>> old_keys;
            old_keys.reserve(tab_.indexes.size());
            for (auto &index : tab_.indexes) {
                old_keys.push_back(make_index_key(index, rec->data));
            }
            for (auto &set_clause : set_clauses_) {
                auto col = tab_.get_col(set_clause.lhs.col_name);
                if (col->type == TYPE_DATETIME && set_clause.rhs.type == TYPE_STRING) {
                    set_clause.rhs.set_datetime(encode_datetime(set_clause.rhs.str_val));
                    set_clause.rhs.init_raw(col->len);
                }
                if (col->type != set_clause.rhs.type) {
                    throw IncompatibleTypeError(coltype2str(col->type), coltype2str(set_clause.rhs.type));
                }
                memcpy(rec->data + col->offset, set_clause.rhs.raw->data, col->len);
            }
            std::vector<std::vector<char>> new_keys;
            new_keys.reserve(tab_.indexes.size());
            for (size_t i = 0; i < tab_.indexes.size(); ++i) {
                auto &index = tab_.indexes[i];
                auto key = make_index_key(index, rec->data);
                if (memcmp(key.data(), old_keys[i].data(), index.col_tot_len) != 0) {
                    auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
                    std::vector<Rid> hits;
                    if (ih->get_value(key.data(), &hits, context_->txn_)) {
                        throw DuplicateKeyError();
                    }
                }
                new_keys.push_back(std::move(key));
            }
            for (size_t i = 0; i < tab_.indexes.size(); ++i) {
                auto &index = tab_.indexes[i];
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
                ih->delete_entry(old_keys[i].data(), context_->txn_);
            }
            RmRecord old_rec(fh_->get_file_hdr().record_size);
            memcpy(old_rec.data, fh_->get_record(rid, context_)->data, fh_->get_file_hdr().record_size);
            fh_->update_record(rid, rec->data, context_);
            if (context_ != nullptr && context_->txn_ != nullptr && context_->log_mgr_ != nullptr) {
                UpdateLogRecord log_record(context_->txn_->get_transaction_id(), old_rec, *rec, rid, tab_name_,
                                           context_->txn_->get_prev_lsn());
                lsn_t lsn = context_->log_mgr_->add_log_to_buffer(&log_record);
                context_->txn_->set_prev_lsn(lsn);
                context_->log_mgr_->flush_log_to_disk();
            }
            for (size_t i = 0; i < tab_.indexes.size(); ++i) {
                auto &index = tab_.indexes[i];
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
                ih->insert_entry(new_keys[i].data(), rid, context_->txn_);
            }
            if (context_ != nullptr && context_->txn_ != nullptr) {
                auto write_record = new WriteRecord(WType::UPDATE_TUPLE, tab_name_, rid, old_rec);
                write_record->SetOldIndexKeys(std::move(old_keys));
                write_record->SetNewIndexKeys(std::move(new_keys));
                context_->txn_->append_write_record(write_record);
            }
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
