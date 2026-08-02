/*
 * managed_project_registry.c — Thread-safe Vulcan project authority.
 * managed_project_registry.c — 线程安全的 Vulcan 项目权威注册表。
 */

#include "daemon/managed_project_registry.h"

#include "foundation/compat_thread.h"
#include "foundation/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

/* Registry storage guarded by one dedicated mutex.
 * 由一个专用互斥锁保护的注册表存储。 */
struct cbm_managed_project_registry {
    /* Serialize registry commits with watcher and job reconciliation.
     * 将注册表提交与 watcher、任务对账串行化。 */
    cbm_mutex_t reconcile_mutex;
    /* Dedicated registry mutex.
     * 专用注册表互斥锁。 */
    cbm_mutex_t mutex;
    /* Current authoritative project array.
     * 当前权威项目数组。 */
    cbm_managed_project_entry_t *projects;
    /* Number of entries in the authoritative array.
     * 权威数组中的条目数量。 */
    size_t project_count;
    /* Last committed full-sync generation.
     * 最近提交的全量同步 generation。 */
    uint64_t generation;
    /* Process runtime generation used to reject stale job events.
     * 用于拒绝陈旧任务事件的进程运行代次。 */
    uint64_t runtime_id;
    /* Next runtime-unique job identifier.
     * 下一个运行时唯一任务标识。 */
    uint64_t next_job_id;
};

/* Return the operating-system process identifier.
 * 返回操作系统进程标识。 */
static uint64_t managed_current_pid(void) {
#ifdef _WIN32
    return (uint64_t)GetCurrentProcessId();
#else
    return (uint64_t)getpid();
#endif
}

/* Build a process-scoped runtime generation from monotonic time and pid.
 * 使用单调时间与进程标识构造进程级运行代次。 */
static uint64_t managed_runtime_id(void) {
    uint64_t mixed = (cbm_now_ms() + 1U) * 11400714819323198485ULL;
    mixed ^= (managed_current_pid() + 1U) * 14029467366897019727ULL;
    return mixed != 0U ? mixed : 1U;
}

/* Clear bounded diagnostics without changing job identity.
 * 清空有界诊断信息且不改变任务身份。 */
static void managed_clear_diagnostics(char *error_code, size_t error_code_size, char *error_message,
                                      size_t error_message_size) {
    if (error_code && error_code_size > 0U) {
        error_code[0] = '\0';
    }
    if (error_message && error_message_size > 0U) {
        error_message[0] = '\0';
    }
}

/* Copy optional terminal diagnostics into bounded storage.
 * 将可选终态诊断复制到有界存储。 */
static void managed_set_diagnostics(char *error_code, size_t error_code_size, char *error_message,
                                    size_t error_message_size, const char *code,
                                    const char *message) {
    if (error_code && error_code_size > 0U) {
        (void)snprintf(error_code, error_code_size, "%s", code ? code : "index_failed");
    }
    if (error_message && error_message_size > 0U) {
        (void)snprintf(error_message, error_message_size, "%s",
                       message ? message : "Managed index failed");
    }
}

/* Return true when a job state is terminal.
 * 当任务状态为终态时返回真。 */
static bool managed_job_state_is_terminal(cbm_managed_job_state_t state) {
    return state == CBM_MANAGED_JOB_STATE_SUCCEEDED || state == CBM_MANAGED_JOB_STATE_FAILED ||
           state == CBM_MANAGED_JOB_STATE_CANCELLED;
}

/* Copy bounded text and reject truncation.
 * 复制有界文本并拒绝截断。
 *
 * Parameters:
 * - destination: output buffer.
 * - destination_size: output buffer capacity.
 * - source: non-empty source text.
 * 参数：
 * - destination：输出缓冲区。
 * - destination_size：输出缓冲区容量。
 * - source：非空源文本。
 *
 * Returns true only when the complete text was copied.
 * 仅在完整文本已复制时返回 true。
 */
static bool managed_copy_text(char *destination, size_t destination_size, const char *source) {
    if (!destination || destination_size == 0U || !source || !source[0]) {
        return false;
    }
    /* Required encoded length.
     * 所需编码长度。 */
    int written = snprintf(destination, destination_size, "%s", source);
    if (written < 0 || (size_t)written >= destination_size) {
        destination[0] = '\0';
        return false;
    }
    return true;
}

