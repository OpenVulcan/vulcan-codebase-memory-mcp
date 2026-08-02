/*
 * managed_project_registry.h — Vulcan-owned multi-project runtime authority.
 * managed_project_registry.h — Vulcan 托管多项目运行时权威注册表。
 */
#ifndef CBM_MANAGED_PROJECT_REGISTRY_H
#define CBM_MANAGED_PROJECT_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Private host contract accepted by the managed service.
 * 托管服务接受的宿主私有协议。 */
#define CBM_VULCAN_MANAGED_CONTRACT "vulcan.codebase-memory/2"

/* Private coordination ABI reported during MCP initialization.
 * MCP 初始化期间返回的私有协调 ABI。 */
#define CBM_VULCAN_COORDINATION_ABI 2U

/* Fixed diagnostic field capacities keep registry snapshots self-contained.
 * 固定诊断字段容量使注册表快照保持自包含。 */
#define CBM_MANAGED_PROJECT_ID_CAP 256U
#define CBM_MANAGED_PROJECT_PATH_CAP 4096U
#define CBM_MANAGED_PROJECT_KEY_CAP 4096U
#define CBM_MANAGED_ERROR_CODE_CAP 64U
#define CBM_MANAGED_ERROR_MESSAGE_CAP 512U
#define CBM_MANAGED_JOB_UNIT_CAP 32U

/* Published index availability independent from background job activity.
 * 与后台任务活动相互独立的已发布索引可用性。 */
typedef enum {
    CBM_MANAGED_AVAILABILITY_UNAVAILABLE = 0,
    CBM_MANAGED_AVAILABILITY_READY,
    CBM_MANAGED_AVAILABILITY_DEGRADED,
    CBM_MANAGED_AVAILABILITY_OFFLINE,
} cbm_managed_availability_t;

/* Managed index operation mode selected by the controller.
 * 控制器选择的托管索引操作模式。 */
typedef enum {
    CBM_MANAGED_JOB_MODE_UPDATE = 1,
    CBM_MANAGED_JOB_MODE_REBUILD,
} cbm_managed_job_mode_t;

/* Source that requested one managed index operation.
 * 请求一次托管索引操作的来源。 */
typedef enum {
    CBM_MANAGED_JOB_TRIGGER_INITIAL = 1,
    CBM_MANAGED_JOB_TRIGGER_WATCHER,
    CBM_MANAGED_JOB_TRIGGER_MANUAL,
    CBM_MANAGED_JOB_TRIGGER_RECOVERY,
} cbm_managed_job_trigger_t;

/* Managed index job state with explicit terminal values.
 * 具有显式终态的托管索引任务状态。 */
typedef enum {
    CBM_MANAGED_JOB_STATE_NONE = 0,
    CBM_MANAGED_JOB_STATE_QUEUED,
    CBM_MANAGED_JOB_STATE_RUNNING,
    CBM_MANAGED_JOB_STATE_SUCCEEDED,
    CBM_MANAGED_JOB_STATE_FAILED,
    CBM_MANAGED_JOB_STATE_CANCELLED,
} cbm_managed_job_state_t;

/* Stable user-facing indexing phase.
 * 稳定的用户可见索引阶段。 */
typedef enum {
    CBM_MANAGED_JOB_PHASE_QUEUED = 1,
    CBM_MANAGED_JOB_PHASE_DISCOVERING,
    CBM_MANAGED_JOB_PHASE_EXTRACTING,
    CBM_MANAGED_JOB_PHASE_RESOLVING,
    CBM_MANAGED_JOB_PHASE_PERSISTING,
    CBM_MANAGED_JOB_PHASE_PUBLISHING,
    CBM_MANAGED_JOB_PHASE_FINALIZING,
} cbm_managed_job_phase_t;

/* One copied job snapshot safe across registry mutations.
 * 一个在注册表变更后仍安全的任务快照。 */
