import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def canonical_sha256(relative: str) -> str:
    content = (ROOT / relative).read_text(encoding="utf-8")
    canonical = content.replace("\r\n", "\n").encode("utf-8")
    return hashlib.sha256(canonical).hexdigest()


def body(source: str, start: str, end: str) -> str:
    return source[source.index(start):source.index(end, source.index(start))]


def test_admin_protocol_and_runtime_are_separate_production_modules() -> None:
    cmake = read("main/CMakeLists.txt")
    dispatch_h = read("main/mesh/meshcore_admin_dispatch.h")
    dispatch_c = read("main/mesh/meshcore_admin_dispatch.c")
    runtime = read("main/mesh/meshcore_admin_runtime.c")
    for source in (
        "mesh/meshcore_admin_dispatch.c",
        "mesh/meshcore_admin_runtime.c",
        "mesh/meshcore_identity_exchange.c",
    ):
        assert f'"{source}"' in cmake
    assert "D1L_MESHCORE_ADMIN_REPLAY_PEER_CAPACITY 8U" in dispatch_h
    assert "D1L_MESHCORE_ADMIN_REPLAY_RESPONSES_PER_PEER 4U" in dispatch_h
    assert "request_deadline_us" in dispatch_h
    assert "idle_deadline_us" in dispatch_h
    assert "absolute_deadline_us" in dispatch_h
    assert "now_us >= deadline_us" in dispatch_c
    assert "refresh_idle_deadline(session, now_us)" in dispatch_c
    assert "replay_cache_contains" in dispatch_c
    assert "entry->responses[response_index]" in dispatch_c
    assert "entry->next_response" in dispatch_c
    assert "d1l_meshcore_admin_timeout(session);" in dispatch_c
    assert "role != D1L_MESHCORE_ADMIN_ROLE_REPEATER" in dispatch_c
    assert "role != D1L_MESHCORE_ADMIN_ROLE_ROOM" in dispatch_c
    assert "D1L_MESHCORE_ADMIN_ROOM_NO_HISTORY_CURSOR" in dispatch_h
    assert "D1L_MESHCORE_ADMIN_ROOM_NO_HISTORY_CURSOR" in runtime
    assert "role == D1L_MESHCORE_ADMIN_ROLE_ROOM && password_len == 0U" in \
        runtime


def test_admin_mutations_are_exactly_allowlisted_and_locally_confirmed() -> None:
    dispatch = read("main/mesh/meshcore_admin_dispatch.c")
    runtime = read("main/mesh/meshcore_admin_runtime.c")
    service = read("main/mesh/meshcore_service.c")
    ui = read("main/ui/ui_phase1.c")

    mutation_command = body(
        dispatch,
        "const char *d1l_meshcore_admin_mutation_command",
        "static const char *mutation_success_reply",
    )
    assert 'return "clear stats";' in mutation_command
    assert 'return "advert.zerohop";' in mutation_command
    assert "default:\n        return NULL;" in mutation_command

    mutation_reply = body(
        dispatch,
        "static const char *mutation_success_reply",
        "bool d1l_meshcore_admin_begin_mutation",
    )
    assert 'return "(OK - stats reset)";' in mutation_reply
    assert 'return "OK - zerohop advert sent";' in mutation_reply

    builder = body(
        runtime,
        "esp_err_t d1l_meshcore_admin_build_mutation_packet",
        "static void clear_session_locked",
    )
    assert "D1L_MESHCORE_TXT_TYPE_CLI_DATA << 2U" in builder
    assert "d1l_meshcore_admin_mutation_command(mutation)" in builder
    assert "d1l_meshcore_admin_secure_zero(plaintext" in builder

    public_request = body(
        service,
        "esp_err_t d1l_meshcore_service_admin_request_mutation",
        "esp_err_t d1l_meshcore_service_request_advert",
    )
    assert "if (!local_confirmed)" in public_request
    assert "d1l_meshcore_admin_mutation_command(mutation)" in public_request
    assert "meshcore_service_send_command(" in public_request
    assert "&cmd, D1L_MESHCORE_SERVICE_COMMAND_TIMEOUT_MS" in public_request

    assert "D1L_UI_ADMIN_MUTATION_CONFIRM_WINDOW_MS = 5000U" in ui
    assert "s_admin_mutation_armed" in ui
    assert "d1l_meshcore_service_admin_request_mutation(mutation,true)" in \
        "".join(ui.split())