/* Compare normalized authorization paths using platform filesystem semantics.
 * 使用平台文件系统语义比较规范化授权路径。
 *
 * Returns true only when the paths identify the same normalized spelling.
 * 仅当路径标识相同规范化拼写时返回 true。
 */
static bool managed_path_equal(const char *left, const char *right) {
    if (!left || !right) {
        return false;
    }
#ifdef _WIN32
    return _stricmp(left, right) == 0;
#else
    return strcmp(left, right) == 0;
#endif
}

/* Find an entry index by project key while the registry mutex is held.
 * 在持有注册表互斥锁时按项目键查找条目索引。
 *
 * Returns SIZE_MAX when no entry matches.
 * 没有匹配条目时返回 SIZE_MAX。
 */
static size_t managed_find_key_locked(const cbm_managed_project_registry_t *registry,
                                      const char *project_key) {
    if (!registry || !project_key) {
        return SIZE_MAX;
    }
    /* Current scan index.
     * 当前扫描索引。 */
    for (size_t index = 0U; index < registry->project_count; index++) {
        if (strcmp(registry->projects[index].project_key, project_key) == 0) {
            return index;
        }
    }
    return SIZE_MAX;
}

/* Find an entry index by canonical root while the registry mutex is held.
 * 在持有注册表互斥锁时按规范化根路径查找条目索引。
 *
 * Returns SIZE_MAX when no entry matches.
 * 没有匹配条目时返回 SIZE_MAX。
 */
static size_t managed_find_root_locked(const cbm_managed_project_registry_t *registry,
                                       const char *canonical_root) {
    if (!registry || !canonical_root) {
        return SIZE_MAX;
    }
    /* Current scan index.
     * 当前扫描索引。 */
    for (size_t index = 0U; index < registry->project_count; index++) {
        if (managed_path_equal(registry->projects[index].canonical_root, canonical_root)) {
            return index;
        }
    }
    return SIZE_MAX;
}

/* Find an entry index by Vulcan project id while the registry mutex is held.
 * 在持有注册表互斥锁时按 Vulcan 项目标识查找条目索引。
 *
 * Returns SIZE_MAX when no entry matches.
 * 没有匹配条目时返回 SIZE_MAX。
 */
static size_t managed_find_id_locked(const cbm_managed_project_registry_t *registry,
                                     const char *project_id) {
    if (!registry || !project_id) {
        return SIZE_MAX;
    }
    /* Current scan index.
     * 当前扫描索引。 */
    for (size_t index = 0U; index < registry->project_count; index++) {
        if (strcmp(registry->projects[index].project_id, project_id) == 0) {
            return index;
        }
    }
    return SIZE_MAX;
}

/* Append one copied synchronization difference.
 * 追加一个复制的同步差异。
 *
 * Returns false only when the preallocated result capacity is exhausted.
 * 仅当预分配结果容量耗尽时返回 false。
 */
static bool managed_append_change(cbm_managed_sync_result_t *result, size_t capacity,
                                  cbm_managed_project_change_kind_t kind,
                                  const cbm_managed_project_entry_t *project) {
    if (!result || !project || result->change_count >= capacity) {
        return false;
    }
    /* Destination change slot.
     * 目标差异槽位。 */
    cbm_managed_project_change_t *change = &result->changes[result->change_count++];
    change->kind = kind;
    change->project = *project;
    if (kind == CBM_MANAGED_PROJECT_ADDED) {
        result->added++;
    } else if (kind == CBM_MANAGED_PROJECT_RETAINED) {
        result->retained++;
    } else if (kind == CBM_MANAGED_PROJECT_REMOVED) {
        result->removed++;
    }
    return true;
}

cbm_managed_project_registry_t *cbm_managed_project_registry_new(void) {
    /* Newly allocated registry.
     * 新分配的注册表。 */
    cbm_managed_project_registry_t *registry = calloc(1, sizeof(*registry));
    if (!registry) {
        return NULL;
    }
    cbm_mutex_init(&registry->mutex);
    cbm_mutex_init(&registry->reconcile_mutex);
    registry->runtime_id = managed_runtime_id();
    registry->next_job_id = 1U;
    return registry;
}

