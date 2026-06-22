/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "log_recovery.h"
#include "execution/executor_abstract.h"
#include "index/ix.h"
#include "record/rm_file_handle.h"

void RecoveryManager::load_logs() {
    if (!logs_.empty()) {
        return;
    }
    if (!disk_manager_->is_file(LOG_FILE_NAME)) {
        return;
    }
    int offset = 0;
    char header[LOG_HEADER_SIZE];
    while (true) {
        int read_bytes = disk_manager_->read_log(header, LOG_HEADER_SIZE, offset);
        if (read_bytes <= 0) {
            break;
        }
        if (read_bytes < LOG_HEADER_SIZE) {
            break;
        }
        uint32_t log_len = *reinterpret_cast<uint32_t *>(header + OFFSET_LOG_TOT_LEN);
        if (log_len < LOG_HEADER_SIZE || log_len > LOG_BUFFER_SIZE) {
            break;
        }
        std::vector<char> buf(log_len);
        read_bytes = disk_manager_->read_log(buf.data(), log_len, offset);
        if (read_bytes < static_cast<int>(log_len)) {
            break;
        }
        LogType type = *reinterpret_cast<LogType *>(buf.data() + OFFSET_LOG_TYPE);
        std::shared_ptr<LogRecord> log;
        switch (type) {
            case LogType::begin:
                log = std::make_shared<BeginLogRecord>();
                break;
            case LogType::commit:
                log = std::make_shared<CommitLogRecord>();
                break;
            case LogType::ABORT:
                log = std::make_shared<AbortLogRecord>();
                break;
            case LogType::INSERT:
                log = std::make_shared<InsertLogRecord>();
                break;
            case LogType::DELETE:
                log = std::make_shared<DeleteLogRecord>();
                break;
            case LogType::UPDATE:
                log = std::make_shared<UpdateLogRecord>();
                break;
            default:
                return;
        }
        log->deserialize(buf.data());
        logs_.push_back(log);
        offset += log_len;
    }
}

void RecoveryManager::insert_indexes(const std::string &tab_name, const RmRecord &rec, const Rid &rid) {
    TabMeta &tab = sm_manager_->db_.get_table(tab_name);
    for (auto &index : tab.indexes) {
        auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
        auto key = make_index_key(index, rec.data);
        std::vector<Rid> hits;
        if (!ih->get_value(key.data(), &hits, nullptr)) {
            ih->insert_entry(key.data(), rid, nullptr);
        }
    }
}

void RecoveryManager::delete_indexes(const std::string &tab_name, const RmRecord &rec) {
    TabMeta &tab = sm_manager_->db_.get_table(tab_name);
    for (auto &index : tab.indexes) {
        auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
        auto key = make_index_key(index, rec.data);
        ih->delete_entry(key.data(), nullptr);
    }
}

bool RecoveryManager::record_exists(RmFileHandle *fh, const Rid &rid) {
    try {
        return fh->is_record(rid);
    } catch (RMDBError &) {
        return false;
    }
}

void RecoveryManager::apply_insert(const InsertLogRecord &log) {
    auto fh = sm_manager_->fhs_.at(log.table_name_).get();
    if (record_exists(fh, log.rid_)) {
        auto old = fh->get_record(log.rid_, nullptr);
        delete_indexes(log.table_name_, *old);
        fh->update_record(log.rid_, log.insert_value_.data, nullptr);
    } else {
        fh->insert_record(log.rid_, log.insert_value_.data);
    }
    insert_indexes(log.table_name_, log.insert_value_, log.rid_);
}

void RecoveryManager::apply_delete(const DeleteLogRecord &log) {
    auto fh = sm_manager_->fhs_.at(log.table_name_).get();
    if (record_exists(fh, log.rid_)) {
        auto old = fh->get_record(log.rid_, nullptr);
        delete_indexes(log.table_name_, *old);
        fh->delete_record(log.rid_, nullptr);
    }
}

void RecoveryManager::apply_update(const UpdateLogRecord &log, bool use_new_value) {
    auto fh = sm_manager_->fhs_.at(log.table_name_).get();
    const RmRecord &target = use_new_value ? log.new_value_ : log.old_value_;
    if (record_exists(fh, log.rid_)) {
        auto old = fh->get_record(log.rid_, nullptr);
        delete_indexes(log.table_name_, *old);
        fh->update_record(log.rid_, target.data, nullptr);
    } else {
        fh->insert_record(log.rid_, target.data);
    }
    insert_indexes(log.table_name_, target, log.rid_);
}

void RecoveryManager::flush_recovered_pages() {
    for (auto &entry : sm_manager_->fhs_) {
        buffer_pool_manager_->flush_all_pages(entry.second->GetFd());
    }
    for (auto &entry : sm_manager_->ihs_) {
        buffer_pool_manager_->flush_all_pages(entry.second->GetFd());
    }
}

/**
 * @description: analyze阶段，需要获得脏页表（DPT）和未完成的事务列表（ATT）
 */
void RecoveryManager::analyze() {
    load_logs();
    active_txns_.clear();
    committed_txns_.clear();
    aborted_txns_.clear();
    for (auto &log : logs_) {
        if (log->log_tid_ == INVALID_TXN_ID) {
            continue;
        }
        if (log->log_type_ == LogType::begin) {
            active_txns_.insert(log->log_tid_);
        } else if (log->log_type_ == LogType::commit) {
            committed_txns_.insert(log->log_tid_);
            active_txns_.erase(log->log_tid_);
        } else if (log->log_type_ == LogType::ABORT) {
            aborted_txns_.insert(log->log_tid_);
            active_txns_.erase(log->log_tid_);
        }
    }
}

/**
 * @description: 重做所有未落盘的操作
 */
void RecoveryManager::redo() {
    for (auto &log : logs_) {
        if (!committed_txns_.count(log->log_tid_)) {
            continue;
        }
        if (auto insert_log = std::dynamic_pointer_cast<InsertLogRecord>(log)) {
            apply_insert(*insert_log);
        } else if (auto delete_log = std::dynamic_pointer_cast<DeleteLogRecord>(log)) {
            apply_delete(*delete_log);
        } else if (auto update_log = std::dynamic_pointer_cast<UpdateLogRecord>(log)) {
            apply_update(*update_log, true);
        }
    }
    flush_recovered_pages();
}

/**
 * @description: 回滚未完成的事务
 */
void RecoveryManager::undo() {
    for (auto it = logs_.rbegin(); it != logs_.rend(); ++it) {
        auto &log = *it;
        if (!active_txns_.count(log->log_tid_)) {
            continue;
        }
        if (auto insert_log = std::dynamic_pointer_cast<InsertLogRecord>(log)) {
            auto fh = sm_manager_->fhs_.at(insert_log->table_name_).get();
            if (record_exists(fh, insert_log->rid_)) {
                auto old = fh->get_record(insert_log->rid_, nullptr);
                delete_indexes(insert_log->table_name_, *old);
                fh->delete_record(insert_log->rid_, nullptr);
            }
        } else if (auto delete_log = std::dynamic_pointer_cast<DeleteLogRecord>(log)) {
            apply_insert(InsertLogRecord(log->log_tid_, delete_log->delete_value_, delete_log->rid_, delete_log->table_name_));
        } else if (auto update_log = std::dynamic_pointer_cast<UpdateLogRecord>(log)) {
            apply_update(*update_log, false);
        }
    }
    flush_recovered_pages();
}