typedef struct {
    /* Process runtime generation that owns the job.
     * 拥有该任务的进程运行代次。 */
    uint64_t runtime_id;
    /* Runtime-unique job identifier.
     * 运行时唯一任务标识。 */
    uint64_t job_id;
    /* Monotonic per-project job generation.
     * 项目内单调递增的任务代次。 */
    uint64_t job_generation;
    /* Requested operation mode.
     * 请求的操作模式。 */
    cbm_managed_job_mode_t mode;
    /* Request source.
     * 请求来源。 */
    cbm_managed_job_trigger_t trigger;
    /* Current or terminal state.
     * 当前或终态状态。 */
    cbm_managed_job_state_t state;
    /* Current stable phase.
     * 当前稳定阶段。 */
    cbm_managed_job_phase_t phase;
    /* Completed progress units.
     * 已完成的进度单位数。 */
    uint64_t completed_units;
    /* Total progress units, meaningful only when has_total_units is true.
     * 进度单位总数，仅在 has_total_units 为真时有意义。 */
    uint64_t total_units;
    /* Whether a stable progress denominator is known.
     * 是否已知稳定的进度分母。 */
    bool has_total_units;
    /* Bounded progress unit label such as files.
     * 有界的进度单位标签，例如 files。 */
    char unit[CBM_MANAGED_JOB_UNIT_CAP];
    /* Job timestamps in Unix milliseconds.
     * 任务的 Unix 毫秒时间戳。 */
    uint64_t started_at_ms;
    uint64_t updated_at_ms;
    uint64_t completed_at_ms;
    /* Whether this response reused an existing same-mode job.
     * 本次响应是否复用了现有同模式任务。 */
    bool coalesced;
    /* Stable terminal error diagnostics.
     * 稳定的终态错误诊断。 */
    char error_code[CBM_MANAGED_ERROR_CODE_CAP];
    char error_message[CBM_MANAGED_ERROR_MESSAGE_CAP];
} cbm_managed_job_snapshot_t;

/* Result of beginning one managed job.
 * 开始一次托管任务的结果。 */
typedef enum {
    CBM_MANAGED_JOB_BEGIN_STARTED = 1,
    CBM_MANAGED_JOB_BEGIN_COALESCED,
    CBM_MANAGED_JOB_BEGIN_CONFLICT,
    CBM_MANAGED_JOB_BEGIN_NOT_FOUND,
} cbm_managed_job_begin_status_t;

/* One immutable-by-caller registry snapshot entry.
 * 一个调用方不可变的注册表快照条目。 */
typedef struct {
    /* Vulcan project identifier used only for host mapping and diagnostics.
     * 仅用于宿主映射和诊断的 Vulcan 项目标识。 */
    char project_id[CBM_MANAGED_PROJECT_ID_CAP];
    /* Canonical filesystem root used as the authorization key.
     * 用作授权主键的规范化文件系统根路径。 */
    char canonical_root[CBM_MANAGED_PROJECT_PATH_CAP];
    /* CBM storage project key derived from the canonical root.
     * 从规范化根路径派生的 CBM 存储项目键。 */
    char project_key[CBM_MANAGED_PROJECT_KEY_CAP];
    /* Published index availability.
     * 已发布索引可用性。 */
    cbm_managed_availability_t availability;
    /* True only while the shared physical watcher is registered.
     * 仅当共享物理 watcher 已注册时为真。 */
    bool watcher_registered;
    /* Monotonic successful index revision for this daemon generation.
     * 当前 daemon generation 内单调递增的成功索引修订号。 */
    uint64_t index_revision;
    /* Last successful index timestamp in Unix milliseconds.
     * 最近一次索引成功的 Unix 毫秒时间戳。 */
    uint64_t last_success_at_ms;
    /* Active non-terminal job, if present.
     * 当前活动的非终态任务（若存在）。 */
    bool has_active_job;
    cbm_managed_job_snapshot_t active_job;
    /* Most recent terminal job, if present.
     * 最近一次终态任务（若存在）。 */
    bool has_last_job;
    cbm_managed_job_snapshot_t last_job;
    /* Monotonic job generation retained across authoritative syncs.
     * 在权威同步间保留的单调任务代次。 */
    uint64_t next_job_generation;
    /* Stable machine-readable error code.
     * 稳定的机器可读错误码。 */
    char last_error_code[CBM_MANAGED_ERROR_CODE_CAP];
    /* Bounded human-readable error message.
     * 有界的人类可读错误消息。 */
    char last_error_message[CBM_MANAGED_ERROR_MESSAGE_CAP];
} cbm_managed_project_entry_t;

/* Fully validated input used to construct an authoritative generation.
 * 用于构造权威 generation 的完整已验证输入。 */