def test_runtime_snapshot_redacts_all_authority_material() -> None:
    runtime_h = read("main/mesh/meshcore_admin_runtime.h")
    runtime_c = read("main/mesh/meshcore_admin_runtime.c")
    context = body(
        runtime_h,
        "typedef struct {\n    d1l_meshcore_admin_binding_t binding;",
        "} d1l_meshcore_admin_context_t;",
    )
    copy_context = body(
        runtime_c,
        "static bool copy_context_locked",
        "static bool binding_matches_locked",
    )
    capture_pending = body(
        runtime_c,
        "bool d1l_meshcore_admin_runtime_capture_pending",
        "bool d1l_meshcore_admin_runtime_capture_authenticated",
    )
    assert "uint64_t request_deadline_us;" in context
    assert "out_context->request_deadline_us = s_session.request_deadline_us" in \
        copy_context
    assert capture_pending.index("d1l_store_lock_take(&s_lock)") < \
        capture_pending.index("copy_context_locked(out_context, false)") < \
        capture_pending.index("d1l_store_lock_give(&s_lock)")

    snapshot = body(
        runtime_h,
        "typedef struct {\n    d1l_meshcore_admin_state_t state;",
        "} d1l_meshcore_admin_runtime_snapshot_t;",
    )
    for forbidden in (
        "session_secret", "peer_public_key", "local_public_key", "password"
    ):
        assert forbidden not in snapshot


