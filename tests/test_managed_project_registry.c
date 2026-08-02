/*
 * Validate the daemon-level Vulcan managed project authority.
 * 验证 daemon 级 Vulcan 托管项目权威注册表。
 */
#include "test_framework.h"

#include "daemon/managed_project_registry.h"

/*
 * Commit generations atomically and preserve state for retained projects.
 * 原子提交 generation，并为保留项目保存状态。
 */
TEST(managed_registry_sync_is_atomic_and_retains_state) {
    cbm_managed_project_registry_t *registry = cbm_managed_project_registry_new();
    ASSERT(registry != NULL);
    cbm_managed_project_input_t first[] = {
        {.project_id = "alpha",
         .canonical_root = "/projects/alpha",
         .project_key = "alpha-key",
         .root_available = true},
        {.project_id = "beta",
         .canonical_root = "/projects/beta",
         .project_key = "beta-key",
         .root_available = true},
    };
    cbm_managed_sync_result_t result = {0};
    ASSERT(cbm_managed_project_registry_sync(registry, 1U, first, 2U, &result));
    ASSERT_EQ(result.added, 2);
    ASSERT_EQ(cbm_managed_project_registry_generation(registry), 1);
    cbm_managed_sync_result_free(&result);

    cbm_managed_job_snapshot_t initial_job = {0};
    ASSERT_EQ(cbm_managed_project_registry_begin_job(
                  registry, "alpha-key", CBM_MANAGED_JOB_MODE_UPDATE,
                  CBM_MANAGED_JOB_TRIGGER_INITIAL, 1200U, &initial_job),
              CBM_MANAGED_JOB_BEGIN_STARTED);
    ASSERT(cbm_managed_project_registry_publish_job(
        registry, "alpha-key", initial_job.runtime_id, initial_job.job_id,
        CBM_MANAGED_JOB_STATE_SUCCEEDED, 1234U, NULL, NULL));
    ASSERT(cbm_managed_project_registry_set_watcher(registry, "alpha-key", true));

    cbm_managed_project_input_t second[] = {
        {.project_id = "alpha",
         .canonical_root = "/projects/alpha",
         .project_key = "alpha-key",
         .root_available = true},
        {.project_id = "gamma",
         .canonical_root = "/projects/gamma",
         .project_key = "gamma-key",
         .root_available = true},
    };
    ASSERT(cbm_managed_project_registry_sync(registry, 2U, second, 2U, &result));
    ASSERT_EQ(result.added, 1);
    ASSERT_EQ(result.retained, 1);
    ASSERT_EQ(result.removed, 1);
    cbm_managed_project_entry_t alpha = {0};
    ASSERT(cbm_managed_project_registry_get_by_id(registry, "alpha", &alpha));
    ASSERT_EQ(alpha.availability, CBM_MANAGED_AVAILABILITY_READY);
    ASSERT_EQ(alpha.index_revision, 1);
    ASSERT(alpha.watcher_registered);
    ASSERT(!cbm_managed_project_registry_get_by_id(registry, "beta", &alpha));
    cbm_managed_sync_result_free(&result);
    cbm_managed_project_registry_free(registry);
    PASS();
}

/*
 * Reject stale and conflicting generations without changing authority.
 * 拒绝过期或冲突 generation，且不改变权威状态。
 */
TEST(managed_registry_rejects_stale_and_conflicting_generations) {
    cbm_managed_project_registry_t *registry = cbm_managed_project_registry_new();
    ASSERT(registry != NULL);
    cbm_managed_project_input_t initial = {
        .project_id = "alpha",
        .canonical_root = "/projects/alpha",
        .project_key = "alpha-key",
        .root_available = true,
    };
    cbm_managed_sync_result_t result = {0};
    ASSERT(cbm_managed_project_registry_sync(registry, 4U, &initial, 1U, &result));
    cbm_managed_sync_result_free(&result);
    ASSERT(!cbm_managed_project_registry_sync(registry, 4U, &initial, 1U, &result));
    ASSERT_EQ(result.status, CBM_MANAGED_SYNC_STALE_GENERATION);
    cbm_managed_sync_result_free(&result);

    cbm_managed_project_input_t conflict[] = {
        {.project_id = "alpha",
         .canonical_root = "/projects/alpha",
         .project_key = "alpha-key",
         .root_available = true},
        {.project_id = "beta",
         .canonical_root = "/projects/alpha",
         .project_key = "beta-key",
         .root_available = true},
    };
    ASSERT(!cbm_managed_project_registry_sync(registry, 5U, conflict, 2U, &result));
    ASSERT_EQ(result.status, CBM_MANAGED_SYNC_CONFLICT);
    ASSERT_EQ(cbm_managed_project_registry_generation(registry), 4);
    ASSERT_EQ(cbm_managed_project_registry_count(registry), 1);
    cbm_managed_sync_result_free(&result);
    cbm_managed_project_registry_free(registry);
    PASS();
}

