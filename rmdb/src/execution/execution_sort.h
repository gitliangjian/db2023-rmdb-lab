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

class SortExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<ColMeta> sort_cols_;
    std::vector<bool> is_desc_;
    int limit_;
    std::vector<std::unique_ptr<RmRecord>> tuples_;
    size_t cursor_ = 0;

   public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, std::vector<TabCol> sel_cols, std::vector<bool> is_desc, int limit) {
        prev_ = std::move(prev);
        for (auto &sel_col : sel_cols) {
            sort_cols_.push_back(prev_->get_col_offset(sel_col));
        }
        is_desc_ = std::move(is_desc);
        limit_ = limit;
    }

    void beginTuple() override { 
        tuples_.clear();
        cursor_ = 0;
        prev_->beginTuple();
        for (; !prev_->is_end(); prev_->nextTuple()) {
            auto rec = prev_->Next();
            if (rec != nullptr) {
                tuples_.push_back(std::move(rec));
            }
        }
        std::stable_sort(tuples_.begin(), tuples_.end(), [&](const auto &lhs, const auto &rhs) {
            for (size_t i = 0; i < sort_cols_.size(); ++i) {
                auto &col = sort_cols_[i];
                int cmp = compare_raw_value(lhs->data + col.offset, rhs->data + col.offset, col.type, col.len);
                if (cmp != 0) {
                    return is_desc_[i] ? cmp > 0 : cmp < 0;
                }
            }
            return false;
        });
        if (limit_ >= 0 && static_cast<size_t>(limit_) < tuples_.size()) {
            tuples_.resize(static_cast<size_t>(limit_));
        }
    }

    void nextTuple() override {
        if (cursor_ < tuples_.size()) {
            cursor_++;
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) {
            return nullptr;
        }
        auto rec = std::make_unique<RmRecord>(prev_->tupleLen());
        memcpy(rec->data, tuples_[cursor_]->data, prev_->tupleLen());
        return rec;
    }

    Rid &rid() override { return _abstract_rid; }
    bool is_end() const override { return cursor_ >= tuples_.size(); }
    size_t tupleLen() const override { return prev_->tupleLen(); }
    const std::vector<ColMeta> &cols() const override { return prev_->cols(); }
    ColMeta get_col_offset(const TabCol &target) override { return prev_->get_col_offset(target); }
};