def test_service_revalidates_every_live_admin_binding_and_zeroizes_copies() -> None:
    service = read("main/mesh/meshcore_service.c")
    direct = body(
        service,
        "static bool parse_rx_admin_response_packet",
        "static void parse_rx_channel_packet",
    )
    for required in (
        "d1l_meshcore_admin_runtime_capture_pending",
        "d1l_contact_store_find_by_fingerprint",
        "d1l_meshcore_admin_role_for_contact",
        "derive_local_identity_shared_secret",
        "d1l_meshcore_admin_runtime_validate_binding",
        "meshcore_decrypt_after_mac",
        "meshcore_service_queue_admin_response",
        "d1l_meshcore_admin_binding_wipe",
        "d1l_meshcore_admin_context_wipe",
        "secure_zero_bytes(plaintext, sizeof(plaintext))",
    ):
        assert required in direct
    derive_failure = body(
        direct,
        "const esp_err_t derive_ret = derive_local_identity_shared_secret(",
        "if (!d1l_meshcore_admin_runtime_validate_binding(",
    )
    assert "if (derive_ret != ESP_OK)" in derive_failure
    assert "d1l_meshcore_admin_runtime_invalidate(ESP_ERR_INVALID_STATE)" in \
        derive_failure
    assert derive_failure.index("d1l_meshcore_admin_runtime_invalidate") < \
        derive_failure.index("d1l_meshcore_admin_binding_wipe")
    assert "d1l_meshcore_admin_runtime_dispatch_response" not in direct
    assert "return true;" in direct
    assert "plaintext,plaintext_len,true,context.request_deadline_us," in \
        "".join(direct.split())

    path_rx = body(service, "static void parse_rx_path_packet",
                   "static void parse_rx_trace_packet")
    handoff = path_rx[path_rx.index("admin_queue_plain_response("):
                      path_rx.index("admin_queue_plain_response(") + 350]
    assert "settings->identity_public_key" in handoff
    assert "secret" in handoff
    assert "if (!admin_response_considered)" in path_rx
    decrypted = path_rx[path_rx.index("uint8_t plain["):]
    before_cleanup, cleanup = decrypted.split("path_candidate_cleanup:", 1)
    cleanup_branches = (
        ("if (!d1l_meshcore_path_plain_decode", "uint8_t packet_hash["),
        ("if (packet_hash_ready && d1l_meshcore_packet_hash_cache_contains", "uint8_t replay_identity["),
        ("esp_err_t ret = calc_path_replay_identity", "if (!d1l_meshcore_path_replay_take"),
        ("if (!d1l_meshcore_path_replay_take", "d1l_contact_entry_t learned_contact"),
        ("ret = d1l_contact_store_update_path_from_source", "} else {"),
    )
    for start, end in cleanup_branches:
        branch = before_cleanup[before_cleanup.index(start):]
        branch = branch[:branch.index(end)]
        assert "goto path_candidate_cleanup;" in branch
    assert before_cleanup.count("goto path_candidate_cleanup;") == len(cleanup_branches)
    assert "continue;" not in before_cleanup
    assert "return;" not in before_cleanup
    assert cleanup.count("secure_zero_bytes(plain, sizeof(plain));") == 1
    assert cleanup.index("secure_zero_bytes(plain, sizeof(plain));") < \
        cleanup.index("if (continue_after_cleanup)") < cleanup.index("return;")

    path_helper = body(
        service,
        "static esp_err_t admin_queue_plain_response",
        "static bool parse_rx_admin_response_packet",
    )
    for required in (
        "d1l_meshcore_admin_runtime_capture_pending",
        "d1l_meshcore_admin_runtime_validate_binding",
        "meshcore_service_queue_admin_response",
        "d1l_meshcore_admin_binding_wipe",
        "d1l_meshcore_admin_context_wipe",
    ):
        assert required in path_helper
    assert "d1l_meshcore_admin_runtime_dispatch_response" not in path_helper
    assert "plaintext,plaintext_len,false,context.request_deadline_us,now_us" in \
        "".join(path_helper.split())

    owner = body(
        service,
        "static esp_err_t meshcore_service_handle_admin_response",
        "static void meshcore_service_reply",
    )
    for required in (
        "d1l_mesh_command_sync_wait_allowed",
        "d1l_mesh_admin_response_request_admit",
        "d1l_meshcore_admin_runtime_capture_pending",
        "d1l_contact_store_find_by_fingerprint",
        "derive_local_identity_shared_secret",
        "d1l_meshcore_admin_runtime_validate_binding",
        "d1l_meshcore_admin_runtime_dispatch_response",
        "d1l_meshcore_admin_binding_wipe",
        "d1l_meshcore_admin_context_wipe",
        "d1l_mesh_admin_response_request_complete",
    ):
        assert required in owner
    assert owner.index("d1l_mesh_admin_response_request_admit") < \
        owner.index("d1l_meshcore_admin_runtime_dispatch_response")
    assert owner.index("d1l_meshcore_admin_runtime_validate_binding") < \
        owner.index("d1l_meshcore_admin_runtime_dispatch_response")
    assert owner.index("d1l_meshcore_admin_runtime_dispatch_response") < \
        owner.index("d1l_mesh_admin_response_request_complete")
    assert "response.deadline_us != cmd->admin_response_deadline_us" in owner
    assert "response.deadline_us != context.request_deadline_us" in owner
    assert "if (response.count_rx_on_accept)" in owner
    assert service.count(
        "d1l_meshcore_admin_runtime_dispatch_response(") == 1

    status = body(
        service,
        "static esp_err_t meshcore_service_handle_admin_request_status",
        "static esp_err_t meshcore_service_handle_admin_logout",
    )
    assert status.index("d1l_meshcore_admin_runtime_validate_binding") < \
        status.index("d1l_meshcore_admin_build_status_packet")
    assert status.index("d1l_meshcore_admin_runtime_begin_status") < \
        status.index("meshcore_service_handle_send_raw")
    assert status.index("d1l_meshcore_admin_runtime_begin_status") < \
        status.index("d1l_meshcore_admin_binding_wipe(&current)") < \
        status.index("meshcore_service_handle_send_raw")
    assert status.index("d1l_meshcore_admin_runtime_begin_status") < \
        status.index("d1l_meshcore_admin_context_wipe(&context)") < \
        status.index("meshcore_service_handle_send_raw")
    assert "meshcore_service_send_raw(" not in status