/*
 * Keep an unavailable registered root addressable without authorizing queries.
 * 保留不可用的已注册根路径，但不授权查询。
 */
TEST(managed_registry_preserves_offline_identity) {
    cbm_managed_project_registry_t *registry = cbm_managed_project_registry_new();
    ASSERT(registry != NULL);
    cbm_managed_project_input_t project = {
        .project_id = "alpha",
        .canonical_root = "/projects/alpha",
        .project_key = "alpha-key",
        .root_available = true,
    };
    cbm_managed_sync_result_t result = {0};
    ASSERT(cbm_managed_project_registry_sync(registry, 1U, &project, 1U, &result));
    cbm_managed_sync_result_free(&result);
    ASSERT(cbm_managed_project_registry_mark_offline(registry, "alpha-key"));
    cbm_managed_project_entry_t entry = {0};
    ASSERT(
        cbm_managed_project_registry_resolve_offline(registry, "alpha", "/projects/alpha", &entry));
    ASSERT_EQ(entry.availability, CBM_MANAGED_AVAILABILITY_OFFLINE);
    ASSERT(!cbm_managed_project_registry_authorize(registry, "alpha", "/projects/alpha", &entry));
    cbm_managed_project_registry_free(registry);
    PASS();
}

/*
 * Reject id or key rebinding until the old authority is removed first.
 * 在先移除旧权威之前，拒绝项目标识或项目键重绑。
 */
TEST(managed_registry_rejects_single_generation_identity_rebinding) {
    /* Registry under test.
     * 被测注册表。 */
    cbm_managed_project_registry_t *registry = cbm_managed_project_registry_new();
    ASSERT(registry != NULL);
    /* Initial stable identity.
     * 初始稳定身份。 */
    cbm_managed_project_input_t initial = {
        .project_id = "alpha",
        .canonical_root = "/projects/alpha",
        .project_key = "alpha-key",
        .root_available = true,
    };
    /* Synchronization result storage.
     * 同步结果存储。 */
    cbm_managed_sync_result_t result = {0};
    ASSERT(cbm_managed_project_registry_sync(registry, 1U, &initial, 1U, &result));
    cbm_managed_sync_result_free(&result);

    /* Same id rebound to another root and key.
     * 将同一标识重绑到另一根路径和项目键。 */
    cbm_managed_project_input_t rebound_id = {
        .project_id = "alpha",
        .canonical_root = "/projects/beta",
        .project_key = "beta-key",
        .root_available = true,
    };
    ASSERT(!cbm_managed_project_registry_sync(registry, 2U, &rebound_id, 1U, &result));
    ASSERT_EQ(result.status, CBM_MANAGED_SYNC_CONFLICT);
    ASSERT_EQ(cbm_managed_project_registry_generation(registry), 1);
    cbm_managed_sync_result_free(&result);

    /* Same key rebound to another id and root.
     * 将同一项目键重绑到另一标识和根路径。 */
    cbm_managed_project_input_t rebound_key = {
        .project_id = "beta",
        .canonical_root = "/projects/beta",
        .project_key = "alpha-key",
        .root_available = true,
    };
    ASSERT(!cbm_managed_project_registry_sync(registry, 2U, &rebound_key, 1U, &result));
    ASSERT_EQ(result.status, CBM_MANAGED_SYNC_CONFLICT);
    ASSERT_EQ(cbm_managed_project_registry_generation(registry), 1);
    cbm_managed_sync_result_free(&result);

    cbm_managed_project_registry_free(registry);
    PASS();
}

/*
 * Recover a watcher-only failure after physical registration succeeds.
 * 在物理 watcher 注册成功后恢复仅由 watcher 引起的失败。
 */