void cbm_managed_project_registry_free(cbm_managed_project_registry_t *registry) {
    if (!registry) {
        return;
    }
    cbm_mutex_destroy(&registry->reconcile_mutex);
    cbm_mutex_destroy(&registry->mutex);
    free(registry->projects);
    free(registry);
}

void cbm_managed_project_registry_reconcile_begin(cbm_managed_project_registry_t *registry) {
    if (registry) {
        cbm_mutex_lock(&registry->reconcile_mutex);
    }
}

void cbm_managed_project_registry_reconcile_end(cbm_managed_project_registry_t *registry) {
    if (registry) {
        cbm_mutex_unlock(&registry->reconcile_mutex);
    }
}

void cbm_managed_sync_result_free(cbm_managed_sync_result_t *result) {
    if (!result) {
        return;
    }
    free(result->changes);
    memset(result, 0, sizeof(*result));
}

bool cbm_managed_project_registry_sync(cbm_managed_project_registry_t *registry,
                                       uint64_t generation,
                                       const cbm_managed_project_input_t *projects,
                                       size_t project_count, cbm_managed_sync_result_t *result) {
    if (!result) {
        return false;
    }
    memset(result, 0, sizeof(*result));
    result->generation = generation;
    result->status = CBM_MANAGED_SYNC_INVALID_INPUT;
    if (!registry || generation == 0U || (project_count > 0U && !projects)) {
        return false;
    }

    /* Candidate authoritative array built before taking the registry lock.
     * 在获取注册表锁前构建的候选权威数组。 */
    cbm_managed_project_entry_t *next_projects =
        project_count ? calloc(project_count, sizeof(*next_projects)) : NULL;
    if (project_count > 0U && !next_projects) {
        result->status = CBM_MANAGED_SYNC_NO_MEMORY;
        return false;
    }

    /* Validate bounded fields and all intra-request uniqueness first.
     * 首先验证有界字段及请求内全部唯一性。 */
    for (size_t index = 0U; index < project_count; index++) {
        /* Current validated input.
         * 当前已验证输入。 */
        const cbm_managed_project_input_t *input = &projects[index];
        /* Current candidate entry.
         * 当前候选条目。 */
        cbm_managed_project_entry_t *entry = &next_projects[index];
        if (!managed_copy_text(entry->project_id, sizeof(entry->project_id), input->project_id) ||
            !managed_copy_text(entry->canonical_root, sizeof(entry->canonical_root),
                               input->canonical_root) ||
            !managed_copy_text(entry->project_key, sizeof(entry->project_key),
                               input->project_key)) {
            free(next_projects);
            return false;
        }
        entry->availability = input->root_available ? CBM_MANAGED_AVAILABILITY_UNAVAILABLE
                                                    : CBM_MANAGED_AVAILABILITY_OFFLINE;
        entry->next_job_generation = 1U;
        for (size_t prior = 0U; prior < index; prior++) {
            if (strcmp(next_projects[prior].project_id, entry->project_id) == 0 ||
                managed_path_equal(next_projects[prior].canonical_root, entry->canonical_root) ||
                strcmp(next_projects[prior].project_key, entry->project_key) == 0) {
                result->status = CBM_MANAGED_SYNC_CONFLICT;
                free(next_projects);
                return false;
            }
        }
    }

    cbm_mutex_lock(&registry->mutex);
    if (generation <= registry->generation) {
        cbm_mutex_unlock(&registry->mutex);
        result->status = CBM_MANAGED_SYNC_STALE_GENERATION;
        free(next_projects);
        return false;
    }

    /* Reject single-generation identity rebinding. A controller must first
     * remove the old authority and only add the relocated identity in a later
     * generation, preventing old jobs from targeting a newly rebound key.
     * 拒绝单个 generation 内的身份重绑。控制器必须先移除旧权威，再在后续
     * generation 新增迁移后的身份，避免旧任务作用于刚重绑的项目键。 */
    for (size_t index = 0U; index < project_count; index++) {
        /* Candidate next entry.
         * 候选下一代条目。 */
        const cbm_managed_project_entry_t *next = &next_projects[index];
        /* Existing entries sharing either stable identity.
         * 共享任一稳定身份的现有条目。 */
        size_t id_index = managed_find_id_locked(registry, next->project_id);
        size_t key_index = managed_find_key_locked(registry, next->project_key);
        if ((id_index != SIZE_MAX &&
             !managed_path_equal(registry->projects[id_index].canonical_root,
                                 next->canonical_root)) ||
            (key_index != SIZE_MAX &&
             !managed_path_equal(registry->projects[key_index].canonical_root,
                                 next->canonical_root))) {
            cbm_mutex_unlock(&registry->mutex);
            result->status = CBM_MANAGED_SYNC_CONFLICT;
            free(next_projects);
            return false;
        }
    }

    /* Maximum added/retained/removed difference count.
     * 新增、保留和移除差异的最大数量。 */
    size_t change_capacity = registry->project_count + project_count;
    result->changes = change_capacity ? calloc(change_capacity, sizeof(*result->changes)) : NULL;
    if (change_capacity > 0U && !result->changes) {
        cbm_mutex_unlock(&registry->mutex);
        result->status = CBM_MANAGED_SYNC_NO_MEMORY;
        free(next_projects);
        return false;
    }

    /* Preserve runtime state only for an exact id/root/key retained mapping.
     * 仅为精确 id/root/key 保留映射保存运行时状态。 */
    for (size_t index = 0U; index < project_count; index++) {
        /* Candidate next entry.
         * 候选下一代条目。 */
        cbm_managed_project_entry_t *next = &next_projects[index];
        /* Existing entry index for the same canonical root.
         * 相同规范化根路径的现有条目索引。 */
        size_t existing_index = managed_find_root_locked(registry, next->canonical_root);
        if (existing_index != SIZE_MAX) {
            /* Existing retained entry.
             * 现有保留条目。 */
            const cbm_managed_project_entry_t *existing = &registry->projects[existing_index];
            if (strcmp(existing->project_id, next->project_id) != 0 ||
                strcmp(existing->project_key, next->project_key) != 0) {
                cbm_mutex_unlock(&registry->mutex);
                result->status = CBM_MANAGED_SYNC_CONFLICT;
                cbm_managed_sync_result_free(result);
                result->status = CBM_MANAGED_SYNC_CONFLICT;
                result->generation = generation;
                free(next_projects);
                return false;
            }
            /* Availability is authoritative for lifecycle and watcher ownership.
             * 可用性对生命周期和 watcher 所有权具有权威性。 */
            cbm_managed_availability_t requested_availability = next->availability;
            *next = *existing;
            if (requested_availability == CBM_MANAGED_AVAILABILITY_OFFLINE) {
                next->availability = CBM_MANAGED_AVAILABILITY_OFFLINE;
                next->watcher_registered = false;
            } else if (next->availability == CBM_MANAGED_AVAILABILITY_OFFLINE) {
                next->availability = next->index_revision > 0U
                                         ? CBM_MANAGED_AVAILABILITY_READY
                                         : CBM_MANAGED_AVAILABILITY_UNAVAILABLE;
            }
            (void)managed_append_change(result, change_capacity, CBM_MANAGED_PROJECT_RETAINED,
                                        next);
        } else {
            (void)managed_append_change(result, change_capacity, CBM_MANAGED_PROJECT_ADDED, next);
        }
    }

    /* Emit removed entries only after the complete next set is known.
     * 仅在完整下一集合确定后产生移除条目。 */
    for (size_t old_index = 0U; old_index < registry->project_count; old_index++) {
        /* Existing candidate for removal.
         * 现有移除候选条目。 */
        const cbm_managed_project_entry_t *old = &registry->projects[old_index];
        /* Whether the old canonical root remains present.
         * 旧规范化根路径是否仍存在。 */
        bool retained = false;
        for (size_t next_index = 0U; next_index < project_count; next_index++) {
            if (managed_path_equal(old->canonical_root, next_projects[next_index].canonical_root)) {
                retained = true;
                break;
            }
        }
        if (!retained) {
            (void)managed_append_change(result, change_capacity, CBM_MANAGED_PROJECT_REMOVED, old);
        }
    }

    /* Previous authoritative array released only after the atomic swap.
     * 仅在原子交换后释放上一权威数组。 */
    cbm_managed_project_entry_t *previous_projects = registry->projects;
    registry->projects = next_projects;
    registry->project_count = project_count;
    registry->generation = generation;
    cbm_mutex_unlock(&registry->mutex);
    free(previous_projects);
    result->status = CBM_MANAGED_SYNC_OK;
    return true;
}

