// Copyright (c) 2015, Baidu.com, Inc. All Rights Reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tera/master/master_zk_adapter.h"

#include "tera/types.h"
#include "tera/zk/zk_util.h"

DECLARE_string(tera_zk_addr_list);
DECLARE_string(tera_zk_root_path);
DECLARE_int32(tera_zk_timeout);
DECLARE_int64(tera_zk_retry_period);
DECLARE_int32(tera_zk_retry_max_times);

namespace tera {
namespace master {

MasterZkAdapter::MasterZkAdapter(MasterImpl * master_impl,
                                 const std::string& server_addr)
    : m_master_impl(master_impl), m_server_addr(server_addr) {
}

MasterZkAdapter::~MasterZkAdapter() {
}

bool MasterZkAdapter::Init(std::string* root_tablet_addr,
                           std::map<std::string, std::string>* tabletnode_list,
                           bool* safe_mode) {
    MutexLock lock(&m_mutex);

    if (!Setup()) {
        return false;
    }

    if (!LockMasterLock()) {
        Reset();
        return false;
    }
    if (!CreateMasterNode()) {
        UnlockMasterLock();
        Reset();
        return false;
    }

    bool root_tablet_node_exist = false;
    if (!WatchRootTabletNode(&root_tablet_node_exist, root_tablet_addr)) {
        DeleteMasterNode();
        UnlockMasterLock();
        Reset();
        return false;
    }

    if (!WatchSafeModeMark(safe_mode)) {
        DeleteMasterNode();
        UnlockMasterLock();
        Reset();
        return false;
    }

    if (!WatchTabletNodeList(tabletnode_list)) {
        DeleteMasterNode();
        UnlockMasterLock();
        Reset();
        return false;
    }

    return true;
}

bool MasterZkAdapter::Setup() {
    LOG(INFO) << "try init zk...";
    int zk_errno = zk::ZE_OK;
    int32_t retry_count = 0;
    while (!ZooKeeperAdapter::Init(FLAGS_tera_zk_addr_list,
                                   FLAGS_tera_zk_root_path,
                                   FLAGS_tera_zk_timeout,
                                   m_server_addr, &zk_errno)) {
        if (retry_count++ >= FLAGS_tera_zk_retry_max_times) {
            LOG(ERROR) << "fail to init zk: " << zk::ZkErrnoToString(zk_errno);
            return false;
        }
        LOG(ERROR) << "init zk fail: " << zk::ZkErrnoToString(zk_errno)
            << ". retry in " << FLAGS_tera_zk_retry_period << " ms, retry: "
            << retry_count;
        ThisThread::Sleep(FLAGS_tera_zk_retry_period);
        zk_errno = zk::ZE_OK;
    }
    LOG(INFO) << "init zk success";
    return true;
}

void MasterZkAdapter::Reset() {
    Finalize();
}

bool MasterZkAdapter::LockMasterLock() {
    LOG(INFO) << "try lock master-lock...";
    int32_t retry_count = 0;
    int zk_errno = zk::ZE_OK;
    while (!SyncLock(kMasterLockPath, &zk_errno, -1)) {
        if (retry_count++ >= FLAGS_tera_zk_retry_max_times) {
            LOG(ERROR) << "fail to acquire master lock";
            return false;
        }
        LOG(ERROR) << "retry lock master-lock in "
            << FLAGS_tera_zk_retry_period << " ms, retry=" << retry_count;
        ThisThread::Sleep(FLAGS_tera_zk_retry_period);
        zk_errno = zk::ZE_OK;
    }
    LOG(INFO) << "acquire master lock success";
    return true;
}

bool MasterZkAdapter::UnlockMasterLock() {
    LOG(INFO) << "try release master-lock...";
    int32_t retry_count = 0;
    int zk_errno = zk::ZE_OK;
    while (!Unlock(kMasterLockPath, &zk_errno)) {
        if (retry_count++ >= FLAGS_tera_zk_retry_max_times) {
            LOG(ERROR) << "fail to release master-lock";
            return false;
        }
        LOG(ERROR) << "retry unlock master-lock in "
            << FLAGS_tera_zk_retry_period << " ms, retry=" << retry_count;
        ThisThread::Sleep(FLAGS_tera_zk_retry_period);
        zk_errno = zk::ZE_OK;
    }
    LOG(INFO) << "release master-lock success";
    return true;
}

bool MasterZkAdapter::CreateMasterNode() {
    LOG(INFO) << "try create master node...";
    int32_t retry_count = 0;
    int zk_errno = zk::ZE_OK;
    while (!CreateEphemeralNode(kMasterNodePath, m_server_addr, &zk_errno)) {
        if (retry_count++ >= FLAGS_tera_zk_retry_max_times) {
            LOG(ERROR) << "fail to create master node";
            return false;
        }
        LOG(ERROR) << "retry create master node in "
            << FLAGS_tera_zk_retry_period << " ms, retry=" << retry_count;
        ThisThread::Sleep(FLAGS_tera_zk_retry_period);
        zk_errno = zk::ZE_OK;
    }
    LOG(INFO) << "create master node success";
    return true;
}

bool MasterZkAdapter::DeleteMasterNode() {
    LOG(INFO) << "try delete master node...";
    int32_t retry_count = 0;
    int zk_errno = zk::ZE_OK;
    while (!DeleteNode(kMasterNodePath, &zk_errno)
        && zk_errno != zk::ZE_NOT_EXIST) {
        if (retry_count++ >= FLAGS_tera_zk_retry_max_times) {
            LOG(ERROR) << "fail to delete master node";
            return false;
        }
        LOG(ERROR) << "retry delete master node in "
            << FLAGS_tera_zk_retry_period << " ms, retry=" << retry_count;
        ThisThread::Sleep(FLAGS_tera_zk_retry_period);
        zk_errno = zk::ZE_OK;
    }
    LOG(INFO) << "delete master node success";
    return true;
}

bool MasterZkAdapter::KickTabletServer(const std::string& ts_host,
                                       const std::string& ts_zk_id) {
    MutexLock lock(&m_mutex);
    int32_t retry_count = 0;
    int zk_errno = zk::ZE_OK;
    while (!CreatePersistentNode(kKickPath + "/" + ts_zk_id, ts_host, &zk_errno)
        && zk_errno != zk::ZE_EXIST) {
        if (retry_count++ >= FLAGS_tera_zk_retry_max_times) {
            LOG(ERROR) << "fail to kick ts [" << ts_host << "]";
            return false;
        }
        LOG(ERROR) << "retry kick ts in "
            << FLAGS_tera_zk_retry_period << " ms, retry=" << retry_count;
        ThisThread::Sleep(FLAGS_tera_zk_retry_period);
        zk_errno = zk::ZE_OK;
    }
    LOG(INFO) << "kick ts [" << ts_host << "] success";
    return true;
}

bool MasterZkAdapter::MarkSafeMode() {
    MutexLock lock(&m_mutex);
    LOG(INFO) << "try mark safemode...";
    int32_t retry_count = 0;
    int zk_errno = zk::ZE_OK;
    while (!CreatePersistentNode(kSafeModeNodePath, "safemode", &zk_errno)
        && zk_errno != zk::ZE_EXIST) {
        if (retry_count++ >= FLAGS_tera_zk_retry_max_times) {
            LOG(ERROR) << "fail to mark safemode";
            return false;
        }
        LOG(ERROR) << "retry mark safemode in "
            << FLAGS_tera_zk_retry_period << " ms, retry=" << retry_count;
        ThisThread::Sleep(FLAGS_tera_zk_retry_period);
        zk_errno = zk::ZE_OK;
    }
    LOG(INFO) << "mark safemode success";
    return true;
}

bool MasterZkAdapter::UnmarkSafeMode() {
    MutexLock lock(&m_mutex);
    LOG(INFO) << "try unmark safemode...";
    int zk_errno = zk::ZE_OK;
    int32_t retry_count = 0;
    while (!DeleteNode(kSafeModeNodePath, &zk_errno)
        && zk_errno != zk::ZE_NOT_EXIST) {
        if (retry_count++ >= FLAGS_tera_zk_retry_max_times) {
            LOG(ERROR) << "fail to unmark safemode";
            return false;
        }
        LOG(ERROR) << "retry unmark safemode in "
            << FLAGS_tera_zk_retry_period << " ms, retry=" << retry_count;
        ThisThread::Sleep(FLAGS_tera_zk_retry_period);
        zk_errno = zk::ZE_OK;
    }
    LOG(INFO) << "unmark safemode success";
    return true;
}

bool MasterZkAdapter::UpdateRootTabletNode(const std::string& root_tablet_addr) {
    MutexLock lock(&m_mutex);
    LOG(INFO) << "try update root node...";
    int32_t retry_count = 0;
    int zk_errno = zk::ZE_OK;
    while (!WriteNode(kRootTabletNodePath, root_tablet_addr, &zk_errno)
        && zk_errno != zk::ZE_NOT_EXIST) {
        if (retry_count++ >= FLAGS_tera_zk_retry_max_times) {
            LOG(INFO) << "fail to update root node";
            return false;
        }
        LOG(ERROR) << "retry update root node in "
            << FLAGS_tera_zk_retry_period << " ms, retry=" << retry_count;
        ThisThread::Sleep(FLAGS_tera_zk_retry_period);
        zk_errno = zk::ZE_OK;
    }
    if (zk_errno == zk::ZE_OK) {
        LOG(INFO) << "update root node success";
        return true;
    }

    LOG(INFO) << "root node not exist, try create root node...";
    retry_count = 0;
    zk_errno = zk::ZE_OK;
    while (!CreatePersistentNode(kRootTabletNodePath, root_tablet_addr,
                                 &zk_errno)) {
        if (retry_count++ >= FLAGS_tera_zk_retry_max_times) {
            LOG(ERROR) << "fail to create root node";
            return false;
        }
        LOG(ERROR) << "retry create root node in "
            << FLAGS_tera_zk_retry_period << " ms, retry=" << retry_count;
        ThisThread::Sleep(FLAGS_tera_zk_retry_period);
        zk_errno = zk::ZE_OK;
    }
    LOG(INFO) << "create root node success";
    return true;
}

bool MasterZkAdapter::WatchRootTabletNode(bool* is_exist,
                                          std::string* root_tablet_addr) {
    LOG(INFO) << "try check root node exist...";
    int32_t retry_count = 0;
    int zk_errno = zk::ZE_OK;
    while (!CheckAndWatchExist(kRootTabletNodePath, is_exist, &zk_errno)) {
        if (retry_count++ >= FLAGS_tera_zk_retry_max_times) {
            LOG(ERROR) << "fail to check root node exist";
            return false;
        }
        LOG(ERROR) << "retry check root node exist in "
            << FLAGS_tera_zk_retry_period << " ms, retry=" << retry_count;
        ThisThread::Sleep(FLAGS_tera_zk_retry_period);
        zk_errno = zk::ZE_OK;
    }
    if (!*is_exist) {
        LOG(INFO) << "root node not exist";
        return true;
    }

    LOG(INFO) << "root node exist, try read root node...";
    retry_count = 0;
    zk_errno = zk::ZE_OK;
    while (!ReadAndWatchNode(kRootTabletNodePath, root_tablet_addr, &zk_errno)
        && zk_errno != zk::ZE_NOT_EXIST) {
        if (retry_count++ >= FLAGS_tera_zk_retry_max_times) {
            LOG(ERROR) << "fail to read root node";
            return false;
        }
        LOG(ERROR) << "retry read root node in "
            << FLAGS_tera_zk_retry_period << " ms, retry=" << retry_count;
        ThisThread::Sleep(FLAGS_tera_zk_retry_period);
        zk_errno = zk::ZE_OK;
    }
    if (zk_errno == zk::ZE_NOT_EXIST) {
        *is_exist = false;
        LOG(INFO) << "root node not exist";
        return true;
    }
    LOG(INFO) << "root node value=[" << *root_tablet_addr << "]";
    return true;
}

bool MasterZkAdapter::WatchSafeModeMark(bool* is_safemode) {
    LOG(INFO) << "try watch safemode mark...";
    int32_t retry_count = 0;
    int zk_errno = zk::ZE_OK;
    while (!CheckAndWatchExist(kSafeModeNodePath, is_safemode, &zk_errno)) {
        if (retry_count++ >= FLAGS_tera_zk_retry_max_times) {
            LOG(ERROR) << "fail to watch safemode mark";
            return false;
        }
        LOG(ERROR) << "retry watch safe mode mark in "
            << FLAGS_tera_zk_retry_period << " ms, retry=" << retry_count;
        ThisThread::Sleep(FLAGS_tera_zk_retry_period);
        zk_errno = zk::ZE_OK;
    }
    LOG(INFO) << "watch safemode success";
    return true;
}

bool MasterZkAdapter::WatchTabletNodeList(std::map<std::string, std::string>* tabletnode_list) {
    LOG(INFO) << "try watch tabletnode list...";
    std::vector<std::string> name_list;
    std::vector<std::string> data_list;
    int32_t retry_count = 0;
    int zk_errno = zk::ZE_OK;
    while (!ListAndWatchChildren(kTsListPath, &name_list, &data_list,
                                 &zk_errno)) {
        if (retry_count++ >= FLAGS_tera_zk_retry_max_times) {
            LOG(ERROR) << "fail to watch tabletnode list";
            return false;
        }
        LOG(ERROR) << "retry watch tabletnode list in "
            << FLAGS_tera_zk_retry_period << " ms, retry=" << retry_count;
        ThisThread::Sleep(FLAGS_tera_zk_retry_period);
        zk_errno = zk::ZE_OK;
    }
    size_t list_count = name_list.size();
    for (size_t i = 0; i < list_count; i++) {
        const std::string& name = name_list[i];
        const std::string& data = data_list[i];
        int seq_num = zk::ZooKeeperUtil::GetSequenceNo(name);
        if (seq_num < 0) {
            LOG(ERROR) << "ignore non-sequential node";
            continue;
        }
        if (data == "") {
            LOG(ERROR) << "cannot get value of child : " << name;
            continue;
        }
        // keep larger(newer) sequence id
        std::map<std::string, std::string>::iterator it = tabletnode_list->find(data);
        if (it != tabletnode_list->end()) {
            int prev_seq_num = zk::ZooKeeperUtil::GetSequenceNo(it->second);
            if (prev_seq_num > seq_num) {
                VLOG(5) << "ignore old node: " << data << " " << name;
                continue;
            }
        }
        // TODO: check value
        (*tabletnode_list)[data] = name;
    }
    LOG(INFO) << "watch tabletnode list success";
    return true;
}

void MasterZkAdapter::OnSafeModeMarkCreated() {
    LOG(ERROR) << "safemode mark node is created";
}

void MasterZkAdapter::OnSafeModeMarkDeleted() {
    LOG(ERROR) << "safemode mark node is deleted";
}

void MasterZkAdapter::OnMasterLockLost() {
    LOG(ERROR) << "master lock lost";
    m_master_impl->SetMasterStatus(MasterImpl::kIsSecondary);
    m_master_impl->DisableQueryTabletNodeTimer();
    DeleteMasterNode();
    Reset();
}

void MasterZkAdapter::OnTabletNodeListDeleted() {
    LOG(ERROR) << "ts dir node is deleted";
    if (!MarkSafeMode()) {
        m_master_impl->SetMasterStatus(MasterImpl::kIsSecondary);
        m_master_impl->DisableQueryTabletNodeTimer();
        DeleteMasterNode();
        UnlockMasterLock();
        Reset();
    }
}

void MasterZkAdapter::OnRootTabletNodeDeleted() {
    LOG(ERROR) << "root tablet node is deleted";
    std::string root_tablet_addr;
    if (m_master_impl->GetMetaTabletAddr(&root_tablet_addr)) {
        if (!UpdateRootTabletNode(root_tablet_addr)) {
            m_master_impl->SetMasterStatus(MasterImpl::kIsSecondary);
            m_master_impl->DisableQueryTabletNodeTimer();
            DeleteMasterNode();
            UnlockMasterLock();
            Reset();
        }
    } else {
        LOG(ERROR) << "root tablet not loaded, will not update zk";
    }
}

void MasterZkAdapter::OnMasterNodeDeleted() {
    LOG(ERROR) << "master node deleted";
    m_master_impl->SetMasterStatus(MasterImpl::kIsSecondary);
    m_master_impl->DisableQueryTabletNodeTimer();
    UnlockMasterLock();
    Reset();
}

void MasterZkAdapter::OnTabletServerKickMarkCreated() {
}

void MasterZkAdapter::OnTabletServerKickMarkDeleted() {
}

void MasterZkAdapter::OnTabletServerStart(const std::string& ts_host) {
}

void MasterZkAdapter::OnTabletServerExist(const std::string& ts_host) {
}

void MasterZkAdapter::OnChildrenChanged(const std::string& path,
                                        const std::vector<std::string>& name_list,
                                        const std::vector<std::string>& data_list) {
    VLOG(5) << "OnChilerenChanged: path=[" << path << "]";
    if (path.compare(kTsListPath) != 0) {
        return;
    }
    std::map<std::string, std::string> ts_node_list;

    m_mutex.Lock();
    size_t list_count = name_list.size();
    for (size_t i = 0; i < list_count; i++) {
        const std::string& name = name_list[i];
        const std::string& data = data_list[i];
        int seq_num = zk::ZooKeeperUtil::GetSequenceNo(name);
        if (seq_num < 0) {
            LOG(ERROR) << "ignore non-sequential node";
            continue;
        }
        if (data == "") {
            LOG(ERROR) << "cannot get value of child : " << name;
            continue;
        }
        // keep larger(newer) sequence id
        std::map<std::string, std::string>::iterator it = ts_node_list.find(data);
        if (it != ts_node_list.end()) {
            int prev_seq_num = zk::ZooKeeperUtil::GetSequenceNo(it->second);
            if (prev_seq_num > seq_num) {
                VLOG(5) << "ignore old node: " << data << " " << name;
                continue;
            }
        }
        // TODO: check value
        ts_node_list[data] = name;
    }
    m_mutex.Unlock();
    m_master_impl->RefreshTabletNodeList(ts_node_list);
}

void MasterZkAdapter::OnNodeValueChanged(const std::string& path,
                                         const std::string& value) {
    VLOG(5) << "OnNodeValueChanged: path=[" << path << "], value=["
        << value << "]";
    MutexLock lock(&m_mutex);
}

void MasterZkAdapter::OnNodeCreated(const std::string& path) {
    VLOG(5) << "OnNodeCreated: path=[" << path << "]";
    MutexLock lock(&m_mutex);
}

void MasterZkAdapter::OnNodeDeleted(const std::string& path) {
    VLOG(5) << "OnNodeDeleted: path=[" << path << "]";

    MutexLock lock(&m_mutex);
    if (path.compare(kSafeModeNodePath) == 0) {
        OnSafeModeMarkDeleted();
    } else if (path.compare(kTsListPath) == 0) {
        OnTabletNodeListDeleted();
    } else if (path.compare(kRootTabletNodePath) == 0) {
        OnRootTabletNodeDeleted();
    } else if (path.compare(kMasterNodePath) == 0) {
        OnMasterNodeDeleted();
    } else {
    }
}

void MasterZkAdapter::OnWatchFailed(const std::string& path, int watch_type,
                                    int err) {
    LOG(ERROR) << "OnWatchFailed: path=[" << path << "], watch_type="
        << watch_type << ", err=" << err;
    // MutexLock lock(&m_mutex);
    _Exit(EXIT_FAILURE);
}


void MasterZkAdapter::OnSessionTimeout() {
    LOG(FATAL) << "zk session timeout!";
    _Exit(EXIT_FAILURE);
}

} // namespace master
} // namespace tera

/* vim: set expandtab ts=4 sw=4 sts=4 tw=100: */