typedef struct {
    /* Borrowed Vulcan project identifier.
     * 借用的 Vulcan 项目标识。 */
    const char *project_id;
    /* Borrowed canonical authorization root.
     * 借用的规范化授权根路径。 */
    const char *canonical_root;
    /* Borrowed derived CBM project key.
     * 借用的派生 CBM 项目键。 */
    const char *project_key;
    /* Whether the root is currently accessible.
     * 根路径当前是否可访问。 */
    bool root_available;
} cbm_managed_project_input_t;

/* Difference kind emitted by one authoritative synchronization.
 * 一次权威同步产生的差异类型。 */
typedef enum {
    CBM_MANAGED_PROJECT_ADDED = 1,
    CBM_MANAGED_PROJECT_RETAINED,
    CBM_MANAGED_PROJECT_REMOVED,
} cbm_managed_project_change_kind_t;

/* One copied difference entry safe after registry mutation.
 * 一个在注册表变更后仍安全的复制差异条目。 */
typedef struct {
    /* Difference classification.
     * 差异分类。 */
    cbm_managed_project_change_kind_t kind;
    /* Project state associated with the difference.
     * 与差异关联的项目状态。 */
    cbm_managed_project_entry_t project;
} cbm_managed_project_change_t;

/* Synchronization result status.
 * 同步结果状态。 */
typedef enum {
    CBM_MANAGED_SYNC_OK = 0,
    CBM_MANAGED_SYNC_STALE_GENERATION,
    CBM_MANAGED_SYNC_INVALID_INPUT,
    CBM_MANAGED_SYNC_CONFLICT,
    CBM_MANAGED_SYNC_NO_MEMORY,
} cbm_managed_sync_status_t;

/* Heap-backed synchronization result owned by the caller.
 * 由调用方拥有的堆分配同步结果。 */
typedef struct {
    /* Synchronization status.
     * 同步状态。 */
    cbm_managed_sync_status_t status;
    /* Generation committed or rejected.
     * 已提交或被拒绝的 generation。 */
    uint64_t generation;
    /* Copied difference entries.
     * 复制的差异条目。 */
    cbm_managed_project_change_t *changes;
    /* Number of copied differences.
     * 复制差异的数量。 */
    size_t change_count;
    /* Added project count.
     * 新增项目数量。 */
    size_t added;
    /* Retained project count.
     * 保留项目数量。 */
    size_t retained;
    /* Removed project count.
     * 移除项目数量。 */
    size_t removed;
} cbm_managed_sync_result_t;

/* Opaque thread-safe daemon-level managed project registry.
 * 不透明的线程安全 daemon 级托管项目注册表。 */
typedef struct cbm_managed_project_registry cbm_managed_project_registry_t;

/*
 * Serialize one registry commit together with its physical side effects.
 * 将一次注册表提交及其物理副作用串行化。
 */
void cbm_managed_project_registry_reconcile_begin(cbm_managed_project_registry_t *registry);

/*
 * Release the reconciliation serialization boundary.
 * 释放对账串行化边界。
 */
void cbm_managed_project_registry_reconcile_end(cbm_managed_project_registry_t *registry);

/* Create an empty registry at generation zero.
 * 创建 generation 为零的空注册表。
 *
 * Returns a caller-owned registry or NULL on allocation failure.
 * 返回调用方拥有的注册表；分配失败时返回 NULL。
 */
cbm_managed_project_registry_t *cbm_managed_project_registry_new(void);

/* Destroy a registry after all MCP sessions and jobs have stopped.
 * 在所有 MCP session 和任务停止后销毁注册表。
 *
 * Parameters:
 * - registry: caller-owned registry, NULL is allowed.
 * 参数：
 * - registry：调用方拥有的注册表，允许为 NULL。
 */
void cbm_managed_project_registry_free(cbm_managed_project_registry_t *registry);

/* Atomically replace the authoritative project set with a newer generation.
 * 使用更新的 generation 原子替换权威项目集合。
 *
 * Parameters:
 * - registry: daemon-owned registry.
 * - generation: strictly increasing host generation.
 * - projects: fully validated canonical project inputs.
 * - project_count: number of project inputs.
 * - result: caller-owned result populated on every recognized failure.
 * 参数：
 * - registry：daemon 拥有的注册表。
 * - generation：严格递增的宿主 generation。
 * - projects：完整验证后的规范化项目输入。
 * - project_count：项目输入数量。
 * - result：调用方拥有的结果，对所有可识别失败均会填充。
 *
 * Returns true only when the new generation was committed.
 * 仅在新 generation 已提交时返回 true。
 */
