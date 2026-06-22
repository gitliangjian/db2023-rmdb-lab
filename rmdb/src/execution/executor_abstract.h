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
#include "common/common.h"
#include "index/ix.h"
#include "system/sm.h"

inline int compare_raw_value(const char *lhs, const char *rhs, ColType type, int len) {
    switch (type) {
        case TYPE_INT: {
            int a = *reinterpret_cast<const int *>(lhs);
            int b = *reinterpret_cast<const int *>(rhs);
            return (a > b) - (a < b);
        }
        case TYPE_FLOAT: {
            float a = *reinterpret_cast<const float *>(lhs);
            float b = *reinterpret_cast<const float *>(rhs);
            return (a > b) - (a < b);
        }
        case TYPE_STRING:
            return memcmp(lhs, rhs, len);
        case TYPE_BIGINT:
        case TYPE_DATETIME: {
            int64_t a = *reinterpret_cast<const int64_t *>(lhs);
            int64_t b = *reinterpret_cast<const int64_t *>(rhs);
            return (a > b) - (a < b);
        }
        default:
            throw InternalError("Unexpected data type");
    }
}

inline bool eval_compare(int cmp, CompOp op) {
    switch (op) {
        case OP_EQ: return cmp == 0;
        case OP_NE: return cmp != 0;
        case OP_LT: return cmp < 0;
        case OP_GT: return cmp > 0;
        case OP_LE: return cmp <= 0;
        case OP_GE: return cmp >= 0;
    }
    return false;
}

inline bool eval_cond_on_record(const std::vector<ColMeta> &cols, const RmRecord *rec, const Condition &cond) {
    auto lhs = std::find_if(cols.begin(), cols.end(), [&](const ColMeta &col) {
        return col.tab_name == cond.lhs_col.tab_name && col.name == cond.lhs_col.col_name;
    });
    if (lhs == cols.end()) {
        throw ColumnNotFoundError(cond.lhs_col.tab_name + "." + cond.lhs_col.col_name);
    }
    const char *rhs_data = nullptr;
    ColType rhs_type = lhs->type;
    int rhs_len = lhs->len;
    if (cond.is_rhs_val) {
        rhs_data = cond.rhs_val.raw->data;
    } else {
        auto rhs = std::find_if(cols.begin(), cols.end(), [&](const ColMeta &col) {
            return col.tab_name == cond.rhs_col.tab_name && col.name == cond.rhs_col.col_name;
        });
        if (rhs == cols.end()) {
            throw ColumnNotFoundError(cond.rhs_col.tab_name + "." + cond.rhs_col.col_name);
        }
        rhs_type = rhs->type;
        rhs_len = rhs->len;
        rhs_data = rec->data + rhs->offset;
    }
    if (lhs->type == TYPE_BIGINT && rhs_type == TYPE_INT) {
        int64_t tmp = *reinterpret_cast<const int *>(rhs_data);
        return eval_compare(compare_raw_value(rec->data + lhs->offset, reinterpret_cast<const char *>(&tmp), TYPE_BIGINT, sizeof(int64_t)), cond.op);
    }
    if (lhs->type != rhs_type || lhs->len != rhs_len) {
        throw IncompatibleTypeError(coltype2str(lhs->type), coltype2str(rhs_type));
    }
    int cmp = compare_raw_value(rec->data + lhs->offset, rhs_data, lhs->type, lhs->len);
    return eval_compare(cmp, cond.op);
}

inline bool eval_conds_on_record(const std::vector<ColMeta> &cols, const RmRecord *rec, const std::vector<Condition> &conds) {
    for (auto &cond : conds) {
        if (!eval_cond_on_record(cols, rec, cond)) {
            return false;
        }
    }
    return true;
}

inline std::vector<char> make_index_key(const IndexMeta &index, const char *record_data) {
    std::vector<char> key(index.col_tot_len);
    int offset = 0;
    for (int i = 0; i < index.col_num; ++i) {
        memcpy(key.data() + offset, record_data + index.cols[i].offset, index.cols[i].len);
        offset += index.cols[i].len;
    }
    return key;
}

class AbstractExecutor {
   public:
    Rid _abstract_rid;

    Context *context_;

    virtual ~AbstractExecutor() = default;

    virtual size_t tupleLen() const { return 0; };

    virtual const std::vector<ColMeta> &cols() const {
        std::vector<ColMeta> *_cols = nullptr;
        return *_cols;
    };

    virtual std::string getType() { return "AbstractExecutor"; };

    virtual void beginTuple(){};

    virtual void nextTuple(){};

    virtual bool is_end() const { return true; };

    virtual Rid &rid() = 0;

    virtual std::unique_ptr<RmRecord> Next() = 0;

    virtual ColMeta get_col_offset(const TabCol &target) { return ColMeta();};

    std::vector<ColMeta>::const_iterator get_col(const std::vector<ColMeta> &rec_cols, const TabCol &target) {
        auto pos = std::find_if(rec_cols.begin(), rec_cols.end(), [&](const ColMeta &col) {
            return col.tab_name == target.tab_name && col.name == target.col_name;
        });
        if (pos == rec_cols.end()) {
            throw ColumnNotFoundError(target.tab_name + '.' + target.col_name);
        }
        return pos;
    }
};