bool cbm_managed_project_registry_authorize(cbm_managed_project_registry_t *registry,
                                            const char *project_id, const char *canonical_root,
                                            cbm_managed_project_entry_t *project_out) {
    if (!registry || !project_id || !canonical_root || !project_out) {
        return false;
    }
    cbm_mutex_lock(&registry->mutex);
    /* Exact canonical root index.
     * 精确规范化根路径索引。 */
    size_t index = managed_find_root_locked(registry, canonical_root);
    /* Whether both the id and root match.
     * id 与根路径是否同时匹配。 */
    bool authorized = index != SIZE_MAX &&
                      registry->projects[index].availability != CBM_MANAGED_AVAILABILITY_OFFLINE &&
                      strcmp(registry->projects[index].project_id, project_id) == 0;
    if (authorized) {
        *project_out = registry->projects[index];
    }
    cbm_mutex_unlock(&registry->mutex);
    return authorized;
}

bool cbm_managed_project_registry_resolve_offline(cbm_managed_project_registry_t *registry,
                                                  const char *project_id,
                                                  const char *normalized_path,
                                                  cbm_managed_project_entry_t *project_out) {
    if (!registry || !project_id || !normalized_path || !project_out) {
        return false;
    }
    cbm_mutex_lock(&registry->mutex);
    /* Existing id index.
     * 现有 id 索引。 */
    size_t index = managed_find_id_locked(registry, project_id);
    /* Exact normalized spelling match.
     * 精确规范化拼写匹配。 */
    bool matched = index != SIZE_MAX &&
                   managed_path_equal(registry->projects[index].canonical_root, normalized_path);
    if (matched) {
        *project_out = registry->projects[index];
    }
    cbm_mutex_unlock(&registry->mutex);
    return matched;
}