TEST(managed_registry_recovers_watcher_registration_failure) {
    /* Registry under test.
     * 被测注册表。 */
    cbm_managed_project_registry_t *registry = cbm_managed_project_registry_new();
    ASSERT(registry != NULL);
    /* Registered project.
     * 已注册项目。 */
    cbm_managed_project_input_t project = {
        .project_id = "alpha",
        .canonical_root = "/projects/alpha",
        .project_key = "alpha-key",
        .root_available = true,
    };
    /* Synchronization result storage.
     * 同步结果存储。 */
    cbm_managed_sync_result_t result = {0};
    ASSERT(cbm_managed_project_registry_sync(registry, 1U, &project, 1U, &result));
    cbm_managed_sync_result_free(&result);
    cbm_managed_job_snapshot_t initial_job = {0};
    ASSERT_EQ(
        cbm_managed_project_registry_begin_job(registry, "alpha-key", CBM_MANAGED_JOB_MODE_UPDATE,
                                               CBM_MANAGED_JOB_TRIGGER_INITIAL, 80U, &initial_job),
        CBM_MANAGED_JOB_BEGIN_STARTED);
    ASSERT(cbm_managed_project_registry_publish_job(
        registry, "alpha-key", initial_job.runtime_id, initial_job.job_id,
        CBM_MANAGED_JOB_STATE_SUCCEEDED, 90U, NULL, NULL));
    ASSERT(cbm_managed_project_registry_mark_degraded(
        registry, "alpha-key", "watch_registration_failed", "Managed watcher registration failed"));
    ASSERT(cbm_managed_project_registry_set_watcher(registry, "alpha-key", true));

    /* Recovered project state.
     * 已恢复的项目状态。 */
    cbm_managed_project_entry_t recovered = {0};
    ASSERT(cbm_managed_project_registry_get_by_key(registry, "alpha-key", &recovered));
    ASSERT_EQ(recovered.availability, CBM_MANAGED_AVAILABILITY_READY);
    ASSERT(recovered.watcher_registered);
    ASSERT(recovered.last_error_code[0] == '\0');
    ASSERT(recovered.last_error_message[0] == '\0');

    cbm_managed_project_registry_free(registry);
    PASS();
}

/*
 * Preserve one published revision while an exact update job advances monotonically.
 * 在精确更新任务单调推进期间保留已发布修订。
 */