bool cbm_managed_project_registry_sync(cbm_managed_project_registry_t *registry,
                                       uint64_t generation,
                                       const cbm_managed_project_input_t *projects,
                                       size_t project_count, cbm_managed_sync_result_t *result);

/* Release heap storage owned by a synchronization result.
 * 释放同步结果拥有的堆存储。
 *
 * Parameters:
 * - result: result to reset.
 * 参数：
 * - result：需要重置的结果。
 */
void cbm_managed_sync_result_free(cbm_managed_sync_result_t *result);

/* Authorize an existing, accessible canonical root and matching project id.
 * 授权已存在、可访问且项目标识匹配的规范化根路径。
 *
 * Parameters:
 * - registry: daemon-owned registry.
 * - project_id: Vulcan project id.
 * - canonical_root: canonical existing filesystem root.
 * - project_out: copied authorized entry.
 * 参数：
 * - registry：daemon 拥有的注册表。
 * - project_id：Vulcan 项目标识。
 * - canonical_root：已存在文件系统的规范化根路径。
 * - project_out：复制出的已授权条目。
 *
 * Returns true only for an exact registered id/root pair.
 * 仅对精确注册的 id/root 组合返回 true。
 */
bool cbm_managed_project_registry_authorize(cbm_managed_project_registry_t *registry,
                                            const char *project_id, const char *canonical_root,
                                            cbm_managed_project_entry_t *project_out);

/* Resolve the stored canonical root for a temporarily unavailable repeated input.
 * 为暂时不可访问的重复输入解析已存储规范化根路径。
 *
 * Parameters:
 * - registry: daemon-owned registry.
 * - project_id: Vulcan project id.
 * - normalized_path: absolute separator-normalized input path.
 * - project_out: copied matching entry.
 * 参数：
 * - registry：daemon 拥有的注册表。
 * - project_id：Vulcan 项目标识。
 * - normalized_path：绝对且分隔符已规范化的输入路径。
 * - project_out：复制出的匹配条目。
 *
 * Returns true only when both id and normalized path match an existing entry.
 * 仅在 id 与规范化路径均匹配现有条目时返回 true。
 */
bool cbm_managed_project_registry_resolve_offline(cbm_managed_project_registry_t *registry,
                                                  const char *project_id,
                                                  const char *normalized_path,
                                                  cbm_managed_project_entry_t *project_out);

/* Read a registered project by its Vulcan project id.
 * 按 Vulcan 项目标识读取已注册项目。
 *
 * Returns true and copies the entry when found.
 * 找到时返回 true 并复制条目。
 */
bool cbm_managed_project_registry_get_by_id(cbm_managed_project_registry_t *registry,
                                            const char *project_id,
                                            cbm_managed_project_entry_t *project_out);

/* Read a registered project by its derived CBM storage key.
 * 按派生的 CBM 存储键读取已注册项目。
 *
 * Returns true and copies the entry when found.
 * 找到时返回 true 并复制条目。
 */
bool cbm_managed_project_registry_get_by_key(cbm_managed_project_registry_t *registry,
                                             const char *project_key,
                                             cbm_managed_project_entry_t *project_out);

/*
 * Read a registered project by canonical authorization root.
 * 按规范化授权根路径读取已注册项目。
 */
bool cbm_managed_project_registry_get_by_root(cbm_managed_project_registry_t *registry,
                                              const char *canonical_root,
                                              cbm_managed_project_entry_t *project_out);

/* Confirm that a storage key still belongs to the expected canonical root.
 * 确认存储键仍属于预期规范化根路径。
 *
 * Returns true only for the exact registered pair.
 * 仅对精确注册组合返回 true。
 */
bool cbm_managed_project_registry_contains(cbm_managed_project_registry_t *registry,
                                           const char *project_key, const char *canonical_root);

/* Update watcher ownership for an existing project.
 * 更新现有项目的 watcher 所有权。
 *
 * Returns false when the project was removed concurrently.
 * 项目被并发移除时返回 false。
 */
bool cbm_managed_project_registry_set_watcher(cbm_managed_project_registry_t *registry,
                                              const char *project_key, bool registered);

/* Mark an existing project as indexing.
 * 将现有项目标记为正在索引。
 *
 * Returns false when the project was removed concurrently.
 * 项目被并发移除时返回 false。
 */