bool cbm_managed_project_registry_get_by_id(cbm_managed_project_registry_t *registry,
                                            const char *project_id,
                                            cbm_managed_project_entry_t *project_out) {
    if (!registry || !project_id || !project_out) {
        return false;
    }
    cbm_mutex_lock(&registry->mutex);
    /* Existing id index.
     * 现有 id 索引。 */
    size_t index = managed_find_id_locked(registry, project_id);
    if (index != SIZE_MAX) {
        *project_out = registry->projects[index];
    }
    cbm_mutex_unlock(&registry->mutex);
    return index != SIZE_MAX;
}

bool cbm_managed_project_registry_get_by_key(cbm_managed_project_registry_t *registry,
                                             const char *project_key,
                                             cbm_managed_project_entry_t *project_out) {
    if (!registry || !project_key || !project_out) {
        return false;
    }
    cbm_mutex_lock(&registry->mutex);
    /* Existing project-key index.
     * 现有项目键索引。 */
    size_t index = managed_find_key_locked(registry, project_key);
    if (index != SIZE_MAX) {
        *project_out = registry->projects[index];
    }
    cbm_mutex_unlock(&registry->mutex);
    return index != SIZE_MAX;
}

bool cbm_managed_project_registry_get_by_root(cbm_managed_project_registry_t *registry,
                                              const char *canonical_root,
                                              cbm_managed_project_entry_t *project_out) {
    if (!registry || !canonical_root || !project_out) {
        return false;
    }
    cbm_mutex_lock(&registry->mutex);
    /* Existing canonical-root index.
     * 现有规范化根路径索引。 */
    size_t index = managed_find_root_locked(registry, canonical_root);
    if (index != SIZE_MAX) {
        *project_out = registry->projects[index];
    }
    cbm_mutex_unlock(&registry->mutex);
    return index != SIZE_MAX;
}

bool cbm_managed_project_registry_contains(cbm_managed_project_registry_t *registry,
                                           const char *project_key, const char *canonical_root) {
    if (!registry || !project_key || !canonical_root) {
        return false;
    }
    cbm_mutex_lock(&registry->mutex);
    /* Existing project-key index.
     * 现有项目键索引。 */
    size_t index = managed_find_key_locked(registry, project_key);
    /* Exact storage authorization match.
     * 精确存储授权匹配。 */
    bool contained = index != SIZE_MAX &&
                     managed_path_equal(registry->projects[index].canonical_root, canonical_root);
    cbm_mutex_unlock(&registry->mutex);
    return contained;
}