def test_outbound_admin_uses_canonical_wire_crypto_and_reserved_state() -> None:
    runtime = read("main/mesh/meshcore_admin_runtime.c")
    service = read("main/mesh/meshcore_service.c")
    login_builder = body(
        runtime, "esp_err_t d1l_meshcore_admin_build_login_packet",
        "esp_err_t d1l_meshcore_admin_build_status_packet",
    )
    for required in (
        "d1l_meshcore_wire_write_prefix", "derive_secret(",
        "d1l_meshcore_admin_encode_login_request", "encrypt(",
        "d1l_meshcore_admin_secure_zero(plaintext",
    ):
        assert required in login_builder
    status_builder = body(
        runtime, "esp_err_t d1l_meshcore_admin_build_status_packet",
        "static void clear_session_locked",
    )
    assert "d1l_meshcore_admin_encode_status_request" in status_builder
    assert "d1l_meshcore_wire_write_prefix" in status_builder
    assert "encrypt(" in status_builder

    login = body(
        service,
        "static esp_err_t meshcore_service_handle_admin_login",
        "static esp_err_t meshcore_service_handle_admin_request_status",
    )
    assert login.index("d1l_meshcore_admin_runtime_begin_login") < \
        login.index("meshcore_service_handle_send_raw")
    assert login.index("d1l_meshcore_admin_runtime_begin_login") < \
        login.index("d1l_meshcore_admin_binding_wipe(&binding)") < \
        login.index("meshcore_service_handle_send_raw")
    assert login.index("d1l_meshcore_admin_build_login_packet") < \
        login.index("secure_zero_bytes(password, sizeof(password))") < \
        login.index("d1l_meshcore_admin_runtime_begin_login")
    assert "d1l_meshcore_admin_runtime_note_login_tx" in login
    assert "append_packet_log" not in login
    assert "meshcore_service_send_raw(" not in login


