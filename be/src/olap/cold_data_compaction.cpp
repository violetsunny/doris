// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "olap/cold_data_compaction.h"

#include "common/compiler_util.h"
#include "olap/compaction.h"
#include "olap/rowset/beta_rowset.h"
#include "olap/rowset/rowset.h"

namespace doris {
using namespace ErrorCode;

ColdDataCompaction::ColdDataCompaction(const TabletSharedPtr& tablet)
        : Compaction(tablet, "ColdDataCompaction:" + std::to_string(tablet->tablet_id())) {}

ColdDataCompaction::~ColdDataCompaction() = default;

Status ColdDataCompaction::prepare_compact() {
    if (UNLIKELY(!_tablet->init_succeeded())) {
        return Status::Error<INVALID_ARGUMENT>();
    }
    return pick_rowsets_to_compact();
}

Status ColdDataCompaction::execute_compact_impl() {
#ifndef __APPLE__
    if (config::enable_base_compaction_idle_sched) {
        Thread::set_idle_sched();
    }
#endif
    SCOPED_ATTACH_TASK(_mem_tracker);
    int64_t permits = get_compaction_permits();
    RETURN_IF_ERROR(do_compaction(permits));
    _state = CompactionState::SUCCESS;
    return Status::OK();
}

Status ColdDataCompaction::pick_rowsets_to_compact() {
    _tablet->traverse_rowsets([this](const auto& rs) {
        if (!rs->is_local()) {
            _input_rowsets.push_back(rs);
        }
    });
    std::sort(_input_rowsets.begin(), _input_rowsets.end(), Rowset::comparator);
    return check_version_continuity(_input_rowsets);
}

Status ColdDataCompaction::modify_rowsets() {
    UniqueId cooldown_meta_id = UniqueId::gen_uid();

    // write remote tablet meta
    std::shared_ptr<io::RemoteFileSystem> fs;
    RETURN_IF_ERROR(get_remote_file_system(_tablet->storage_policy_id(), &fs));
    std::vector<RowsetMetaSharedPtr> to_deletes;
    for (auto& rs : _input_rowsets) {
        to_deletes.emplace_back(rs->rowset_meta());
    }
    RETURN_IF_ERROR(_tablet->write_cooldown_meta(fs, cooldown_meta_id,
                                                 _output_rowset->rowset_meta(), to_deletes));
    {
        std::lock_guard wlock(_tablet->get_header_lock());
        // Merged cooldowned rowsets MUST NOT be managed by version graph, they will be reclaimed by `remove_unused_remote_files`.
        _tablet->delete_rowsets(_input_rowsets, false);
        _tablet->add_rowsets({_output_rowset});
        // TODO(plat1ko): process primary key
        _tablet->tablet_meta()->set_cooldown_meta_id(cooldown_meta_id);
    }

    {
        std::shared_lock rlock(_tablet->get_header_lock());
        _tablet->save_meta();
    }
    return Status::OK();
}

} // namespace doris