TEST(managed_registry_tracks_exact_job_and_monotonic_progress) {
    /* Registry under test.
     * 被测注册表。 */
    cbm_managed_project_registry_t *registry = cbm_managed_project_registry_new();
    ASSERT(registry != NULL);
    /* Registered project fixture.
     * 已注册项目夹具。 */
    cbm_managed_project_input_t project = {
        .project_id = "alpha",
        .canonical_root = "/projects/alpha",
        .project_key = "alpha-key",
        .root_available = true,
    };
    /* Synchronization result storage.
     * 同步结果存储。 */
    cbm_managed_sync_result_t result = {0};
    ASSERT(cbm_managed_project_registry_sync(registry, 1U, &project, 1U, &result));
    cbm_managed_sync_result_free(&result);
    ASSERT(cbm_managed_project_registry_restore_snapshot(registry, "alpha-key", 100U));

    /* Exact update job admitted by the registry.
     * 注册表准入的精确更新任务。 */
    cbm_managed_job_snapshot_t job = {0};
    ASSERT_EQ(cbm_managed_project_registry_begin_job(registry, "alpha-key",
                                                     CBM_MANAGED_JOB_MODE_UPDATE,
                                                     CBM_MANAGED_JOB_TRIGGER_MANUAL, 200U, &job),
              CBM_MANAGED_JOB_BEGIN_STARTED);
    ASSERT(cbm_managed_project_registry_mark_job_running(registry, "alpha-key", job.runtime_id,
                                                         job.job_id, 210U));
    ASSERT(cbm_managed_project_registry_update_job_progress(
        registry, "alpha-key", job.runtime_id, job.job_id, CBM_MANAGED_JOB_PHASE_EXTRACTING, 4U,
        10U, true, "files", 220U));
    ASSERT(cbm_managed_project_registry_update_job_progress(
        registry, "alpha-key", job.runtime_id, job.job_id, CBM_MANAGED_JOB_PHASE_DISCOVERING, 2U,
        10U, true, "files", 230U));
    ASSERT(!cbm_managed_project_registry_update_job_progress(
        registry, "alpha-key", job.runtime_id, job.job_id + 1U, CBM_MANAGED_JOB_PHASE_PERSISTING,
        8U, 10U, true, "files", 240U));

    /* Snapshot observed before terminal publication.
     * 终态发布前观察到的快照。 */
    cbm_managed_project_entry_t active = {0};
    ASSERT(cbm_managed_project_registry_get_by_key(registry, "alpha-key", &active));
    ASSERT_EQ(active.availability, CBM_MANAGED_AVAILABILITY_READY);
    ASSERT_EQ(active.index_revision, 1);
    ASSERT(active.has_active_job);
    ASSERT_EQ(active.active_job.state, CBM_MANAGED_JOB_STATE_RUNNING);
    ASSERT_EQ(active.active_job.phase, CBM_MANAGED_JOB_PHASE_EXTRACTING);
    ASSERT_EQ(active.active_job.completed_units, 4);
    ASSERT_EQ(active.active_job.total_units, 10);
    ASSERT(strcmp(active.active_job.unit, "files") == 0);

    ASSERT(cbm_managed_project_registry_publish_job(registry, "alpha-key", job.runtime_id,
                                                    job.job_id, CBM_MANAGED_JOB_STATE_SUCCEEDED,
                                                    300U, NULL, NULL));
    ASSERT(!cbm_managed_project_registry_publish_job(registry, "alpha-key", job.runtime_id,
                                                     job.job_id, CBM_MANAGED_JOB_STATE_FAILED, 310U,
                                                     "stale", "Stale terminal result"));
    /* Published terminal snapshot.
     * 已发布的终态快照。 */
    cbm_managed_project_entry_t published = {0};
    ASSERT(cbm_managed_project_registry_get_by_key(registry, "alpha-key", &published));
    ASSERT_EQ(published.availability, CBM_MANAGED_AVAILABILITY_READY);
    ASSERT_EQ(published.index_revision, 2);
    ASSERT(!published.has_active_job);
    ASSERT(published.has_last_job);
    ASSERT_EQ(published.last_job.state, CBM_MANAGED_JOB_STATE_SUCCEEDED);

    cbm_managed_project_registry_free(registry);
    PASS();
}

/*
 * Coalesce only the same mode and preserve a published revision after failure.
 * 仅合并相同模式，并在失败后保留已发布修订。
 */
TEST(managed_registry_coalesces_same_mode_and_degrades_on_failure) {
    /* Registry under test.
     * 被测注册表。 */
    cbm_managed_project_registry_t *registry = cbm_managed_project_registry_new();
    ASSERT(registry != NULL);
    /* Registered project fixture.
     * 已注册项目夹具。 */
    cbm_managed_project_input_t project = {
        .project_id = "alpha",
        .canonical_root = "/projects/alpha",
        .project_key = "alpha-key",
        .root_available = true,
    };
    /* Synchronization result storage.
     * 同步结果存储。 */
    cbm_managed_sync_result_t result = {0};
    ASSERT(cbm_managed_project_registry_sync(registry, 1U, &project, 1U, &result));
    cbm_managed_sync_result_free(&result);
    ASSERT(cbm_managed_project_registry_restore_snapshot(registry, "alpha-key", 100U));

    /* Original update and subsequent admission observations.
     * 原始更新任务及后续准入观测。 */
    cbm_managed_job_snapshot_t original = {0};
    cbm_managed_job_snapshot_t duplicate = {0};
    cbm_managed_job_snapshot_t conflict = {0};
    ASSERT_EQ(
        cbm_managed_project_registry_begin_job(registry, "alpha-key", CBM_MANAGED_JOB_MODE_UPDATE,
                                               CBM_MANAGED_JOB_TRIGGER_WATCHER, 200U, &original),
        CBM_MANAGED_JOB_BEGIN_STARTED);
    ASSERT_EQ(
        cbm_managed_project_registry_begin_job(registry, "alpha-key", CBM_MANAGED_JOB_MODE_UPDATE,
                                               CBM_MANAGED_JOB_TRIGGER_MANUAL, 210U, &duplicate),
        CBM_MANAGED_JOB_BEGIN_COALESCED);
    ASSERT(duplicate.coalesced);
    ASSERT_EQ(duplicate.job_id, original.job_id);
    ASSERT_EQ(
        cbm_managed_project_registry_begin_job(registry, "alpha-key", CBM_MANAGED_JOB_MODE_REBUILD,
                                               CBM_MANAGED_JOB_TRIGGER_MANUAL, 220U, &conflict),
        CBM_MANAGED_JOB_BEGIN_CONFLICT);
    ASSERT_EQ(conflict.job_id, original.job_id);

    ASSERT(cbm_managed_project_registry_publish_job(registry, "alpha-key", original.runtime_id,
                                                    original.job_id, CBM_MANAGED_JOB_STATE_FAILED,
                                                    300U, "extract_failed", "Extraction failed"));
    /* State after an update failure over a usable revision.
     * 可用修订上的更新失败状态。 */
    cbm_managed_project_entry_t failed = {0};
    ASSERT(cbm_managed_project_registry_get_by_key(registry, "alpha-key", &failed));
    ASSERT_EQ(failed.availability, CBM_MANAGED_AVAILABILITY_DEGRADED);
    ASSERT_EQ(failed.index_revision, 1);
    ASSERT(failed.has_last_job);
    ASSERT_EQ(failed.last_job.state, CBM_MANAGED_JOB_STATE_FAILED);
    ASSERT(strcmp(failed.last_error_code, "extract_failed") == 0);

    cbm_managed_project_registry_free(registry);
    PASS();
}