def test_admin_commands_and_secrets_are_owned_by_the_mesh_runtime_task() -> None:
    service = read("main/mesh/meshcore_service.c")
    header = read("main/mesh/meshcore_service.h")
    command_guard = read("main/mesh/meshcore_command_guard.h")
    command = body(
        service,
        "typedef struct {\n    d1l_meshcore_service_cmd_type_t type;",
        "} d1l_meshcore_service_cmd_t;",
    )
    slots = body(
        service,
        "typedef struct {\n    d1l_mesh_command_request_t request;",
        "} d1l_meshcore_request_slot_t;",
    )
    task = body(service, "static void meshcore_service_task",
                "static esp_err_t meshcore_service_start_task")
    public_login = body(
        service,
        "esp_err_t d1l_meshcore_service_admin_login",
        "esp_err_t d1l_meshcore_service_admin_request_status",
    )
    public_status = body(
        service,
        "esp_err_t d1l_meshcore_service_admin_request_status",
        "esp_err_t d1l_meshcore_service_admin_logout",
    )
    public_logout = body(
        service,
        "esp_err_t d1l_meshcore_service_admin_logout",
        "esp_err_t d1l_meshcore_service_request_advert",
    )

    for command_name in (
        "D1L_MESHCORE_SERVICE_CMD_ADMIN_LOGIN",
        "D1L_MESHCORE_SERVICE_CMD_ADMIN_REQUEST_STATUS",
        "D1L_MESHCORE_SERVICE_CMD_ADMIN_LOGOUT",
        "D1L_MESHCORE_SERVICE_CMD_ADMIN_RESPONSE",
    ):
        assert command_name in task
    admission_drop = task[
        task.index("if (!meshcore_request_admit(&cmd))"):
        task.index("esp_err_t ret = ESP_ERR_INVALID_ARG;")
    ]
    assert admission_drop.index("meshcore_service_command_wipe(&cmd);") < \
        admission_drop.index("continue;")
    assert task.rindex("meshcore_service_command_wipe(&cmd);") > \
        task.index("meshcore_service_reply(&cmd, ret);")
    assert "admin_password" not in command
    for forbidden in (
        "plaintext",
        "session_secret",
        "peer_public_key",
        "local_public_key",
        "password",
    ):
        assert forbidden not in command
    assert "admin_response_id" in command
    assert "admin_response_deadline_us" in command
    assert "admin_response_slot" in command
    assert "d1l_mesh_command_request_t request;" in slots
    assert "s_admin_response_slots[D1L_MESHCORE_PRIORITY_QUEUE_LEN]" in service
    assert "admin_password[D1L_MESH_COMMAND_ADMIN_PASSWORD_CAPACITY]" in \
        command_guard
    assert "admin_password_present" in command_guard
    assert '#include "mesh/meshcore_command_guard.h"' in service
    assert "d1l_meshcore_admin_runtime_init();" in task
    assert "d1l_meshcore_admin_runtime_init();" not in body(
        service, "void d1l_meshcore_service_init",
        "esp_err_t d1l_meshcore_service_ensure_identity",
    )
    assert "meshcore_service_handle_admin_response(&cmd)" in task

    assert "meshcore_service_send_admin_login_command" in public_login
    assert "d1l_meshcore_admin_runtime_begin_login" not in public_login
    assert "d1l_settings_next_mesh_timestamp" not in public_login
    assert "meshcore_service_send_command" in public_status
    assert "d1l_meshcore_admin_runtime_begin_status" not in public_status
    assert "meshcore_service_send_command" in public_logout
    assert "d1l_meshcore_admin_runtime_logout" not in public_logout
    assert "esp_err_t d1l_meshcore_service_admin_logout(void);" in header


def test_admin_request_timeout_saturation_and_zeroization_fail_closed() -> None:
    service = read("main/mesh/meshcore_service.c")
    command_guard = read("main/mesh/meshcore_command_guard.h")
    secret_store = body(
        service,
        "static esp_err_t meshcore_request_store_admin_password",
        "static bool meshcore_request_take_admin_password",
    )
    secret_take = body(
        service,
        "static bool meshcore_request_take_admin_password",
        "static esp_err_t meshcore_request_consume",
    )
    wait = body(
        service,
        "static esp_err_t meshcore_request_wait",
        "static void meshcore_request_abort_unqueued",
    )
    admit = body(service, "static bool meshcore_request_admit",
                 "static void meshcore_request_complete")
    send_claimed = body(
        service,
        "static esp_err_t meshcore_service_send_claimed_command",
        "static esp_err_t meshcore_service_send_command",
    )
    enqueue = body(
        service,
        "static bool meshcore_service_enqueue_claimed",
        "static esp_err_t meshcore_service_send_claimed_command",
    )
    send_login = body(
        service,
        "static esp_err_t meshcore_service_send_admin_login_command",
        "esp_err_t d1l_meshcore_service_start_rx_async",
    )
    login_handler = body(
        service,
        "static esp_err_t meshcore_service_handle_admin_login",
        "static esp_err_t meshcore_service_handle_admin_request_status",
    )

    assert "password_len > D1L_MESHCORE_ADMIN_MAX_PASSWORD_BYTES" in secret_store
    assert "d1l_mesh_command_request_store_admin_password" in secret_store
    assert "d1l_mesh_command_request_take_admin_password" in secret_take
    assert "d1l_mesh_command_request_expire" in admit
    assert "d1l_mesh_command_request_admit" in admit
    assert "d1l_mesh_request_guard_release(" not in admit
    assert "d1l_mesh_command_request_enqueue" in send_claimed
    assert "xQueueSend(enqueue_context->queue, command" in enqueue
    assert "runtime_note_command_saturation(false);" in send_claimed
    assert "D1L_MESH_COMMAND_ENQUEUE_SATURATED" in send_claimed
    assert "meshcore_service_called_from_owner()" in send_login
    assert "meshcore_service_command_wipe(cmd);" in send_login
    assert "d1l_mesh_command_request_cancel_and_release" in wait
    assert "meshcore_request_take_admin_password" in login_handler
    assert "secure_zero_bytes(password, sizeof(password));" in login_handler
    assert "d1l_meshcore_admin_binding_wipe(&binding);" in login_handler
    assert "secure_zero_bytes(&raw_cmd, sizeof(raw_cmd));" in login_handler

    cancel_guard = body(
        command_guard,
        "static inline bool d1l_mesh_command_request_cancel_and_release",
        "static inline bool d1l_mesh_command_request_expire",
    )
    expire_guard = body(
        command_guard,
        "static inline bool d1l_mesh_command_request_expire",
        "static inline bool d1l_mesh_command_request_admit",
    )
    take_guard = body(
        command_guard,
        "static inline bool d1l_mesh_command_request_take_admin_password",
        "static inline bool d1l_mesh_command_request_cancel_and_release",
    )
    queue_guard = body(
        command_guard,
        "d1l_mesh_command_request_enqueue(",
        "static inline bool d1l_mesh_command_sync_wait_allowed",
    )
    assert cancel_guard.index("D1L_MESH_REQUEST_CALLER_CANCELLED") < \
        cancel_guard.index("d1l_mesh_command_request_wipe_admin_password") < \
        cancel_guard.index("d1l_mesh_request_guard_release")
    assert expire_guard.index("D1L_MESH_REQUEST_OWNER_EXPIRED") < \
        expire_guard.index("d1l_mesh_command_request_wipe_admin_password")
    assert take_guard.index("memcpy(out_password") < \
        take_guard.index("d1l_mesh_command_request_wipe_admin_password")
    assert "d1l_mesh_command_request_cancel_and_release" in queue_guard


