/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <iomanip>
#include <sstream>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "system/sm.h"

class AggregateExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<SelectItem> items_;
    std::vector<ColMeta> cols_;
    size_t len_ = 0;
    bool produced_ = false;
    bool consumed_ = false;
    std::unique_ptr<RmRecord> result_;

    static std::string agg_name(AggType type) {
        switch (type) {
            case AGG_SUM: return "SUM";
            case AGG_MAX: return "MAX";
            case AGG_MIN: return "MIN";
            case AGG_COUNT: return "COUNT";
            default: return "";
        }
    }

   public:
    AggregateExecutor(std::unique_ptr<AbstractExecutor> prev, std::vector<SelectItem> items) {
        prev_ = std::move(prev);
        items_ = std::move(items);
        size_t curr_offset = 0;
        for (auto &item : items_) {
            ColMeta col;
            col.tab_name = "";
            col.name = item.alias.empty() ? agg_name(item.agg_type) : item.alias;
            col.offset = curr_offset;
            col.index = false;
            if (item.agg_type == AGG_COUNT) {
                col.type = TYPE_INT;
                col.len = sizeof(int);
            } else {
                auto src = prev_->get_col_offset(item.col);
                col.type = src.type;
                col.len = src.len;
                if (item.agg_type == AGG_SUM && src.type == TYPE_STRING) {
                    throw IncompatibleTypeError("SUM", coltype2str(src.type));
                }
            }
            curr_offset += col.len;
            cols_.push_back(col);
        }
        len_ = curr_offset;
    }

    void beginTuple() override {
        produced_ = false;
        consumed_ = false;
        result_ = std::make_unique<RmRecord>(len_);
        std::vector<bool> initialized(items_.size(), false);
        std::vector<double> sums(items_.size(), 0);
        std::vector<int> counts(items_.size(), 0);

        prev_->beginTuple();
        for (; !prev_->is_end(); prev_->nextTuple()) {
            auto rec = prev_->Next();
            if (rec == nullptr) {
                continue;
            }
            for (size_t i = 0; i < items_.size(); ++i) {
                auto &item = items_[i];
                auto &out_col = cols_[i];
                if (item.agg_type == AGG_COUNT) {
                    counts[i]++;
                    initialized[i] = true;
                    continue;
                }
                auto src_col = prev_->get_col_offset(item.col);
                const char *src = rec->data + src_col.offset;
                char *dst = result_->data + out_col.offset;
                if (item.agg_type == AGG_SUM) {
                    if (src_col.type == TYPE_INT) {
                        sums[i] += *reinterpret_cast<const int *>(src);
                    } else if (src_col.type == TYPE_FLOAT) {
                        sums[i] += *reinterpret_cast<const float *>(src);
                    } else {
                        throw IncompatibleTypeError("SUM", coltype2str(src_col.type));
                    }
                    initialized[i] = true;
                } else if (!initialized[i]) {
                    memcpy(dst, src, src_col.len);
                    initialized[i] = true;
                } else {
                    int cmp = compare_raw_value(src, dst, src_col.type, src_col.len);
                    if ((item.agg_type == AGG_MAX && cmp > 0) || (item.agg_type == AGG_MIN && cmp < 0)) {
                        memcpy(dst, src, src_col.len);
                    }
                }
            }
        }

        for (size_t i = 0; i < items_.size(); ++i) {
            char *dst = result_->data + cols_[i].offset;
            if (items_[i].agg_type == AGG_COUNT) {
                *reinterpret_cast<int *>(dst) = counts[i];
            } else if (items_[i].agg_type == AGG_SUM) {
                if (cols_[i].type == TYPE_INT) {
                    *reinterpret_cast<int *>(dst) = static_cast<int>(sums[i]);
                } else if (cols_[i].type == TYPE_FLOAT) {
                    *reinterpret_cast<float *>(dst) = static_cast<float>(sums[i]);
                }
            }
        }
        produced_ = true;
    }

    void nextTuple() override { consumed_ = true; }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) {
            return nullptr;
        }
        auto rec = std::make_unique<RmRecord>(len_);
        memcpy(rec->data, result_->data, len_);
        return rec;
    }

    Rid &rid() override { return _abstract_rid; }
    bool is_end() const override { return !produced_ || consumed_; }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
};
