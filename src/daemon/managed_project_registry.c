/*
 * managed_project_registry.c — Thread-safe Vulcan project authority.
 * managed_project_registry.c — 线程安全的 Vulcan 项目权威注册表。
 */

#include "daemon/managed_project_registry.h"

#include "foundation/compat_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
};

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
        entry->lifecycle =
            input->root_available ? CBM_MANAGED_PROJECT_PENDING : CBM_MANAGED_PROJECT_OFFLINE;
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
            cbm_managed_project_lifecycle_t requested_lifecycle = next->lifecycle;
            *next = *existing;
            if (requested_lifecycle == CBM_MANAGED_PROJECT_OFFLINE) {
                next->lifecycle = CBM_MANAGED_PROJECT_OFFLINE;
                next->watcher_registered = false;
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
                      registry->projects[index].lifecycle != CBM_MANAGED_PROJECT_OFFLINE &&
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
        registry->projects[index].watcher_registered = registered;
        if (registered && (registry->projects[index].lifecycle == CBM_MANAGED_PROJECT_PENDING ||
                           registry->projects[index].lifecycle == CBM_MANAGED_PROJECT_OFFLINE ||
                           (registry->projects[index].lifecycle == CBM_MANAGED_PROJECT_FAILED &&
                            strcmp(registry->projects[index].last_error_code,
                                   "watch_registration_failed") == 0))) {
            registry->projects[index].lifecycle = CBM_MANAGED_PROJECT_READY;
            registry->projects[index].last_error_code[0] = '\0';
            registry->projects[index].last_error_message[0] = '\0';
        }
    }
    cbm_mutex_unlock(&registry->mutex);
    return index != SIZE_MAX;
}

bool cbm_managed_project_registry_mark_indexing(cbm_managed_project_registry_t *registry,
                                                const char *project_key) {
    if (!registry || !project_key) {
        return false;
    }
    cbm_mutex_lock(&registry->mutex);
    /* Existing project-key index.
     * 现有项目键索引。 */
    size_t index = managed_find_key_locked(registry, project_key);
    if (index != SIZE_MAX) {
        registry->projects[index].lifecycle = CBM_MANAGED_PROJECT_INDEXING;
        registry->projects[index].last_error_code[0] = '\0';
        registry->projects[index].last_error_message[0] = '\0';
    }
    cbm_mutex_unlock(&registry->mutex);
    return index != SIZE_MAX;
}

bool cbm_managed_project_registry_publish_index(cbm_managed_project_registry_t *registry,
                                                const char *project_key, bool successful,
                                                uint64_t completed_at_ms, const char *error_code,
                                                const char *error_message) {
    if (!registry || !project_key) {
        return false;
    }
    cbm_mutex_lock(&registry->mutex);
    /* Existing project-key index.
     * 现有项目键索引。 */
    size_t index = managed_find_key_locked(registry, project_key);
    if (index != SIZE_MAX) {
        /* Mutable project state.
         * 可变项目状态。 */
        cbm_managed_project_entry_t *project = &registry->projects[index];
        if (successful) {
            project->lifecycle = CBM_MANAGED_PROJECT_READY;
            project->index_revision++;
            project->last_success_at_ms = completed_at_ms;
            project->last_error_code[0] = '\0';
            project->last_error_message[0] = '\0';
        } else {
            project->lifecycle = CBM_MANAGED_PROJECT_FAILED;
            (void)snprintf(project->last_error_code, sizeof(project->last_error_code), "%s",
                           error_code ? error_code : "index_failed");
            (void)snprintf(project->last_error_message, sizeof(project->last_error_message), "%s",
                           error_message ? error_message : "Managed index failed");
        }
    }
    cbm_mutex_unlock(&registry->mutex);
    return index != SIZE_MAX;
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
        registry->projects[index].lifecycle = CBM_MANAGED_PROJECT_OFFLINE;
        registry->projects[index].watcher_registered = false;
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