bool cbm_managed_project_registry_set_watcher(cbm_managed_project_registry_t *registry,
                                              const char *project_key, bool registered) {
    if (!registry || !project_key) {
        return false;
    }
    cbm_mutex_lock(&registry->mutex);
    /* Existing project-key index.
     * 现有项目键索引。 */
    size_t index = managed_find_key_locked(registry, project_key);
    if (index != SIZE_MAX) {
        cbm_managed_project_entry_t *project = &registry->projects[index];
        project->watcher_registered = registered;
        if (registered && project->index_revision > 0U &&
            strcmp(project->last_error_code, "watch_registration_failed") == 0) {
            project->availability = CBM_MANAGED_AVAILABILITY_READY;
            managed_clear_diagnostics(project->last_error_code, sizeof(project->last_error_code),
                                      project->last_error_message,
                                      sizeof(project->last_error_message));
        }
    }
    cbm_mutex_unlock(&registry->mutex);
    return index != SIZE_MAX;
}

cbm_managed_job_begin_status_t cbm_managed_project_registry_begin_job(
    cbm_managed_project_registry_t *registry, const char *project_key, cbm_managed_job_mode_t mode,
    cbm_managed_job_trigger_t trigger, uint64_t started_at_ms,
    cbm_managed_job_snapshot_t *job_out) {
    if (!registry || !project_key || !job_out ||
        (mode != CBM_MANAGED_JOB_MODE_UPDATE && mode != CBM_MANAGED_JOB_MODE_REBUILD)) {
        return CBM_MANAGED_JOB_BEGIN_NOT_FOUND;
    }
    memset(job_out, 0, sizeof(*job_out));
    cbm_mutex_lock(&registry->mutex);
    size_t index = managed_find_key_locked(registry, project_key);
    if (index == SIZE_MAX) {
        cbm_mutex_unlock(&registry->mutex);
        return CBM_MANAGED_JOB_BEGIN_NOT_FOUND;
    }
    cbm_managed_project_entry_t *project = &registry->projects[index];
    if (project->has_active_job) {
        *job_out = project->active_job;
        if (project->active_job.mode == mode) {
            job_out->coalesced = true;
            cbm_mutex_unlock(&registry->mutex);
            return CBM_MANAGED_JOB_BEGIN_COALESCED;
        }
        cbm_mutex_unlock(&registry->mutex);
        return CBM_MANAGED_JOB_BEGIN_CONFLICT;
    }
    cbm_managed_job_snapshot_t job;
    memset(&job, 0, sizeof(job));
    job.runtime_id = registry->runtime_id;
    job.job_id = registry->next_job_id++;
    if (registry->next_job_id == 0U) {
        registry->next_job_id = 1U;
    }
    job.job_generation = project->next_job_generation++;
    if (project->next_job_generation == 0U) {
        project->next_job_generation = 1U;
    }
    job.mode = mode;
    job.trigger = trigger;
    job.state = CBM_MANAGED_JOB_STATE_QUEUED;
    job.phase = CBM_MANAGED_JOB_PHASE_QUEUED;
    job.started_at_ms = started_at_ms;
    job.updated_at_ms = started_at_ms;
    project->has_active_job = true;
    project->active_job = job;
    managed_clear_diagnostics(project->last_error_code, sizeof(project->last_error_code),
                              project->last_error_message, sizeof(project->last_error_message));
    *job_out = job;
    cbm_mutex_unlock(&registry->mutex);
    return CBM_MANAGED_JOB_BEGIN_STARTED;
}

bool cbm_managed_project_registry_mark_job_running(cbm_managed_project_registry_t *registry,
                                                   const char *project_key, uint64_t runtime_id,
                                                   uint64_t job_id, uint64_t updated_at_ms) {
    if (!registry || !project_key) {
        return false;
    }
    cbm_mutex_lock(&registry->mutex);
    size_t index = managed_find_key_locked(registry, project_key);
    bool matched = index != SIZE_MAX && registry->projects[index].has_active_job &&
                   registry->projects[index].active_job.runtime_id == runtime_id &&
                   registry->projects[index].active_job.job_id == job_id;
    if (matched) {
        cbm_managed_job_snapshot_t *job = &registry->projects[index].active_job;
        job->state = CBM_MANAGED_JOB_STATE_RUNNING;
        job->updated_at_ms = updated_at_ms;
    }
    cbm_mutex_unlock(&registry->mutex);
    return matched;
}