def test_admin_response_handoff_is_bounded_generation_safe_and_zeroized() -> None:
    service = read("main/mesh/meshcore_service.c")
    command_guard = read("main/mesh/meshcore_command_guard.h")
    queue = body(
        service,
        "static bool meshcore_service_enqueue_admin_response_token",
        "static esp_err_t admin_queue_plain_response",
    )
    for required in (
        "xQueueSend((QueueHandle_t)context, command, 0)",
        "d1l_mesh_admin_response_request_claim",
        "d1l_mesh_admin_response_request_publish",
        "d1l_mesh_admin_response_request_enqueue",
        "D1L_MESH_COMMAND_ENQUEUE_SATURATED",
        "runtime_note_command_saturation(true)",
        "meshcore_service_command_wipe(&cmd)",
    ):
        assert required in queue
    assert "memcpy(cmd.raw" not in queue
    assert "secure_zero_bytes(&cmd, sizeof(cmd))" in queue
    assert "cmd.admin_response_id = request_id" in queue
    assert "cmd.admin_response_slot = slot_index" in queue
    assert "cmd.admin_response_deadline_us = deadline_us" in queue
    assert "request_deadline_us == 0U || now_us >= request_deadline_us" in \
        queue
    assert "const uint64_t deadline_us = request_deadline_us" in queue
    assert "now_us + D1L_MESHCORE_ADMIN_REQUEST_TIMEOUT_US" not in queue

    wipe = body(
        command_guard,
        "static inline void d1l_mesh_admin_response_request_wipe",
        "static inline bool d1l_mesh_admin_response_request_claim",
    )
    for field in (
        "request->fingerprint",
        "request->plaintext",
        "request->deadline_us = 0U",
        "request->session_generation = 0U",
        "request->plaintext_len = 0U",
        "request->count_rx_on_accept = false",
        "request->present = false",
    ):
        assert field in wipe

    enqueue = body(
        command_guard,
        "d1l_mesh_admin_response_request_enqueue(",
        "/* Only the runtime owner may reap queued Admin response bytes.",
    )
    assert "d1l_mesh_admin_response_request_cancel" in enqueue
    assert "D1L_MESH_COMMAND_ENQUEUE_SATURATED" in enqueue

    reap = body(
        command_guard,
        "static inline bool d1l_mesh_admin_response_request_owner_reap",
        "static inline d1l_mesh_admin_response_admit_result_t",
    )
    assert reap.index("if (!request || !caller_is_owner)") < \
        reap.index("__atomic_load_n(") < reap.index(
            "__atomic_compare_exchange_n(")
    assert "state != D1L_MESH_REQUEST_PENDING" in reap
    assert "request->deadline_us == 0U" in reap
    assert "now_us >= request->deadline_us" in reap
    assert "request->session_generation !=\n        current_session_generation" \
        in reap
    assert "D1L_MESH_REQUEST_OWNER_EXPIRED" in reap
    assert reap.index("__atomic_compare_exchange_n(") < \
        reap.index("d1l_mesh_admin_response_request_wipe(request)") < \
        reap.index("d1l_mesh_request_guard_release(")

    admit = body(
        command_guard,
        "d1l_mesh_admin_response_request_admit(",
        "static inline bool d1l_mesh_admin_response_request_complete",
    )
    assert admit.index("if (!caller_is_owner)") < admit.index(
        "d1l_mesh_request_guard_transition")
    assert "D1L_MESH_ADMIN_RESPONSE_ADMIT_OWNER_REQUIRED" in admit
    assert "request->session_generation !=" in admit
    assert "current_session_generation" in admit
    assert "D1L_MESH_ADMIN_RESPONSE_ADMIT_STALE_SESSION" in admit
    assert "now_us >= request->deadline_us" in admit
    assert "D1L_MESH_ADMIN_RESPONSE_ADMIT_EXPIRED" in admit
    assert "d1l_mesh_admin_response_request_wipe(request)" in admit
    assert "d1l_mesh_request_guard_release" in admit

    complete = command_guard[
        command_guard.index(
            "static inline bool d1l_mesh_admin_response_request_complete"):
    ]
    assert complete.index("d1l_mesh_admin_response_request_wipe") < \
        complete.index("d1l_mesh_request_guard_release")


