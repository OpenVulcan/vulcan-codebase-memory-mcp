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

    ASSERT(cbm_managed_project_registry_mark_indexing(registry, "alpha-key"));
    ASSERT(
        cbm_managed_project_registry_publish_index(registry, "alpha-key", true, 1234U, NULL, NULL));
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
    ASSERT_EQ(alpha.lifecycle, CBM_MANAGED_PROJECT_READY);
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
    ASSERT_EQ(entry.lifecycle, CBM_MANAGED_PROJECT_OFFLINE);
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
    ASSERT(cbm_managed_project_registry_publish_index(registry, "alpha-key", false, 100U,
                                                      "watch_registration_failed",
                                                      "Managed watcher registration failed"));
    ASSERT(cbm_managed_project_registry_set_watcher(registry, "alpha-key", true));

    /* Recovered project state.
     * 已恢复的项目状态。 */
    cbm_managed_project_entry_t recovered = {0};
    ASSERT(cbm_managed_project_registry_get_by_key(registry, "alpha-key", &recovered));
    ASSERT_EQ(recovered.lifecycle, CBM_MANAGED_PROJECT_READY);
    ASSERT(recovered.watcher_registered);
    ASSERT(recovered.last_error_code[0] == '\0');
    ASSERT(recovered.last_error_message[0] == '\0');

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
}