bool cbm_managed_project_registry_update_job_progress(
    cbm_managed_project_registry_t *registry, const char *project_key, uint64_t runtime_id,
    uint64_t job_id, cbm_managed_job_phase_t phase, uint64_t completed_units, uint64_t total_units,
    bool has_total_units, const char *unit, uint64_t updated_at_ms) {
    if (!registry || !project_key || phase < CBM_MANAGED_JOB_PHASE_QUEUED ||
        phase > CBM_MANAGED_JOB_PHASE_FINALIZING) {
        return false;
    }
    cbm_mutex_lock(&registry->mutex);
    size_t index = managed_find_key_locked(registry, project_key);
    bool matched = index != SIZE_MAX && registry->projects[index].has_active_job &&
                   registry->projects[index].active_job.runtime_id == runtime_id &&
                   registry->projects[index].active_job.job_id == job_id;
    if (matched) {
        cbm_managed_job_snapshot_t *job = &registry->projects[index].active_job;
        cbm_managed_job_phase_t prior_phase = job->phase;
        if (phase >= prior_phase) {
            job->phase = phase;
            if (phase > prior_phase || completed_units >= job->completed_units) {
                job->completed_units = completed_units;
            }
            job->has_total_units = has_total_units;
            job->total_units = has_total_units ? total_units : 0U;
            if (unit && unit[0]) {
                (void)snprintf(job->unit, sizeof(job->unit), "%s", unit);
            } else {
                job->unit[0] = '\0';
            }
        }
        job->updated_at_ms = updated_at_ms;
    }
    cbm_mutex_unlock(&registry->mutex);
    return matched;
}

bool cbm_managed_project_registry_restore_snapshot(cbm_managed_project_registry_t *registry,
                                                   const char *project_key,
                                                   uint64_t observed_at_ms) {
    if (!registry || !project_key) {
        return false;
    }
    cbm_mutex_lock(&registry->mutex);
    size_t index = managed_find_key_locked(registry, project_key);
    if (index != SIZE_MAX) {
        cbm_managed_project_entry_t *project = &registry->projects[index];
        project->availability = CBM_MANAGED_AVAILABILITY_READY;
        if (project->index_revision == 0U) {
            project->index_revision = 1U;
        }
        if (project->last_success_at_ms == 0U) {
            project->last_success_at_ms = observed_at_ms;
        }
        managed_clear_diagnostics(project->last_error_code, sizeof(project->last_error_code),
                                  project->last_error_message, sizeof(project->last_error_message));
    }
    cbm_mutex_unlock(&registry->mutex);
    return index != SIZE_MAX;
}

bool cbm_managed_project_registry_publish_job(cbm_managed_project_registry_t *registry,
                                              const char *project_key, uint64_t runtime_id,
                                              uint64_t job_id,
                                              cbm_managed_job_state_t terminal_state,
                                              uint64_t completed_at_ms, const char *error_code,
                                              const char *error_message) {
    if (!registry || !project_key || !managed_job_state_is_terminal(terminal_state)) {
        return false;
    }
    cbm_mutex_lock(&registry->mutex);
    size_t index = managed_find_key_locked(registry, project_key);
    bool matched = index != SIZE_MAX && registry->projects[index].has_active_job &&
                   registry->projects[index].active_job.runtime_id == runtime_id &&
                   registry->projects[index].active_job.job_id == job_id;
    if (matched) {
        cbm_managed_project_entry_t *project = &registry->projects[index];
        cbm_managed_job_snapshot_t job = project->active_job;
        job.state = terminal_state;
        job.phase = CBM_MANAGED_JOB_PHASE_FINALIZING;
        job.updated_at_ms = completed_at_ms;
        job.completed_at_ms = completed_at_ms;
        if (terminal_state == CBM_MANAGED_JOB_STATE_SUCCEEDED) {
            project->availability = CBM_MANAGED_AVAILABILITY_READY;
            project->index_revision++;
            project->last_success_at_ms = completed_at_ms;
            managed_clear_diagnostics(job.error_code, sizeof(job.error_code), job.error_message,
                                      sizeof(job.error_message));
            managed_clear_diagnostics(project->last_error_code, sizeof(project->last_error_code),
                                      project->last_error_message,
                                      sizeof(project->last_error_message));
        } else {
            project->availability = project->index_revision > 0U
                                        ? CBM_MANAGED_AVAILABILITY_DEGRADED
                                        : CBM_MANAGED_AVAILABILITY_UNAVAILABLE;
            managed_set_diagnostics(job.error_code, sizeof(job.error_code), job.error_message,
                                    sizeof(job.error_message), error_code, error_message);
            managed_set_diagnostics(project->last_error_code, sizeof(project->last_error_code),
                                    project->last_error_message,
                                    sizeof(project->last_error_message), error_code, error_message);
        }
        project->last_job = job;
        project->has_last_job = true;
        memset(&project->active_job, 0, sizeof(project->active_job));
        project->has_active_job = false;
    }
    cbm_mutex_unlock(&registry->mutex);
    return matched;
}