def test_owner_maintenance_expires_idle_absolute_and_request_deadlines() -> None:
    service = read("main/mesh/meshcore_service.c")
    maintenance = body(
        service, "static void meshcore_service_run_owner_maintenance",
        "static void meshcore_service_handle_radio_rx_done",
    )
    expire = maintenance.index("d1l_meshcore_admin_runtime_expire(now_us)")
    capture = maintenance.index(
        "d1l_meshcore_admin_runtime_capture_pending(&pending_admin)")
    sweep = maintenance.index(
        "d1l_mesh_admin_response_request_owner_reap(")
    wipe = maintenance.index(
        "d1l_meshcore_admin_context_wipe(&pending_admin)")
    assert expire < capture < sweep < wipe
    assert "pending_admin.generation : 0U" in maintenance
    assert "index < D1L_MESHCORE_PRIORITY_QUEUE_LEN" in maintenance
    assert "pending_admin_generation, now_us" in maintenance


def test_admin_command_guards_are_exact_production_binding_sources() -> None:
    manifest = json.loads(read("tests/meshcore_oracle/manifest.json"))
    runner = read("scripts/meshcore_conformance_d1l.py")
    allowlist = runner.split(
        "EXPECTED_ORACLE_PRODUCTION_BINDING_SOURCE_PATHS = {", 1
    )[1].split("}", 1)[0]

    for relative in (
        "main/mesh/meshcore_admin_runtime.c",
        "main/mesh/meshcore_admin_runtime.h",
        "main/mesh/meshcore_command_guard.h",
        "main/mesh/meshcore_runtime_guard.h",
        "main/mesh/meshcore_service.c",
        "main/mesh/meshcore_service.h",
    ):
        assert manifest["production_binding_sources"][relative] == \
            canonical_sha256(relative)
        assert f'"{relative}"' in allowlist