/*
 * Cancel the exact active job when the authoritative root becomes unavailable.
 * 当权威根路径不可用时取消精确活动任务。
 */
TEST(managed_registry_offline_transition_cancels_active_job) {
    /* Registry under test.
     * 被测注册表。 */
    cbm_managed_project_registry_t *registry = cbm_managed_project_registry_new();
    ASSERT(registry != NULL);
    /* Registered project fixture.
     * 已注册项目夹具。 */
    cbm_managed_project_input_t project = {
        .project_id = "alpha",
        .canonical_root = "/projects/alpha",
        .project_key = "alpha-key",
        .root_available = true,
    };
    /* Synchronization result storage.
     * 同步结果存储。 */
    cbm_managed_sync_result_t result = {0};
    ASSERT(cbm_managed_project_registry_sync(registry, 1U, &project, 1U, &result));
    cbm_managed_sync_result_free(&result);
    /* Rebuild job that is cancelled by the offline transition.
     * 被离线转换取消的重建任务。 */
    cbm_managed_job_snapshot_t job = {0};
    ASSERT_EQ(cbm_managed_project_registry_begin_job(registry, "alpha-key",
                                                     CBM_MANAGED_JOB_MODE_REBUILD,
                                                     CBM_MANAGED_JOB_TRIGGER_RECOVERY, 200U, &job),
              CBM_MANAGED_JOB_BEGIN_STARTED);
    ASSERT(cbm_managed_project_registry_mark_offline(registry, "alpha-key"));

    /* Offline terminal snapshot.
     * 离线终态快照。 */
    cbm_managed_project_entry_t offline = {0};
    ASSERT(cbm_managed_project_registry_get_by_key(registry, "alpha-key", &offline));
    ASSERT_EQ(offline.availability, CBM_MANAGED_AVAILABILITY_OFFLINE);
    ASSERT(!offline.has_active_job);
    ASSERT(offline.has_last_job);
    ASSERT_EQ(offline.last_job.runtime_id, job.runtime_id);
    ASSERT_EQ(offline.last_job.job_id, job.job_id);
    ASSERT_EQ(offline.last_job.state, CBM_MANAGED_JOB_STATE_CANCELLED);
    ASSERT(strcmp(offline.last_job.error_code, "project_offline") == 0);

    cbm_managed_project_registry_free(registry);
    PASS();
}

/*
 * Run the managed registry unit suite.
 * 运行托管注册表单元测试套件。
 */
SUITE(managed_project_registry) {
    RUN_TEST(managed_registry_sync_is_atomic_and_retains_state);
    RUN_TEST(managed_registry_rejects_stale_and_conflicting_generations);
    RUN_TEST(managed_registry_preserves_offline_identity);
    RUN_TEST(managed_registry_rejects_single_generation_identity_rebinding);
    RUN_TEST(managed_registry_recovers_watcher_registration_failure);
    RUN_TEST(managed_registry_tracks_exact_job_and_monotonic_progress);
    RUN_TEST(managed_registry_coalesces_same_mode_and_degrades_on_failure);
    RUN_TEST(managed_registry_offline_transition_cancels_active_job);
}