bool cbm_managed_project_registry_mark_degraded(cbm_managed_project_registry_t *registry,
                                                const char *project_key, const char *error_code,
                                                const char *error_message) {
    if (!registry || !project_key) {
        return false;
    }
    cbm_mutex_lock(&registry->mutex);
    size_t index = managed_find_key_locked(registry, project_key);
    bool marked = index != SIZE_MAX && registry->projects[index].index_revision > 0U;
    if (marked) {
        cbm_managed_project_entry_t *project = &registry->projects[index];
        project->availability = CBM_MANAGED_AVAILABILITY_DEGRADED;
        managed_set_diagnostics(project->last_error_code, sizeof(project->last_error_code),
                                project->last_error_message, sizeof(project->last_error_message),
                                error_code, error_message);
    }
    cbm_mutex_unlock(&registry->mutex);
    return marked;
}

uint64_t cbm_managed_project_registry_runtime_id(cbm_managed_project_registry_t *registry) {
    if (!registry) {
        return 0U;
    }
    cbm_mutex_lock(&registry->mutex);
    uint64_t runtime_id = registry->runtime_id;
    cbm_mutex_unlock(&registry->mutex);
    return runtime_id;
}

bool cbm_managed_project_registry_mark_offline(cbm_managed_project_registry_t *registry,
                                               const char *project_key) {
    if (!registry || !project_key) {
        return false;
    }
    cbm_mutex_lock(&registry->mutex);
    /* Existing project-key index.
     * 现有项目键索引。 */
    size_t index = managed_find_key_locked(registry, project_key);
    if (index != SIZE_MAX) {
        cbm_managed_project_entry_t *project = &registry->projects[index];
        project->availability = CBM_MANAGED_AVAILABILITY_OFFLINE;
        project->watcher_registered = false;
        if (project->has_active_job) {
            cbm_managed_job_snapshot_t job = project->active_job;
            job.state = CBM_MANAGED_JOB_STATE_CANCELLED;
            job.phase = CBM_MANAGED_JOB_PHASE_FINALIZING;
            job.updated_at_ms = cbm_unix_epoch_ms();
            job.completed_at_ms = job.updated_at_ms;
            managed_set_diagnostics(job.error_code, sizeof(job.error_code), job.error_message,
                                    sizeof(job.error_message), "project_offline",
                                    "Managed project became unavailable");
            project->last_job = job;
            project->has_last_job = true;
            project->has_active_job = false;
            memset(&project->active_job, 0, sizeof(project->active_job));
        }
    }
    cbm_mutex_unlock(&registry->mutex);
    return index != SIZE_MAX;
}

uint64_t cbm_managed_project_registry_generation(cbm_managed_project_registry_t *registry) {
    if (!registry) {
        return 0U;
    }
    cbm_mutex_lock(&registry->mutex);
    /* Copied committed generation.
     * 复制的已提交 generation。 */
    uint64_t generation = registry->generation;
    cbm_mutex_unlock(&registry->mutex);
    return generation;
}

size_t cbm_managed_project_registry_count(cbm_managed_project_registry_t *registry) {
    if (!registry) {
        return 0U;
    }
    cbm_mutex_lock(&registry->mutex);
    /* Copied project count.
     * 复制的项目数量。 */
    size_t count = registry->project_count;
    cbm_mutex_unlock(&registry->mutex);
    return count;
}
