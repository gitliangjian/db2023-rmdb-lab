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
#include <algorithm>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    static constexpr size_t JOIN_BUFFER_BYTES = 8 * 1024 * 1024;
    static constexpr size_t JOIN_BUFFER_MAX_RECORDS = 65536;

    std::unique_ptr<AbstractExecutor> left_;    // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_;   // 右儿子节点（需要join的表）
    size_t len_;                                // join后获得的每条记录的长度
    std::vector<ColMeta> cols_;                 // join后获得的记录的字段

    std::vector<Condition> fed_conds_;          // join条件
    bool isend;
    std::vector<std::unique_ptr<RmRecord>> left_block_;
    size_t left_block_pos_;
    std::unique_ptr<RmRecord> right_rec_;

    bool load_left_block() {
        left_block_.clear();
        left_block_pos_ = 0;

        size_t max_records = JOIN_BUFFER_BYTES / std::max<size_t>(left_->tupleLen(), 1);
        max_records = std::max<size_t>(1, std::min(max_records, JOIN_BUFFER_MAX_RECORDS));
        while (!left_->is_end() && left_block_.size() < max_records) {
            auto rec = left_->Next();
            if (rec != nullptr) {
                left_block_.push_back(std::move(rec));
            }
            left_->nextTuple();
        }
        return !left_block_.empty();
    }

    void restart_right_scan() {
        right_->beginTuple();
        right_rec_.reset();
        left_block_pos_ = 0;
    }

    bool advance_to_match() {
        while (true) {
            if (left_block_.empty()) {
                if (!load_left_block()) {
                    return false;
                }
                restart_right_scan();
            }

            while (!right_->is_end()) {
                if (right_rec_ == nullptr) {
                    right_rec_ = right_->Next();
                }

                for (; left_block_pos_ < left_block_.size(); ++left_block_pos_) {
                    auto &left_rec = left_block_[left_block_pos_];
                    if (left_rec == nullptr || right_rec_ == nullptr) {
                        continue;
                    }
                    auto joined = std::make_unique<RmRecord>(len_);
                    memcpy(joined->data, left_rec->data, left_->tupleLen());
                    memcpy(joined->data + left_->tupleLen(), right_rec_->data, right_->tupleLen());
                    if (eval_conds_on_record(cols_, joined.get(), fed_conds_)) {
                        return true;
                    }
                }

                right_->nextTuple();
                right_rec_.reset();
                left_block_pos_ = 0;
            }

            if (!load_left_block()) {
                return false;
            }
            restart_right_scan();
            if (right_->is_end()) {
                return false;
            }
        }
    }

    std::unique_ptr<RmRecord> make_joined_record() const {
        if (left_block_pos_ >= left_block_.size() || left_block_[left_block_pos_] == nullptr || right_rec_ == nullptr) {
            return nullptr;
        }
        auto joined = std::make_unique<RmRecord>(len_);
        memcpy(joined->data, left_block_[left_block_pos_]->data, left_->tupleLen());
        memcpy(joined->data + left_->tupleLen(), right_rec_->data, right_->tupleLen());
        return joined;
    }

    bool advance_from_current() {
        if (left_block_pos_ < left_block_.size()) {
            ++left_block_pos_;
        }
        return advance_to_match();
    }

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right, 
                            std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }

        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        isend = false;
        left_block_pos_ = 0;
        fed_conds_ = std::move(conds);

    }

    void beginTuple() override {
        left_->beginTuple();
        left_block_.clear();
        left_block_pos_ = 0;
        right_rec_.reset();
        isend = !advance_to_match();
    }

    void nextTuple() override {
        if (isend) {
            return;
        }
        isend = !advance_from_current();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (isend) {
            return nullptr;
        }
        return make_joined_record();
    }

    Rid &rid() override { return _abstract_rid; }
    bool is_end() const override { return isend; }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
};