cbm_managed_job_begin_status_t cbm_managed_project_registry_begin_job(
    cbm_managed_project_registry_t *registry, const char *project_key, cbm_managed_job_mode_t mode,
    cbm_managed_job_trigger_t trigger, uint64_t started_at_ms, cbm_managed_job_snapshot_t *job_out);

/* Mark the exact active job as running.
 * 将精确匹配的活动任务标记为运行中。 */
bool cbm_managed_project_registry_mark_job_running(cbm_managed_project_registry_t *registry,
                                                   const char *project_key, uint64_t runtime_id,
                                                   uint64_t job_id, uint64_t updated_at_ms);

/* Advance progress for the exact active job without allowing phase regression.
 * 推进精确活动任务的进度，同时禁止阶段倒退。 */
bool cbm_managed_project_registry_update_job_progress(
    cbm_managed_project_registry_t *registry, const char *project_key, uint64_t runtime_id,
    uint64_t job_id, cbm_managed_job_phase_t phase, uint64_t completed_units, uint64_t total_units,
    bool has_total_units, const char *unit, uint64_t updated_at_ms);

/* Restore an already-published database discovered during project sync.
 * 恢复项目同步期间发现的已发布数据库事实。 */
bool cbm_managed_project_registry_restore_snapshot(cbm_managed_project_registry_t *registry,
                                                   const char *project_key,
                                                   uint64_t observed_at_ms);

/* Publish a managed index terminal result and advance revision on success.
 * 发布托管索引终态，并在成功时推进修订号。
 *
 * Parameters:
 * - registry: daemon-owned registry.
 * - project_key: registered CBM storage key.
 * - successful: whether the physical index committed successfully.
 * - completed_at_ms: Unix completion timestamp.
 * - error_code: stable failure code, ignored on success.
 * - error_message: bounded failure message, ignored on success.
 * 参数：
 * - registry：daemon 拥有的注册表。
 * - project_key：已注册的 CBM 存储键。
 * - successful：物理索引是否成功提交。
 * - completed_at_ms：完成时的 Unix 毫秒时间戳。
 * - error_code：稳定失败码，成功时忽略。
 * - error_message：有界失败消息，成功时忽略。
 *
 * Returns false when the project was removed concurrently.
 * 项目被并发移除时返回 false。
 */
bool cbm_managed_project_registry_publish_job(cbm_managed_project_registry_t *registry,
                                              const char *project_key, uint64_t runtime_id,
                                              uint64_t job_id,
                                              cbm_managed_job_state_t terminal_state,
                                              uint64_t completed_at_ms, const char *error_code,
                                              const char *error_message);

/* Mark a published project degraded because a post-publication service failed.
 * 因发布后的服务失败而将已发布项目标记为降级。
 *
 * Parameters:
 * - registry: daemon-owned registry.
 * - project_key: exact registered storage key.
 * - error_code: stable bounded diagnostic code.
 * - error_message: bounded human-readable diagnostic message.
 * Returns false when the project is absent or has no published revision.
 *
 * 参数：
 * - registry：daemon 拥有的注册表。
 * - project_key：精确注册的存储键。
 * - error_code：稳定且有界的诊断代码。
 * - error_message：有界的人类可读诊断消息。
 * 项目不存在或尚无已发布修订时返回 false。 */
bool cbm_managed_project_registry_mark_degraded(cbm_managed_project_registry_t *registry,
                                                const char *project_key, const char *error_code,
                                                const char *error_message);

/* Return this Sidecar process runtime generation.
 * 返回当前 Sidecar 进程运行代次。 */
uint64_t cbm_managed_project_registry_runtime_id(cbm_managed_project_registry_t *registry);

/* Mark a registered root offline without deleting its state or storage.
 * 将已注册根路径标记为离线，但不删除其状态或存储。
 *
 * Returns false when the project was removed concurrently.
 * 项目被并发移除时返回 false。
 */
bool cbm_managed_project_registry_mark_offline(cbm_managed_project_registry_t *registry,
                                               const char *project_key);

/* Return the committed authoritative generation.
 * 返回已提交的权威 generation。
 */
uint64_t cbm_managed_project_registry_generation(cbm_managed_project_registry_t *registry);

/* Return the number of registered projects.
 * 返回已注册项目数量。
 */
size_t cbm_managed_project_registry_count(cbm_managed_project_registry_t *registry);

#endif /* CBM_MANAGED_PROJECT_REGISTRY_H */
